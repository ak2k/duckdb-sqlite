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
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/re2_regex.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/atomic.hpp"

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
	
	~DuckDBVFSWrapper() noexcept {
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

// Forward declaration
struct DuckDBVFSWrapper;

//===--------------------------------------------------------------------===//
// HTTP Error Mapping
//===--------------------------------------------------------------------===//

// HTTP error patterns for detection - compile-time constants
static constexpr const char* HTTP_ERROR_PATTERNS[] = {
	"\"exception_type\":\"HTTP\"",
	"\"exception_type\":\"IO\"",
	"404 (Not Found)",
	"403 (Forbidden)",
	"401 (Unauthorized)", 
	"500 (Internal Server Error)",
	"502 (Bad Gateway)",
	"503 (Service Unavailable)",
	"Unable to connect to URL",
	"Could not establish connection",
	"HTTP HEAD to",
	"HTTP GET to"
};

// Modern regex-based HTTP status code extraction using DuckDB's regex wrapper
static int ExtractHTTPStatus(const string &error_msg) {
	// Comprehensive regex pattern for all HTTP status code formats:
	// Group 1: "status_code":"XXX" (JSON)
	// Group 2: XXX (Description) (httpfs format) 
	// Group 3: (HTTP XXX) or HTTP code XXX or HTTP XXX
	static duckdb_re2::Regex status_regex(
		"\"status_code\":\"(\\d{3})\"|" // JSON format
		"(\\d{3})\\s*\\([^)]+\\)|" // "404 (Not Found)"
		"\\(?HTTP\\s+(?:code\\s+)?(\\d{3})\\)?" // "(HTTP 404)", "HTTP code 403", "HTTP 500"
	);
	
	duckdb_re2::Match match;
	if (duckdb_re2::RegexSearch(error_msg, match, status_regex)) {
		// Check which group captured the status code (groups are 1-indexed)
		for (idx_t i = 1; i < match.groups.size(); i++) {
			if (!match.groups[i].text.empty()) {
				int32_t result;
				if (TryCast::Operation<string_t, int32_t>(string_t(match.groups[i].text), result)) {
					return result;
				}
			}
		}
	}
	
	return 0; // No status code found
}

// Modern idiomatic HTTP error detection using StringUtil
static bool IsHTTPError(const string &error_msg) {
	// Use std::any_of with StringUtil::Contains for cleaner, more efficient checking
	return std::any_of(std::begin(HTTP_ERROR_PATTERNS), std::end(HTTP_ERROR_PATTERNS),
		[&error_msg](const char* pattern) {
			return StringUtil::Contains(error_msg, pattern);
		});
}

// Map HTTP status code to SQLite error code
static int HTTPStatusToSQLiteError(int http_status) {
	switch (http_status) {
		case 404: 
			return SQLITE_CANTOPEN;       // SQLite will interpret as "unable to open database file"
		case 401:                         // Unauthorized (auth required)
		case 403: 
			return SQLITE_PERM;           // "permission denied"
		case 408: 
			return SQLITE_IOERR_ACCESS;   // Request timeout
		case 429: 
			return SQLITE_BUSY;           // Too many requests
		default:
			if (http_status >= 500 && http_status < 600) {
				return SQLITE_IOERR;      // Server errors
			}
			return 0; // Unknown/unmapped status
	}
}

template<typename T, typename Func>
static T SafeVFSCall(T error_value, Func&& func, DuckDBVFSWrapper *wrapper = nullptr, const char *path = nullptr, const char *method = nullptr) {
	try {
		return func();
	} catch (const std::exception &e) {
		string error_msg = e.what();
		// fprintf(stderr, "DEBUG VFS: Exception caught in %s: %.200s\n", method ? method : "unknown", error_msg.c_str());
		// fprintf(stderr, "DEBUG VFS: IsHTTPError result: %s\n", IsHTTPError(error_msg) ? "true" : "false");
		
		// Check if this is an HTTP error
		if (IsHTTPError(error_msg)) {
			// Store HTTP error context
			if (wrapper) {
				string full_error = "HTTP Error: ";
				full_error += error_msg;
				if (path) {
					full_error += " (URL: ";
					full_error += path;
					full_error += ")";
				}
				wrapper->SetLastError(full_error);
			}
			
			// Try to map HTTP status to specific SQLite error
			int http_status = ExtractHTTPStatus(error_msg);
			int sqlite_error = HTTPStatusToSQLiteError(http_status);
			// Debug (uncomment for troubleshooting)
			// fprintf(stderr, "DEBUG: Error message (first 200 chars): %.200s\n", error_msg.c_str());
			// fprintf(stderr, "DEBUG: HTTP Status: %d, SQLite Error: %d\n", http_status, sqlite_error);
			if (sqlite_error != 0) {
				return sqlite_error;
			}
			
			// Special case: Network connection failures should be treated as "unable to open"
			if (error_msg.find("Unable to connect to URL") != string::npos ||
			    error_msg.find("Could not establish connection") != string::npos) {
				return error_value == SQLITE_OK ? SQLITE_CANTOPEN : error_value;
			}
			
			// Default for unmapped HTTP errors (server errors, etc.)
			return error_value == SQLITE_OK ? SQLITE_IOERR : error_value;
		}
		
		// Check for specific DuckDB exception types in the message
		if (error_msg.find("Permission") != string::npos) {
			if (wrapper) {
				string full_error = "Permission denied: ";
				full_error += error_msg;
				if (path) {
					full_error += " (Path: ";
					full_error += path;
					full_error += ")";
				}
				wrapper->SetLastError(full_error);
			}
			return error_value == SQLITE_OK ? SQLITE_PERM : error_value;
		}
		
		// Store generic error context
		if (wrapper) {
			string full_error = "Error: ";
			full_error += error_msg;
			if (path) {
				full_error += " (Path: ";
				full_error += path;
				full_error += ")";
			}
			wrapper->SetLastError(full_error);
		}
		
		// Default error handling
		return error_value;
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
	static atomic<uint64_t> vfs_counter{0};
	return "duckdb_cache_vfs_" + to_string(vfs_counter.fetch_add(1));
}

//===--------------------------------------------------------------------===//
// DuckDBCachedFile Implementation
//===--------------------------------------------------------------------===//

DuckDBCachedFile::DuckDBCachedFile(ClientContext &context, const string &path) 
    : context(context), path(path) {
	// Defer actual file opening until first use to avoid doing DuckDB operations
	// during SQLite VFS callbacks, which might be in a different serialization context
}

DuckDBCachedFile::~DuckDBCachedFile() {
	// Ensure proper cleanup of the caching handle
	// The unique_ptr will automatically release the handle,
	// but we add this explicit destructor for clarity and
	// to enable future debugging/validation if needed
	if (caching_handle) {
		// Reset explicitly to ensure deterministic cleanup order
		caching_handle.reset();
	}
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
	// fprintf(stderr, "DEBUG: About to open file: %s\n", path.c_str());
	caching_handle = caching_fs.OpenFile(file_info, flags);
	
		
	initialized = true;
}


int DuckDBCachedFile::Read(void *buffer, int amount, sqlite3_int64 offset) {
	// Validate inputs to prevent integer overflow attacks
	if (offset < 0 || amount < 0) {
		return SQLITE_IOERR_READ;
	}
	
	// Early return for empty reads (SQLite sometimes requests 0 bytes)
	if (!buffer || amount == 0) {
		return SQLITE_OK;
	}
	
	// Ensure we're initialized before first read
	// Let exceptions propagate to SafeVFSCall for unified error handling
	EnsureInitialized();
	
	// Safety check - should never happen in normal operation
	if (!caching_handle) {
		return SQLITE_IOERR_READ;
	}

	// Get current file size from DuckDB (handles validation/caching)
	const sqlite3_int64 file_size = static_cast<sqlite3_int64>(caching_handle->GetFileSize());
	
	// Check if we're reading past EOF
	if (offset >= file_size) {
		// Reading completely past EOF - zero-fill entire buffer
		memset(buffer, 0, amount);
		return SQLITE_IOERR_SHORT_READ;
	}
	
	// Calculate how many bytes we can actually read
	const sqlite3_int64 available_bytes = file_size - offset;
	const int bytes_to_read = (available_bytes < amount) ? static_cast<int>(available_bytes) : amount;
	
	// Calculate optimal read-ahead size based on access pattern
	const idx_t readahead_size = CalculateReadAheadSize(offset, bytes_to_read);
	
	// Ensure we read at least the requested amount (up to EOF)
	idx_t actual_read_size = MaxValue(static_cast<idx_t>(bytes_to_read), readahead_size);
	
	// Don't read beyond file end (use safe arithmetic to prevent overflow)
	// We already know offset < file_size from the check above
	const sqlite3_int64 remaining_bytes = file_size - offset;
	if (static_cast<sqlite3_int64>(actual_read_size) > remaining_bytes) {
		actual_read_size = static_cast<idx_t>(remaining_bytes);
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
	
	// Note: SQLite validates the database header itself when opening the database,
	// so we don't need to duplicate that validation here.
	
	// If we read less than requested, zero-fill the remainder
	if (bytes_to_read < amount) {
		memset(static_cast<char*>(buffer) + bytes_to_read, 0, amount - bytes_to_read);
	}
	
	// Update read-ahead state after successful read
	UpdateReadAheadState(offset, bytes_to_read);
	
	// Return appropriate code based on whether we satisfied the full request
	return (bytes_to_read < amount) ? SQLITE_IOERR_SHORT_READ : SQLITE_OK;
}

sqlite3_int64 DuckDBCachedFile::GetFileSize() {
	try {
		EnsureInitialized();
		// Let DuckDB handle all caching/validation logic
		return static_cast<sqlite3_int64>(caching_handle->GetFileSize());
	} catch (...) {
		return -1;
	}
}

idx_t DuckDBCachedFile::CalculateReadAheadSize(sqlite3_int64 offset, int amount) const {
	// First read or non-sequential access - use minimum size
	if (last_read_offset == -1 || !IsSequentialRead(offset)) {
		return MIN_READAHEAD_SIZE;
	}
	
	// Sequential read - double the current size up to maximum
	const idx_t next_size = current_readahead_size * 2;
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
	wrapper->vfs_name = static_cast<char*>(sqlite3_malloc64(temp_name.length() + 1));
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
		
		// wrapper is already declared in outer scope
		ClientContext *context = wrapper->context;
		
		// Defensive check: Validate context is still valid
		if (!context || !context->db) {
			return SQLITE_CANTOPEN;
		}

		// Initialize the structure members properly
		duckdb_file->base.pMethods = &wrapper->io_methods;
		duckdb_file->duckdb_file = nullptr;
		
		// Create the DuckDB file handle with proper exception handling
		try {
			duckdb_file->duckdb_file = new DuckDBCachedFile(*context, filename);
		} catch (...) {
			// Clean up on failure
			duckdb_file->base.pMethods = nullptr;
			duckdb_file->duckdb_file = nullptr;
			return SQLITE_CANTOPEN;
		}
		
		// Don't validate SQLite header here - defer until first read
		// to avoid triggering DuckDB operations in VFS callbacks

		if (out_flags) {
			*out_flags = flags;
		}

		return SQLITE_OK;
	}, wrapper, filename, "xOpen");
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
	try {
		if (err_msg && bytes > 0) {
			strncpy(err_msg, "Dynamic loading not supported for remote files", bytes - 1);
			err_msg[bytes - 1] = '\0';
		}
	} catch (...) {
		// Best effort - if we can't even set the error message, just return
		if (err_msg && bytes > 0) {
			err_msg[0] = '\0';
		}
	}
}

void (*SQLiteDuckDBCacheVFS::DlSym(sqlite3_vfs *vfs, void *handle, const char *symbol))(void) {
	return nullptr;
}

void SQLiteDuckDBCacheVFS::DlClose(sqlite3_vfs *vfs, void *handle) {
	// No-op - dynamic libraries not supported
}

int SQLiteDuckDBCacheVFS::GetLastError(sqlite3_vfs *vfs, int bytes, char *err_msg) {
	return SafeVFSCall<int>(0, [&]() {
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
		
		return static_cast<int>(error.length());
	});
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