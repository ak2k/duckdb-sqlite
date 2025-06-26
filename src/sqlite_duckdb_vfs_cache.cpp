//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlite_duckdb_vfs_cache.cpp
//
//
//===----------------------------------------------------------------------===//

#include "sqlite_duckdb_vfs_cache.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <cstring>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Concurrency Design
//===--------------------------------------------------------------------===//
// This VFS implementation is designed for safe concurrent access:
//
// VFS Registry (mutex-protected):
//   - Each ClientContext gets its own VFS instance with unique name
//   - Registration/unregistration operations are thread-safe
//   - Mutex protects registry map operations, not file I/O
//
// File Operations (lock-free):
//   - Each VFS instance is independent with no shared mutable state
//   - File handles contain their own ClientContext pointer
//   - DuckDB's CachingFileSystem provides internal synchronization
//
// Lifetime Management:
//   - ClientContext MUST outlive all SQLite connections using its VFS
//   - VFS automatically unregistered when ClientContext is destroyed
//   - Multiple VFS instances share the same ExternalFileCache at DatabaseInstance level
//   - Cache sharing is thread-safe through DuckDB's internal locking mechanisms
//===--------------------------------------------------------------------===//

// Each ClientContext gets its own VFS instance with a unique name,
// enabling safe concurrent access while maintaining cache sharing.
// VFS wrapper combines RAII with SQLite's C allocator for cross-DLL safety.
// The wrapper itself uses unique_ptr for automatic cleanup, while vfs_name
// uses sqlite3_malloc/free because SQLite may access it across module boundaries.
struct DuckDBVFSWrapper {
	sqlite3_vfs base;           // Must be first - SQLite VFS structure
	ClientContext *context;     // The DuckDB context for this VFS
	char *vfs_name;            // Unique name for this VFS instance (allocated via sqlite3_malloc)
	sqlite3_io_methods io_methods; // IO methods for this VFS instance
	
	// Error context storage for better diagnostics
	mutable mutex error_mutex;
	string last_error_message;
	
	~DuckDBVFSWrapper() {
		// Clean up using SQLite's allocator to match sqlite3_malloc
		if (vfs_name) {
			sqlite3_free(vfs_name);
			vfs_name = nullptr;
		}
	}
	
	void SetLastError(const string &error) {
		lock_guard<mutex> lock(error_mutex);
		last_error_message = error;
	}
	
	string GetLastError() const {
		lock_guard<mutex> lock(error_mutex);
		return last_error_message;
	}
};

//===--------------------------------------------------------------------===//
// C/C++ Boundary Safety
//===--------------------------------------------------------------------===//
// Template to safely execute C++ code from SQLite's C callbacks.
// Catches all exceptions and converts them to appropriate SQLite error codes.
// This is critical because SQLite is written in C and cannot handle C++ exceptions.
//
// Usage:
//   return SafeVFSCall<int>(SQLITE_IOERR, [&]() {
//       // C++ code that might throw
//       return SQLITE_OK;
//   });
//===--------------------------------------------------------------------===//

template<typename T>
static T SafeVFSCall(T error_value, const std::function<T()> &func, DuckDBVFSWrapper *wrapper = nullptr, const char *path = nullptr) {
	try {
		return func();
	} catch (const HTTPException &e) {
		// Store HTTP error context
		if (wrapper) {
			string error_msg = "HTTP Error: ";
			error_msg += e.what();
			if (path) {
				error_msg += " (URL: ";
				error_msg += path;
				error_msg += ")";
			}
			wrapper->SetLastError(error_msg);
		}
		return error_value == SQLITE_OK ? SQLITE_IOERR : error_value;
	} catch (const PermissionException &e) {
		if (wrapper) {
			string error_msg = "Permission denied: ";
			error_msg += e.what();
			if (path) {
				error_msg += " (Path: ";
				error_msg += path;
				error_msg += ")";
			}
			wrapper->SetLastError(error_msg);
		}
		return error_value == SQLITE_OK ? SQLITE_PERM : error_value;
	} catch (const IOException &e) {
		if (wrapper) {
			string error_msg = "I/O Error: ";
			error_msg += e.what();
			if (path) {
				error_msg += " (Path: ";
				error_msg += path;
				error_msg += ")";
			}
			wrapper->SetLastError(error_msg);
		}
		return error_value == SQLITE_OK ? SQLITE_IOERR : error_value;
	} catch (const Exception &e) {
		// DuckDB exceptions - store context
		if (wrapper) {
			string error_msg = "Database Error: ";
			error_msg += e.what();
			if (path) {
				error_msg += " (Path: ";
				error_msg += path;
				error_msg += ")";
			}
			wrapper->SetLastError(error_msg);
		}
		return error_value;
	} catch (const std::bad_alloc &) {
		if (wrapper) {
			wrapper->SetLastError("Out of memory");
		}
		return error_value == SQLITE_OK ? SQLITE_NOMEM : error_value;
	} catch (...) {
		// Unknown exception
		if (wrapper) {
			wrapper->SetLastError("Unknown error occurred");
		}
		return error_value;
	}
}

// Global registry of VFS wrappers to manage their lifetime
// Use function-local statics to ensure proper initialization order on Windows
struct VFSRegistryData {
	mutex registry_mutex;
	unordered_map<ClientContext*, unique_ptr<DuckDBVFSWrapper>> registry;
};

static VFSRegistryData& GetVFSRegistryData() {
	// Function-local static ensures thread-safe initialization
	static VFSRegistryData data;
	return data;
}

// Sector size: minimum atomic write unit for the storage device
static constexpr int SQLITE_SECTOR_SIZE = 4096;

// Initialize IO methods for a VFS wrapper
static void InitializeIOMethods(sqlite3_io_methods &io_methods) {
	memset(&io_methods, 0, sizeof(io_methods));
	
	io_methods.iVersion = 1;
	io_methods.xClose = SQLiteDuckDBCacheVFS::Close;
	io_methods.xRead = SQLiteDuckDBCacheVFS::Read;
	io_methods.xWrite = SQLiteDuckDBCacheVFS::Write;
	io_methods.xTruncate = SQLiteDuckDBCacheVFS::Truncate;
	io_methods.xSync = SQLiteDuckDBCacheVFS::Sync;
	io_methods.xFileSize = SQLiteDuckDBCacheVFS::FileSize;
	io_methods.xLock = SQLiteDuckDBCacheVFS::Lock;
	io_methods.xUnlock = SQLiteDuckDBCacheVFS::Unlock;
	io_methods.xCheckReservedLock = SQLiteDuckDBCacheVFS::CheckReservedLock;
	io_methods.xFileControl = SQLiteDuckDBCacheVFS::FileControl;
	io_methods.xSectorSize = SQLiteDuckDBCacheVFS::SectorSize;
	io_methods.xDeviceCharacteristics = SQLiteDuckDBCacheVFS::DeviceCharacteristics;
	// Shared memory methods not needed for read-only remote files
	io_methods.xShmMap = nullptr;
	io_methods.xShmLock = nullptr;
	io_methods.xShmBarrier = nullptr;
	io_methods.xShmUnmap = nullptr;
	io_methods.xFetch = nullptr;
	io_methods.xUnfetch = nullptr;
}

// Get the unique VFS name for a ClientContext
static string GetUniqueVFSName(const ClientContext *context) {
	return "duckdb_cache_vfs_" + to_string(reinterpret_cast<uintptr_t>(context));
}

//===--------------------------------------------------------------------===//
// DuckDBCachedFile Implementation
//===--------------------------------------------------------------------===//

DuckDBCachedFile::DuckDBCachedFile(ClientContext &context, const string &path) 
    : context(context), path(path) {
	// Defer actual file opening until first use to avoid doing DuckDB operations
	// during SQLite VFS callbacks, which might be in a different serialization context
}

void DuckDBCachedFile::EnsureInitialized() {
	if (initialized) {
		return;
	}
	
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
		
	initialized = true;
}


int DuckDBCachedFile::Read(void *buffer, int amount, sqlite3_int64 offset) {
	// Early return for empty reads (SQLite sometimes requests 0 bytes)
	if (!buffer || amount <= 0) {
		return SQLITE_OK;
	}
	
	// Ensure we're initialized before first read
	try {
		EnsureInitialized();
	} catch (...) {
		return SQLITE_IOERR_READ;
	}
	
	// Safety check - should never happen in normal operation
	if (!caching_handle) {
		return SQLITE_IOERR_READ;
	}

	try {
		// Check if we're reading past EOF
		if (offset >= cached_file_size) {
			// Reading completely past EOF - zero-fill entire buffer
			memset(buffer, 0, amount);
			return SQLITE_IOERR_SHORT_READ;
		}
		
		// Calculate how many bytes we can actually read
		const sqlite3_int64 available_bytes = cached_file_size - offset;
		const int bytes_to_read = (available_bytes < amount) ? static_cast<int>(available_bytes) : amount;
		
		// Calculate optimal read-ahead size based on access pattern
		const uint64_t readahead_size = CalculateReadAheadSize(offset, bytes_to_read);
		
		// Ensure we read at least the requested amount (up to EOF)
		uint64_t actual_read_size = MaxValue(static_cast<uint64_t>(bytes_to_read), readahead_size);
		
		// Don't read beyond file end
		if (offset + static_cast<sqlite3_int64>(actual_read_size) > cached_file_size) {
			actual_read_size = static_cast<uint64_t>(cached_file_size - offset);
		}
		
		// Use DuckDB's CachingFileSystem with adaptive read-ahead
		data_ptr_t read_buffer = nullptr;
		auto buffer_handle = caching_handle->Read(read_buffer, actual_read_size, offset);
		
		// Validate read buffer before copying
		if (!read_buffer) {
			return SQLITE_IOERR_READ;
		}
		
		// Copy the data we read
		memcpy(buffer, read_buffer, bytes_to_read);
		
		// If we read less than requested, zero-fill the remainder
		if (bytes_to_read < amount) {
			memset(static_cast<char*>(buffer) + bytes_to_read, 0, amount - bytes_to_read);
		}
		
		// Update read-ahead state after successful read
		UpdateReadAheadState(offset, bytes_to_read);
		
		// Return appropriate code based on whether we satisfied the full request
		return (bytes_to_read < amount) ? SQLITE_IOERR_SHORT_READ : SQLITE_OK;
	} catch (...) {
		// Map all exceptions to SQLite I/O errors.
		// DuckDB will have already logged the actual error details.
		return SQLITE_IOERR_READ;
	}
}

sqlite3_int64 DuckDBCachedFile::GetFileSize() {
	try {
		EnsureInitialized();
	} catch (...) {
		return -1;
	}
	return cached_file_size;
}

uint64_t DuckDBCachedFile::CalculateReadAheadSize(sqlite3_int64 offset, int amount) const {
	// First read or non-sequential access - use minimum size
	if (last_read_offset == -1 || !IsSequentialRead(offset)) {
		return MIN_READAHEAD_SIZE;
	}
	
	// Sequential read - double the current size up to maximum
	const uint64_t next_size = current_readahead_size * 2;
	return MinValue(next_size, MAX_READAHEAD_SIZE);
}

bool DuckDBCachedFile::IsSequentialRead(sqlite3_int64 offset) const {
	// Consider sequential if:
	// 1. Reading from exactly where last read ended, OR
	// 2. Reading within SEQUENTIAL_THRESHOLD of where last read ended
	return (offset >= last_read_end) && 
	       (offset <= last_read_end + static_cast<sqlite3_int64>(SEQUENTIAL_THRESHOLD));
}

void DuckDBCachedFile::UpdateReadAheadState(sqlite3_int64 offset, int amount) {
	// Update read-ahead size based on access pattern
	if (IsSequentialRead(offset)) {
		// Sequential read - grow read-ahead size
		current_readahead_size = MinValue(current_readahead_size * 2, MAX_READAHEAD_SIZE);
	} else {
		// Non-sequential read - reset to minimum
		current_readahead_size = MIN_READAHEAD_SIZE;
	}
	
	// Update position tracking
	last_read_offset = offset;
	last_read_end = offset + amount;
}


//===--------------------------------------------------------------------===//
// SQLiteDuckDBCacheVFS Implementation
//===--------------------------------------------------------------------===//

bool SQLiteDuckDBCacheVFS::CanHandlePath(ClientContext &context, const string &path) {
	return FileSystem::IsRemoteFile(path);
}

void SQLiteDuckDBCacheVFS::Register(ClientContext &context) {
	auto& registry_data = GetVFSRegistryData();
	lock_guard<mutex> lock(registry_data.registry_mutex);
	
	// Context is a reference, so it cannot be null
	// Just proceed with registration
	
	// Check if this context already has a VFS registered
	auto it = registry_data.registry.find(&context);
	if (it != registry_data.registry.end()) {
		// Already registered for this context
		return;
	}

	// Find SQLite's default VFS to delegate some operations
	sqlite3_vfs *default_vfs = sqlite3_vfs_find(nullptr);
	if (!default_vfs) {
		throw InternalException("Failed to find default SQLite VFS - SQLite may not be properly initialized");
	}

	// Create a new VFS wrapper for this context
	auto wrapper = make_uniq<DuckDBVFSWrapper>();
	wrapper->context = &context;
	
	// Allocate VFS name using SQLite's allocator for DLL safety
	const string temp_name = GetUniqueVFSName(&context);
	wrapper->vfs_name = (char*)sqlite3_malloc64(temp_name.length() + 1);
	if (!wrapper->vfs_name) {
		throw InternalException("Failed to allocate memory for VFS name");
	}
	memcpy(wrapper->vfs_name, temp_name.c_str(), temp_name.length() + 1);
	
	// Initialize the IO methods for this VFS instance
	InitializeIOMethods(wrapper->io_methods);
	
	// Initialize the VFS structure
	memset(&wrapper->base, 0, sizeof(wrapper->base));
	wrapper->base.iVersion = 1;
	wrapper->base.szOsFile = sizeof(SQLiteDuckDBCachedFile);
	wrapper->base.mxPathname = default_vfs->mxPathname;
	wrapper->base.zName = wrapper->vfs_name;  // Now using C-style string
	wrapper->base.pAppData = wrapper.get();  // Store pointer to wrapper

	// Configure VFS methods
	wrapper->base.xOpen = Open;
	wrapper->base.xDelete = Delete;
	wrapper->base.xAccess = Access;
	wrapper->base.xFullPathname = FullPathname;
	wrapper->base.xDlOpen = DlOpen;
	wrapper->base.xDlError = DlError;
	wrapper->base.xDlSym = DlSym;
	wrapper->base.xDlClose = DlClose;
	wrapper->base.xRandomness = Randomness;
	wrapper->base.xSleep = Sleep;
	wrapper->base.xCurrentTime = CurrentTime;
	wrapper->base.xGetLastError = GetLastError;

	// Register this VFS with SQLite
	int rc = sqlite3_vfs_register(&wrapper->base, 0);
	if (rc != SQLITE_OK) {
		throw InternalException("Failed to register DuckDB Cache VFS: %s", sqlite3_errstr(rc));
	}

	// Store in registry - wrapper ownership transfers to registry
	registry_data.registry[&context] = std::move(wrapper);
}

// Unregister the VFS associated with a ClientContext when it's being destroyed.
// This ensures proper cleanup of VFS resources.
void SQLiteDuckDBCacheVFS::Unregister(ClientContext &context) {
	auto& registry_data = GetVFSRegistryData();
	lock_guard<mutex> lock(registry_data.registry_mutex);
	
	auto it = registry_data.registry.find(&context);
	if (it != registry_data.registry.end()) {
		// Unregister from SQLite
		sqlite3_vfs_unregister(&it->second->base);
		// Remove from registry
		registry_data.registry.erase(it);
	}
}

// Get the unique VFS name associated with a specific ClientContext.
// Returns the default VFS name if no specific VFS is registered for this context.
const char *SQLiteDuckDBCacheVFS::GetVFSNameForContext(ClientContext &context) {
	auto& registry_data = GetVFSRegistryData();
	lock_guard<mutex> lock(registry_data.registry_mutex);
	
	auto it = registry_data.registry.find(&context);
	if (it != registry_data.registry.end() && it->second->vfs_name) {
		return it->second->vfs_name;  // Return the C-style string directly
	}
	
	// Fallback to default name if not found (shouldn't happen)
	return GetVFSName();
}

//===--------------------------------------------------------------------===//
// VFS Methods
//===--------------------------------------------------------------------===//

// Macro to delegate VFS operations to SQLite's default VFS implementation.
// Used for system-level operations (randomness, sleep, time) that don't involve
// file I/O and thus don't need special handling for remote files.
#define DELEGATE_TO_DEFAULT_VFS(method_name, ...) \
	sqlite3_vfs *default_vfs = sqlite3_vfs_find(nullptr); \
	if (default_vfs && default_vfs->method_name) { \
		return default_vfs->method_name(default_vfs, __VA_ARGS__); \
	} \
	return SQLITE_OK;

int SQLiteDuckDBCacheVFS::Open(sqlite3_vfs *vfs, const char *filename, sqlite3_file *file, int flags, int *out_flags) {
	// Get wrapper for error context
	auto *wrapper = vfs && vfs->pAppData ? static_cast<DuckDBVFSWrapper*>(vfs->pAppData) : nullptr;
	
	return SafeVFSCall<int>(SQLITE_CANTOPEN, [&]() {
		// Validate parameters and ensure read-only access
		if (!vfs || !filename || !file || (flags & SQLITE_OPEN_READONLY) == 0) {
			return SQLITE_CANTOPEN;
		}

		// Ensure SQLite allocated enough space for our file structure
		if (vfs->szOsFile < static_cast<int>(sizeof(SQLiteDuckDBCachedFile))) {
			return SQLITE_CANTOPEN;
		}
		
		auto *duckdb_file = reinterpret_cast<SQLiteDuckDBCachedFile*>(file);
		
		// Retrieve the VFS wrapper and context from pAppData
		if (!vfs->pAppData) {
			return SQLITE_CANTOPEN;
		}
		
		auto *wrapper = static_cast<DuckDBVFSWrapper*>(vfs->pAppData);
		ClientContext *context = wrapper->context;
		
		if (!context) {
			return SQLITE_CANTOPEN;
		}
		
		// Validate that the ClientContext is still valid by checking if it has a database
		if (!context->db) {
			return SQLITE_CANTOPEN;
		}

		// Initialize the structure members properly
		duckdb_file->base.pMethods = &wrapper->io_methods;
		duckdb_file->duckdb_file = nullptr;
		duckdb_file->context = context;
		
		// Create the DuckDB file handle with proper exception handling
		try {
			duckdb_file->duckdb_file = new DuckDBCachedFile(*context, filename);
		} catch (...) {
			// Clean up on failure
			duckdb_file->base.pMethods = nullptr;
			duckdb_file->duckdb_file = nullptr;
			duckdb_file->context = nullptr;
			return SQLITE_CANTOPEN;
		}
		
		// Don't validate SQLite header here - defer until first read
		// to avoid triggering DuckDB operations in VFS callbacks

		if (out_flags) {
			*out_flags = flags;
		}

		return SQLITE_OK;
	}, wrapper, filename);
}

int SQLiteDuckDBCacheVFS::Delete(sqlite3_vfs *vfs, const char *filename, int sync_dir) {
	// Remote files cannot be deleted through this VFS
	return SQLITE_IOERR_DELETE;
}

int SQLiteDuckDBCacheVFS::Access(sqlite3_vfs *vfs, const char *filename, int flags, int *result) {
	auto *wrapper = vfs && vfs->pAppData ? static_cast<DuckDBVFSWrapper*>(vfs->pAppData) : nullptr;
	
	return SafeVFSCall<int>(SQLITE_IOERR, [&]() {
		if (!filename || !result) {
			return SQLITE_IOERR;
		}

		// Initialize result to safe default
		*result = 0;

		// For remote files, we need to handle journal/WAL file checks properly.
		// SQLite uses Access() to check for the existence of journal and WAL files
		// to determine if a database might have uncommitted transactions.
		
		if (flags == SQLITE_ACCESS_EXISTS) {
			// Check if this is a journal or WAL file by examining the suffix
			const string file_path(filename);
			bool is_journal = false;
			bool is_wal = false;
			
			// Check for journal file suffixes
			if (file_path.size() > 8) {
				string suffix = file_path.substr(file_path.size() - 8);
				if (suffix == "-journal" || suffix == "-wal") {
					is_journal = (suffix == "-journal");
					is_wal = (suffix == "-wal");
				}
			}
			
			if (is_journal || is_wal) {
				// For journal/WAL files, we need to check if they actually exist
				// This is critical for SQLite to properly detect hot journals
				if (!vfs->pAppData) {
					*result = 0;
					return SQLITE_OK;
				}
				
				auto *wrapper = static_cast<DuckDBVFSWrapper*>(vfs->pAppData);
				ClientContext *context = wrapper->context;
				
				if (!context || !context->db) {
					*result = 0;
					return SQLITE_OK;
				}
				
				// Try to check file existence through DuckDB's filesystem
				try {
					auto &fs = context->db->GetFileSystem();
					*result = fs.FileExists(file_path) ? 1 : 0;
				} catch (...) {
					// If we can't check, assume it doesn't exist
					*result = 0;
				}
			} else {
				// For the main database file, return 0 to let SQLite try to open it
				// This avoids triggering DuckDB operations in the wrong context
				*result = 0;
			}
		} else if (flags == SQLITE_ACCESS_READWRITE) {
			// Remote files are always read-only
			*result = 0;
		} else if (flags == SQLITE_ACCESS_READ) {
			// We can read remote files, but defer actual check to open
			*result = 0;
		} else {
			// Unknown access type
			*result = 0;
		}

		return SQLITE_OK;
	}, wrapper, filename);
}

int SQLiteDuckDBCacheVFS::FullPathname(sqlite3_vfs *vfs, const char *filename, int out_size, char *out_buf) {
	return SafeVFSCall<int>(SQLITE_IOERR, [&]() {
		if (!filename || !out_buf || out_size <= 0) {
			return SQLITE_IOERR;
		}

		// Remote paths are already absolute URLs - return as-is
		strncpy(out_buf, filename, out_size - 1);
		out_buf[out_size - 1] = '\0';
		return SQLITE_OK;
	});
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
	if (!vfs || !vfs->pAppData || !err_msg || bytes <= 0) {
		return 0;
	}
	
	auto *wrapper = static_cast<DuckDBVFSWrapper*>(vfs->pAppData);
	const string error = wrapper->GetLastError();
	
	if (error.empty()) {
		err_msg[0] = '\0';
		return 0;
	}
	
	// Copy error message to buffer
	strncpy(err_msg, error.c_str(), bytes - 1);
	err_msg[bytes - 1] = '\0';
	
	return error.length();
}

//===--------------------------------------------------------------------===//
// File Methods
//===--------------------------------------------------------------------===//

int SQLiteDuckDBCacheVFS::Close(sqlite3_file *file) {
	return SafeVFSCall<int>(SQLITE_OK, [&]() {
		if (file) {
			auto *duckdb_file = reinterpret_cast<SQLiteDuckDBCachedFile*>(file);
			// Explicitly delete the raw pointer
			delete duckdb_file->duckdb_file;
			duckdb_file->duckdb_file = nullptr;
			duckdb_file->context = nullptr;
		}
		return SQLITE_OK;
	});
}

int SQLiteDuckDBCacheVFS::Read(sqlite3_file *file, void *buffer, int amount, sqlite3_int64 offset) {
	return SafeVFSCall<int>(SQLITE_IOERR_READ, [&]() {
		if (!file || !buffer) {
			return SQLITE_IOERR_READ;
		}

		auto *duckdb_file = reinterpret_cast<SQLiteDuckDBCachedFile*>(file);
		if (!duckdb_file->duckdb_file) {
			return SQLITE_IOERR_READ;
		}

		return duckdb_file->duckdb_file->Read(buffer, amount, offset);
	});
}

int SQLiteDuckDBCacheVFS::FileSize(sqlite3_file *file, sqlite3_int64 *size) {
	return SafeVFSCall<int>(SQLITE_IOERR, [&]() {
		if (!file || !size) {
			return SQLITE_IOERR;
		}

		auto *duckdb_file = reinterpret_cast<SQLiteDuckDBCachedFile*>(file);
		if (!duckdb_file->duckdb_file) {
			return SQLITE_IOERR;
		}

		*size = duckdb_file->duckdb_file->GetFileSize();
		return SQLITE_OK;
	});
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
	return SQLITE_SECTOR_SIZE;
}

int SQLiteDuckDBCacheVFS::DeviceCharacteristics(sqlite3_file *file) {
	// Remote files are immutable - they cannot be modified
	return SQLITE_IOCAP_IMMUTABLE;
}

} // namespace duckdb