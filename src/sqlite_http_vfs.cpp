#include "sqlite_http_vfs.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/exception.hpp"
#include <cstring>
#include <chrono>

namespace duckdb {

ClientContext *SqliteHttpVFS::current_context = nullptr;
std::mutex SqliteHttpVFS::context_mutex;

static sqlite3_io_methods http_io_methods = {
    1,                                    // iVersion
    SqliteHttpVFS::Close,                 // xClose
    SqliteHttpVFS::Read,                  // xRead
    SqliteHttpVFS::Write,                 // xWrite
    SqliteHttpVFS::Truncate,              // xTruncate
    SqliteHttpVFS::Sync,                  // xSync
    SqliteHttpVFS::FileSize,              // xFileSize
    SqliteHttpVFS::Lock,                  // xLock
    SqliteHttpVFS::Unlock,                // xUnlock
    SqliteHttpVFS::CheckReservedLock,     // xCheckReservedLock
    SqliteHttpVFS::FileControl,           // xFileControl
    SqliteHttpVFS::SectorSize,            // xSectorSize
    SqliteHttpVFS::DeviceCharacteristics, // xDeviceCharacteristics
    nullptr,                              // xShmMap
    nullptr,                              // xShmLock
    nullptr,                              // xShmBarrier
    nullptr,                              // xShmUnmap
    nullptr,                              // xFetch
    nullptr                               // xUnfetch
};

//===--------------------------------------------------------------------===//
// HttpFile Implementation
//===--------------------------------------------------------------------===//

HttpFile::HttpFile(ClientContext &context, const string &url) 
    : context(context), url(url), file_size(-1), size_fetched(false) {
}

HttpFile::~HttpFile() {
}

int HttpFile::Read(void *buffer, int amount, sqlite3_int64 offset) {
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

	idx_t bytes_read = 0;
	auto *output = reinterpret_cast<char*>(buffer);
	auto read_offset = static_cast<idx_t>(offset);
	auto read_amount = static_cast<idx_t>(amount);

	// Adjust read amount if it would go past EOF
	if (read_offset + read_amount > static_cast<idx_t>(file_size)) {
		read_amount = static_cast<idx_t>(file_size) - read_offset;
	}

	while (bytes_read < read_amount) {
		// Calculate which block we need
		idx_t block_idx = read_offset / GetBlockSize();
		idx_t block_offset = read_offset % GetBlockSize();
		idx_t bytes_to_read = MinValue<idx_t>(GetBlockSize() - block_offset, read_amount - bytes_read);

		// Fetch the block if not in cache
		{
			std::lock_guard<std::mutex> lock(cache_mutex);
			if (block_cache.find(block_idx) == block_cache.end()) {
				FetchBlock(block_idx);
			}

			// Read from the cached block
			auto &block = block_cache[block_idx];
			memcpy(output + bytes_read, block->data.get() + block_offset, bytes_to_read);
		}

		bytes_read += bytes_to_read;
		read_offset += bytes_to_read;
	}

	// SQLite expects SQLITE_IOERR_SHORT_READ if we read less than requested
	if (bytes_read < static_cast<idx_t>(amount)) {
		memset(output + bytes_read, 0, amount - bytes_read);
		return SQLITE_IOERR_SHORT_READ;
	}

	return SQLITE_OK;
}

sqlite3_int64 HttpFile::GetFileSize() {
	if (size_fetched) {
		return file_size;
	}

	// Use DuckDB's httpfs to get file info
	auto &fs = context.db->GetFileSystem();
	auto handle = fs.OpenFile(url, FileFlags::FILE_FLAGS_READ);
	file_size = fs.GetFileSize(*handle);
	size_fetched = true;
	
	return file_size;
}

void HttpFile::FetchBlock(idx_t block_idx) {
	auto &fs = context.db->GetFileSystem();
	auto handle = fs.OpenFile(url, FileFlags::FILE_FLAGS_READ);

	idx_t block_offset = block_idx * GetBlockSize();
	idx_t block_size = GetBlockSize();

	// Adjust block size if it would go past EOF
	if (block_offset + block_size > static_cast<idx_t>(file_size)) {
		block_size = static_cast<idx_t>(file_size) - block_offset;
	}

	auto block = make_uniq<HttpBlock>();
	block->offset = block_offset;
	block->size = block_size;
	block->data = unique_ptr<char[]>(new char[block_size]);

	// Read the block
	fs.Read(*handle, block->data.get(), block_size, block_offset);

	// Store in cache
	block_cache[block_idx] = std::move(block);
}

//===--------------------------------------------------------------------===//
// SqliteHttpVFS Implementation
//===--------------------------------------------------------------------===//

bool SqliteHttpVFS::IsHTTPPath(const string &path) {
	return StringUtil::StartsWith(path, "http://") || 
	       StringUtil::StartsWith(path, "https://") ||
	       StringUtil::StartsWith(path, "s3://") ||
	       StringUtil::StartsWith(path, "gs://") ||
	       StringUtil::StartsWith(path, "r2://");
}

void SqliteHttpVFS::Register(ClientContext &context) {
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
	auto *http_vfs = new sqlite3_vfs();
	memset(http_vfs, 0, sizeof(sqlite3_vfs));

	http_vfs->iVersion = 1;
	http_vfs->szOsFile = sizeof(SqliteHttpFile);
	http_vfs->mxPathname = default_vfs->mxPathname;
	http_vfs->zName = GetVFSName();
	http_vfs->pAppData = nullptr;

	// Set up methods
	http_vfs->xOpen = Open;
	http_vfs->xDelete = Delete;
	http_vfs->xAccess = Access;
	http_vfs->xFullPathname = FullPathname;
	http_vfs->xDlOpen = DlOpen;
	http_vfs->xDlError = DlError;
	http_vfs->xDlSym = DlSym;
	http_vfs->xDlClose = DlClose;
	http_vfs->xRandomness = Randomness;
	http_vfs->xSleep = Sleep;
	http_vfs->xCurrentTime = CurrentTime;
	http_vfs->xGetLastError = GetLastError;

	// Register the VFS
	int rc = sqlite3_vfs_register(http_vfs, 0);
	if (rc != SQLITE_OK) {
		delete http_vfs;
		throw InternalException("Failed to register HTTP VFS: %s", sqlite3_errstr(rc));
	}
}

//===--------------------------------------------------------------------===//
// VFS Methods
//===--------------------------------------------------------------------===//

int SqliteHttpVFS::Open(sqlite3_vfs *vfs, const char *filename, sqlite3_file *file, int flags, int *out_flags) {
	if (!filename) {
		return SQLITE_IOERR;
	}

	// Only support read-only access
	if ((flags & SQLITE_OPEN_READONLY) == 0) {
		return SQLITE_CANTOPEN;
	}

	try {
		auto *http_file = reinterpret_cast<SqliteHttpFile*>(file);
		
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
		memset(http_file, 0, sizeof(SqliteHttpFile));
		http_file->base.pMethods = &http_io_methods;
		
		// Create the HTTP file
		http_file->http_file = make_uniq<HttpFile>(*context, filename);

		if (out_flags) {
			*out_flags = flags;
		}

		return SQLITE_OK;
	} catch (...) {
		return SQLITE_CANTOPEN;
	}
}

int SqliteHttpVFS::Delete(sqlite3_vfs *vfs, const char *filename, int sync_dir) {
	// Cannot delete HTTP files
	return SQLITE_IOERR_DELETE;
}

int SqliteHttpVFS::Access(sqlite3_vfs *vfs, const char *filename, int flags, int *result) {
	if (!filename || !result) {
		return SQLITE_IOERR;
	}

	// For HTTP files, we only support checking existence
	if (flags == SQLITE_ACCESS_EXISTS) {
		try {
			// Get the current context
			ClientContext *context = nullptr;
			{
				std::lock_guard<std::mutex> lock(context_mutex);
				context = current_context;
			}
			
			if (!context) {
				*result = 0;
				return SQLITE_OK;
			}

			// Try to open the file to check if it exists
			auto &fs = context->db->GetFileSystem();
			try {
				auto handle = fs.OpenFile(filename, FileFlags::FILE_FLAGS_READ);
				*result = 1;
			} catch (...) {
				*result = 0;
			}
		} catch (...) {
			*result = 0;
		}
	} else {
		// HTTP files are read-only
		*result = 0;
	}

	return SQLITE_OK;
}

int SqliteHttpVFS::FullPathname(sqlite3_vfs *vfs, const char *filename, int out_size, char *out_buf) {
	if (!filename || !out_buf || out_size <= 0) {
		return SQLITE_IOERR;
	}

	// For HTTP URLs, just return the URL as-is
	strncpy(out_buf, filename, out_size - 1);
	out_buf[out_size - 1] = '\0';
	return SQLITE_OK;
}

void *SqliteHttpVFS::DlOpen(sqlite3_vfs *vfs, const char *filename) {
	// Not supported for HTTP VFS
	return nullptr;
}

void SqliteHttpVFS::DlError(sqlite3_vfs *vfs, int bytes, char *err_msg) {
	if (err_msg && bytes > 0) {
		strncpy(err_msg, "Dynamic loading not supported for HTTP VFS", bytes - 1);
		err_msg[bytes - 1] = '\0';
	}
}

void (*SqliteHttpVFS::DlSym(sqlite3_vfs *vfs, void *handle, const char *symbol))(void) {
	return nullptr;
}

void SqliteHttpVFS::DlClose(sqlite3_vfs *vfs, void *handle) {
	// Nothing to do
}

int SqliteHttpVFS::Randomness(sqlite3_vfs *vfs, int bytes, char *out) {
	// Use default VFS for randomness
	sqlite3_vfs *default_vfs = sqlite3_vfs_find(nullptr);
	if (default_vfs && default_vfs->xRandomness) {
		return default_vfs->xRandomness(default_vfs, bytes, out);
	}
	return SQLITE_OK;
}

int SqliteHttpVFS::Sleep(sqlite3_vfs *vfs, int microseconds) {
	// Use default VFS for sleep
	sqlite3_vfs *default_vfs = sqlite3_vfs_find(nullptr);
	if (default_vfs && default_vfs->xSleep) {
		return default_vfs->xSleep(default_vfs, microseconds);
	}
	return SQLITE_OK;
}

int SqliteHttpVFS::CurrentTime(sqlite3_vfs *vfs, double *time) {
	// Use default VFS for current time
	sqlite3_vfs *default_vfs = sqlite3_vfs_find(nullptr);
	if (default_vfs && default_vfs->xCurrentTime) {
		return default_vfs->xCurrentTime(default_vfs, time);
	}
	return SQLITE_OK;
}

int SqliteHttpVFS::GetLastError(sqlite3_vfs *vfs, int bytes, char *err_msg) {
	if (err_msg && bytes > 0) {
		err_msg[0] = '\0';
	}
	return 0;
}

//===--------------------------------------------------------------------===//
// File Methods
//===--------------------------------------------------------------------===//

int SqliteHttpVFS::Close(sqlite3_file *file) {
	if (!file) {
		return SQLITE_OK;
	}

	auto *http_file = reinterpret_cast<SqliteHttpFile*>(file);
	http_file->http_file.reset();
	return SQLITE_OK;
}

int SqliteHttpVFS::Read(sqlite3_file *file, void *buffer, int amount, sqlite3_int64 offset) {
	if (!file || !buffer) {
		return SQLITE_IOERR_READ;
	}

	auto *http_file = reinterpret_cast<SqliteHttpFile*>(file);
	if (!http_file->http_file) {
		return SQLITE_IOERR_READ;
	}

	return http_file->http_file->Read(buffer, amount, offset);
}

int SqliteHttpVFS::Write(sqlite3_file *file, const void *buffer, int amount, sqlite3_int64 offset) {
	// HTTP files are read-only
	return SQLITE_READONLY;
}

int SqliteHttpVFS::Truncate(sqlite3_file *file, sqlite3_int64 size) {
	// HTTP files are read-only
	return SQLITE_READONLY;
}

int SqliteHttpVFS::Sync(sqlite3_file *file, int flags) {
	// Nothing to sync for read-only HTTP files
	return SQLITE_OK;
}

int SqliteHttpVFS::FileSize(sqlite3_file *file, sqlite3_int64 *size) {
	if (!file || !size) {
		return SQLITE_IOERR;
	}

	auto *http_file = reinterpret_cast<SqliteHttpFile*>(file);
	if (!http_file->http_file) {
		return SQLITE_IOERR;
	}

	*size = http_file->http_file->GetFileSize();
	return SQLITE_OK;
}

int SqliteHttpVFS::Lock(sqlite3_file *file, int level) {
	// Locking not needed for read-only HTTP files
	return SQLITE_OK;
}

int SqliteHttpVFS::Unlock(sqlite3_file *file, int level) {
	// Locking not needed for read-only HTTP files
	return SQLITE_OK;
}

int SqliteHttpVFS::CheckReservedLock(sqlite3_file *file, int *result) {
	if (result) {
		*result = 0;
	}
	return SQLITE_OK;
}

int SqliteHttpVFS::FileControl(sqlite3_file *file, int op, void *arg) {
	// No special file control operations
	return SQLITE_NOTFOUND;
}

int SqliteHttpVFS::SectorSize(sqlite3_file *file) {
	return 4096; // Default sector size
}

int SqliteHttpVFS::DeviceCharacteristics(sqlite3_file *file) {
	// Indicate that this is a read-only device
	return SQLITE_IOCAP_IMMUTABLE;
}

} // namespace duckdb