//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlite_scanner.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "sqlite_utils.hpp"

namespace duckdb {
class SQLiteDB;
class TableCatalogEntry;

struct SqliteBindData : public TableFunctionData {
	string file_name;
	string table_name;

	vector<string> names;
	vector<LogicalType> types;
	string sql;

	RowIdInfo row_id_info;
	bool all_varchar = false;

	optional_idx rows_per_group = 122880;
	SQLiteDB *global_db;

	optional_ptr<TableCatalogEntry> table;
};

class SqliteScanFunction : public TableFunction {
public:
	SqliteScanFunction();
	
	// Static methods to access scan function pointers
	static table_function_init_global_t GetInitGlobal();
	static table_function_init_local_t GetInitLocal();
	static table_function_t GetFunction();
};

class SqliteAttachFunction : public TableFunction {
public:
	SqliteAttachFunction();
};

class SQLiteQueryFunction : public TableFunction {
public:
	SQLiteQueryFunction();
};

} // namespace duckdb
