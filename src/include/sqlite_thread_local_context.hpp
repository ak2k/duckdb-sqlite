//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlite_thread_local_context.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/unique_ptr.hpp"

namespace duckdb {

class ClientContext;
class DatabaseInstance;

// Get or create a thread-local connection/context for the current thread
// Uses Connection-based approach for proper resource isolation
// If enable_thread_local is false, returns the parent context
ClientContext* GetOrCreateThreadLocalContext(ClientContext &parent_context, bool enable_thread_local);

// Clean up all thread-local connections for the current thread
void CleanupThreadLocalContext();

// Check if the current thread is using thread-local connections
bool IsUsingThreadLocalContext();

} // namespace duckdb