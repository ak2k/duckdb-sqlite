//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/sqlite_transaction_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/sqlite_catalog.hpp"
#include "storage/sqlite_transaction.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

class SQLiteTransactionManager : public TransactionManager {
public:
	SQLiteTransactionManager(AttachedDatabase &db_p, SQLiteCatalog &sqlite_catalog);

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;

	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	SQLiteCatalog &sqlite_catalog;
	reference_map_t<Transaction, unique_ptr<SQLiteTransaction>> transactions;
	
	// Function-local static mutex to avoid Windows DLL initialization issues
	static mutex& GetTransactionLock();
};

} // namespace duckdb
