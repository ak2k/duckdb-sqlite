#include "sqlite_duckdb_vfs_cache.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include <cstring>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace duckdb {

//===--------------------------------------------------------------------===//
// Thread Safety Documentation
//===--------------------------------------------------------------------===//
// This implementation provides the following thread safety guarantees:
//
// 1. VFS Registration/Unregistration (THREAD-SAFE)
//    - Protected by vfs_registry_mutex
//    - Safe to call from multiple threads simultaneously
//    - Each ClientContext gets its own independent VFS instance
//
// 2. File Operations (THREAD-SAFE)
//    - No shared mutable state between VFS instances
//    - Each file handle contains its own context pointer
//    - DuckDB's CachingFileSystem handles internal synchronization
//
// 3. Context Lifetime Requirements
//    - ClientContext MUST outlive all SQLite connections using its VFS
//    - Call Unregister() before destroying the ClientContext
//    - Consider using SQLiteVFSRegistration RAII helper for automatic cleanup
//
// 4. What the Mutex Protects
//    - vfs_registry map operations (insert/find/erase)
//    - VFS name generation and lookup
//    - Does NOT protect file I/O operations (not needed)
//
// 5. Cache Sharing
//    - Multiple VFS instances share the same ExternalFileCache
//    - Cache is managed at the DatabaseInstance level
//    - Thread-safe through DuckDB's internal locking mechanisms
//===--------------------------------------------------------------------===//

// Dynamic VFS registration approach to eliminate thread-local storage issues.
// Each ClientContext gets its own VFS instance with a unique name.
// This avoids Windows SIGSEGV issues while maintaining cache sharing.
struct DuckDBVFSWrapper {
	sqlite3_vfs base;           // Must be first - SQLite VFS structure
	ClientContext *context;     // The DuckDB context for this VFS
	char *vfs_name;            // Unique name for this VFS instance (C-style for DLL safety)
	sqlite3_io_methods io_methods; // IO methods for this VFS instance
	
	~DuckDBVFSWrapper() {
		// Clean up the C-style allocated name
		if (vfs_name) {
			sqlite3_free(vfs_name);
			vfs_name = nullptr;
		}
	}
};

// Global registry of VFS wrappers to manage their lifetime
static std::mutex vfs_registry_mutex;
static std::unordered_map<ClientContext*, unique_ptr<DuckDBVFSWrapper>> vfs_registry;

// SQLite page size constant for sector size calculations
static constexpr int DEFAULT_SQLITE_SECTOR_SIZE = 4096;

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
static string GetUniqueVFSName(ClientContext *context) {
	return "duckdb_cache_vfs_" + std::to_string(reinterpret_cast<uintptr_t>(context));
}

//===--------------------------------------------------------------------===//
// DuckDBCachedFile Implementation
//===--------------------------------------------------------------------===//

DuckDBCachedFile::DuckDBCachedFile(ClientContext &context, const string &path) 
    : context(context), path(path), cached_file_size(-1), initialized(false) {
#ifdef _WIN32
#ifdef DEBUG
	fprintf(stderr, "[SQLITE_VFS_DEBUG] DuckDBCachedFile constructor called\n");
	fprintf(stderr, "[SQLITE_VFS_DEBUG] Path: %s\n", path.c_str());
#endif
#endif
	// Defer actual file opening until first use to avoid doing DuckDB operations
	// during SQLite VFS callbacks, which might be in a different serialization context
}

DuckDBCachedFile::~DuckDBCachedFile() = default;

void DuckDBCachedFile::EnsureInitialized() {
	if (initialized) {
		return;
	}
	
#ifdef _WIN32
#ifdef DEBUG
	fprintf(stderr, "[SQLITE_VFS_DEBUG] EnsureInitialized() called\n");
	fprintf(stderr, "[SQLITE_VFS_DEBUG] Thread ID: %lu\n", (unsigned long)GetCurrentThreadId());
#endif
#endif
	
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
	
	// Now validate this is actually a SQLite database file
	// This is deferred from Open() to avoid DuckDB operations in VFS callbacks
	constexpr char SQLITE_HEADER[] = "SQLite format 3\000";
	constexpr size_t SQLITE_HEADER_SIZE = 16;
	
	// Read the SQLite file header directly through our caching handle
	char header_buffer[SQLITE_HEADER_SIZE];
	data_ptr_t read_buffer = nullptr;
	auto buffer_handle = caching_handle->Read(read_buffer, SQLITE_HEADER_SIZE, 0);
	
	if (!read_buffer) {
		throw InvalidInputException("Failed to read SQLite header from '%s' - file may be inaccessible or empty", path);
	}
	
	memcpy(header_buffer, read_buffer, SQLITE_HEADER_SIZE);
	
	// Ensure this is actually a SQLite database file
	if (memcmp(header_buffer, SQLITE_HEADER, SQLITE_HEADER_SIZE) != 0) {
		throw InvalidInputException("File '%s' is not a valid SQLite database - header mismatch", path);
	}
	
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
		// Read-ahead optimization: SQLite typically reads in 4KB pages, but
		// DuckDB's CachingFileSystem works better with larger blocks.
		// We'll read at least 1MB to populate the cache.
		static constexpr int64_t MIN_READ_SIZE = 1024 * 1024; // 1MB
		
		// Calculate the read-ahead size
		int64_t read_ahead_size = (std::max)(static_cast<int64_t>(amount), MIN_READ_SIZE);
		
		// Make sure we don't read past the end of the file
		int64_t remaining = cached_file_size - offset;
		read_ahead_size = (std::min)(read_ahead_size, remaining);
		
		// Read the larger block to populate DuckDB's cache
		data_ptr_t read_buffer = nullptr;
		auto buffer_handle = caching_handle->Read(read_buffer, read_ahead_size, offset);
		
		// Safety check - CachingFileSystem should always return a valid buffer
		if (!read_buffer) {
			return SQLITE_IOERR_READ;
		}
		
		// Copy only the requested amount to the output buffer
		memcpy(buffer, read_buffer, amount);
		return SQLITE_OK;
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


//===--------------------------------------------------------------------===//
// SQLiteDuckDBCacheVFS Implementation
//===--------------------------------------------------------------------===//

bool SQLiteDuckDBCacheVFS::CanHandlePath(ClientContext &context, const string &path) {
	return FileSystem::IsRemoteFile(path);
}

void SQLiteDuckDBCacheVFS::Register(ClientContext &context) {
#ifdef _WIN32
#ifdef DEBUG
	fprintf(stderr, "[SQLITE_VFS_DEBUG] Register() called\n");
	fprintf(stderr, "[SQLITE_VFS_DEBUG] Context pointer: %p\n", (void*)&context);
	fprintf(stderr, "[SQLITE_VFS_DEBUG] Thread ID: %lu\n", (unsigned long)GetCurrentThreadId());
#endif
#endif

	std::lock_guard<std::mutex> lock(vfs_registry_mutex);
	
	// Check if this context already has a VFS registered
	auto it = vfs_registry.find(&context);
	if (it != vfs_registry.end()) {
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
	string temp_name = GetUniqueVFSName(&context);
	wrapper->vfs_name = (char*)sqlite3_malloc64(temp_name.length() + 1);
	if (!wrapper->vfs_name) {
		throw InternalException("Failed to allocate memory for VFS name");
	}
	strcpy(wrapper->vfs_name, temp_name.c_str());
	
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

	// Store in registry
	vfs_registry[&context] = std::move(wrapper);
	
#ifdef _WIN32
#ifdef DEBUG
	fprintf(stderr, "[SQLITE_VFS_DEBUG] Registered VFS: %s\n", vfs_registry[&context]->vfs_name);
#endif
#endif
}

// New method to unregister VFS when context is destroyed
void SQLiteDuckDBCacheVFS::Unregister(ClientContext &context) {
	std::lock_guard<std::mutex> lock(vfs_registry_mutex);
	
	auto it = vfs_registry.find(&context);
	if (it != vfs_registry.end()) {
		// Unregister from SQLite
		sqlite3_vfs_unregister(&it->second->base);
		// Remove from registry
		vfs_registry.erase(it);
	}
}

// New method to get VFS name for a context
const char *SQLiteDuckDBCacheVFS::GetVFSNameForContext(ClientContext &context) {
	std::lock_guard<std::mutex> lock(vfs_registry_mutex);
	
	auto it = vfs_registry.find(&context);
	if (it != vfs_registry.end() && it->second->vfs_name) {
		return it->second->vfs_name;  // Return the C-style string directly
	}
	
	// Fallback to default name if not found (shouldn't happen)
	return GetVFSName();
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
#ifdef _WIN32
#ifdef DEBUG
	fprintf(stderr, "[SQLITE_VFS_DEBUG] Open() called for: %s\n", filename);
	fprintf(stderr, "[SQLITE_VFS_DEBUG] Thread ID: %lu\n", (unsigned long)GetCurrentThreadId());
#endif
#endif

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
		
		// Retrieve the VFS wrapper and context from pAppData
		if (!vfs->pAppData) {
			return SQLITE_CANTOPEN;
		}
		
		auto *wrapper = static_cast<DuckDBVFSWrapper*>(vfs->pAppData);
		ClientContext *context = wrapper->context;
		
		if (!context) {
			return SQLITE_CANTOPEN;
		}

#ifdef _WIN32
#ifdef DEBUG
		fprintf(stderr, "[SQLITE_VFS_DEBUG] Context retrieved from pAppData: %p\n", (void*)context);
#endif
#endif

		// Zero-initialize the entire structure for safety
		memset(duckdb_file, 0, sizeof(SQLiteDuckDBCachedFile));
		duckdb_file->base.pMethods = &wrapper->io_methods;
		duckdb_file->context = context;
		
#ifdef _WIN32
#ifdef DEBUG
		fprintf(stderr, "[SQLITE_VFS_DEBUG] Before creating DuckDBCachedFile\n");
#endif
#endif
		
		// Create the DuckDB file handle with proper exception handling
		try {
			duckdb_file->duckdb_file = make_uniq<DuckDBCachedFile>(*context, filename);
		} catch (...) {
#ifdef _WIN32
#ifdef DEBUG
			fprintf(stderr, "[SQLITE_VFS_DEBUG] Exception in DuckDBCachedFile constructor\n");
#endif
#endif
			// Clean up on failure
			memset(duckdb_file, 0, sizeof(SQLiteDuckDBCachedFile));
			return SQLITE_CANTOPEN;
		}
		
		// Don't validate SQLite header here - defer until first read
		// to avoid triggering DuckDB operations in VFS callbacks

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

	// For remote files, we can't easily check existence without potentially
	// triggering DuckDB operations in the wrong context. SQLite will handle
	// the error when it tries to open a non-existent file.
	// 
	// Return 0 (file doesn't exist) for all remote files to be safe.
	// SQLite will attempt to open the file anyway and handle any errors.
	if (flags == SQLITE_ACCESS_EXISTS) {
		// Always return 0 for remote files to avoid DuckDB operations
		*result = 0;
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