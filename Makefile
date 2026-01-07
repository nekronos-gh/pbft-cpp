# Default Variables
BUILD_DIR_DEBUG   ?= build/debug
BUILD_DIR_RELEASE ?= build/release

# Flags for each configuration
DEBUG_FLAGS   := -DCMAKE_BUILD_TYPE=Debug -DPBFT_BUILD_EXAMPLES=ON -DPBFT_BUILD_TESTS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
RELEASE_FLAGS := -DCMAKE_BUILD_TYPE=Release -DPBFT_BUILD_EXAMPLES=OFF -DPBFT_BUILD_TESTS=OFF

# Default target
all: build-debug

.PHONY: help
help: ## Show this help message
	@echo "Available targets:"
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2}'

# ==============================================================================
# Development Workflow
# ==============================================================================
.PHONY: configure-debug
configure-debug: ## Configure Debug build 
	mkdir -p $(BUILD_DIR_DEBUG)
	cd $(BUILD_DIR_DEBUG) && cmake $(DEBUG_FLAGS) ../..

.PHONY: build-debug
build-debug: configure-debug ## Build Debug version
	cmake --build $(BUILD_DIR_DEBUG) -- -j$(shell nproc)

.PHONY: unit-test
unit-test: build-debug ## Run unit tests
	cd $(BUILD_DIR_DEBUG)/test && ctest --output-on-failure

.PHONY: integration-test
integration-test: build-debug ## Run integration tests
	rm -rf $(BUILD_DIR_DEBUG)/test/logs/
	- cd $(BUILD_DIR_DEBUG)/test && ./integration_test
	cd $(BUILD_DIR_DEBUG)/test/logs/ && cat node-*.log | sort > all-nodes.log

.PHONY: test
test: unit-test integration-test

# ==============================================================================
# Release / Install Workflow
# ==============================================================================
.PHONY: configure-release
configure-release: ## Configure Release build 
	mkdir -p $(BUILD_DIR_RELEASE)
	cd $(BUILD_DIR_RELEASE) && cmake $(RELEASE_FLAGS) ../..

.PHONY: build-release
build-release: configure-release ## Build Release version
	cmake --build $(BUILD_DIR_RELEASE) -- -j$(shell nproc)

.PHONY: install
install: build-release ## Install Release version 
	cd $(BUILD_DIR_RELEASE) && make install

# ==============================================================================
# Utilities
# ==============================================================================
.PHONY: clean
clean: ## Clean all build directories
	rm -rf build

.PHONY: compdb
compdb: configure-debug ## Generate compile_commands.json 
	ln -sf $(BUILD_DIR_DEBUG)/compile_commands.json .
