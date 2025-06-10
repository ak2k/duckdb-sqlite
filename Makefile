PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=sqlite_scanner
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile


# Setup the sqlite3 tpch database
data/db/tpch.db: release
	command -v sqlite3 || (command -v brew && brew install sqlite) || (command -v choco && choco install sqlite -y) || (command -v apt-get && apt-get install -y sqlite3) || (command -v apk && apk add sqlite) || echo "no sqlite3"
	./build/release/$(DUCKDB_PATH) < data/sql/tpch-export.duckdb || tree ./build/release || echo "neither tree not duck"
	sqlite3 data/db/tpch.db < data/sql/tpch-create.sqlite

# Override the test target implementations from the duckdb_extension.Makefile
test_release_internal: data/db/tpch.db
	SQLITE_TPCH_GENERATED=1 ./build/release/$(TEST_PATH) "$(PROJ_DIR)test/*"

test_debug_internal: data/db/tpch.db
	SQLITE_TPCH_GENERATED=1 ./build/debug/$(TEST_PATH) "$(PROJ_DIR)test/*"

test_reldebug_internal: data/db/tpch.db
	SQLITE_TPCH_GENERATED=1 ./build/reldebug/$(TEST_PATH) "$(PROJ_DIR)test/*"

# Tidy check target for static analysis
.PHONY: tidy-check
tidy-check: release
	@command -v clang-tidy >/dev/null 2>&1 || { \
		echo "Error: clang-tidy is not installed."; \
		exit 1; \
	}
	@echo "Running clang-tidy on source files..."
	@find src/ -name '*.cpp' -not -path 'src/sqlite/*' | while read file; do \
		echo "Checking $$file..."; \
		clang-tidy $$file \
			-p build/release \
			--header-filter='$(PROJ_DIR)src/include/.*' \
			--config-file=.clang-tidy \
			|| exit 1; \
	done
	@echo "Clang-tidy check completed successfully!"

# Tidy fix target to automatically fix issues
.PHONY: tidy-fix
tidy-fix: release
	@command -v clang-tidy >/dev/null 2>&1 || { \
		echo "Error: clang-tidy is not installed."; \
		exit 1; \
	}
	@echo "Running clang-tidy with fixes on source files..."
	@find src/ -name '*.cpp' -not -path 'src/sqlite/*' | while read file; do \
		echo "Fixing $$file..."; \
		clang-tidy $$file \
			-p build/release \
			--header-filter='$(PROJ_DIR)src/include/.*' \
			--config-file=.clang-tidy \
			--fix \
			--fix-errors; \
	done
	@echo "Clang-tidy fixes applied!"
