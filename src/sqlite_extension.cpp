#ifndef DUCKDB_BUILD_LOADABLE_EXTENSION
#define DUCKDB_BUILD_LOADABLE_EXTENSION
#endif
#include "duckdb.hpp"

#include "sqlite_db.hpp"
#include "sqlite_scanner.hpp"
#include "sqlite_storage.hpp"
#include "sqlite_scanner_extension.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

using namespace duckdb;

extern "C" {

static void SetSqliteDebugQueryPrint(ClientContext &context, SetScope scope, Value &parameter) {
	SQLiteDB::DebugSetPrintQueries(BooleanValue::Get(parameter));
}

static void LoadInternal(ExtensionLoader &loader) {
	SqliteScanFunction sqlite_fun;
	loader.RegisterFunction(sqlite_fun);

	SqliteAttachFunction attach_func;
	loader.RegisterFunction(attach_func);

	SQLiteQueryFunction query_func;
	loader.RegisterFunction(query_func);

	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);
	config.AddExtensionOption("sqlite_all_varchar", "Load all SQLite columns as VARCHAR columns", LogicalType::BOOLEAN);

	config.AddExtensionOption("sqlite_debug_show_queries", "DEBUG SETTING: print all queries sent to SQLite to stdout",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false), SetSqliteDebugQueryPrint);

	config.storage_extensions["sqlite_scanner"] = make_uniq<SQLiteStorageExtension>();
}

void SqliteScannerExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

DUCKDB_EXTENSION_API void sqlite_scanner_init(duckdb::DatabaseInstance &db) {
	ExtensionLoader loader(db, "sqlite_scanner");
	LoadInternal(loader);
}

DUCKDB_EXTENSION_API void sqlite_scanner_duckdb_cpp_init(duckdb::DatabaseInstance &db) {
	sqlite_scanner_init(db);
}

DUCKDB_EXTENSION_API const char *sqlite_scanner_version() {
	return DuckDB::LibraryVersion();
}

DUCKDB_EXTENSION_API void sqlite_scanner_storage_init(DBConfig &config) {
	config.storage_extensions["sqlite_scanner"] = make_uniq<SQLiteStorageExtension>();
}
}
