#include "sqlite_duckdb_vfs_cache.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/common/enums/memory_tag.hpp"
#include <cstring>

namespace duckdb {

ClientContext *SqliteDuckDBCacheVFS::current_context = nullptr;
std::mutex SqliteDuckDBCacheVFS::context_mutex;
bool SqliteDuckDBCacheVFS::vfs_registered = false;
sqlite3_vfs *SqliteDuckDBCacheVFS::registered_vfs = nullptr;

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
	
	// Use DuckDB's intended high-level CachingFileSystem interface
	auto caching_fs = CachingFileSystem::Get(context);
	auto flags = FileFlags::FILE_FLAGS_READ;
	if (FileSystem::IsRemoteFile(path)) {
		flags |= FileFlags::FILE_FLAGS_DIRECT_IO;  // Proper remote file handling
	}
	
	// Open the file through CachingFileSystem - this handles all cache management automatically
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
		// Use DuckDB's optimized cached read implementation
		data_ptr_t read_ptr;
		auto buffer_handle = caching_handle->Read(read_ptr, static_cast<idx_t>(amount), static_cast<idx_t>(offset));
		
		// Copy from DuckDB's cached buffer to SQLite's buffer
		memcpy(buffer, read_ptr, amount);
		
		return SQLITE_OK;
	} catch (const Exception &e) {
		// Convert DuckDB exceptions to SQLite errors
		return SQLITE_IOERR_READ;
	} catch (const std::exception &e) {
		return SQLITE_IOERR_READ;
	} catch (...) {
		return SQLITE_IOERR_READ;
	}
}

sqlite3_int64 DuckDBCachedFile::GetFileSize() {
	// Let DuckDB handle ALL cache management automatically
	return caching_handle->GetFileSize();
}

//===--------------------------------------------------------------------===//
// SqliteDuckDBCacheVFS Implementation
//===--------------------------------------------------------------------===//

bool SqliteDuckDBCacheVFS::CanHandlePath(ClientContext &context, const string &path) {
	return FileSystem::IsRemoteFile(path);
}

void SqliteDuckDBCacheVFS::Register(ClientContext &context) {
	
	// Store the context for use in VFS callbacks
	{
		std::lock_guard<std::mutex> lock(context_mutex);
		current_context = &context;
	}

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
	duckdb_vfs->pAppData = nullptr;

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
		
		// Get the current context
		ClientContext *context = nullptr;
		{
			std::lock_guard<std::mutex> lock(context_mutex);
			context = current_context;
		}
		
		if (!context) {
			return SQLITE_CANTOPEN;
		}

		// Initialize the file structure
		memset(duckdb_file, 0, sizeof(SqliteDuckDBCachedFile));
		duckdb_file->base.pMethods = &duckdb_cache_io_methods;
		duckdb_file->context = context; // Store context per-file instead of globally
		
		// Create the DuckDB cached file
		duckdb_file->duckdb_file = make_uniq<DuckDBCachedFile>(*context, filename);

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
			ClientContext *context = nullptr;
			{
				std::lock_guard<std::mutex> lock(context_mutex);
				context = current_context;
			}
			
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

	// For remote filesystems, just return the path as-is
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