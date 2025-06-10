//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlite_functions_singleton.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"

namespace duckdb {

class SqliteScanFunction;
class SqliteAttachFunction;
class SQLiteQueryFunction;

// Singleton pattern to ensure thread-safe initialization on Windows
// This prevents the binary_deserializer assertion in debug builds
struct SqliteFunctions {
	static SqliteScanFunction& GetScanFunction();
	static SqliteAttachFunction& GetAttachFunction();
	static SQLiteQueryFunction& GetQueryFunction();
};

} // namespace duckdb