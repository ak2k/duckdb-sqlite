//===----------------------------------------------------------------------===//
//                         DuckDB
//
// http_sqlite_filesystem.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/storage/caching_file_system.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/main/database.hpp"
#include "sqlite_duckdb_vfs_cache.hpp"

namespace duckdb {

//! FileSystem implementation for HTTP SQLite databases
//! Provides DuckDB FileSystem interface for remote SQLite files
class HttpSqliteFileSystem : public FileSystem {
public:
	//! Check if this filesystem can handle the given path
	bool CanHandleFile(const string &path) override;
	
	//! Support extended open file interface for context passing
	bool SupportsOpenFileExtended() const override {
		return true;
	}
	
	//! Open a file handle for HTTP SQLite database with context
	unique_ptr<FileHandle> OpenFileExtended(const OpenFileInfo &file, FileOpenFlags flags,
	                                        optional_ptr<FileOpener> opener) override;
	
	//! Get file size for HTTP SQLite database
	int64_t GetFileSize(FileHandle &handle) override;
	
	//! Read from HTTP SQLite database
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	
	//! Check if file exists
	bool FileExists(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	
	//! Get filesystem name for debugging
	string GetName() const override { return "HttpSqliteFileSystem"; }
	
	//! Register this filesystem with DuckDB
	static void Register(DatabaseInstance &db);

private:
	//! OpenFile method - redirects to OpenFileExtended
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener) override;
};

//! FileHandle implementation for HTTP SQLite files
class HttpSqliteFileHandle : public FileHandle {
public:
	HttpSqliteFileHandle(FileSystem &fs, const string &path, 
	                     ClientContext *context);
	
	//! Close the file handle
	void Close() override;
	
	//! Get the underlying cached file
	DuckDBCachedFile* GetCachedFile() { return cached_file.get(); }

private:
	//! Validate that the file has a proper SQLite header
	void ValidateSQLiteHeader();
	
	//! ClientContext for this file handle
	ClientContext *context;
	//! Cached file implementation
	unique_ptr<DuckDBCachedFile> cached_file;
};

} // namespace duckdb