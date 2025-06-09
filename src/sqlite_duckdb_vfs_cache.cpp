#include "sqlite_duckdb_vfs_cache.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include <cstring>

namespace duckdb {

// Thread-local storage for the current ClientContext pointer.
// This allows VFS callbacks to access the DuckDB context without storing it in the VFS itself,
// preventing use-after-free issues when contexts are destroyed.
#ifdef _WIN32
// On Windows, thread-local storage in DLLs can be problematic, especially when
// callbacks cross DLL boundaries. We use __declspec(thread) for better compatibility.
__declspec(thread) ClientContext* current_vfs_context = nullptr;
#else
thread_local ClientContext* current_vfs_context = nullptr;
#endif

// SQLite page size constant for sector size calculations
static constexpr int DEFAULT_SQLITE_SECTOR_SIZE = 4096;

// Forward declare the io_methods structure - will be initialized at runtime
// to avoid static initialization order issues on Windows
static sqlite3_io_methods duckdb_cache_io_methods = {};
static bool io_methods_initialized = false;

static void InitializeIOMethods() {
	if (io_methods_initialized) {
		return;
	}
	
	memset(&duckdb_cache_io_methods, 0, sizeof(duckdb_cache_io_methods));
	
	duckdb_cache_io_methods.iVersion = 1;
	duckdb_cache_io_methods.xClose = SQLiteDuckDBCacheVFS::Close;
	duckdb_cache_io_methods.xRead = SQLiteDuckDBCacheVFS::Read;
	duckdb_cache_io_methods.xWrite = SQLiteDuckDBCacheVFS::Write;
	duckdb_cache_io_methods.xTruncate = SQLiteDuckDBCacheVFS::Truncate;
	duckdb_cache_io_methods.xSync = SQLiteDuckDBCacheVFS::Sync;
	duckdb_cache_io_methods.xFileSize = SQLiteDuckDBCacheVFS::FileSize;
	duckdb_cache_io_methods.xLock = SQLiteDuckDBCacheVFS::Lock;
	duckdb_cache_io_methods.xUnlock = SQLiteDuckDBCacheVFS::Unlock;
	duckdb_cache_io_methods.xCheckReservedLock = SQLiteDuckDBCacheVFS::CheckReservedLock;
	duckdb_cache_io_methods.xFileControl = SQLiteDuckDBCacheVFS::FileControl;
	duckdb_cache_io_methods.xSectorSize = SQLiteDuckDBCacheVFS::SectorSize;
	duckdb_cache_io_methods.xDeviceCharacteristics = SQLiteDuckDBCacheVFS::DeviceCharacteristics;
	// Shared memory methods not needed for read-only remote files
	duckdb_cache_io_methods.xShmMap = nullptr;
	duckdb_cache_io_methods.xShmLock = nullptr;
	duckdb_cache_io_methods.xShmBarrier = nullptr;
	duckdb_cache_io_methods.xShmUnmap = nullptr;
	duckdb_cache_io_methods.xFetch = nullptr;
	duckdb_cache_io_methods.xUnfetch = nullptr;
	
	io_methods_initialized = true;
}

//===--------------------------------------------------------------------===//
// DuckDBCachedFile Implementation
//===--------------------------------------------------------------------===//

DuckDBCachedFile::DuckDBCachedFile(ClientContext &context, const string &path) 
    : path(path) {
	
	// Configure file flags for optimal caching behavior.
	// Remote files use DIRECT_IO to bypass OS caching since DuckDB's
	// CachingFileSystem provides its own intelligent block caching.
	auto flags = FileFlags::FILE_FLAGS_READ;
	if (FileSystem::IsRemoteFile(path)) {
		flags |= FileFlags::FILE_FLAGS_DIRECT_IO;
	}
	
	// Open the file through DuckDB's CachingFileSystem.
	// The CachingFileSystem provides efficient caching of remote file data,
	// though the actual read patterns are determined by our Read implementation.
	auto caching_fs = CachingFileSystem::Get(context);
	OpenFileInfo file_info(path);
	caching_handle = caching_fs.OpenFile(file_info, flags);
	
	// Cache the file size to avoid repeated remote calls
	cached_file_size = caching_handle->GetFileSize();
}

DuckDBCachedFile::~DuckDBCachedFile() = default;


int DuckDBCachedFile::Read(void *buffer, int amount, sqlite3_int64 offset) {
	// Early return for empty reads (SQLite sometimes requests 0 bytes)
	if (!buffer || amount <= 0) {
		return SQLITE_OK;
	}
	
	// Safety check - should never happen in normal operation
	if (!caching_handle) {
		return SQLITE_IOERR_READ;
	}

	try {
		// DuckDB's CachingFileSystem returns a pointer to its internal buffer.
		// This avoids unnecessary copies for cached data.
		data_ptr_t read_buffer = nullptr;
		auto buffer_handle = caching_handle->Read(read_buffer, amount, offset);
		
		// Safety check - CachingFileSystem should always return a valid buffer
		if (!read_buffer) {
			return SQLITE_IOERR_READ;
		}
		
		memcpy(buffer, read_buffer, amount);
		return SQLITE_OK;
	} catch (...) {
		// Map all exceptions to SQLite I/O errors.
		// DuckDB will have already logged the actual error details.
		return SQLITE_IOERR_READ;
	}
}

sqlite3_int64 DuckDBCachedFile::GetFileSize() {
	return cached_file_size;
}

static void ValidateSQLiteHeader(DuckDBCachedFile &file) {
	// SQLite database files always start with a 16-byte magic header.
	// This validation ensures we don't try to open non-SQLite files.
	constexpr char SQLITE_HEADER[] = "SQLite format 3\000";
	constexpr size_t SQLITE_HEADER_SIZE = 16;
	
	// Read and validate the SQLite file header
	char header_buffer[SQLITE_HEADER_SIZE];
	int result = file.Read(header_buffer, SQLITE_HEADER_SIZE, 0);
	
	if (result != SQLITE_OK) {
		throw InvalidInputException("Failed to read file header");
	}
	
	// Ensure this is actually a SQLite database file
	if (memcmp(header_buffer, SQLITE_HEADER, SQLITE_HEADER_SIZE) != 0) {
		throw InvalidInputException("File is not a valid SQLite database");
	}
}

//===--------------------------------------------------------------------===//
// SQLiteDuckDBCacheVFS Implementation
//===--------------------------------------------------------------------===//

bool SQLiteDuckDBCacheVFS::CanHandlePath(ClientContext &context, const string &path) {
	return FileSystem::IsRemoteFile(path);
}

// Static VFS structure must be at file scope for Windows DLL compatibility.
// This ensures proper initialization across DLL boundaries.
static sqlite3_vfs duckdb_vfs = {};
static bool vfs_initialized = false;

void SQLiteDuckDBCacheVFS::Register(ClientContext &context) {
	// Initialize IO methods structure on first use
	InitializeIOMethods();
	
	// Store context for this thread's VFS operations
	current_vfs_context = &context;
	
	// SQLite VFS registration is global, so we only need to register once.
	// Multiple calls just update the thread-local context.
	sqlite3_vfs *existing_vfs = sqlite3_vfs_find(GetVFSName());
	if (existing_vfs) {
		return;
	}

	// Find SQLite's default VFS to delegate some operations
	sqlite3_vfs *default_vfs = sqlite3_vfs_find(nullptr);
	if (!default_vfs) {
		throw InternalException("Failed to find default SQLite VFS");
	}

	// Initialize the VFS structure only once
	if (!vfs_initialized) {
		memset(&duckdb_vfs, 0, sizeof(duckdb_vfs));
		
		duckdb_vfs.iVersion = 1;
		duckdb_vfs.szOsFile = sizeof(SQLiteDuckDBCachedFile);
		duckdb_vfs.mxPathname = default_vfs->mxPathname;
		duckdb_vfs.zName = GetVFSName();
		// We use thread-local storage instead of pAppData to avoid lifetime issues.
		// The ClientContext might be destroyed before the VFS is unregistered.
		duckdb_vfs.pAppData = nullptr;

		// Configure VFS methods - most delegate to our implementations
		duckdb_vfs.xOpen = Open;
		duckdb_vfs.xDelete = Delete;
		duckdb_vfs.xAccess = Access;
		duckdb_vfs.xFullPathname = FullPathname;
		duckdb_vfs.xDlOpen = DlOpen;
		duckdb_vfs.xDlError = DlError;
		duckdb_vfs.xDlSym = DlSym;
		duckdb_vfs.xDlClose = DlClose;
		duckdb_vfs.xRandomness = Randomness;
		duckdb_vfs.xSleep = Sleep;
		duckdb_vfs.xCurrentTime = CurrentTime;
		duckdb_vfs.xGetLastError = GetLastError;
		
		vfs_initialized = true;
	}

	// Register our VFS with SQLite (non-default)
	int rc = sqlite3_vfs_register(&duckdb_vfs, 0);
	if (rc != SQLITE_OK) {
		throw InternalException("Failed to register DuckDB Cache VFS: %s", sqlite3_errstr(rc));
	}
}

//===--------------------------------------------------------------------===//
// VFS Methods
//===--------------------------------------------------------------------===//

// Macro to delegate operations to SQLite's default VFS.
// This is used for operations that don't need special handling for remote files.
#define DELEGATE_TO_DEFAULT_VFS(method_name, ...) \
	sqlite3_vfs *default_vfs = sqlite3_vfs_find(nullptr); \
	if (default_vfs && default_vfs->method_name) { \
		return default_vfs->method_name(default_vfs, __VA_ARGS__); \
	} \
	return SQLITE_OK;

int SQLiteDuckDBCacheVFS::Open(sqlite3_vfs *vfs, const char *filename, sqlite3_file *file, int flags, int *out_flags) {
	// Validate parameters and ensure read-only access
	if (!vfs || !filename || !file || (flags & SQLITE_OPEN_READONLY) == 0) {
		return SQLITE_CANTOPEN;
	}

	try {
		// Ensure SQLite allocated enough space for our file structure
		if (vfs->szOsFile < sizeof(SQLiteDuckDBCachedFile)) {
			return SQLITE_CANTOPEN;
		}
		
		auto *duckdb_file = reinterpret_cast<SQLiteDuckDBCachedFile*>(file);
		
		// Retrieve the context for this thread
		ClientContext *context = current_vfs_context;
		if (!context) {
			// On Windows, thread-local storage might not be initialized in callbacks
			// This is a safety check - the context should always be set by Register()
			return SQLITE_CANTOPEN;
		}
		
		// Validate the context pointer is accessible
		try {
			// Perform a simple operation to verify the context is valid
			auto &db = context->db;
			if (!db) {
				return SQLITE_CANTOPEN;
			}
		} catch (...) {
			// Context pointer is invalid
			return SQLITE_CANTOPEN;
		}

		// Zero-initialize the entire structure for safety
		memset(duckdb_file, 0, sizeof(SQLiteDuckDBCachedFile));
		duckdb_file->base.pMethods = &duckdb_cache_io_methods;
		duckdb_file->context = context;
		
		// Create the DuckDB file handle with proper exception handling
		try {
			duckdb_file->duckdb_file = make_uniq<DuckDBCachedFile>(*context, filename);
		} catch (...) {
			// Clean up on failure
			memset(duckdb_file, 0, sizeof(SQLiteDuckDBCachedFile));
			return SQLITE_CANTOPEN;
		}
		
		// Verify this is actually a SQLite database file.
		// This prevents SQLite from trying to interpret arbitrary files.
		try {
			ValidateSQLiteHeader(*duckdb_file->duckdb_file);
		} catch (const Exception &e) {
			// Clean up on failure
			duckdb_file->duckdb_file.reset();
			memset(duckdb_file, 0, sizeof(SQLiteDuckDBCachedFile));
			return SQLITE_CANTOPEN;
		}

		if (out_flags) {
			*out_flags = flags;
		}

		return SQLITE_OK;
	} catch (const PermissionException &e) {
		return SQLITE_PERM;
	} catch (...) {
		// All other exceptions map to CANTOPEN
		return SQLITE_CANTOPEN;
	}
}

int SQLiteDuckDBCacheVFS::Delete(sqlite3_vfs *vfs, const char *filename, int sync_dir) {
	// Remote files cannot be deleted through this VFS
	return SQLITE_IOERR_DELETE;
}

int SQLiteDuckDBCacheVFS::Access(sqlite3_vfs *vfs, const char *filename, int flags, int *result) {
	if (!filename || !result) {
		return SQLITE_IOERR;
	}

	// Initialize result to safe default
	*result = 0;

	if (flags == SQLITE_ACCESS_EXISTS) {
		try {
			// Use DuckDB's filesystem to check file existence
			ClientContext *context = current_vfs_context;
			
			if (context) {
				// Validate context is accessible
				try {
					auto &db = context->db;
					if (db) {
						auto &fs = db->GetFileSystem();
						*result = fs.FileExists(filename) ? 1 : 0;
					}
				} catch (...) {
					// Context is invalid - file doesn't exist from our perspective
					*result = 0;
				}
			} else {
				// No context - file doesn't exist from our perspective
				*result = 0;
			}
		} catch (...) {
			*result = 0;
		}
	} else {
		// Remote files don't support write or delete access
		*result = 0;
	}

	return SQLITE_OK;
}

int SQLiteDuckDBCacheVFS::FullPathname(sqlite3_vfs *vfs, const char *filename, int out_size, char *out_buf) {
	if (!filename || !out_buf || out_size <= 0) {
		return SQLITE_IOERR;
	}

	// Remote paths are already absolute URLs - return as-is
	strncpy(out_buf, filename, out_size - 1);
	out_buf[out_size - 1] = '\0';
	return SQLITE_OK;
}

// These methods don't need special handling - delegate to default VFS
int SQLiteDuckDBCacheVFS::Randomness(sqlite3_vfs *vfs, int bytes, char *out) {
	DELEGATE_TO_DEFAULT_VFS(xRandomness, bytes, out);
}

int SQLiteDuckDBCacheVFS::Sleep(sqlite3_vfs *vfs, int microseconds) {
	DELEGATE_TO_DEFAULT_VFS(xSleep, microseconds);
}

int SQLiteDuckDBCacheVFS::CurrentTime(sqlite3_vfs *vfs, double *time) {
	DELEGATE_TO_DEFAULT_VFS(xCurrentTime, time);
}

// Dynamic library operations are not supported for remote files
void *SQLiteDuckDBCacheVFS::DlOpen(sqlite3_vfs *vfs, const char *filename) {
	return nullptr;
}

void SQLiteDuckDBCacheVFS::DlError(sqlite3_vfs *vfs, int bytes, char *err_msg) {
	if (err_msg && bytes > 0) {
		strncpy(err_msg, "Dynamic loading not supported for remote files", bytes - 1);
		err_msg[bytes - 1] = '\0';
	}
}

void (*SQLiteDuckDBCacheVFS::DlSym(sqlite3_vfs *vfs, void *handle, const char *symbol))(void) {
	return nullptr;
}

void SQLiteDuckDBCacheVFS::DlClose(sqlite3_vfs *vfs, void *handle) {
	// No-op - dynamic libraries not supported
}

int SQLiteDuckDBCacheVFS::GetLastError(sqlite3_vfs *vfs, int bytes, char *err_msg) {
	if (err_msg && bytes > 0) {
		err_msg[0] = '\0';
	}
	return 0;
}

//===--------------------------------------------------------------------===//
// File Methods
//===--------------------------------------------------------------------===//

int SQLiteDuckDBCacheVFS::Close(sqlite3_file *file) {
	if (file) {
		auto *duckdb_file = reinterpret_cast<SQLiteDuckDBCachedFile*>(file);
		duckdb_file->duckdb_file.reset();
	}
	return SQLITE_OK;
}

int SQLiteDuckDBCacheVFS::Read(sqlite3_file *file, void *buffer, int amount, sqlite3_int64 offset) {
	if (!file || !buffer) {
		return SQLITE_IOERR_READ;
	}

	auto *duckdb_file = reinterpret_cast<SQLiteDuckDBCachedFile*>(file);
	if (!duckdb_file->duckdb_file) {
		return SQLITE_IOERR_READ;
	}

	return duckdb_file->duckdb_file->Read(buffer, amount, offset);
}

int SQLiteDuckDBCacheVFS::FileSize(sqlite3_file *file, sqlite3_int64 *size) {
	if (!file || !size) {
		return SQLITE_IOERR;
	}

	auto *duckdb_file = reinterpret_cast<SQLiteDuckDBCachedFile*>(file);
	if (!duckdb_file->duckdb_file) {
		return SQLITE_IOERR;
	}

	try {
		*size = duckdb_file->duckdb_file->GetFileSize();
		return SQLITE_OK;
	} catch (...) {
		return SQLITE_IOERR;
	}
}

// Write operations return SQLITE_READONLY since remote files are read-only
int SQLiteDuckDBCacheVFS::Write(sqlite3_file *file, const void *buffer, int amount, sqlite3_int64 offset) {
	return SQLITE_READONLY;
}

int SQLiteDuckDBCacheVFS::Truncate(sqlite3_file *file, sqlite3_int64 size) {
	return SQLITE_READONLY;
}

int SQLiteDuckDBCacheVFS::Sync(sqlite3_file *file, int flags) {
	// No-op for read-only files
	return SQLITE_OK;
}

int SQLiteDuckDBCacheVFS::Lock(sqlite3_file *file, int level) {
	// Remote files don't need locking - they're read-only and immutable
	return SQLITE_OK;
}

int SQLiteDuckDBCacheVFS::Unlock(sqlite3_file *file, int level) {
	// Remote files don't need locking - they're read-only and immutable
	return SQLITE_OK;
}

int SQLiteDuckDBCacheVFS::CheckReservedLock(sqlite3_file *file, int *result) {
	if (result) {
		*result = 0;
	}
	return SQLITE_OK;
}

int SQLiteDuckDBCacheVFS::FileControl(sqlite3_file *file, int op, void *arg) {
	// No special file control operations are implemented
	return SQLITE_NOTFOUND;
}

int SQLiteDuckDBCacheVFS::SectorSize(sqlite3_file *file) {
	return DEFAULT_SQLITE_SECTOR_SIZE;
}

int SQLiteDuckDBCacheVFS::DeviceCharacteristics(sqlite3_file *file) {
	// Remote files are immutable - they cannot be modified
	return SQLITE_IOCAP_IMMUTABLE;
}

} // namespace duckdb