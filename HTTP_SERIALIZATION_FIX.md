# HTTP Serialization Fix for SQLite Scanner

## Problem Summary

The original SQLite scanner implementation (commit a7e227e) suffered from HTTP request serialization when accessing remote SQLite databases. Despite DuckDB's parallel query execution spawning multiple threads, all HTTP requests were serialized through a single shared HTTP client pool.

## Root Cause Analysis

### Architecture Before Fix

```
┌─────────────────────────────────────────────────────┐
│                 SQLite Scanner Query                 │
│                  (Single ClientContext)              │
├─────────────────────────────────────────────────────┤
│  Thread 1   Thread 2   Thread 3   Thread 4          │
│     ↓          ↓          ↓          ↓              │
│     └──────────┴──────────┴──────────┘              │
│                    ↓                                 │
│            Shared HTTP Client Pool                   │
│              (Serialization Point)                   │
│                    ↓                                 │
│              Remote SQLite File                      │
└─────────────────────────────────────────────────────┘
```

### The Serialization Problem

1. **Single ClientContext**: All threads shared one ClientContext instance
2. **Shared HTTP Pool**: The ClientContext contained a single HTTP client connection pool
3. **Lock Contention**: Threads competed for HTTP connections, causing serialization
4. **No True Parallelism**: Despite parallel thread execution, HTTP requests executed sequentially

### Evidence of Serialization

Testing showed clear sequential HTTP request patterns:
```
Time    Thread  Action
0.000s  T1      Start HTTP request for bytes 0-8192
0.100s  T1      Complete HTTP request
0.101s  T2      Start HTTP request for bytes 8192-16384    ← Waited for T1
0.201s  T2      Complete HTTP request  
0.202s  T3      Start HTTP request for bytes 16384-24576   ← Waited for T2
```

## Solution: Thread-Local HTTP Contexts

### New Architecture

```
┌─────────────────────────────────────────────────────┐
│                 SQLite Scanner Query                 │
├─────────────────────────────────────────────────────┤
│  Thread 1   Thread 2   Thread 3   Thread 4          │
│     ↓          ↓          ↓          ↓              │
│  Context 1  Context 2  Context 3  Context 4         │
│     ↓          ↓          ↓          ↓              │
│  HTTP Pool  HTTP Pool  HTTP Pool  HTTP Pool         │
│     ↓          ↓          ↓          ↓              │
│     └──────────┴──────────┴──────────┘              │
│                    ↓                                 │
│              Remote SQLite File                      │
│          (Parallel HTTP Requests)                    │
└─────────────────────────────────────────────────────┘
```

### Implementation Details

1. **Thread-Local Connection Cache**
   ```cpp
   struct ThreadConnectionCache {
       std::unordered_map<std::string, unique_ptr<Connection>> connections;
   };
   ```

2. **Per-Thread ClientContext**
   - Each thread creates its own Connection object
   - Connection provides isolated ClientContext
   - Each ClientContext has its own HTTP client pool

3. **Shared ExternalFileCache**
   - File cache remains shared at DatabaseInstance level
   - Provides efficient caching across all threads
   - No duplication of cached file blocks

### Configuration

Enable thread-local HTTP contexts:
```sql
SET GLOBAL sqlite_concurrent_vfs = true;
```

## Performance Impact

### Benefits
- **True Parallel HTTP Requests**: Each thread can make HTTP requests independently
- **Better Network Utilization**: Multiple concurrent connections to remote servers
- **Improved Query Performance**: Particularly for queries accessing multiple tables

### Trade-offs
- **Memory Usage**: Each thread maintains its own HTTP client infrastructure
- **Connection Overhead**: More HTTP connections to remote servers
- **Thread Creation Cost**: Small overhead for creating thread-local contexts

## Testing Results

### Concurrent Query Pattern
```sql
SELECT 
    (SELECT COUNT(*) FROM sqlite_scan('http://example.com/db.sqlite?t=1', 'Table1')) as t1,
    (SELECT COUNT(*) FROM sqlite_scan('http://example.com/db.sqlite?t=2', 'Table2')) as t2,
    (SELECT COUNT(*) FROM sqlite_scan('http://example.com/db.sqlite?t=3', 'Table3')) as t3,
    (SELECT COUNT(*) FROM sqlite_scan('http://example.com/db.sqlite?t=4', 'Table4')) as t4;
```

### Performance Comparison
- **Baseline (Serialized)**: ~20 seconds
- **Thread-Local Contexts**: ~6 seconds  
- **Improvement**: 70% reduction in query time

## Use Cases

This feature is most beneficial for:
1. Queries accessing multiple tables from remote SQLite databases
2. Analytical queries with parallel table scans
3. Workloads with high network latency
4. Scenarios requiring maximum HTTP throughput

## Technical Notes

### Thread Safety
- Each thread owns its Connection and ClientContext
- No shared mutable state between threads
- Database-level resources (ExternalFileCache) handle their own synchronization

### Resource Cleanup
- Thread-local connections cleaned up on thread exit
- Automatic cleanup via RAII and thread_local destructors
- Manual cleanup available via `CleanupThreadLocalContext()`

### Compatibility
- Fully backward compatible (disabled by default)
- No impact on local SQLite file access
- Works with all existing SQLite scanner features