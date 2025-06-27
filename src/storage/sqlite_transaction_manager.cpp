#include "storage/sqlite_transaction_manager.hpp"
#include "duckdb/main/attached_database.hpp"

namespace duckdb {

// Function-local static mutex to avoid Windows DLL initialization issues
mutex& SQLiteTransactionManager::GetTransactionLock() {
	static mutex transaction_lock;
	return transaction_lock;
}

SQLiteTransactionManager::SQLiteTransactionManager(AttachedDatabase &db_p, SQLiteCatalog &sqlite_catalog)
    : TransactionManager(db_p), sqlite_catalog(sqlite_catalog) {
}

Transaction &SQLiteTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<SQLiteTransaction>(sqlite_catalog, *this, context);
	// Defer transaction start until first use to avoid potential deadlocks.
	// Starting here would trigger DB connection initialization which can block
	// for remote files while the MetaTransaction lock is held.
	// The transaction will be started lazily in SQLiteTransaction::GetDB()
	auto &result = *transaction;
	lock_guard<mutex> l(GetTransactionLock());
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData SQLiteTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	auto &sqlite_transaction = transaction.Cast<SQLiteTransaction>();
	sqlite_transaction.Commit();
	lock_guard<mutex> l(GetTransactionLock());
	transactions.erase(transaction);
	return ErrorData();
}

void SQLiteTransactionManager::RollbackTransaction(Transaction &transaction) {
	auto &sqlite_transaction = transaction.Cast<SQLiteTransaction>();
	sqlite_transaction.Rollback();
	lock_guard<mutex> l(GetTransactionLock());
	transactions.erase(transaction);
}

void SQLiteTransactionManager::Checkpoint(ClientContext &context, bool force) {
	auto &transaction = SQLiteTransaction::Get(context, db.GetCatalog());
	auto &db = transaction.GetDB();
	db.Execute("PRAGMA wal_checkpoint");
}

} // namespace duckdb
