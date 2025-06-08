#include "sqlite_duckdb_vfs_cache.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include <cstring>

namespace duckdb {

// Thread-local storage for current context during VFS operations
thread_local ClientContext* current_vfs_context = nullptr;

// SQLite VFS registration is handled by sqlite3_vfs_register()

// Constants
static constexpr int DEFAULT_SQLITE_SECTOR_SIZE = 4096;

static const sqlite3_io_methods duckdb_cache_io_methods = {
    1,                                         // iVersion
    SQLiteDuckDBCacheVFS::Close,               // xClose
    SQLiteDuckDBCacheVFS::Read,                // xRead
    SQLiteDuckDBCacheVFS::Write,               // xWrite
    SQLiteDuckDBCacheVFS::Truncate,            // xTruncate
    SQLiteDuckDBCacheVFS::Sync,                // xSync
    SQLiteDuckDBCacheVFS::FileSize,            // xFileSize
    SQLiteDuckDBCacheVFS::Lock,                // xLock
    SQLiteDuckDBCacheVFS::Unlock,              // xUnlock
    SQLiteDuckDBCacheVFS::CheckReservedLock,   // xCheckReservedLock
    SQLiteDuckDBCacheVFS::FileControl,         // xFileControl
    SQLiteDuckDBCacheVFS::SectorSize,          // xSectorSize
    SQLiteDuckDBCacheVFS::DeviceCharacteristics, // xDeviceCharacteristics
    nullptr,                                   // xShmMap
    nullptr,                                   // xShmLock
    nullptr,                                   // xShmBarrier
    nullptr,                                   // xShmUnmap
    nullptr,                                   // xFetch
    nullptr                                    // xUnfetch
};

//===--------------------------------------------------------------------===//
// DuckDBCachedFile Implementation
//===--------------------------------------------------------------------===//

DuckDBCachedFile::DuckDBCachedFile(ClientContext &context, const string &path) 
    : path(path) {
	
	// Use DuckDB's caching filesystem for efficient 1MB block access
	auto flags = FileFlags::FILE_FLAGS_READ;
	if (FileSystem::IsRemoteFile(path)) {
		flags |= FileFlags::FILE_FLAGS_DIRECT_IO;
	}
	
	// Use DuckDB's caching filesystem for efficient 1MB block access
	auto caching_fs = CachingFileSystem::Get(context);
	OpenFileInfo file_info(path);
	caching_handle = caching_fs.OpenFile(file_info, flags);
	
	// Cache file size from caching handle
	cached_file_size = caching_handle->GetFileSize();
}

DuckDBCachedFile::~DuckDBCachedFile() {
}


int DuckDBCachedFile::Read(void *buffer, int amount, sqlite3_int64 offset) {
	// Validate input parameters
	if (!buffer || amount <= 0) {
		return SQLITE_OK;
	}
	
	// Validate caching handle
	if (!caching_handle) {
		return SQLITE_IOERR_READ;
	}

	try {
		// Use DuckDB's CachingFileSystem for 1MB read-ahead
		data_ptr_t read_buffer = nullptr;
		auto buffer_handle = caching_handle->Read(read_buffer, amount, offset);
		
		// Validate read buffer before copying
		if (!read_buffer) {
			return SQLITE_IOERR_READ;
		}
		
		memcpy(buffer, read_buffer, amount);
		return SQLITE_OK;
	} catch (const std::exception &e) {
		// Convert any exception to SQLite I/O error
		return SQLITE_IOERR_READ;
	} catch (...) {
		// Convert any unknown exception to SQLite I/O error
		return SQLITE_IOERR_READ;
	}
}

sqlite3_int64 DuckDBCachedFile::GetFileSize() {
	// Return cached file size to avoid repeated calls
	return cached_file_size;
}

static void ValidateSQLiteHeader(DuckDBCachedFile &file) {
	// SQLite database file header is exactly 16 bytes: "SQLite format 3\000"
	constexpr char SQLITE_HEADER[] = "SQLite format 3\000";
	constexpr size_t SQLITE_HEADER_SIZE = 16;
	
	// Read the first 16 bytes to check SQLite header
	char header_buffer[SQLITE_HEADER_SIZE];
	int result = file.Read(header_buffer, SQLITE_HEADER_SIZE, 0);
	
	if (result != SQLITE_OK) {
		throw InvalidInputException("Failed to read file header");
	}
	
	// Compare with expected SQLite header
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

void SQLiteDuckDBCacheVFS::Register(ClientContext &context) {
	// Check if VFS is already registered
	sqlite3_vfs *existing_vfs = sqlite3_vfs_find(GetVFSName());
	if (existing_vfs) {
		// Set thread-local context for subsequent operations
		current_vfs_context = &context;
		return; // Already registered
	}

	// Get the default VFS to use as a base
	sqlite3_vfs *default_vfs = sqlite3_vfs_find(nullptr);
	if (!default_vfs) {
		throw InternalException("Failed to find default SQLite VFS");
	}

	// Use static VFS structure to avoid memory leak
	static sqlite3_vfs duckdb_vfs = {};
	
	duckdb_vfs.iVersion = 1;
	duckdb_vfs.szOsFile = sizeof(SQLiteDuckDBCachedFile);
	duckdb_vfs.mxPathname = default_vfs->mxPathname;
	duckdb_vfs.zName = GetVFSName();
	duckdb_vfs.pAppData = nullptr; // Don't store context pointer to avoid use-after-free

	// Set up methods
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

	// Register the VFS
	int rc = sqlite3_vfs_register(&duckdb_vfs, 0);
	if (rc != SQLITE_OK) {
		throw InternalException("Failed to register DuckDB Cache VFS: %s", sqlite3_errstr(rc));
	}
	
	// Set thread-local context for subsequent operations
	current_vfs_context = &context;
}

//===--------------------------------------------------------------------===//
// VFS Methods - Use default VFS where possible
//===--------------------------------------------------------------------===//

// Helper macro to delegate to default VFS
#define DELEGATE_TO_DEFAULT_VFS(method_name, ...) \
	sqlite3_vfs *default_vfs = sqlite3_vfs_find(nullptr); \
	if (default_vfs && default_vfs->method_name) { \
		return default_vfs->method_name(default_vfs, __VA_ARGS__); \
	} \
	return SQLITE_OK;

int SQLiteDuckDBCacheVFS::Open(sqlite3_vfs *vfs, const char *filename, sqlite3_file *file, int flags, int *out_flags) {
	// Validate input parameters
	if (!vfs || !filename || !file) {
		return SQLITE_CANTOPEN;
	}
	
	if ((flags & SQLITE_OPEN_READONLY) == 0) {
		return SQLITE_CANTOPEN;
	}

	try {
		// Validate file structure size and alignment
		if (vfs->szOsFile < sizeof(SQLiteDuckDBCachedFile)) {
			return SQLITE_CANTOPEN;
		}
		
		auto *duckdb_file = reinterpret_cast<SQLiteDuckDBCachedFile*>(file);
		
		// Get ClientContext from thread-local storage
		ClientContext *context = current_vfs_context;
		if (!context) {
			return SQLITE_CANTOPEN;
		}

		// Initialize the file structure (zero-initialize first for safety)
		memset(duckdb_file, 0, sizeof(SQLiteDuckDBCachedFile));
		duckdb_file->base.pMethods = &duckdb_cache_io_methods;
		duckdb_file->context = context; // Store context for file operations
		
		// Initialize cached file for remote access with additional error handling
		try {
			duckdb_file->duckdb_file = make_uniq<DuckDBCachedFile>(*context, filename);
		} catch (const std::exception &e) {
			// Clean up on failure
			memset(duckdb_file, 0, sizeof(SQLiteDuckDBCachedFile));
			return SQLITE_CANTOPEN;
		} catch (...) {
			// Clean up on unknown failure
			memset(duckdb_file, 0, sizeof(SQLiteDuckDBCachedFile));
			return SQLITE_CANTOPEN;
		}
		
		// Validate SQLite file format by checking header
		try {
			ValidateSQLiteHeader(*duckdb_file->duckdb_file);
		} catch (const Exception &e) {
			return SQLITE_CANTOPEN;
		}

		if (out_flags) {
			*out_flags = flags;
		}

		return SQLITE_OK;
	} catch (const HTTPException &e) {
		// Network-related errors (404, connection failed, etc.)
		return SQLITE_CANTOPEN;
	} catch (const IOException &e) {
		// File system errors (permissions, disk full, etc.)
		return SQLITE_CANTOPEN;
	} catch (const PermissionException &e) {
		// Access denied, authentication failures
		return SQLITE_PERM;
	} catch (const Exception &e) {
		// Other DuckDB exceptions
		return SQLITE_CANTOPEN;
	} catch (const std::exception &e) {
		// Standard library exceptions
		return SQLITE_CANTOPEN;
	} catch (...) {
		// Unknown exceptions
		return SQLITE_CANTOPEN;
	}
}

int SQLiteDuckDBCacheVFS::Delete(sqlite3_vfs *vfs, const char *filename, int sync_dir) {
	return SQLITE_IOERR_DELETE; // Cannot delete remote files
}

int SQLiteDuckDBCacheVFS::Access(sqlite3_vfs *vfs, const char *filename, int flags, int *result) {
	if (!filename || !result) {
		return SQLITE_IOERR;
	}

	if (flags == SQLITE_ACCESS_EXISTS) {
		try {
			// Get ClientContext from thread-local storage
			ClientContext *context = current_vfs_context;
			
			if (context) {
				auto &fs = context->db->GetFileSystem();
				*result = fs.FileExists(filename) ? 1 : 0;
			} else {
				*result = 0;
			}
		} catch (...) {
			*result = 0;
		}
	} else {
		*result = 0; // Remote files are read-only
	}

	return SQLITE_OK;
}

int SQLiteDuckDBCacheVFS::FullPathname(sqlite3_vfs *vfs, const char *filename, int out_size, char *out_buf) {
	if (!filename || !out_buf || out_size <= 0) {
		return SQLITE_IOERR;
	}

	// Return path unchanged for remote files
	strncpy(out_buf, filename, out_size - 1);
	out_buf[out_size - 1] = '\0';
	return SQLITE_OK;
}

// Delegate simple methods to default VFS
int SQLiteDuckDBCacheVFS::Randomness(sqlite3_vfs *vfs, int bytes, char *out) {
	DELEGATE_TO_DEFAULT_VFS(xRandomness, bytes, out);
}

int SQLiteDuckDBCacheVFS::Sleep(sqlite3_vfs *vfs, int microseconds) {
	DELEGATE_TO_DEFAULT_VFS(xSleep, microseconds);
}

int SQLiteDuckDBCacheVFS::CurrentTime(sqlite3_vfs *vfs, double *time) {
	DELEGATE_TO_DEFAULT_VFS(xCurrentTime, time);
}

// Unsupported operations for remote files
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
	} catch (const Exception &e) {
		return SQLITE_IOERR;
	} catch (const std::exception &e) {
		return SQLITE_IOERR;
	} catch (...) {
		return SQLITE_IOERR;
	}
}

// Read-only file operations
int SQLiteDuckDBCacheVFS::Write(sqlite3_file *file, const void *buffer, int amount, sqlite3_int64 offset) {
	return SQLITE_READONLY;
}

int SQLiteDuckDBCacheVFS::Truncate(sqlite3_file *file, sqlite3_int64 size) {
	return SQLITE_READONLY;
}

int SQLiteDuckDBCacheVFS::Sync(sqlite3_file *file, int flags) {
	return SQLITE_OK; // Nothing to sync for read-only files
}

int SQLiteDuckDBCacheVFS::Lock(sqlite3_file *file, int level) {
	return SQLITE_OK; // No locking needed for read-only remote files
}

int SQLiteDuckDBCacheVFS::Unlock(sqlite3_file *file, int level) {
	return SQLITE_OK; // No locking needed for read-only remote files
}

int SQLiteDuckDBCacheVFS::CheckReservedLock(sqlite3_file *file, int *result) {
	if (result) {
		*result = 0;
	}
	return SQLITE_OK;
}

int SQLiteDuckDBCacheVFS::FileControl(sqlite3_file *file, int op, void *arg) {
	return SQLITE_NOTFOUND; // No special file control operations
}

int SQLiteDuckDBCacheVFS::SectorSize(sqlite3_file *file) {
	return DEFAULT_SQLITE_SECTOR_SIZE;
}

int SQLiteDuckDBCacheVFS::DeviceCharacteristics(sqlite3_file *file) {
	return SQLITE_IOCAP_IMMUTABLE; // Read-only device
}

} // namespace duckdb