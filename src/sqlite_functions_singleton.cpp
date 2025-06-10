#include "sqlite_functions_singleton.hpp"
#include "sqlite_scanner.hpp"

namespace duckdb {

SqliteScanFunction& SqliteFunctions::GetScanFunction() {
#ifdef _WIN32
	fprintf(stderr, "[SQLITE_SCAN_DEBUG] GetScanFunction called\n");
	fflush(stderr);
#endif
	static SqliteScanFunction instance;
	return instance;
}

SqliteAttachFunction& SqliteFunctions::GetAttachFunction() {
	static SqliteAttachFunction instance;
	return instance;
}

SQLiteQueryFunction& SqliteFunctions::GetQueryFunction() {
	static SQLiteQueryFunction instance;
	return instance;
}

} // namespace duckdb