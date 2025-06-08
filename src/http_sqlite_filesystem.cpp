//===----------------------------------------------------------------------===//
//                         DuckDB
//
// http_sqlite_filesystem.cpp
//
//
//===----------------------------------------------------------------------===//

#include "http_sqlite_filesystem.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/string_util.hpp"
#include <cstring>

// Thread-local flag to prevent infinite recursion
thread_local bool http_sqlite_opening = false;

namespace duckdb {

//===--------------------------------------------------------------------===//
// HttpSqliteFileSystem
//===--------------------------------------------------------------------===//

bool HttpSqliteFileSystem::CanHandleFile(const string &path) {
	// CRITICAL: Only handle files when explicitly called from our VFS
	// This prevents infinite recursion and lets httpfs handle direct HTTP access
	if (!http_sqlite_opening) {
		return false;  // Not called from our VFS, let httpfs handle it
	}
	
	// Only handle remote files when called from VFS
	if (!FileSystem::IsRemoteFile(path)) {
		return false;
	}
	
	// We can handle any remote file when called from VFS - SQLite validation happens during opening
	return true;
}

unique_ptr<FileHandle> HttpSqliteFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                      optional_ptr<FileOpener> opener) {
	// Delegate to extended interface
	OpenFileInfo file_info(path);
	return OpenFileExtended(file_info, flags, opener);
}

unique_ptr<FileHandle> HttpSqliteFileSystem::OpenFileExtended(const OpenFileInfo &file, FileOpenFlags flags,
                                                              optional_ptr<FileOpener> opener) {
	if (!CanHandleFile(file.path)) {
		throw InvalidInputException("HttpSqliteFileSystem cannot handle file: %s", file.path);
	}
	
	if (flags.OpenForWriting() || flags.OpenForAppending()) {
		throw InvalidInputException("HttpSqliteFileSystem only supports read-only access");
	}
	
	// Extract ClientContext from FileOpener
	ClientContext *context = nullptr;
	if (opener) {
		context = FileOpener::TryGetClientContext(opener).get();
	}
	
	if (!context) {
		throw InvalidInputException("HttpSqliteFileSystem requires ClientContext for file: %s", file.path);
	}
	
	// RAII guard to automatically manage flag state
	struct FlagGuard {
		bool &flag;
		explicit FlagGuard(bool &f) : flag(f) { flag = true; }
		~FlagGuard() { flag = false; }
	};
	
	// Set flag to prevent recursion when creating cached file
	FlagGuard guard(http_sqlite_opening);
	return make_uniq<HttpSqliteFileHandle>(*this, file.path, context);
}

int64_t HttpSqliteFileSystem::GetFileSize(FileHandle &handle) {
	auto &sqlite_handle = handle.Cast<HttpSqliteFileHandle>();
	auto caching_handle = sqlite_handle.GetCachingHandle();
	if (!caching_handle) {
		throw InternalException("HttpSqliteFileHandle has no caching handle");
	}
	return caching_handle->GetFileSize();
}

void HttpSqliteFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &sqlite_handle = handle.Cast<HttpSqliteFileHandle>();
	auto caching_handle = sqlite_handle.GetCachingHandle();
	if (!caching_handle) {
		throw InternalException("HttpSqliteFileHandle has no caching handle");
	}
	
	// Use DuckDB's caching read API
	data_ptr_t read_buffer;
	auto buffer_handle = caching_handle->Read(read_buffer, nr_bytes, location);
	memcpy(buffer, read_buffer, nr_bytes);
}

bool HttpSqliteFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	if (!CanHandleFile(filename)) {
		return false;
	}
	
	// Remote file existence cannot be efficiently validated without opening
	return FileSystem::IsRemoteFile(filename);
}

void HttpSqliteFileSystem::Register(DatabaseInstance &db) {
	// Register HTTP SQLite filesystem as subsystem
	auto &fs = db.GetFileSystem();
	fs.RegisterSubSystem(make_uniq<HttpSqliteFileSystem>());
}

//===--------------------------------------------------------------------===//
// HttpSqliteFileHandle
//===--------------------------------------------------------------------===//

HttpSqliteFileHandle::HttpSqliteFileHandle(FileSystem &fs, const string &path, ClientContext *context)
    : FileHandle(fs, path, FileOpenFlags::FILE_FLAGS_READ), context(context) {
	
	if (!context) {
		throw InternalException("HttpSqliteFileHandle requires valid ClientContext");
	}
	
	// Use CachingFileSystem directly with httpfs (no DuckDBCachedFile wrapper)
	// This avoids recursion since we're the bridge between VFS and DuckDB
	auto caching_fs = CachingFileSystem::Get(*context);
	auto flags = FileOpenFlags::FILE_FLAGS_READ | FileOpenFlags::FILE_FLAGS_DIRECT_IO;
	OpenFileInfo file_info(path);
	
	// Reset the flag so httpfs can handle this
	http_sqlite_opening = false;
	try {
		caching_handle = caching_fs.OpenFile(file_info, flags);
		http_sqlite_opening = true;  // Restore for cleanup
	} catch (...) {
		http_sqlite_opening = true;  // Restore for cleanup
		throw;
	}
	
	// Verify file is a valid SQLite database
	ValidateSQLiteHeader();
}

void HttpSqliteFileHandle::ValidateSQLiteHeader() {
	// SQLite format validation per https://www.sqlite.org/fileformat.html
	constexpr char SQLITE_HEADER[] = "SQLite format 3\000";
	constexpr size_t SQLITE_HEADER_SIZE = 16;
	
	// Use DuckDB's caching read API
	data_ptr_t read_buffer;
	auto buffer_handle = caching_handle->Read(read_buffer, SQLITE_HEADER_SIZE, 0);
	if (memcmp(read_buffer, SQLITE_HEADER, SQLITE_HEADER_SIZE) != 0) {
		throw InvalidInputException("File is not a valid SQLite database: %s", path);
	}
}

void HttpSqliteFileHandle::Close() {
	// Release caching handle resources
	caching_handle.reset();
}

} // namespace duckdb