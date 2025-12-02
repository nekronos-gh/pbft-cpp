BUILD_DIR ?= build
BUILD_TYPE ?= Debug

all: build

.PHONY: configure
configure:
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ..

.PHONY: build
build: configure
	cmake --build $(BUILD_DIR) -- -j$(shell nproc)

.PHONY: test
test: build
	cd $(BUILD_DIR)/test && ctest --output-on-failure

.PHONY: release
release:
	$(MAKE) BUILD_TYPE=Release clean build

.PHONY: install
install: release
	cd $(BUILD_DIR) && make install

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

.PHONY: compdb
compdb:
	$(MAKE) configure
	ln -sf $(BUILD_DIR)/compile_commands.json .
