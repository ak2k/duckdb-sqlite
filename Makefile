PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=sqlite_scanner
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile


# Setup the sqlite3 tpch database
data/db/tpch.db: release
	@if command -v sqlite3 >/dev/null 2>&1; then \
		echo "sqlite3 already installed"; \
	elif [ -n "$$CI" ]; then \
		echo "Running in CI environment"; \
		if command -v yum >/dev/null 2>&1; then \
			echo "Installing sqlite via yum"; \
			yum install -y sqlite || echo "Failed to install sqlite via yum"; \
		elif command -v apt-get >/dev/null 2>&1; then \
			echo "Installing sqlite3 via apt-get"; \
			apt-get update && apt-get install -y sqlite3 || echo "Failed to install sqlite3 via apt-get"; \
		elif command -v apk >/dev/null 2>&1; then \
			echo "Installing sqlite via apk"; \
			apk add sqlite || echo "Failed to install sqlite via apk"; \
		else \
			echo "WARNING: No supported package manager found in CI"; \
		fi \
	elif command -v brew >/dev/null 2>&1; then \
		brew install sqlite; \
	elif command -v choco >/dev/null 2>&1; then \
		choco install sqlite -y; \
	elif command -v apt-get >/dev/null 2>&1; then \
		sudo apt-get install -y sqlite3; \
	elif command -v yum >/dev/null 2>&1; then \
		sudo yum install -y sqlite; \
	elif command -v apk >/dev/null 2>&1; then \
		apk add sqlite; \
	else \
		echo "WARNING: sqlite3 not found and no package manager available to install it"; \
	fi
	./build/release/$(DUCKDB_PATH) < data/sql/tpch-export.duckdb || tree ./build/release || echo "neither tree not duck"
	sqlite3 data/db/tpch.db < data/sql/tpch-create.sqlite

# Override the test target implementations from the duckdb_extension.Makefile
test_release_internal: data/db/tpch.db
	SQLITE_TPCH_GENERATED=1 ./build/release/$(TEST_PATH) "$(PROJ_DIR)test/*"

test_debug_internal: data/db/tpch.db
	SQLITE_TPCH_GENERATED=1 ./build/debug/$(TEST_PATH) "$(PROJ_DIR)test/*"

test_reldebug_internal: data/db/tpch.db
	SQLITE_TPCH_GENERATED=1 ./build/reldebug/$(TEST_PATH) "$(PROJ_DIR)test/*"
