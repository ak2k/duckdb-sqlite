#include "storage/sqlite_transaction_manager.hpp"
#include "duckdb/main/attached_database.hpp"

namespace duckdb {

SQLiteTransactionManager::SQLiteTransactionManager(AttachedDatabase &db_p, SQLiteCatalog &sqlite_catalog)
    : TransactionManager(db_p), sqlite_catalog(sqlite_catalog) {
}

Transaction &SQLiteTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<SQLiteTransaction>(sqlite_catalog, *this, context);
	
	// CRITICAL FIX: Do NOT call Start() here to avoid deadlock
	// Start() will trigger lazy DB connection which can block for remote files
	// Defer Start() until the transaction is actually used (in GetDB())
	// This prevents blocking while MetaTransaction lock is held
	
	auto &result = *transaction;
	lock_guard<mutex> l(transaction_lock);
	transactions[result] = std::move(transaction);
	
	return result;
}

ErrorData SQLiteTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	auto &sqlite_transaction = transaction.Cast<SQLiteTransaction>();
	sqlite_transaction.Commit();
	lock_guard<mutex> l(transaction_lock);
	transactions.erase(transaction);
	return ErrorData();
}

void SQLiteTransactionManager::RollbackTransaction(Transaction &transaction) {
	auto &sqlite_transaction = transaction.Cast<SQLiteTransaction>();
	sqlite_transaction.Rollback();
	lock_guard<mutex> l(transaction_lock);
	transactions.erase(transaction);
}

void SQLiteTransactionManager::Checkpoint(ClientContext &context, bool force) {
	auto &transaction = SQLiteTransaction::Get(context, db.GetCatalog());
	auto &db = transaction.GetDB();
	db.Execute("PRAGMA wal_checkpoint");
}

} // namespace duckdb
