//===----------------------------------------------------------------------===//
//                         DuckDB
//
// sqlite_concurrent_vfs.hpp
//
// Header for concurrent VFS support
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/string_util.hpp"
#include <atomic>

namespace duckdb {

// Forward declarations
class DBConfig;

// SQLite Concurrent VFS interface - header-only to avoid linking issues
class SQLiteConcurrentVFS {
private:
    static std::atomic<bool> &GetConcurrentFlag() {
        static std::atomic<bool> flag(false);
        return flag;
    }

public:
    // Check if concurrent mode is enabled
    static bool IsConcurrentModeEnabled() {
        return GetConcurrentFlag().load();
    }
    
    static void SetConcurrentMode(bool enabled) {
        GetConcurrentFlag().store(enabled);
        // Mode changed successfully
    }
    
};

// Setting callback is defined inline in sqlite_extension.cpp to avoid linking issues

} // namespace duckdb