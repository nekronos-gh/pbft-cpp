BUILD_DIR ?= build
BUILD_TYPE ?= Debug

all: build


.PHONY: help
help: ## Show this help message
	@echo "Available targets:"
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2}'

.PHONY: configure
configure: ## Configure CMake build system
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ..

.PHONY: build ## Build the library
build: configure
	cmake --build $(BUILD_DIR) -- -j$(shell nproc)

.PHONY: unit-test
unit-test: build ## Run unit tests
	cd $(BUILD_DIR)/test && ctest --output-on-failure

.PHONY: integration-test
integration-test: build ## Run integration tests
	rm -rf $(BUILD_DIR)/test/logs/ 
	- cd $(BUILD_DIR)/test && ./integration_test
	cd $(BUILD_DIR)/test/logs/ && cat node-*.log | sort > all-nodes.log

.PHONY: test ## Run all tests
test: unit-test integration-test

.PHONY: release
release: ## Build release version
	$(MAKE) BUILD_TYPE=Release clean build

.PHONY: install
install: release ## Install release build
	cd $(BUILD_DIR) && make install

.PHONY: clean
clean: ## Clean build files
	rm -rf $(BUILD_DIR)

.PHONY: compdb
compdb: ## Generate compile commands
	$(MAKE) configure
	ln -sf $(BUILD_DIR)/compile_commands.json .
