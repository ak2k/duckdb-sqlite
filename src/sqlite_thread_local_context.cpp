//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlite_thread_local_context.cpp
//
//
//===----------------------------------------------------------------------===//

#include "sqlite_thread_local_context.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/client_data.hpp"
#include <thread>

namespace duckdb {

// Thread-local storage for per-thread connections
// This allows each thread to have its own HTTP client infrastructure
// while still sharing the same ExternalFileCache at the database level
struct ThreadConnectionCache {
    std::unordered_map<std::string, unique_ptr<Connection>> connections;
    
    Connection* GetConnection(DatabaseInstance* db) {
        auto key = std::to_string(reinterpret_cast<uintptr_t>(db));
        auto it = connections.find(key);
        if (it == connections.end()) {
            // Create new connection - inherits loaded extensions from database
            auto conn = make_uniq<Connection>(*db);
            
            // Start a transaction to enable file operations
            // This mimics what the main connection does during query execution
            try {
                conn->BeginTransaction();
            } catch (const std::exception &e) {
                // If we can't start a transaction, the connection might still be usable
                // for read-only operations that don't require explicit transactions
            }
            
            // Successfully created thread-local connection
            
            Connection* conn_ptr = conn.get();
            connections[key] = std::move(conn);
            return conn_ptr;
        }
        return it->second.get();
    }
    
    void Cleanup() {
        // Clean up thread-local connections
        connections.clear();
    }
    
    ~ThreadConnectionCache() {
        Cleanup();
    }
};

// Function-local static to ensure proper initialization across DLL boundaries
static ThreadConnectionCache& GetThreadLocalCache() {
    static thread_local ThreadConnectionCache tls_connections;
    return tls_connections;
}

ClientContext* GetOrCreateThreadLocalContext(ClientContext &parent_context, bool enable_thread_local) {
    if (!enable_thread_local) {
        // Use parent context if thread-local mode is disabled
        return &parent_context;
    }
    
    // Get or create thread-local connection for this database
    auto* connection = GetThreadLocalCache().GetConnection(parent_context.db.get());
    return connection->context.get();
}

void CleanupThreadLocalContext() {
    GetThreadLocalCache().Cleanup();
}

bool IsUsingThreadLocalContext() {
    return !GetThreadLocalCache().connections.empty();
}

} // namespace duckdb