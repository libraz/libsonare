.PHONY: all build release test test-optional-fixtures test-librosa-live clean rebuild format lint wasm coverage \
       coverage-build coverage-clean build-shared build-node build-wasm-binding \
       test-python test-node test-wasm

BUILD_DIR := build
OPTIONAL_FIXTURE_BUILD_DIR := build-optional-fixtures
RYE ?= rye
CMAKE ?= cmake
PYTHON_PKG_DIR := bindings/python/src/libsonare
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SHARED_LIB := $(BUILD_DIR)/lib/libsonare.dylib
PYTHON_SHARED_LIB := $(PYTHON_PKG_DIR)/libsonare.dylib
else
SHARED_LIB := $(BUILD_DIR)/lib/libsonare.so
PYTHON_SHARED_LIB := $(PYTHON_PKG_DIR)/libsonare.so
endif

all: build

build:
	$(CMAKE) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug
	$(CMAKE) --build $(BUILD_DIR) -j

release:
	$(CMAKE) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build $(BUILD_DIR) -j

wasm:
	emcmake $(CMAKE) -B build-wasm -DBUILD_WASM=ON -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build build-wasm -j
	cd bindings/wasm && yarn build:js

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure --parallel

test-optional-fixtures:
	$(CMAKE) -B $(OPTIONAL_FIXTURE_BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug -DSONARE_ENABLE_OPTIONAL_FIXTURE_TESTS=ON
	$(CMAKE) --build $(OPTIONAL_FIXTURE_BUILD_DIR) -j
	ctest --test-dir $(OPTIONAL_FIXTURE_BUILD_DIR) --output-on-failure -R "optional|fixture|EBU R128" --parallel

test-librosa-live: build
	$(RYE) sync --pyproject tests/librosa/pyproject.toml
	$(RYE) run --pyproject tests/librosa/pyproject.toml python -m ensurepip --upgrade
	$(RYE) run --pyproject tests/librosa/pyproject.toml python -m pip install --no-build-isolation ../librosa
	$(RYE) run --pyproject tests/librosa/pyproject.toml python tests/librosa/run_live_reference_check.py --build-dir $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean build

format:
	find src tests bindings/node/src bindings/node/tests \( -name '*.cpp' -o -name '*.h' \) | xargs clang-format -i
	cd bindings/wasm && yarn format
	cd bindings/node && yarn format
	$(RYE) sync --pyproject bindings/python/pyproject.toml
	$(RYE) run --pyproject bindings/python/pyproject.toml ruff format bindings/python/src bindings/python/tests
	$(RYE) run --pyproject bindings/python/pyproject.toml ruff check --fix bindings/python/src bindings/python/tests

lint:
	cd bindings/wasm && yarn lint
	cd bindings/node && yarn lint
	$(RYE) sync --pyproject bindings/python/pyproject.toml
	$(RYE) run --pyproject bindings/python/pyproject.toml ruff check bindings/python/src bindings/python/tests

# Binding targets
build-shared:
	$(CMAKE) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED=ON
	$(CMAKE) --build $(BUILD_DIR) -j
	cp -L $(SHARED_LIB) $(PYTHON_SHARED_LIB)
ifeq ($(UNAME_S),Darwin)
	-install_name_tool -id @loader_path/libsonare.dylib $(PYTHON_SHARED_LIB)
endif

build-node:
	cd bindings/node && yarn install && yarn build

build-wasm-binding:
	cd bindings/wasm && yarn install --immutable && yarn build

test-python: build-shared
	$(RYE) sync --pyproject bindings/python/pyproject.toml
	$(RYE) run --pyproject bindings/python/pyproject.toml python -m pytest bindings/python/tests/ -v

test-node: build-node
	cd bindings/node && yarn test

test-wasm: build-wasm-binding
	cd bindings/wasm && yarn test

# Coverage targets
coverage-build:
	$(CMAKE) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON
	$(CMAKE) --build $(BUILD_DIR) -j

coverage: coverage-build
	@mkdir -p $(BUILD_DIR)/coverage
	cd $(BUILD_DIR) && lcov --directory . --zerocounters
	-cd $(BUILD_DIR) && ctest --output-on-failure --parallel
	cd $(BUILD_DIR) && lcov --directory . --capture --output-file coverage/coverage.info
	cd $(BUILD_DIR) && lcov --extract coverage/coverage.info '$(CURDIR)/src/*' --output-file coverage/coverage_filtered.info
	cd $(BUILD_DIR) && genhtml coverage/coverage_filtered.info --output-directory coverage/html
	@echo "Coverage report: $(BUILD_DIR)/coverage/html/index.html"

coverage-clean:
	find $(BUILD_DIR) -name '*.gcda' -delete 2>/dev/null || true
	rm -rf $(BUILD_DIR)/coverage
