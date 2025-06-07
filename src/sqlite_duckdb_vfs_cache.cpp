#include "sqlite_duckdb_vfs_cache.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/buffer/block_handle.hpp"
#include "duckdb/common/enums/memory_tag.hpp"
#include <cstring>
#include <mutex>

namespace duckdb {

// Initialize static members
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
    : context(context), path(path), 
      cache(ExternalFileCache::Get(context)),
      cached_file(cache.GetOrCreateCachedFile(path)),
      file_size(-1), size_fetched(false), last_modified(0) {
	
	// Check if external file cache is enabled (following DuckDB pattern)
	if (!cache.IsEnabled()) {
		throw IOException("External file cache is not enabled for remote file access: %s", path);
	}
	
	// Validate that we got a valid cache and cached_file
	try {
		// Try to acquire a lock to verify cached_file is valid
		auto guard = cached_file.lock.GetSharedLock();
		(void)guard; // Suppress unused variable warning
	} catch (...) {
		throw IOException("Failed to initialize external file cache for path: %s", path);
	}
}

DuckDBCachedFile::~DuckDBCachedFile() {
}

BufferHandle DuckDBCachedFile::TryGetCachedRange(idx_t offset, idx_t amount) {
	auto guard = cached_file.lock.GetSharedLock();
	auto &ranges = cached_file.Ranges(guard);
	
	// Check if we have an exact match or containing range
	for (auto &range_pair : ranges) {
		auto &range = range_pair.second;
		if (range->GetOverlap(amount, offset) == ExternalFileCache::CachedFileRangeOverlap::FULL) {
			// Validate the range is still valid (following DuckDB pattern)
			if (range->version_tag == version_tag) {
				try {
					return cache.GetBufferManager().Pin(range->block_handle);
				} catch (...) {
					// If pinning fails, continue to next range
					continue;
				}
			}
		}
	}
	return BufferHandle(); // Invalid handle = cache miss
}

void DuckDBCachedFile::EnsureFileOpen() {
	if (file_handle) {
		return;
	}
	
	auto &fs = context.db->GetFileSystem();
	
	try {
		file_handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	} catch (const Exception &e) {
		throw IOException("Failed to open remote file '%s': %s", path, e.what());
	}
	
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
	
	// Use DuckDB's standard caching approach - allocate through buffer manager
	auto &buffer_manager = cache.GetBufferManager();
	auto buffer_handle = buffer_manager.Allocate(MemoryTag::EXTERNAL_FILE_CACHE, amount);
	
	// Read data from the file
	try {
		file_handle->Read(buffer_handle.Ptr(), amount, offset);
	} catch (const Exception &e) {
		// If read fails, don't cache anything
		throw IOException("Failed to read from remote file at offset %llu: %s", offset, e.what());
	}
	
	// Create and store cached range - follow DuckDB's caching pattern
	auto new_range = make_shared_ptr<ExternalFileCache::CachedFileRange>(
		buffer_handle.GetBlockHandle(), amount, offset, version_tag);
	
	// Add checksum for validation (following DuckDB pattern)
	new_range->AddCheckSum();
	
	// Store in cache with proper locking
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
			// Calculate block-aligned read (following DuckDB's approach)
			idx_t block_start = (read_offset / BLOCK_SIZE) * BLOCK_SIZE;
			idx_t block_offset = read_offset % BLOCK_SIZE;
			idx_t block_size = MinValue<idx_t>(BLOCK_SIZE, static_cast<idx_t>(file_size) - block_start);
			idx_t bytes_to_read = MinValue<idx_t>(block_size - block_offset, read_amount - bytes_read);
			
			// Try to read from cache first
			auto buffer_handle = TryGetCachedRange(block_start, block_size);
			if (!buffer_handle.IsValid()) {
				// Cache miss - fetch the entire block
				buffer_handle = ReadFromCache(block_start, block_size);
			}
			
			// Ensure we have a valid handle before accessing memory
			if (!buffer_handle.IsValid()) {
				throw IOException("Failed to read block at offset %llu", block_start);
			}
			
			auto *src_ptr = buffer_handle.Ptr() + block_offset;
			
			// Copy to output buffer with bounds checking
			if (block_offset + bytes_to_read > block_size) {
				throw IOException("Invalid read bounds: offset %llu, size %llu, block_size %llu", 
					block_offset, bytes_to_read, block_size);
			}
			
			memcpy(output + bytes_read, src_ptr, bytes_to_read);
			
			bytes_read += bytes_to_read;
			read_offset += bytes_to_read;
			
			// Explicitly keep buffer_handle alive until after memcpy
			(void)buffer_handle;
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
	std::lock_guard<std::mutex> lock(context_mutex);
	
	// Store the context for use in VFS callbacks
	current_context = &context;

	// Check if VFS is already registered
	sqlite3_vfs *existing_vfs = sqlite3_vfs_find(GetVFSName());
	if (existing_vfs) {
		vfs_registered = true;
		return; // Already registered
	}
	
	if (vfs_registered) {
		return; // Already registered by another thread
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
	
	vfs_registered = true;
	registered_vfs = duckdb_vfs;
}

void SqliteDuckDBCacheVFS::Cleanup() {
	std::lock_guard<std::mutex> lock(context_mutex);
	
	if (registered_vfs) {
		// Note: SQLite doesn't provide sqlite3_vfs_unregister, so we can't unregister
		// But we can clean up our reference and mark as not registered
		registered_vfs = nullptr;
		vfs_registered = false;
		current_context = nullptr;
	}
}

//===--------------------------------------------------------------------===//
// VFS Methods - Use default VFS where possible
//===--------------------------------------------------------------------===//

// Helper function to get default VFS
static sqlite3_vfs* GetDefaultVFS() {
	return sqlite3_vfs_find(nullptr);
}

int SqliteDuckDBCacheVFS::Open(sqlite3_vfs *vfs, const char *filename, sqlite3_file *file, int flags, int *out_flags) {
	if (!filename || (flags & SQLITE_OPEN_READONLY) == 0) {
		return SQLITE_CANTOPEN;
	}

	try {
		auto *duckdb_file = reinterpret_cast<SqliteDuckDBCachedFile*>(file);
		
		// Get the current context (temporarily from global, but store per-file)
		ClientContext *context = nullptr;
		{
			std::lock_guard<std::mutex> lock(context_mutex);
			context = current_context;
		}
		
		if (!context) {
			return SQLITE_CANTOPEN;
		}
		
		// Additional validation: ensure context is still valid
		try {
			auto &db = context->db;
			if (!db) {
				return SQLITE_CANTOPEN;
			}
		} catch (...) {
			return SQLITE_CANTOPEN;
		}

		// Initialize the file structure
		memset(duckdb_file, 0, sizeof(SqliteDuckDBCachedFile));
		duckdb_file->base.pMethods = &duckdb_cache_io_methods;
		duckdb_file->context = context; // Store context per-file
		
		// Create the DuckDB cached file
		duckdb_file->duckdb_file = make_uniq<DuckDBCachedFile>(*context, filename);

		if (out_flags) {
			*out_flags = flags;
		}

		return SQLITE_OK;
	} catch (const Exception& e) {
		// DuckDB exception - log it for debugging
		return SQLITE_CANTOPEN;
	} catch (const std::exception& e) {
		// Standard C++ exception
		return SQLITE_CANTOPEN;
	} catch (...) {
		// Unknown exception
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
	sqlite3_vfs *default_vfs = GetDefaultVFS();
	if (default_vfs && default_vfs->xRandomness) {
		return default_vfs->xRandomness(default_vfs, bytes, out);
	}
	return SQLITE_OK;
}

int SqliteDuckDBCacheVFS::Sleep(sqlite3_vfs *vfs, int microseconds) {
	sqlite3_vfs *default_vfs = GetDefaultVFS();
	if (default_vfs && default_vfs->xSleep) {
		return default_vfs->xSleep(default_vfs, microseconds);
	}
	return SQLITE_OK;
}

int SqliteDuckDBCacheVFS::CurrentTime(sqlite3_vfs *vfs, double *time) {
	sqlite3_vfs *default_vfs = GetDefaultVFS();
	if (default_vfs && default_vfs->xCurrentTime) {
		return default_vfs->xCurrentTime(default_vfs, time);
	}
	return SQLITE_OK;
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