.PHONY: all build release test test-slow test-optional-fixtures test-librosa-live clean rebuild format lint wasm coverage \
       coverage-build coverage-clean build-shared build-node build-wasm-binding \
       test-python test-python-slow test-node test-wasm parity

BUILD_DIR := build
OPTIONAL_FIXTURE_BUILD_DIR := build-optional-fixtures
RYE ?= rye
CMAKE ?= cmake
UV_CACHE_DIR ?= $(CURDIR)/.uv-cache
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

# Heavy cases (>~2 s each) are tagged [.][slow] and hidden from the default
# ctest run; this runs just those. Must run from the repo root (librosa
# fixtures load by relative path).
test-slow: build
	./$(BUILD_DIR)/bin/sonare_tests "[slow]"

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

# `format` applies every auto-fixable change the CI lint gate checks, then runs
# `lint` to verify. The binding steps use `lint:fix` (biome check --write), not
# `yarn format` (biome format --write): the former also applies import
# organization and the safe lint fixes that `yarn lint` (biome check) enforces in
# CI, so `make format` can no longer succeed while CI lint would fail. Anything
# left (e.g. unused imports, an unsafe fix biome will not auto-apply) surfaces in
# the final `lint` step for manual resolution.
format:
	git ls-files -z --cached --others --exclude-standard -- '*.h' '*.hpp' '*.c' '*.cpp' ':!:third_party/**' | python3 -c 'import os, sys; paths = [p for p in sys.stdin.buffer.read().split(b"\0") if p and os.path.exists(os.fsdecode(p))]; sys.stdout.buffer.write(b"\0".join(paths) + (b"\0" if paths else b""))' | xargs -0 clang-format -i
	cd bindings/wasm && yarn lint:fix
	cd bindings/node && yarn lint:fix
	UV_CACHE_DIR=$(UV_CACHE_DIR) $(RYE) sync --pyproject bindings/python/pyproject.toml
	UV_CACHE_DIR=$(UV_CACHE_DIR) $(RYE) run --pyproject bindings/python/pyproject.toml ruff format bindings/python/src bindings/python/tests
	UV_CACHE_DIR=$(UV_CACHE_DIR) $(RYE) run --pyproject bindings/python/pyproject.toml ruff check --fix bindings/python/src bindings/python/tests
	$(MAKE) lint

lint:
	cd bindings/wasm && yarn lint
	cd bindings/node && yarn lint
	UV_CACHE_DIR=$(UV_CACHE_DIR) $(RYE) sync --pyproject bindings/python/pyproject.toml
	UV_CACHE_DIR=$(UV_CACHE_DIR) $(RYE) run --pyproject bindings/python/pyproject.toml ruff check bindings/python/src bindings/python/tests

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

# Tests marked @pytest.mark.slow are excluded by the default addopts
# (-m "not slow"); the explicit -m here overrides that and runs just them.
test-python-slow: build-shared
	$(RYE) sync --pyproject bindings/python/pyproject.toml
	$(RYE) run --pyproject bindings/python/pyproject.toml python -m pytest bindings/python/tests/ -v -m slow

test-node: build-node
	cd bindings/node && yarn test

test-wasm: build-wasm-binding
	cd bindings/wasm && yarn test

# Cross-binding parity gate (C API is canonical). Stdlib-only, no build needed:
# it reads the binding sources directly and exits non-zero on active drift.
parity:
	python3 tools/parity/check_parity.py

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
