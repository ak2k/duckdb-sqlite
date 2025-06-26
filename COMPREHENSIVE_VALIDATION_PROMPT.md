# Comprehensive Code Validation Prompt for C/C++ SQLite/DuckDB Extensions

Use this prompt to systematically validate C/C++ code that interfaces between SQLite and DuckDB, especially when implementing SQLite VFS or similar cross-language boundaries.

## The Prompt:

```
Please perform a comprehensive safety and quality review of this C/C++ codebase that interfaces between SQLite (C API) and DuckDB (C++ API). Check for ALL of the following categories of issues:

### 1. SQLite VFS Contract Compliance
- Does xRead zero-fill short reads and return SQLITE_IOERR_SHORT_READ?
- Does xAccess correctly handle journal/WAL file existence checks?
- Do all VFS methods return proper SQLite error codes (not throw exceptions)?
- Does the Read() method return actual bytes read?
- Are all required VFS methods implemented?
- Do file operations handle EOF correctly?

### 2. C/C++ Boundary Safety
- Are ALL VFS callbacks wrapped in try-catch blocks?
- Are there any C++ objects (unique_ptr, shared_ptr, STL containers) in C-visible structures?
- Are there any C++ destructors in structures passed to C code?
- Is memory allocation consistent (sqlite3_malloc with sqlite3_free, new with delete)?
- Are exceptions prevented from crossing extern "C" boundaries?
- Are calling conventions explicitly specified for callbacks?
- Is struct packing consistent across platforms?
- Are there any std::string or STL types in DLL-exported APIs?

### 3. Thread Safety
- Check for double-checked locking bugs (need memory barriers/atomics)
- Are mutexes scoped appropriately (per-database, not global)?
- Are all shared state accesses protected?
- Are initialization flags atomic?
- Is there proper synchronization for lazy initialization?
- Are thread-local storage assumptions valid across C/C++ boundaries?

### 4. Type Safety
- Are conversions between SQLite int and DuckDB idx_t explicit?
- Is there proper bounds checking for type conversions?
- Are file sizes consistently represented (sqlite3_int64)?
- Are arithmetic operations checked for overflow?
- Do format strings match their arguments?
- Are negative values properly handled when converting to unsigned?

### 5. Resource Management
- Is cleanup guaranteed in all error paths?
- Are VFS register/unregister calls properly paired?
- Are file handles closed on errors?
- Is RAII used correctly (and not across C boundaries)?
- Are circular references possible?
- Who owns pointers passed across boundaries?

### 6. Memory Allocation Boundaries
- Is memory allocated and freed in the same module?
- Are the correct allocators used (sqlite3_malloc vs new)?
- Are custom allocators consistent?
- Is memory allocated with C++ new/malloc freed with sqlite3_free?
- Are there cross-DLL heap issues on Windows?

### 7. DuckDB Lifetime Management
- Does ClientContext outlive all VFS operations using it?
- What happens if DuckDB shuts down while SQLite is using the VFS?
- Are there dangling pointers to DuckDB objects?
- Is the VFS properly cleaned up when ClientContext is destroyed?
- Are callbacks unregistered before their owners are destroyed?
- Do stored pointers have clear ownership?

### 8. Windows DLL Compatibility
- Are DLL export attributes correct (DUCKDB_EXTENSION_API)?
- Are calling conventions consistent (__cdecl, __stdcall)?
- Is struct packing specified consistently?
- Are there static variables that might have multiple instances?
- Are CRT (C Runtime) versions compatible?
- Do virtual function tables work across DLL boundaries?

### 9. Security Concerns
- Is user input validated before use?
- Are there integer overflows in size calculations?
- Is sensitive information (paths, addresses) exposed?
- Are URLs validated before making HTTP requests?
- Are there race conditions that could be exploited?
- Is there proper bounds checking?

### 10. Error Handling
- Are errors properly propagated across language boundaries?
- Is error context preserved (not just generic errors)?
- Are error messages informative without leaking sensitive data?
- Do all functions check return values?
- Are partial operations rolled back on failure?

### 11. Performance Considerations
- Are there unnecessary mutex locks?
- Is caching appropriate (not duplicating DuckDB's cache)?
- Are read-ahead sizes configurable?
- Are there unnecessary copies of large data?
- Is lazy initialization used appropriately?

### 12. Platform Compatibility
- Does the code work on 32-bit and 64-bit systems?
- Are there Windows-specific code paths that need testing?
- Are there assumptions about pointer sizes?
- Are there endianness assumptions?
- Are path separators handled correctly?

### 13. DuckDB Integration
- Does the code follow DuckDB's error handling patterns?
- Are DuckDB's configuration options respected?
- Is transaction isolation maintained?
- Are DuckDB's memory limits respected?
- Is the code using DuckDB's threading primitives appropriately?

### 14. Static Analysis Warnings
- Are there implicit conversions that could lose data?
- Are all headers that are used included?
- Are there unused variables or parameters?
- Are there unreachable code paths?
- Are switch statements exhaustive?

### 15. Testing Concerns
- Are there external dependencies in tests (URLs, network)?
- Are error paths tested?
- Is thread safety tested?
- Are platform-specific features tested?
- Are boundary conditions tested?

### 16. Advanced Threading Issues
- Are sqlite3* handles shared across threads without synchronization?
- Is there false sharing in frequently accessed structures?
- Are there lock-free programming attempts without proper memory ordering?
- Do detached threads access objects that might be destroyed?
- Are there thread joins in destructors (deadlock risk)?
- Is fork() called in multithreaded contexts?
- Are condition variables used correctly (spurious wakeups handled)?
- Is thread-local storage accessed across DLL boundaries?

### 17. Code Organization & Maintainability
- Are there circular dependencies between headers?
- Is implementation code in header files (except templates)?
- Are raw new/delete used instead of smart pointers?
- Is there missing const correctness?
- Are there public data members without accessors?
- Do virtual functions lack override specifiers?
- Are there hardcoded paths or magic numbers?
- Is error handling inconsistent (mixing codes and exceptions)?

### 18. API Design & Documentation
- Is ownership clearly documented for all pointers?
- Are preconditions/postconditions documented?
- Is thread safety documented for each class/function?
- Are there move constructors/operators where appropriate?
- Do classes follow Rule of 0/3/5?
- Are there unnecessary virtual functions?
- Is the API versioned for compatibility?
- Do comments explain WHY, not just WHAT?
- Are comments focused on helping future maintainers?
- Are there comments about "how we got here" instead of current state?
- Do comments document assumptions and invariants?
- Are error conditions and edge cases documented?
- Is complex logic explained with examples?

### 19. Performance & Cache Efficiency
- Are there unnecessary copies of large objects?
- Are structures laid out for cache efficiency?
- Are there virtual calls in tight loops?
- Is string concatenation done efficiently?
- Are vectors pre-reserved when size is known?
- Are there repeated allocations in hot paths?

### 20. Build System & Deployment
- Are symbols properly hidden/exported?
- Are there hardcoded compiler flags?
- Is there proper dependency management?
- Are debug symbols preserved appropriately?
- Are static/dynamic linking issues handled?
- Are there assumptions about install paths?

### 21. Pull Request Hygiene & Code Style
- Are there unused #includes that should be removed?
- Do #include statements follow DuckDB's ordering convention?
  * First: corresponding .hpp file (for .cpp files)
  * Second: other project headers
  * Third: DuckDB headers
  * Fourth: system/STL headers
  * Fifth: third-party headers
- Are changes minimal and focused on the PR's purpose?
- Are there unrelated formatting or refactoring changes?
- Do modified files follow existing code style?
- Are there changes to files outside the feature scope?
- Is the diff clean without whitespace-only changes?
- Are there commented-out code blocks that should be removed?
- Do all new files have proper license headers?

### 22. Import/Include Best Practices
- Are all #includes actually used in the file?
- Are forward declarations used where possible instead of includes?
- Are there missing includes (relying on transitive includes)?
- Are system headers included with <> not ""?
- Are includes sorted within each group?
- Are there circular include dependencies?
- Is there unnecessary inclusion of implementation files?

### 23. PR Scope Management
- Does the changeset only modify files necessary for the feature?
- Are there "while I'm here" fixes that should be separate PRs?
- Are test changes directly related to the implementation?
- Is documentation updated only where relevant?
- Are there bulk reformatting changes mixed with logic changes?
- Are there changes to build files that aren't required?
- Is the commit history clean and logical?

For each issue found, provide:
1. Issue description
2. File location and line numbers
3. Risk assessment (Critical/High/Medium/Low)
4. Specific code example
5. Recommended fix
6. Whether it's a new issue or pre-existing

Also check for these specific anti-patterns:
- std::to_string instead of DuckDB's to_string
- Missing #include <algorithm> when using std::min/max
- Global mutexes instead of per-database mutexes
- Throwing exceptions after C allocations
- Non-atomic flags for multi-threaded code
- Raw pointers without clear ownership
- Static initialization order dependencies
- volatile for thread synchronization (use atomic instead)
- catch(...) without rethrowing in destructors
- Thread creation in constructors
- Blocking operations without cancellation support
- Missing D_ASSERT for debug checks
- std::cout/printf instead of DuckDB logging
- Hardcoded buffer sizes instead of configurable options
- Missing move constructors for large objects
- Unused #includes cluttering files
- Wrong include order (not following project style)
- Out-of-scope changes in PR
- Mixed formatting and logic changes
- Circular include dependencies
- Including .cpp files instead of headers
- Missing includes (relying on transitive includes)
- "While I'm here" fixes that bloat the PR
- Comments about development history instead of current behavior
- Comments that just restate the code
- Missing documentation of assumptions
- Outdated TODOs without issue tracking
```

## How to Use This Prompt:

1. **For Initial Review**: Use the full prompt to check all categories
2. **For Focused Review**: Extract relevant sections (e.g., just "C/C++ Boundary Safety")
3. **For Regression Testing**: Check specific categories after making changes
4. **For New Code**: Focus on the patterns most relevant to the new functionality

## Additional Validation Commands:

```bash
# Find missing exception handling
grep -n "VFS::" *.cpp | grep -v "try"

# Find C++ objects in C structures  
grep -A5 "struct.*{" *.hpp | grep -E "(unique_ptr|shared_ptr|std::)"

# Find type mixing
grep -E "sqlite3_.*\(.*idx_t" *.cpp

# Find missing includes
grep -E "(std::max|std::min)" *.cpp | while read f; do grep -L "algorithm" "$f"; done

# Find static variables
grep "^static" *.cpp | grep -v "static_cast"

# Find potential race conditions
grep -E "(initialized|started|loaded)" *.cpp | grep -v -E "(atomic|mutex|lock)"

# Find threading issues
grep -E "sqlite3_.*\s*;" *.hpp  # SQLite handles in class members
grep "std::thread" *.cpp | grep -B5 -A5 "detach"
grep -E "lock.*lock|mutex.*mutex" *.cpp  # Multiple locks

# Find code quality issues
grep -E "\bnew\s+\w+\[" *.cpp  # Raw array allocation
grep -E "delete\s+\[?\]?" *.cpp  # Manual delete
grep -E "public:\s*\w+\s+\w+;" *.hpp  # Public data members

# Find missing documentation
grep -E "^\s*(class|struct)\s+\w+" *.hpp | grep -B1 -v "//"
grep -E "unique_ptr|shared_ptr" *.hpp | grep -v "owner"

# Check include hygiene
grep "^#include" *.cpp | grep -v "^#include \"" | head -20  # System includes first?
grep "^#include" *.cpp *.hpp | sort | uniq -c | sort -n  # Find duplicate includes

# Find unused includes (requires include-what-you-use or manual review)
for f in *.cpp; do echo "=== $f ==="; grep "^#include" "$f"; done

# Check for out-of-scope changes
git diff --name-only | grep -v -E "(sqlite|vfs|http)"  # Files unrelated to HTTP SQLite
git diff --stat | grep -E "^\s+[0-9]+\s+\+\s+[0-9]+\s+-"  # Large diffs

# Find whitespace-only changes
git diff --check  # Git's built-in whitespace checker
git diff -w --stat  # Show files with non-whitespace changes
```

## Critical Must-Fix Patterns:

1. **VFS callbacks without try-catch** → Undefined behavior
2. **C++ objects in C structs** → ABI incompatibility  
3. **Exceptions crossing extern "C"** → Crash
4. **Non-atomic lazy initialization** → Race conditions
5. **Global mutexes** → Performance bottlenecks
6. **Missing bounds checking** → Security vulnerabilities
7. **ClientContext lifetime issues** → Use-after-free
8. **SQLite handles shared across threads** → Data corruption
9. **Fork() in multithreaded program** → Deadlock/crash
10. **Memory ordering bugs** → Race conditions
11. **False sharing in hot structs** → Performance collapse
12. **Circular shared_ptr references** → Memory leaks
13. **Unused includes** → Compilation time & clarity
14. **Out-of-scope PR changes** → Review difficulty
15. **No retry logic** → Poor reliability
16. **Unbounded resource growth** → OOM/crashes
17. **Missing observability** → Hard to debug in production