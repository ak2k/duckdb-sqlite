//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlite_duckdb_vfs_cache.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/storage/caching_file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "sqlite3.h"

namespace duckdb {

class ClientContext;

// Wrapper around DuckDB's CachingFileSystem for remote SQLite file access.
// Uses DuckDB's caching infrastructure to efficiently handle remote file I/O.
class DuckDBCachedFile {
public:
	DuckDBCachedFile(ClientContext &context, const string &path);
	~DuckDBCachedFile() = default;

	// Read data from the file at the specified offset
	int Read(void *buffer, int amount, sqlite3_int64 offset);
	// Get the cached file size
	sqlite3_int64 GetFileSize();
	// Get the file path
	const string &GetPath() const { return path; }

private:
	// Lazy initialization - defer DuckDB operations until first use
	void EnsureInitialized();

	ClientContext &context;
	string path;
	unique_ptr<CachingFileHandle> caching_handle;
	unique_ptr<FileHandle> base_handle;  // Unused - kept for potential future use
	sqlite3_int64 cached_file_size;  // Cached to avoid repeated remote calls
	bool initialized = false;
	
	// The following members are reserved for future adaptive read-ahead implementation.
	// Currently, DuckDB's CachingFileSystem handles all caching automatically.
	mutable mutex readahead_mutex;
	sqlite3_int64 last_read_offset;
	sqlite3_int64 last_read_end;
	uint64_t current_readahead_size;
	
	// Read-ahead size constants (not currently used)
	static constexpr uint64_t MIN_READAHEAD_SIZE = static_cast<uint64_t>(1024) * 1024;       // 1MB
	static constexpr uint64_t MAX_READAHEAD_SIZE = static_cast<uint64_t>(128) * 1024 * 1024; // 128MB
	static constexpr uint64_t SEQUENTIAL_THRESHOLD = static_cast<uint64_t>(64) * 1024;       // 64KB
	
	// Future read-ahead methods (not implemented)
	uint64_t CalculateReadAheadSize(sqlite3_int64 offset, int amount) const;
	bool IsSequentialRead(sqlite3_int64 offset) const;
	void UpdateReadAheadState(sqlite3_int64 offset, int amount);
};

// SQLite Virtual File System (VFS) implementation that uses DuckDB's
// CachingFileSystem for efficient remote SQLite database access.
class SQLiteDuckDBCacheVFS {
public:
	// Register the VFS with SQLite (thread-safe, idempotent)
	static void Register(ClientContext &context);
	// Unregister the VFS when context is destroyed
	static void Unregister(ClientContext &context);
	// Check if this path should be handled by our VFS (i.e., is it remote?)
	static bool CanHandlePath(ClientContext &context, const string &path);
	// Get the VFS registration name for a context
	static const char *GetVFSNameForContext(ClientContext &context);
	// Get the default VFS registration name (for compatibility)
	static const char *GetVFSName() { return "duckdb_cache_fs"; }

	// SQLite VFS interface methods (must be public for C callback registration)
	// Note: SQLite expects these to use the C calling convention
#ifndef SQLITE_CALLBACK
	#ifdef _WIN32
		#define SQLITE_CALLBACK __cdecl
	#else
		#define SQLITE_CALLBACK
	#endif
#endif
	
	static int SQLITE_CALLBACK Open(sqlite3_vfs *vfs, const char *filename, sqlite3_file *file, int flags, int *out_flags);
	static int SQLITE_CALLBACK Delete(sqlite3_vfs *vfs, const char *filename, int sync_dir);
	static int SQLITE_CALLBACK Access(sqlite3_vfs *vfs, const char *filename, int flags, int *result);
	static int SQLITE_CALLBACK FullPathname(sqlite3_vfs *vfs, const char *filename, int out_size, char *out_buf);
	static void * SQLITE_CALLBACK DlOpen(sqlite3_vfs *vfs, const char *filename);
	static void SQLITE_CALLBACK DlError(sqlite3_vfs *vfs, int bytes, char *err_msg);
	static void (* SQLITE_CALLBACK DlSym(sqlite3_vfs *vfs, void *handle, const char *symbol))(void);
	static void SQLITE_CALLBACK DlClose(sqlite3_vfs *vfs, void *handle);
	static int SQLITE_CALLBACK Randomness(sqlite3_vfs *vfs, int bytes, char *out);
	static int SQLITE_CALLBACK Sleep(sqlite3_vfs *vfs, int microseconds);
	static int SQLITE_CALLBACK CurrentTime(sqlite3_vfs *vfs, double *time);
	static int SQLITE_CALLBACK GetLastError(sqlite3_vfs *vfs, int bytes, char *err_msg);

	// SQLite file I/O methods (must be public for C callback registration)
	static int SQLITE_CALLBACK Close(sqlite3_file *file);
	static int SQLITE_CALLBACK Read(sqlite3_file *file, void *buffer, int amount, sqlite3_int64 offset);
	static int SQLITE_CALLBACK Write(sqlite3_file *file, const void *buffer, int amount, sqlite3_int64 offset);
	static int SQLITE_CALLBACK Truncate(sqlite3_file *file, sqlite3_int64 size);
	static int SQLITE_CALLBACK Sync(sqlite3_file *file, int flags);
	static int SQLITE_CALLBACK FileSize(sqlite3_file *file, sqlite3_int64 *size);
	static int SQLITE_CALLBACK Lock(sqlite3_file *file, int level);
	static int SQLITE_CALLBACK Unlock(sqlite3_file *file, int level);
	static int SQLITE_CALLBACK CheckReservedLock(sqlite3_file *file, int *result);
	static int SQLITE_CALLBACK FileControl(sqlite3_file *file, int op, void *arg);
	static int SQLITE_CALLBACK SectorSize(sqlite3_file *file);
	static int SQLITE_CALLBACK DeviceCharacteristics(sqlite3_file *file);

private:
	// No private members - all state is managed through static methods
};

// SQLite file handle structure that wraps our DuckDBCachedFile.
// Memory layout must be compatible with SQLite's expectations.
#ifdef _WIN32
#pragma pack(push, 8)
struct SQLiteDuckDBCachedFile {
	sqlite3_file base;  // Must be first member for C compatibility
	unique_ptr<DuckDBCachedFile> duckdb_file;  // The actual file implementation
	ClientContext *context;  // DuckDB context for this file
};
#pragma pack(pop)
#else
struct SQLiteDuckDBCachedFile {
	sqlite3_file base;  // Must be first member for C compatibility
	unique_ptr<DuckDBCachedFile> duckdb_file;  // The actual file implementation
	ClientContext *context;  // DuckDB context for this file
};
#endif

// RAII helper for automatic VFS registration/unregistration.
// Ensures that the VFS is properly cleaned up when the context is destroyed.
class SQLiteVFSRegistration {
public:
	explicit SQLiteVFSRegistration(ClientContext &context) : context(context) {
		SQLiteDuckDBCacheVFS::Register(context);
	}
	
	~SQLiteVFSRegistration() {
		SQLiteDuckDBCacheVFS::Unregister(context);
	}
	
	// Disable copy and move to ensure single ownership
	SQLiteVFSRegistration(const SQLiteVFSRegistration&) = delete;
	SQLiteVFSRegistration& operator=(const SQLiteVFSRegistration&) = delete;
	SQLiteVFSRegistration(SQLiteVFSRegistration&&) = delete;
	SQLiteVFSRegistration& operator=(SQLiteVFSRegistration&&) = delete;
	
private:
	ClientContext &context;
};

} // namespace duckdb