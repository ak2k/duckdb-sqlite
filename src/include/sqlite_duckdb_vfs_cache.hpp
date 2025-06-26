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
#include "duckdb/common/mutex.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/caching_file_system.hpp"

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

private:
	// Lazy initialization - defer DuckDB operations until first use
	void EnsureInitialized();

	// Adaptive read-ahead constants
	static constexpr uint64_t MIN_READAHEAD_SIZE = static_cast<uint64_t>(1024) * 1024;       // 1MB
	static constexpr uint64_t MAX_READAHEAD_SIZE = static_cast<uint64_t>(128) * 1024 * 1024; // 128MB
	static constexpr uint64_t SEQUENTIAL_THRESHOLD = static_cast<uint64_t>(64) * 1024;       // 64KB gap tolerance

	ClientContext &context;
	const string path;
	unique_ptr<CachingFileHandle> caching_handle;
	sqlite3_int64 cached_file_size = -1;  // Cached to avoid repeated remote calls
	bool initialized = false;
	
	// Adaptive read-ahead state
	sqlite3_int64 last_read_offset = -1;     // Track last read position
	sqlite3_int64 last_read_end = -1;        // End of last read (offset + amount)
	uint64_t current_readahead_size = MIN_READAHEAD_SIZE;    // Current read-ahead block size
	
	// Helper methods for adaptive read-ahead
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
	static int Open(sqlite3_vfs *vfs, const char *filename, sqlite3_file *file, int flags, int *out_flags);
	static int Delete(sqlite3_vfs *vfs, const char *filename, int sync_dir);
	static int Access(sqlite3_vfs *vfs, const char *filename, int flags, int *result);
	static int FullPathname(sqlite3_vfs *vfs, const char *filename, int out_size, char *out_buf);
	static void *DlOpen(sqlite3_vfs *vfs, const char *filename);
	static void DlError(sqlite3_vfs *vfs, int bytes, char *err_msg);
	static void (*DlSym(sqlite3_vfs *vfs, void *handle, const char *symbol))(void);
	static void DlClose(sqlite3_vfs *vfs, void *handle);
	static int Randomness(sqlite3_vfs *vfs, int bytes, char *out);
	static int Sleep(sqlite3_vfs *vfs, int microseconds);
	static int CurrentTime(sqlite3_vfs *vfs, double *time);
	static int GetLastError(sqlite3_vfs *vfs, int bytes, char *err_msg);

	// SQLite file I/O methods (must be public for C callback registration)
	static int Close(sqlite3_file *file);
	static int Read(sqlite3_file *file, void *buffer, int amount, sqlite3_int64 offset);
	static int Write(sqlite3_file *file, const void *buffer, int amount, sqlite3_int64 offset);
	static int Truncate(sqlite3_file *file, sqlite3_int64 size);
	static int Sync(sqlite3_file *file, int flags);
	static int FileSize(sqlite3_file *file, sqlite3_int64 *size);
	static int Lock(sqlite3_file *file, int level);
	static int Unlock(sqlite3_file *file, int level);
	static int CheckReservedLock(sqlite3_file *file, int *result);
	static int FileControl(sqlite3_file *file, int op, void *arg);
	static int SectorSize(sqlite3_file *file);
	static int DeviceCharacteristics(sqlite3_file *file);

private:
	// No private members - all state is managed through static methods
};

// SQLite file handle structure that wraps our DuckDBCachedFile.
// Memory layout must be compatible with SQLite's expectations.
// IMPORTANT: This structure is allocated by SQLite and may cross module boundaries.
// We use raw pointers with explicit ownership rules to avoid DLL issues.
struct SQLiteDuckDBCachedFile {
	sqlite3_file base;  // Must be first member for C compatibility
	DuckDBCachedFile *duckdb_file;  // Raw pointer - explicitly deleted in Close()
	ClientContext *context;  // Non-owning pointer to DuckDB context
};

} // namespace duckdb