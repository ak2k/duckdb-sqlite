#include "sqlite_duckdb_vfs_cache.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/common/enums/memory_tag.hpp"
#include <cstring>

namespace duckdb {

// SQLite VFS registration is handled by sqlite3_vfs_register()

static sqlite3_io_methods duckdb_cache_io_methods = {
    1,                                         // iVersion
    SqliteDuckDBCacheVFS::Close,               // xClose
    SqliteDuckDBCacheVFS::Read,                // xRead
    SqliteDuckDBCacheVFS::Write,               // xWrite
    SqliteDuckDBCacheVFS::Truncate,            // xTruncate
    SqliteDuckDBCacheVFS::Sync,                // xSync
    SqliteDuckDBCacheVFS::FileSize,            // xFileSize
    SqliteDuckDBCacheVFS::Lock,                // xLock
    SqliteDuckDBCacheVFS::Unlock,              // xUnlock
    SqliteDuckDBCacheVFS::CheckReservedLock,   // xCheckReservedLock
    SqliteDuckDBCacheVFS::FileControl,         // xFileControl
    SqliteDuckDBCacheVFS::SectorSize,          // xSectorSize
    SqliteDuckDBCacheVFS::DeviceCharacteristics, // xDeviceCharacteristics
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
	
	// Initialize DuckDB's CachingFileSystem for remote file access
	auto caching_fs = CachingFileSystem::Get(context);
	auto flags = FileFlags::FILE_FLAGS_READ;
	if (FileSystem::IsRemoteFile(path)) {
		flags |= FileFlags::FILE_FLAGS_DIRECT_IO;
	}
	OpenFileInfo file_info(path);
	caching_handle = caching_fs.OpenFile(file_info, flags);
}

DuckDBCachedFile::~DuckDBCachedFile() {
}


int DuckDBCachedFile::Read(void *buffer, int amount, sqlite3_int64 offset) {
	if (amount <= 0) {
		return SQLITE_OK;
	}

	try {
		// Read-ahead optimization: SQLite uses 4KB pages, but we read 1MB blocks
		// to reduce HTTP requests and leverage DuckDB's caching
		constexpr idx_t READ_AHEAD_SIZE = 1024 * 1024;
		
		idx_t requested_offset = static_cast<idx_t>(offset);
		idx_t requested_amount = static_cast<idx_t>(amount);
		
		// Align reads to 1MB block boundaries
		idx_t block_start = (requested_offset / READ_AHEAD_SIZE) * READ_AHEAD_SIZE;
		idx_t block_end = block_start + READ_AHEAD_SIZE;
		
		// Get file size to avoid reading past end of file
		idx_t file_size = static_cast<idx_t>(GetFileSize());
		if (block_end > file_size) {
			block_end = file_size;
		}
		
		// Read the aligned block (up to 1MB or end of file)
		idx_t read_amount = block_end - block_start;
		if (read_amount > 0) {
			data_ptr_t read_ptr;
			auto buffer_handle = caching_handle->Read(read_ptr, read_amount, block_start);
			
			// Extract requested data from the larger read block
			idx_t offset_in_block = requested_offset - block_start;
			if (offset_in_block + requested_amount <= read_amount) {
				memcpy(buffer, read_ptr + offset_in_block, requested_amount);
			} else {
				// Handle edge case where request spans beyond block boundary
				idx_t available = read_amount - offset_in_block;
				if (available > 0) {
					memcpy(buffer, read_ptr + offset_in_block, available);
					// Zero remaining buffer for short read
					memset(static_cast<char*>(buffer) + available, 0, requested_amount - available);
				}
				return SQLITE_IOERR_SHORT_READ;
			}
		}
		
		return SQLITE_OK;
	} catch (...) {
		// Convert any exception to SQLite I/O error
		return SQLITE_IOERR_READ;
	}
}

sqlite3_int64 DuckDBCachedFile::GetFileSize() {
	// File size is managed by DuckDB's caching layer
	return caching_handle->GetFileSize();
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
// SqliteDuckDBCacheVFS Implementation
//===--------------------------------------------------------------------===//

bool SqliteDuckDBCacheVFS::CanHandlePath(ClientContext &context, const string &path) {
	return FileSystem::IsRemoteFile(path);
}

void SqliteDuckDBCacheVFS::Register(ClientContext &context) {
	// Check if VFS is already registered
	sqlite3_vfs *existing_vfs = sqlite3_vfs_find(GetVFSName());
	if (existing_vfs) {
		return; // Already registered
	}

	// Get the default VFS to use as a base
	sqlite3_vfs *default_vfs = sqlite3_vfs_find(nullptr);
	if (!default_vfs) {
		throw InternalException("Failed to find default SQLite VFS");
	}

	// Create our custom VFS
	auto *duckdb_vfs = new sqlite3_vfs();
	memset(duckdb_vfs, 0, sizeof(sqlite3_vfs));

	duckdb_vfs->iVersion = 1;
	duckdb_vfs->szOsFile = sizeof(SqliteDuckDBCachedFile);
	duckdb_vfs->mxPathname = default_vfs->mxPathname;
	duckdb_vfs->zName = GetVFSName();
	duckdb_vfs->pAppData = &context; // Store context in VFS for file operations

	// Set up methods
	duckdb_vfs->xOpen = Open;
	duckdb_vfs->xDelete = Delete;
	duckdb_vfs->xAccess = Access;
	duckdb_vfs->xFullPathname = FullPathname;
	duckdb_vfs->xDlOpen = DlOpen;
	duckdb_vfs->xDlError = DlError;
	duckdb_vfs->xDlSym = DlSym;
	duckdb_vfs->xDlClose = DlClose;
	duckdb_vfs->xRandomness = Randomness;
	duckdb_vfs->xSleep = Sleep;
	duckdb_vfs->xCurrentTime = CurrentTime;
	duckdb_vfs->xGetLastError = GetLastError;

	// Register the VFS
	int rc = sqlite3_vfs_register(duckdb_vfs, 0);
	if (rc != SQLITE_OK) {
		delete duckdb_vfs;
		throw InternalException("Failed to register DuckDB Cache VFS: %s", sqlite3_errstr(rc));
	}
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

int SqliteDuckDBCacheVFS::Open(sqlite3_vfs *vfs, const char *filename, sqlite3_file *file, int flags, int *out_flags) {
	if (!filename || (flags & SQLITE_OPEN_READONLY) == 0) {
		return SQLITE_CANTOPEN;
	}

	try {
		auto *duckdb_file = reinterpret_cast<SqliteDuckDBCachedFile*>(file);
		
		// Get ClientContext from VFS
		ClientContext *context = static_cast<ClientContext*>(vfs->pAppData);
		if (!context) {
			return SQLITE_CANTOPEN;
		}

		// Initialize the file structure
		memset(duckdb_file, 0, sizeof(SqliteDuckDBCachedFile));
		duckdb_file->base.pMethods = &duckdb_cache_io_methods;
		duckdb_file->context = context; // Store context for file operations
		
		// Initialize cached file for remote access
		duckdb_file->duckdb_file = make_uniq<DuckDBCachedFile>(*context, filename);
		
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
	} catch (const Exception &e) {
		return SQLITE_CANTOPEN;
	} catch (const std::exception &e) {
		return SQLITE_CANTOPEN;
	} catch (...) {
		return SQLITE_CANTOPEN;
	}
}

int SqliteDuckDBCacheVFS::Delete(sqlite3_vfs *vfs, const char *filename, int sync_dir) {
	return SQLITE_IOERR_DELETE; // Cannot delete remote files
}

int SqliteDuckDBCacheVFS::Access(sqlite3_vfs *vfs, const char *filename, int flags, int *result) {
	if (!filename || !result) {
		return SQLITE_IOERR;
	}

	if (flags == SQLITE_ACCESS_EXISTS) {
		try {
			// Get ClientContext from VFS
			ClientContext *context = static_cast<ClientContext*>(vfs->pAppData);
			
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

int SqliteDuckDBCacheVFS::FullPathname(sqlite3_vfs *vfs, const char *filename, int out_size, char *out_buf) {
	if (!filename || !out_buf || out_size <= 0) {
		return SQLITE_IOERR;
	}

	// Return path unchanged for remote files
	strncpy(out_buf, filename, out_size - 1);
	out_buf[out_size - 1] = '\0';
	return SQLITE_OK;
}

// Delegate simple methods to default VFS
int SqliteDuckDBCacheVFS::Randomness(sqlite3_vfs *vfs, int bytes, char *out) {
	DELEGATE_TO_DEFAULT_VFS(xRandomness, bytes, out);
}

int SqliteDuckDBCacheVFS::Sleep(sqlite3_vfs *vfs, int microseconds) {
	DELEGATE_TO_DEFAULT_VFS(xSleep, microseconds);
}

int SqliteDuckDBCacheVFS::CurrentTime(sqlite3_vfs *vfs, double *time) {
	DELEGATE_TO_DEFAULT_VFS(xCurrentTime, time);
}

// Unsupported operations for remote files
void *SqliteDuckDBCacheVFS::DlOpen(sqlite3_vfs *vfs, const char *filename) {
	return nullptr;
}

void SqliteDuckDBCacheVFS::DlError(sqlite3_vfs *vfs, int bytes, char *err_msg) {
	if (err_msg && bytes > 0) {
		strncpy(err_msg, "Dynamic loading not supported for remote files", bytes - 1);
		err_msg[bytes - 1] = '\0';
	}
}

void (*SqliteDuckDBCacheVFS::DlSym(sqlite3_vfs *vfs, void *handle, const char *symbol))(void) {
	return nullptr;
}

void SqliteDuckDBCacheVFS::DlClose(sqlite3_vfs *vfs, void *handle) {
}

int SqliteDuckDBCacheVFS::GetLastError(sqlite3_vfs *vfs, int bytes, char *err_msg) {
	if (err_msg && bytes > 0) {
		err_msg[0] = '\0';
	}
	return 0;
}

//===--------------------------------------------------------------------===//
// File Methods
//===--------------------------------------------------------------------===//

int SqliteDuckDBCacheVFS::Close(sqlite3_file *file) {
	if (file) {
		auto *duckdb_file = reinterpret_cast<SqliteDuckDBCachedFile*>(file);
		duckdb_file->duckdb_file.reset();
	}
	return SQLITE_OK;
}

int SqliteDuckDBCacheVFS::Read(sqlite3_file *file, void *buffer, int amount, sqlite3_int64 offset) {
	if (!file || !buffer) {
		return SQLITE_IOERR_READ;
	}

	auto *duckdb_file = reinterpret_cast<SqliteDuckDBCachedFile*>(file);
	if (!duckdb_file->duckdb_file) {
		return SQLITE_IOERR_READ;
	}

	return duckdb_file->duckdb_file->Read(buffer, amount, offset);
}

int SqliteDuckDBCacheVFS::FileSize(sqlite3_file *file, sqlite3_int64 *size) {
	if (!file || !size) {
		return SQLITE_IOERR;
	}

	auto *duckdb_file = reinterpret_cast<SqliteDuckDBCachedFile*>(file);
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
int SqliteDuckDBCacheVFS::Write(sqlite3_file *file, const void *buffer, int amount, sqlite3_int64 offset) {
	return SQLITE_READONLY;
}

int SqliteDuckDBCacheVFS::Truncate(sqlite3_file *file, sqlite3_int64 size) {
	return SQLITE_READONLY;
}

int SqliteDuckDBCacheVFS::Sync(sqlite3_file *file, int flags) {
	return SQLITE_OK; // Nothing to sync for read-only files
}

int SqliteDuckDBCacheVFS::Lock(sqlite3_file *file, int level) {
	return SQLITE_OK; // No locking needed for read-only remote files
}

int SqliteDuckDBCacheVFS::Unlock(sqlite3_file *file, int level) {
	return SQLITE_OK; // No locking needed for read-only remote files
}

int SqliteDuckDBCacheVFS::CheckReservedLock(sqlite3_file *file, int *result) {
	if (result) {
		*result = 0;
	}
	return SQLITE_OK;
}

int SqliteDuckDBCacheVFS::FileControl(sqlite3_file *file, int op, void *arg) {
	return SQLITE_NOTFOUND; // No special file control operations
}

int SqliteDuckDBCacheVFS::SectorSize(sqlite3_file *file) {
	return 4096; // Default sector size
}

int SqliteDuckDBCacheVFS::DeviceCharacteristics(sqlite3_file *file) {
	return SQLITE_IOCAP_IMMUTABLE; // Read-only device
}

} // namespace duckdb