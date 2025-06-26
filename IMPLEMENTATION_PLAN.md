# HTTP SQLite Implementation Plan

## Overview
This plan addresses 56 confirmed issues (out of 62 total) in the HTTP SQLite implementation, organized into 7 phases. Each phase groups related fixes that can be compiled, tested, and committed independently.

## Phase 1: Critical VFS Contract Violations (MUST FIX FIRST)
**Priority**: 🔴 CRITICAL  
**Issues**: #1, #2, #3, #4, #9  
**Files**: `src/sqlite_duckdb_vfs_cache.cpp`

### Changes:
1. **Fix xRead implementation (#1)**:
   - Implement proper short read handling with zero-filling
   - Return SQLITE_IOERR_SHORT_READ when reading past EOF
   - Add bounds checking for buffer access

2. **Fix Access() implementation (#3)**:
   - Check for journal/WAL files existence
   - Return 1 when files exist (for remote SQLite databases with journals)
   - Handle SQLITE_ACCESS_EXISTS, SQLITE_ACCESS_READWRITE, SQLITE_ACCESS_READ

3. **Make Read() return bytes read (#9)**:
   - Track actual bytes read vs requested
   - Handle EOF correctly

### Verification:
```bash
# Compile
make GEN=ninja release

# Test basic functionality
./build/release/duckdb -unsigned
LOAD 'build/release/extension/sqlite_scanner/sqlite_scanner.duckdb_extension';
SELECT * FROM sqlite_scan('https://blobs.duckdb.org/databases/tpch_sf01.db', 'lineitem') LIMIT 10;

# Run specific tests
make test TEST_PATTERN="http_sqlite"

# Check with sanitizers
make clean && make GEN=ninja debug
ASAN_OPTIONS=detect_leaks=1 ./build/debug/duckdb test/sql/scanner/http_sqlite_*.test
```

### Commit Message:
```
Fix critical SQLite VFS contract violations

- Implement proper xRead with zero-filling and SQLITE_IOERR_SHORT_READ
- Fix Access() to correctly detect journal/WAL files
- Make Read() return actual bytes read
- Add comprehensive bounds checking
```

## Phase 2: C/C++ Boundary Safety
**Priority**: 🔴 CRITICAL  
**Issues**: #6, #7, #10, #11, #12, #45, #46, #47, #48  
**Files**: `src/sqlite_duckdb_vfs_cache.cpp`, `src/sqlite_db.cpp`, `src/storage/sqlite_transaction.cpp`

### Changes:
1. **Add systematic exception handling (#6)**:
   - Create SafeVFSCall template for all VFS callbacks
   - Wrap all C++ operations in try-catch blocks
   - Convert exceptions to SQLite error codes

2. **Fix memory allocation boundaries (#10, #11, #12)**:
   - Use sqlite3_malloc for VFS name allocation
   - Ensure matching allocator/deallocator pairs
   - Add RAII wrappers for cross-boundary allocations

3. **Fix lifetime management (#45, #46, #47, #48)**:
   - Ensure ClientContext outlives VFS operations
   - Add validation before dereferencing pointers
   - Clear pointers after cleanup

### Verification:
```bash
# Compile with strict warnings
make clean && CXX_FLAGS="-Wall -Wextra -Werror" make GEN=ninja release

# Test with valgrind
valgrind --leak-check=full --track-origins=yes ./build/release/duckdb -unsigned -c "
LOAD 'build/release/extension/sqlite_scanner/sqlite_scanner.duckdb_extension';
SELECT COUNT(*) FROM sqlite_scan('https://blobs.duckdb.org/databases/tpch_sf01.db', 'lineitem');
"

# Run thread sanitizer
make clean && make GEN=ninja debug TSAN=1
./build/debug/duckdb test/sql/scanner/http_sqlite_*.test
```

### Commit Message:
```
Add comprehensive C/C++ boundary safety

- Implement SafeVFSCall template for exception handling
- Fix memory allocation across DLL boundaries
- Ensure proper lifetime management of ClientContext
- Add RAII wrappers for cross-boundary resources
```

## Phase 3: Thread Safety Improvements
**Priority**: 🟡 HIGH  
**Issues**: #13, #14, #15, #16, #49, #50, #51, #52  
**Files**: `src/storage/sqlite_transaction.cpp`, `src/sqlite_duckdb_vfs_cache.cpp`

### Changes:
1. **Replace global mutex with per-database pattern (#13, #14, #15)**:
   - Implement database-specific mutex registry
   - Use double-checked locking with atomics
   - Add proper memory ordering

2. **Fix thread safety issues (#49, #50, #51, #52)**:
   - Ensure sqlite3 handles not shared between threads
   - Add thread-local storage for SQLite contexts
   - Validate thread ownership in debug builds

### Verification:
```bash
# Run concurrent tests
python3 scripts/test_concurrent_access.py

# Thread sanitizer
make clean && make GEN=ninja debug TSAN=1
./build/debug/duckdb test/sql/scanner/http_sqlite_concurrent.test

# Stress test with multiple threads
./build/release/duckdb -unsigned -c "
LOAD 'build/release/extension/sqlite_scanner/sqlite_scanner.duckdb_extension';
CREATE TABLE t1 AS SELECT * FROM sqlite_scan('https://blobs.duckdb.org/databases/tpch_sf01.db', 'lineitem');
CREATE TABLE t2 AS SELECT * FROM sqlite_scan('https://blobs.duckdb.org/databases/tpch_sf01.db', 'orders');
"
```

### Commit Message:
```
Improve thread safety with per-database synchronization

- Replace global mutex with per-database pattern
- Add atomic operations for initialization flags
- Ensure SQLite handles are thread-local
- Fix double-checked locking pattern
```

## Phase 4: Error Handling & Diagnostics
**Priority**: 🟡 HIGH  
**Issues**: #16, #17, #18, #19, #20, #53, #54  
**Files**: `src/sqlite_db.cpp`, `src/sqlite_duckdb_vfs_cache.cpp`, tests

### Changes:
1. **Improve HTTP error messages (#16, #17)**:
   - Add HTTPException handling with status codes
   - Preserve original error context
   - Use DuckDB's error propagation

2. **Better diagnostics (#18, #19, #53, #54)**:
   - Add file path to error messages
   - Include operation context
   - Add debug logging option

### Verification:
```bash
# Test error cases
./build/release/duckdb -unsigned -c "
LOAD 'build/release/extension/sqlite_scanner/sqlite_scanner.duckdb_extension';
SELECT * FROM sqlite_scan('https://example.com/nonexistent.db', 'test');
"

# Check error messages are informative
./build/release/duckdb test/sql/scanner/http_sqlite_08_error_handling.test
```

### Commit Message:
```
Enhance error handling and diagnostics

- Add specific HTTP error context preservation
- Include file paths and operations in error messages
- Improve error propagation across C/C++ boundary
- Add debug logging capabilities
```

## Phase 5: Code Quality & Portability
**Priority**: 🟢 MEDIUM  
**Issues**: #21, #22, #23, #24, #25, #26, #27, #28, #29, #30, #31, #32, #33, #34, #35, #36, #37, #38, #39, #40, #41, #42, #44  
**Files**: Multiple files across the codebase

### Changes:
1. **Remove unnecessary Windows ifdefs (#21, #22, #23)**:
   - SQLite handles platform differences internally
   - Remove redundant conditionals

2. **Use DuckDB conventions (#24, #25, #26, #27, #28, #29)**:
   - Replace std::to_string with duckdb::to_string
   - Use NumericCast for type conversions
   - Replace std::min/max with DuckDB alternatives

3. **Fix type safety (#39, #40, #41, #42)**:
   - Add safe conversion helpers
   - Use consistent types across boundaries

### Verification:
```bash
# Cross-platform build test
make clean && make GEN=ninja release

# Static analysis
cppcheck --enable=all src/

# Compile with different standards
CXX_STANDARD=c++17 make GEN=ninja release
CXX_STANDARD=c++20 make GEN=ninja release
```

### Commit Message:
```
Improve code quality and portability

- Remove unnecessary Windows-specific code
- Adopt DuckDB coding conventions consistently
- Add type-safe conversions
- Clean up includes and dependencies
```

## Phase 6: Testing & Documentation
**Priority**: 🟢 MEDIUM  
**Issues**: #55, #56, #57, #58, #59, #60, #61, #62  
**Files**: Tests, documentation, examples

### Changes:
1. **Add comprehensive tests**:
   - Journal/WAL file detection tests
   - Concurrent access tests
   - Error handling tests
   - Performance benchmarks

2. **Update documentation**:
   - Document thread safety guarantees
   - Add usage examples
   - Document known limitations

### Verification:
```bash
# Run all tests
make test

# Check test coverage
make coverage

# Verify examples work
./scripts/run_examples.sh
```

### Commit Message:
```
Add comprehensive tests and documentation

- Add tests for journal/WAL detection
- Add concurrent access tests
- Document thread safety model
- Add usage examples
```

## Phase 7: Performance Optimization
**Priority**: 🟢 LOW  
**Issues**: Performance-related improvements  
**Files**: `src/sqlite_duckdb_vfs_cache.cpp`

### Changes:
1. **Optimize read-ahead**:
   - Tune read-ahead sizes
   - Add adaptive strategies
   - Cache frequently accessed blocks

2. **Reduce overhead**:
   - Minimize mutex contention
   - Optimize hot paths
   - Add performance counters

### Verification:
```bash
# Benchmark before/after
./scripts/benchmark_http_sqlite.py

# Profile hot paths
perf record ./build/release/duckdb benchmark.sql
perf report
```

### Commit Message:
```
Optimize HTTP SQLite performance

- Improve read-ahead strategies
- Reduce synchronization overhead
- Add performance monitoring
```

## Final Validation

After all phases complete:

```bash
# Full test suite
make test

# All sanitizers
make clean && make GEN=ninja debug ASAN=1
./build/debug/duckdb test/sql/scanner/http_sqlite_*.test

make clean && make GEN=ninja debug TSAN=1
./build/debug/duckdb test/sql/scanner/http_sqlite_*.test

make clean && make GEN=ninja debug UBSAN=1
./build/debug/duckdb test/sql/scanner/http_sqlite_*.test

# Memory leaks
valgrind --leak-check=full --show-leak-kinds=all ./build/release/duckdb test.sql

# Static analysis
cppcheck --enable=all --error-exitcode=1 src/
scan-build make GEN=ninja release

# Linting
python scripts/check_style.py
```

## Risk Mitigation

1. **Create feature branch**: `git checkout -b fix-http-sqlite-issues`
2. **Commit after each phase**: Allows easy rollback
3. **Run tests before committing**: Ensure nothing breaks
4. **Use draft PR**: Get early feedback
5. **Add [skip ci] to WIP commits**: Avoid CI spam

## Success Criteria

- [ ] All critical issues (🔴) fixed
- [ ] All tests pass
- [ ] No memory leaks (valgrind clean)
- [ ] No data races (TSAN clean)
- [ ] No undefined behavior (UBSAN clean)
- [ ] Performance maintained or improved
- [ ] Code follows DuckDB conventions
- [ ] PR reviewable in logical chunks