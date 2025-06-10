#include "sqlite_functions_singleton.hpp"
#include "sqlite_scanner.hpp"

namespace duckdb {

SqliteScanFunction& SqliteFunctions::GetScanFunction() {
#ifdef _WIN32
	static int call_count = 0;
	call_count++;
	fprintf(stderr, "[SQLITE_SCAN_DEBUG] GetScanFunction called (call #%d)\n", call_count);
	fflush(stderr);
#endif
	// Use Meyers' singleton pattern - C++11 guarantees thread-safe initialization
	static SqliteScanFunction instance;
#ifdef _WIN32
	fprintf(stderr, "[SQLITE_SCAN_DEBUG] GetScanFunction returning instance at %p\n", (void*)&instance);
	fflush(stderr);
#endif
	return instance;
}

SqliteAttachFunction& SqliteFunctions::GetAttachFunction() {
#ifdef _WIN32
	fprintf(stderr, "[SQLITE_SCAN_DEBUG] GetAttachFunction called\n");
	fflush(stderr);
#endif
	static SqliteAttachFunction instance;
#ifdef _WIN32
	fprintf(stderr, "[SQLITE_SCAN_DEBUG] GetAttachFunction returning instance at %p\n", (void*)&instance);
	fflush(stderr);
#endif
	return instance;
}

SQLiteQueryFunction& SqliteFunctions::GetQueryFunction() {
#ifdef _WIN32
	fprintf(stderr, "[SQLITE_SCAN_DEBUG] GetQueryFunction called\n");
	fflush(stderr);
#endif
	// First ensure SqliteScanFunction is initialized since SQLiteQueryFunction depends on it
	GetScanFunction();
	
	static SQLiteQueryFunction instance;
#ifdef _WIN32
	fprintf(stderr, "[SQLITE_SCAN_DEBUG] GetQueryFunction returning instance at %p\n", (void*)&instance);
	fflush(stderr);
#endif
	return instance;
}

} // namespace duckdb