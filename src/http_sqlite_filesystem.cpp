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

namespace duckdb {

//===--------------------------------------------------------------------===//
// HttpSqliteFileSystem
//===--------------------------------------------------------------------===//

bool HttpSqliteFileSystem::CanHandleFile(const string &path) {
	// Only handle remote files
	if (!FileSystem::IsRemoteFile(path)) {
		return false;
	}
	
	// SQLite format validation is deferred to file opening to avoid extra HTTP requests
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
	
	// Create file handle with context
	return make_uniq<HttpSqliteFileHandle>(*this, file.path, context);
}

int64_t HttpSqliteFileSystem::GetFileSize(FileHandle &handle) {
	auto &sqlite_handle = handle.Cast<HttpSqliteFileHandle>();
	auto cached_file = sqlite_handle.GetCachedFile();
	if (!cached_file) {
		throw InternalException("HttpSqliteFileHandle has no cached file");
	}
	return cached_file->GetFileSize();
}

void HttpSqliteFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &sqlite_handle = handle.Cast<HttpSqliteFileHandle>();
	auto cached_file = sqlite_handle.GetCachedFile();
	if (!cached_file) {
		throw InternalException("HttpSqliteFileHandle has no cached file");
	}
	cached_file->Read(buffer, nr_bytes, location);
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
	
	// Initialize cached file with DuckDB's remote file caching
	cached_file = make_uniq<DuckDBCachedFile>(*context, path);
	
	// Verify file is a valid SQLite database
	ValidateSQLiteHeader();
}

void HttpSqliteFileHandle::ValidateSQLiteHeader() {
	// SQLite format validation per https://www.sqlite.org/fileformat.html
	constexpr char SQLITE_HEADER[] = "SQLite format 3\000";
	constexpr size_t SQLITE_HEADER_SIZE = 16;
	
	char header_buffer[SQLITE_HEADER_SIZE];
	cached_file->Read(header_buffer, SQLITE_HEADER_SIZE, 0);
	if (memcmp(header_buffer, SQLITE_HEADER, SQLITE_HEADER_SIZE) != 0) {
		throw InvalidInputException("File is not a valid SQLite database: %s", path);
	}
}

void HttpSqliteFileHandle::Close() {
	// Release cached file resources
	cached_file.reset();
}

} // namespace duckdb