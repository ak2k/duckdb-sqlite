//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlite_http_vfs.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "sqlite3.h"
#include <mutex>
#include <unordered_map>

namespace duckdb {

class ClientContext;

struct HttpBlock {
	idx_t offset;
	idx_t size;
	unique_ptr<char[]> data;
};

class HttpFile {
public:
	HttpFile(ClientContext &context, const string &url);
	~HttpFile();

	//! Read data from the HTTP file
	int Read(void *buffer, int amount, sqlite3_int64 offset);
	//! Get the file size
	sqlite3_int64 GetFileSize();
	//! Get the URL
	const string &GetURL() const { return url; }

private:
	//! Fetch a block from the HTTP server
	void FetchBlock(idx_t block_idx);
	//! Get the block size
	idx_t GetBlockSize() const { return 1024 * 1024; } // 1MB blocks

private:
	ClientContext &context;
	string url;
	sqlite3_int64 file_size;
	std::mutex cache_mutex;
	std::unordered_map<idx_t, unique_ptr<HttpBlock>> block_cache;
	bool size_fetched;
};

class SqliteHttpVFS {
public:
	//! Register the HTTP VFS with SQLite
	static void Register(ClientContext &context);
	//! Check if a path is an HTTP URL
	static bool IsHTTPPath(const string &path);
	//! Get the VFS name
	static const char *GetVFSName() { return "duckdb_httpfs"; }

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

//! SQLite file structure for HTTP files
struct SqliteHttpFile {
	sqlite3_file base;  // Must be first
	unique_ptr<HttpFile> http_file;
};

} // namespace duckdb