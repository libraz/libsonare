.PHONY: all build release test test-slow test-optional-fixtures test-librosa-live clean rebuild format lint wasm coverage \
       coverage-build coverage-clean build-shared build-node build-wasm-binding \
       test-python test-python-slow test-node test-wasm parity conformance abi-layout abi-layout-check check-abi-version ci-local \
       test-hardening test-hardening-asan test-hardening-tsan test-hardening-host test-hardening-wasm

BUILD_DIR := build
OPTIONAL_FIXTURE_BUILD_DIR := build-optional-fixtures
RYE ?= rye
CMAKE ?= cmake
HARDENING_JOBS ?= 2
UV_CACHE_DIR ?= $(CURDIR)/.uv-cache
PYTHON_PKG_DIR := bindings/python/src/libsonare
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SHARED_LIB := $(BUILD_DIR)/lib/libsonare.dylib
PYTHON_SHARED_LIB := $(PYTHON_PKG_DIR)/libsonare.dylib
HARDENING_ASAN_OPTIONS := strict_string_checks=1
else
SHARED_LIB := $(BUILD_DIR)/lib/libsonare.so
PYTHON_SHARED_LIB := $(PYTHON_PKG_DIR)/libsonare.so
HARDENING_ASAN_OPTIONS := detect_leaks=1:strict_string_checks=1
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
	rm -rf build build-*/ cmake-build-*/ build-*.log
	rm -rf bindings/node/build
	rm -rf bindings/wasm/build-wasm bindings/wasm/build-wasm-bench

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
	$(CMAKE) --build $(BUILD_DIR) --target sonare_shared -j
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

# Focused security-hardening gates. Each test command writes its complete log
# under the matching build directory, and --no-tests=error prevents a renamed
# or accidentally undiscovered regression test from passing silently.
test-hardening-asan:
	CC=clang CXX=clang++ $(CMAKE) -B build-hardening-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DBUILD_CLI=OFF -DSONARE_WITH_FFMPEG=OFF -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
	$(CMAKE) --build build-hardening-asan --target sonare_tests --parallel $(HARDENING_JOBS)
	ASAN_OPTIONS=$(HARDENING_ASAN_OPTIONS) UBSAN_OPTIONS=print_stacktrace=1 ctest --test-dir build-hardening-asan --output-on-failure --no-tests=error --output-log build-hardening-asan/test-hardening.log -R "public input corpus|set_markers rejects an invalid list|duplicate parameter rejection|offline results reject shapes|default Audio exposes a valid empty iterator"

test-hardening-tsan:
	CC=clang CXX=clang++ $(CMAKE) -B build-hardening-tsan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DBUILD_CLI=OFF -DSONARE_WITH_FFMPEG=OFF -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"
	$(CMAKE) --build build-hardening-tsan --target sonare_tests --parallel $(HARDENING_JOBS)
	TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-hardening-tsan --output-on-failure --no-tests=error --output-log build-hardening-tsan/test-hardening.log -R "^StreamAnalyzer publishes frames and stats to one concurrent consumer$$"

test-hardening-host:
ifeq ($(UNAME_S),Darwin)
	$(CMAKE) -B build-hardening-host -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DBUILD_CLI=OFF -DSONARE_WITH_FFMPEG=OFF -DBUILD_COREAUDIO=ON -DBUILD_COREMIDI=ON -DBUILD_AU_HOST=ON
	$(CMAKE) --build build-hardening-host --target sonare_tests --parallel $(HARDENING_JOBS)
	ctest --test-dir build-hardening-host --output-on-failure --no-tests=error --output-log build-hardening-host/test-hardening.log -R "CoreAudio oversize callback|CoreMIDI (input|output|scripted)|AU (effect factory|process paths)"
else
	@echo "test-hardening-host: skipped (Darwin only)"
endif

test-hardening-wasm: build-wasm-binding
	cd bindings/wasm && yarn vitest run tests/basic.test.ts tests/public-input-conformance.test.ts -t "processes realtime engine clips|keeps marker transactions conformant" --reporter=verbose > build-wasm/test-hardening.log

test-hardening: test-hardening-asan test-hardening-tsan test-hardening-host test-hardening-wasm

# Cross-binding parity gate (C API is canonical). Stdlib-only, no build needed:
# it reads the binding sources directly and exits non-zero on active drift.
parity: conformance
	python3 tools/parity/check_parity.py

# Shared public-input schema plus public streaming field/flag/default snapshot.
# Also gates request-object coverage: every one-shot facade export keeps a
# *Request overload, and every *Request a public function accepts stays exported
# from the package entry (both are invisible to the C-ABI parity checker).
conformance:
	python3 tools/conformance/check_public_contracts.py
	python3 tools/api/check_request_object_coverage.py

# Regenerate the authoritative C-ABI struct layout snapshot. Compiles a tiny
# probe (needs a C++ compiler, not a full build) that reports sizeof/alignof/
# offsetof straight from the headers. The JSON is tracked; the Python guard
# (tests/test_abi_layout.py) and the WASM abi-layout vitest compare against it.
abi-layout:
	$(RYE) run --pyproject bindings/python/pyproject.toml python tools/abi/gen_abi_layout.py

# Fail if the committed snapshot is stale (regenerate + git-diff style check).
abi-layout-check:
	$(RYE) run --pyproject bindings/python/pyproject.toml python tools/abi/gen_abi_layout.py --check

# Verify the ABI-version mirror literals in every binding match the C source of
# truth (per-subsystem + packed aggregate). Stdlib-only, read-only.
check-abi-version:
	python3 tools/abi/check_abi_versions.py

# Aggregate the fast, non-modifying mechanical gates so a pre-commit run can't
# silently skip one. Check-only (run `make format` first to auto-fix); excludes
# the heavy build + ctest (`make test`) by design. Ordered build-independent
# checks first, then the compiler-backed layout snapshot check (needs a C++
# compiler, not a full build).
ci-local:
	git ls-files -z -- '*.h' '*.hpp' '*.c' '*.cpp' ':!:third_party/**' | \
		xargs -0 clang-format --dry-run --Werror
	$(MAKE) lint
	$(MAKE) parity
	$(MAKE) check-abi-version
	$(MAKE) abi-layout-check

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
