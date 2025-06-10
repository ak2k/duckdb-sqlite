#include "duckdb.hpp"

#include "sqlite3.h"
#include "sqlite_utils.hpp"
#include "sqlite_storage.hpp"
#include "storage/sqlite_catalog.hpp"
#include "storage/sqlite_transaction_manager.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/limits.hpp"

namespace duckdb {

static unique_ptr<Catalog> SQLiteAttach(StorageExtensionInfo *storage_info, ClientContext &context,
                                        AttachedDatabase &db, const string &name, AttachInfo &info,
                                        AccessMode access_mode) {
#ifdef _WIN32
	fprintf(stderr, "[SQLITE_ATTACH_DEBUG] SQLiteAttach called with path: %s, name: %s\n", info.path.c_str(), name.c_str());
	fflush(stderr);
#endif
	SQLiteOpenOptions options;
	options.access_mode = access_mode;
	for (auto &entry : info.options) {
		if (StringUtil::CIEquals(entry.first, "busy_timeout")) {
			uint64_t timeout_value = entry.second.GetValue<uint64_t>();
			if (timeout_value > NumericLimits<int>::Maximum()) {
				throw InvalidInputException("busy_timeout out of range - must be within valid range for type int");
			}
			options.busy_timeout = timeout_value;
		} else if (StringUtil::CIEquals(entry.first, "journal_mode")) {
			options.journal_mode = entry.second.ToString();
		}
	}
#ifdef _WIN32
	fprintf(stderr, "[SQLITE_ATTACH_DEBUG] Creating SQLiteCatalog\n");
	fflush(stderr);
#endif
	auto catalog = make_uniq<SQLiteCatalog>(db, info.path, std::move(options));
#ifdef _WIN32
	fprintf(stderr, "[SQLITE_ATTACH_DEBUG] SQLiteCatalog created successfully\n");
	fflush(stderr);
#endif
	return catalog;
}

static unique_ptr<TransactionManager> SQLiteCreateTransactionManager(StorageExtensionInfo *storage_info,
                                                                     AttachedDatabase &db, Catalog &catalog) {
#ifdef _WIN32
	fprintf(stderr, "[SQLITE_TRANSACTION_DEBUG] SQLiteCreateTransactionManager called\n");
	fflush(stderr);
#endif
	auto &sqlite_catalog = catalog.Cast<SQLiteCatalog>();
	auto result = make_uniq<SQLiteTransactionManager>(db, sqlite_catalog);
#ifdef _WIN32
	fprintf(stderr, "[SQLITE_TRANSACTION_DEBUG] SQLiteTransactionManager created successfully\n");
	fflush(stderr);
#endif
	return result;
}

SQLiteStorageExtension::SQLiteStorageExtension() {
	attach = SQLiteAttach;
	create_transaction_manager = SQLiteCreateTransactionManager;
}

} // namespace duckdb
