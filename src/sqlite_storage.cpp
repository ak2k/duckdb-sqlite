#include "duckdb.hpp"
#include "sqlite_storage.hpp"
#include "sqlite_utils.hpp"
#include "storage/sqlite_catalog.hpp"
#include "storage/sqlite_transaction_manager.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "sqlite3.h"

namespace duckdb {

static unique_ptr<Catalog> SQLiteAttach(StorageExtensionInfo *storage_info, ClientContext &context,
                                        AttachedDatabase &db, const string &name, AttachInfo &info,
                                        AccessMode access_mode) {

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

	auto catalog = make_uniq<SQLiteCatalog>(db, info.path, std::move(options));

	return catalog;
}

static unique_ptr<TransactionManager> SQLiteCreateTransactionManager(StorageExtensionInfo *storage_info,
                                                                     AttachedDatabase &db, Catalog &catalog) {

	auto &sqlite_catalog = catalog.Cast<SQLiteCatalog>();
	auto result = make_uniq<SQLiteTransactionManager>(db, sqlite_catalog);

	return result;
}

SQLiteStorageExtension::SQLiteStorageExtension() {
	attach = SQLiteAttach;
	create_transaction_manager = SQLiteCreateTransactionManager;
}

} // namespace duckdb
