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
    : context(context), path(path), 
      cache(ExternalFileCache::Get(context)),
      cached_file(cache.GetOrCreateCachedFile(path)),
      file_size(-1), size_fetched(false), last_modified(0) {
}

DuckDBCachedFile::~DuckDBCachedFile() {
}

BufferHandle DuckDBCachedFile::TryGetCachedRange(idx_t offset, idx_t amount) {
	auto guard = cached_file.lock.GetSharedLock();
	auto &ranges = cached_file.Ranges(guard);
	auto it = ranges.find(offset);
	
	if (it != ranges.end() && 
	    it->second->GetOverlap(amount, offset) == ExternalFileCache::CachedFileRangeOverlap::FULL) {
		return cache.GetBufferManager().Pin(it->second->block_handle);
	}
	return BufferHandle(); // Invalid handle = cache miss
}

void DuckDBCachedFile::EnsureFileOpen() {
	if (file_handle) {
		return;
	}
	
	auto &fs = context.db->GetFileSystem();
	file_handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	
	// Update cached file metadata
	auto write_guard = cached_file.lock.GetExclusiveLock();
	cached_file.FileSize(write_guard) = file_handle->GetFileSize();
	cached_file.LastModified(write_guard) = fs.GetLastModifiedTime(*file_handle);
	cached_file.VersionTag(write_guard) = fs.GetVersionTag(*file_handle);
	cached_file.CanSeek(write_guard) = file_handle->CanSeek();
	cached_file.OnDiskFile(write_guard) = file_handle->OnDiskFile();
	version_tag = cached_file.VersionTag(write_guard);
}

BufferHandle DuckDBCachedFile::ReadFromCache(idx_t offset, idx_t amount) {
	// Try cache hit first
	auto cached = TryGetCachedRange(offset, amount);
	if (cached.IsValid()) {
		return cached;
	}
	
	// Cache miss - ensure file is open and fetch the data
	EnsureFileOpen();
	
	// Allocate buffer and read data
	auto &buffer_manager = cache.GetBufferManager();
	auto buffer_handle = buffer_manager.Allocate(MemoryTag::EXTERNAL_FILE_CACHE, amount);
	file_handle->Read(buffer_handle.Ptr(), amount, offset);
	
	// Create and store cached range
	auto new_range = make_shared_ptr<ExternalFileCache::CachedFileRange>(
		buffer_handle.GetBlockHandle(), amount, offset, version_tag);
	
	auto write_guard = cached_file.lock.GetExclusiveLock();
	cached_file.Ranges(write_guard)[offset] = new_range;
	
	return buffer_handle;
}

int DuckDBCachedFile::Read(void *buffer, int amount, sqlite3_int64 offset) {
	if (amount <= 0) {
		return SQLITE_OK;
	}

	// Get file size if not already fetched
	if (!size_fetched) {
		GetFileSize();
	}

	if (offset >= file_size) {
		return SQLITE_IOERR_SHORT_READ;
	}

	auto *output = reinterpret_cast<char*>(buffer);
	auto read_offset = static_cast<idx_t>(offset);
	auto read_amount = static_cast<idx_t>(amount);

	// Adjust read amount if it would go past EOF
	if (read_offset + read_amount > static_cast<idx_t>(file_size)) {
		read_amount = static_cast<idx_t>(file_size) - read_offset;
	}

	try {
		idx_t bytes_read = 0;
		
		while (bytes_read < read_amount) {
			// Calculate block-aligned read
			idx_t block_start = (read_offset / BLOCK_SIZE) * BLOCK_SIZE;
			idx_t block_offset = read_offset % BLOCK_SIZE;
			idx_t block_size = MinValue<idx_t>(BLOCK_SIZE, static_cast<idx_t>(file_size) - block_start);
			idx_t bytes_to_read = MinValue<idx_t>(block_size - block_offset, read_amount - bytes_read);
			
			// Read the block from cache
			auto buffer_handle = ReadFromCache(block_start, block_size);
			
			// Copy to output buffer
			memcpy(output + bytes_read, buffer_handle.Ptr() + block_offset, bytes_to_read);
			
			bytes_read += bytes_to_read;
			read_offset += bytes_to_read;
		}

		// SQLite expects SQLITE_IOERR_SHORT_READ if we read less than requested
		if (bytes_read < static_cast<idx_t>(amount)) {
			memset(output + bytes_read, 0, amount - bytes_read);
			return SQLITE_IOERR_SHORT_READ;
		}

		return SQLITE_OK;
	} catch (const Exception &e) {
		// Convert DuckDB exceptions to SQLite errors
		return SQLITE_IOERR_READ;
	}
}

sqlite3_int64 DuckDBCachedFile::GetFileSize() {
	if (size_fetched) {
		return file_size;
	}

	// Check cached file size first
	auto guard = cached_file.lock.GetSharedLock();
	file_size = cached_file.FileSize(guard);
	
	if (file_size == 0) {
		// Need to open file to get size
		guard.reset();
		EnsureFileOpen();
		file_size = file_handle->GetFileSize();
	}
	
	size_fetched = true;
	return file_size;
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
		
		// Create the DuckDB cached file
		duckdb_file->duckdb_file = make_uniq<DuckDBCachedFile>(*context, filename);

		if (out_flags) {
			*out_flags = flags;
		}

		return SQLITE_OK;
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

	*size = duckdb_file->duckdb_file->GetFileSize();
	return SQLITE_OK;
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