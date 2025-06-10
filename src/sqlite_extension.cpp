#ifndef DUCKDB_BUILD_LOADABLE_EXTENSION
#define DUCKDB_BUILD_LOADABLE_EXTENSION
#endif
#include "duckdb.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include "sqlite_db.hpp"
#include "sqlite_scanner.hpp"
#include "sqlite_storage.hpp"
#include "sqlite_scanner_extension.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

using namespace duckdb;

extern "C" {

static void SetSqliteDebugQueryPrint(ClientContext &context, SetScope scope, Value &parameter) {
	SQLiteDB::DebugSetPrintQueries(BooleanValue::Get(parameter));
}

static void LoadInternal(DatabaseInstance &db) {
#ifdef _WIN32
	fprintf(stderr, "[SQLITE_SCAN_DEBUG] LoadInternal called\n");
	fflush(stderr);
#endif

	// Use function-local statics for TableFunction objects
	// This pattern avoids static initialization issues on Windows
	static SqliteScanFunction sqlite_scan;
#ifdef _WIN32
	fprintf(stderr, "[SQLITE_SCAN_DEBUG] Using SqliteScanFunction at %p\n", (void*)&sqlite_scan);
	fflush(stderr);
#endif
	ExtensionUtil::RegisterFunction(db, sqlite_scan);
#ifdef _WIN32
	fprintf(stderr, "[SQLITE_SCAN_DEBUG] sqlite_scan function registered\n");
	fflush(stderr);
#endif

	static SqliteAttachFunction sqlite_attach;
	ExtensionUtil::RegisterFunction(db, sqlite_attach);

	static SQLiteQueryFunction sqlite_query;
	ExtensionUtil::RegisterFunction(db, sqlite_query);

	auto &config = DBConfig::GetConfig(db);
	config.AddExtensionOption("sqlite_all_varchar", "Load all SQLite columns as VARCHAR columns", LogicalType::BOOLEAN);

	config.AddExtensionOption("sqlite_debug_show_queries", "DEBUG SETTING: print all queries sent to SQLite to stdout",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false), SetSqliteDebugQueryPrint);

	// Only register storage extension if not already present
	if (config.storage_extensions.find("sqlite_scanner") == config.storage_extensions.end()) {
		config.storage_extensions["sqlite_scanner"] = make_uniq<SQLiteStorageExtension>();
	}
	
	// HTTP SQLite support is handled entirely by VFS through DuckDB's CachingFileSystem
}

void SqliteScannerExtension::Load(DuckDB &db) {
	LoadInternal(*db.instance);
}

DUCKDB_EXTENSION_API void sqlite_scanner_init(duckdb::DatabaseInstance &db) {
#ifdef _WIN32
	fprintf(stderr, "[SQLITE_SCAN_DEBUG] sqlite_scanner_init called\n");
	// Check stack size on Windows
	ULONG_PTR low, high;
	GetCurrentThreadStackLimits(&low, &high);
	fprintf(stderr, "[SQLITE_SCAN_DEBUG] Stack size: %llu KB\n", (high - low) / 1024);
	fflush(stderr);
#endif
	try {
		LoadInternal(db);
#ifdef _WIN32
		fprintf(stderr, "[SQLITE_SCAN_DEBUG] sqlite_scanner_init completed successfully\n");
		fflush(stderr);
#endif
	} catch (const std::exception &e) {
#ifdef _WIN32
		fprintf(stderr, "[SQLITE_SCAN_DEBUG] Exception in sqlite_scanner_init: %s\n", e.what());
		fflush(stderr);
#endif
		throw;
	} catch (...) {
#ifdef _WIN32
		fprintf(stderr, "[SQLITE_SCAN_DEBUG] Unknown exception in sqlite_scanner_init\n");
		fflush(stderr);
#endif
		throw;
	}
}

DUCKDB_EXTENSION_API const char *sqlite_scanner_version() {
	return DuckDB::LibraryVersion();
}

DUCKDB_EXTENSION_API void sqlite_scanner_storage_init(DBConfig &config) {
	// Only register if not already present
	if (config.storage_extensions.find("sqlite_scanner") == config.storage_extensions.end()) {
		config.storage_extensions["sqlite_scanner"] = make_uniq<SQLiteStorageExtension>();
	}
}
}
