# Comprehensive Code Validation Prompt for C/C++ SQLite/DuckDB Extensions (v2)

A structured approach to reviewing C/C++ code that interfaces between SQLite and DuckDB.

## Quick Safety Checklist (5 min scan)

```
□ All VFS callbacks have try-catch blocks
□ No C++ objects (unique_ptr, STL) in C structures  
□ No exceptions can cross extern "C" boundaries
□ SQLite handles not shared between threads
□ Memory allocated/freed with matching functions
□ All error codes checked and handled
□ Resources cleaned up in error paths
□ PR changes focused on stated objective
```

## The Review Prompt

```
Please perform a comprehensive safety and quality review of this C/C++ codebase. 
Report findings in this format:

SUMMARY:
- Risk Level: [Critical/High/Medium/Low]
- Must Fix: [count] issues
- Should Fix: [count] issues  
- Consider: [count] suggestions

CRITICAL ISSUES (blocks merge):
[List with file:line and specific fix needed]

HIGH PRIORITY (should fix before merge):
[List with file:line and recommended fix]

SUGGESTIONS (optional improvements):
[List with brief description]
```

## 🔴 CRITICAL - Must Fix (Data Loss/Crash/Security Risk)

### 1. Memory & Resource Safety
- **Memory leaks**: Unmatched allocations, missing cleanup
- **Use-after-free**: Dangling pointers, deleted objects still referenced
- **Buffer overflows**: Unchecked array/string operations
- **Resource leaks**: Files, sockets, handles not closed on all paths
- **Wrong deallocator**: malloc→delete, new→free, sqlite3_malloc→delete
- **RAII violations**: C++ objects in C structures, destructors in C-visible structs

### 2. C/C++ Boundary Violations  
- **Exceptions crossing extern "C"**: Undefined behavior, crashes
- **C++ objects in C structures**: unique_ptr/STL in sqlite callbacks
- **ABI incompatibility**: Different calling conventions, struct packing
- **Module boundary issues**: Objects deleted in different DLL than created

### 3. Thread Safety & Concurrency
- **Data races**: Shared state without synchronization
- **SQLite handle sharing**: sqlite3* used by multiple threads
- **Non-atomic flags**: Race conditions in initialization
- **Deadlocks**: Lock ordering issues, recursive locks
- **Memory ordering bugs**: Missing barriers, incorrect atomics

### 4. Security Vulnerabilities
- **SQL injection**: Unsanitized user input in queries
- **Integer overflow**: Size calculations that can wrap
- **Path traversal**: Unchecked file paths from user input
- **Information disclosure**: Exposing memory addresses, internal paths
- **Unvalidated input**: Missing bounds checks, format string bugs

### 5. Data Integrity
- **VFS contract violations**: xRead not zero-filling, wrong error codes
- **Transaction violations**: Operations outside transaction boundaries
- **Corrupted state**: Partial updates not rolled back
- **Missing validation**: SQLite file headers, checksums not verified

## 🟡 HIGH - Should Fix (Reliability/Maintainability Risk)

### 6. Error Handling & Recovery
- **Missing error checks**: Ignoring return codes, unchecked allocations
- **Lost error context**: Generic errors without specific information  
- **No retry logic**: Transient failures cause permanent errors
- **Incomplete rollback**: Partial operations not cleaned up
- **Silent failures**: Errors swallowed without logging

### 7. Resource Management
- **Unbounded growth**: Caches, connection pools without limits
- **Missing timeouts**: Blocking operations that can hang forever
- **No backpressure**: Can overwhelm remote systems
- **Lifecycle issues**: Objects outliving their dependencies
- **Circular references**: Memory leaks from reference cycles

### 8. API & Integration Issues
- **DuckDB convention violations**: Not using DuckDB error types, logging
- **SQLite misuse**: Incorrect VFS implementation, API assumptions
- **Type mismatches**: idx_t vs int without proper conversion
- **Missing features**: Required callbacks not implemented

## 🟢 MEDIUM - Consider Fixing (Code Quality)

### 9. Performance & Efficiency
- **Unnecessary copies**: Large objects passed by value
- **False sharing**: Hot data on same cache line
- **Missing optimizations**: No move semantics, repeated allocations
- **Algorithm complexity**: O(n²) where O(n) possible
- **Cache unfriendly**: Poor data layout, random access patterns

### 10. Code Organization & Style
- **Include hygiene**: Wrong order, unused includes, transitive dependencies
- **Large functions**: Methods over 50 lines, deep nesting
- **Code duplication**: Copy-pasted error handling, conversions
- **Inconsistent style**: Mixing naming conventions, formatting
- **Dead code**: Commented out blocks, unused functions

### 11. Documentation & Maintainability
- **Missing ownership docs**: Unclear who frees pointers
- **No thread safety docs**: Unclear which APIs are thread-safe
- **Explains what not why**: Comments just restate code
- **Outdated comments**: Documentation doesn't match implementation
- **Missing examples**: Complex APIs without usage examples

## ✅ PR Hygiene Checklist

### 12. Scope Management
```
□ Changes limited to feature/bugfix scope
□ No unrelated refactoring ("while I'm here")  
□ Test changes directly support implementation
□ No bulk reformatting mixed with logic
□ Clean commit history with clear messages
□ No changes to unrelated subsystems
```

### 13. Quality Gates
```
□ All tests pass (including new tests for changes)
□ No compiler warnings introduced
□ Memory sanitizers clean (ASAN, TSAN)
□ Code coverage maintained or improved
□ Performance benchmarks show no regression
□ Documentation updated for API changes
```

## Validation Commands

```bash
# Critical safety checks
grep -r "catch.*\.\.\." .  # Generic catch blocks losing context
grep -r "unique_ptr.*sqlite" .  # C++ objects in C interfaces
grep -r "sqlite3_.*shared" .  # SQLite handle sharing
grep -r "throw.*extern.*C" .  # Exceptions across boundaries

# Memory and resource checks  
valgrind --leak-check=full ./test
clang++ -fsanitize=address,undefined -g test.cpp
grep -r "new\s\+[A-Z]" . | grep -v "make_uniq"  # Raw new usage

# Thread safety
clang++ -fsanitize=thread -g test.cpp
grep -r "static.*sqlite3" .  # Shared SQLite handles
grep -r "mutex.*mutex" .  # Multiple mutex acquisition

# PR scope validation
git diff --name-only | grep -v "sqlite"  # Out of scope files
git diff --stat  # Large diffs suggesting scope creep
```

## Review Priority Guide

1. **Start with CRITICAL** - Any single issue blocks merge
2. **Then HIGH** - Multiple issues may block, single issues need justification  
3. **Then MEDIUM** - Note for future cleanup, don't block merge
4. **Check PR HYGIENE** - Ensure reviewability and maintainability

## Common Anti-Patterns Reference

### Memory/Resource Anti-patterns
- `catch(...)` without rethrowing in destructors
- Mixing allocators (new/free, malloc/delete)
- RAII objects in extern "C" functions
- Missing nullptr checks before dereference

### Threading Anti-patterns
- `volatile` for synchronization (use atomic)
- Double-checked locking without memory barriers
- Shared sqlite3 handles without mutexes
- Thread creation in constructors

### C/C++ Boundary Anti-patterns
- std::string in DLL-exported APIs
- Template instantiation across modules
- C++ exceptions in C callbacks
- Virtual functions in C-visible structures

### Code Quality Anti-patterns
- Magic numbers without constants
- Copy-paste error handling
- Long functions with multiple responsibilities
- Missing error checks on external APIs