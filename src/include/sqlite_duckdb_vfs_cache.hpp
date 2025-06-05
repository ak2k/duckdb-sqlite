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
#include "duckdb/storage/external_file_cache.hpp"
#include "duckdb/storage/caching_file_system.hpp"
#include "sqlite3.h"
#include <mutex>
#include <memory>

namespace duckdb {

class ClientContext;

// DuckDB file that uses external file cache for proper cache sharing
class DuckDBCachedFile {
public:
	DuckDBCachedFile(ClientContext &context, const string &path);
	~DuckDBCachedFile();

	//! Read data from the file using external file cache
	int Read(void *buffer, int amount, sqlite3_int64 offset);
	//! Get the file size
	sqlite3_int64 GetFileSize();
	//! Get the path
	const string &GetPath() const { return path; }

private:
	//! Try to get cached range without fetching
	BufferHandle TryGetCachedRange(idx_t offset, idx_t amount);
	//! Ensure file is open and metadata is loaded
	void EnsureFileOpen();
	//! Try to read from cache or fetch if needed
	BufferHandle ReadFromCache(idx_t offset, idx_t amount);
	
	//! Get the block size (1MB like original)
	static constexpr idx_t BLOCK_SIZE = 1024 * 1024;

private:
	ClientContext &context;
	string path;
	ExternalFileCache &cache;
	ExternalFileCache::CachedFile &cached_file;
	unique_ptr<FileHandle> file_handle;
	sqlite3_int64 file_size;
	bool size_fetched;
	
	// Version tracking for cache validation
	time_t last_modified;
	string version_tag;
};

// VFS that uses DuckDB's external file cache for proper sharing
class SqliteDuckDBCacheVFS {
public:
	//! Register the cached DuckDB VFS with SQLite
	static void Register(ClientContext &context);
	//! Check if DuckDB can handle this path
	static bool CanHandlePath(ClientContext &context, const string &path);
	//! Get the VFS name
	static const char *GetVFSName() { return "duckdb_cache_fs"; }

	//! VFS methods - must be public for static initialization
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

	//! File methods - must be public for static initialization
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
	static ClientContext *current_context;
	static std::mutex context_mutex;
};

//! SQLite file structure for DuckDB cached files
struct SqliteDuckDBCachedFile {
	sqlite3_file base;  // Must be first
	unique_ptr<DuckDBCachedFile> duckdb_file;
};

} // namespace duckdb