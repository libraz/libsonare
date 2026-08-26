.PHONY: all build fixtures release install test-install test test-slow test-golden test-optional-fixtures test-librosa-live clean rebuild format format-check lint wasm coverage \
       coverage-build coverage-clean build-shared build-node build-wasm-binding \
       test-python test-python-slow test-node test-wasm parity conformance test-gm-cross-surface test-mix-assistant-cross-surface abi-layout abi-layout-check check-abi-version \
       capability-catalog capability-catalog-check processor-types processor-types-check ci-local \
       build-bank-shared bank-versions bank-versions-check \
       surface-coverage surface-coverage-check \
       test-hardening test-hardening-asan test-hardening-tsan test-hardening-host test-hardening-wasm \
       build-feature-matrix accuracy-report voice-gate voice-status test-voicematch

BUILD_DIR := build
OPTIONAL_FIXTURE_BUILD_DIR := build-optional-fixtures
ACCURACY_REPORT_JSON ?= $(CURDIR)/build-optional-fixtures/accuracy-report.json
INSTALL_PREFIX_DIR := $(CURDIR)/build-install-prefix
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

# Install the C++ library, its headers, the CMake package files and the native
# CLI under CMAKE_INSTALL_PREFIX (/usr/local by default; override with
# `make install CMAKE_INSTALL_PREFIX=~/.local`). Retargets build/ to Release,
# same as `make release`.
install: release
	$(CMAKE) --install $(BUILD_DIR)

# Gate for the installed package. Installs into a scratch prefix, then
# configures a consumer project that knows nothing but find_package(sonare) and
# builds it. This is the only check that can see the defects an in-tree build
# hides: a source-tree path leaking into an exported target, a header the
# install rules miss, an archive whose declared link interface is incomplete.
# BUILD_SHARED is on so the run also covers the shared artifact and sonare.pc.
# The build tree is reused between runs; the prefix and the consumer tree are
# rebuilt from scratch so a removed file cannot survive as a stale copy.
test-install:
	rm -rf $(INSTALL_PREFIX_DIR) build-install-consumer
	$(CMAKE) -B build-install -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
	  -DBUILD_SHARED=ON -DCMAKE_INSTALL_PREFIX=$(INSTALL_PREFIX_DIR)
	$(CMAKE) --build build-install -j
	$(CMAKE) --install build-install
	$(CMAKE) -S tests/cmake/consumer -B build-install-consumer \
	  -DCMAKE_PREFIX_PATH=$(INSTALL_PREFIX_DIR)
	$(CMAKE) --build build-install-consumer -j
	ctest --test-dir build-install-consumer --output-on-failure --no-tests=error

wasm:
	emcmake $(CMAKE) -B build-wasm -DBUILD_WASM=ON -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build build-wasm -j
	cd bindings/wasm && yarn build:js

# The K-weighting cases compare against a reference this script computes, and
# the file it writes is gitignored, so a fresh checkout has no copy of it. The
# cases fail rather than skip when it is absent, so every target that runs them
# generates it first and CI invokes this same target rather than repeating the
# command.
fixtures:
	python3 tools/scripts/k_weighting_reference.py

test: build fixtures
	ctest --test-dir $(BUILD_DIR) --output-on-failure --parallel
	$(MAKE) test-voicematch

# The calibration harness is Python, and nothing else runs it: `tools/` is
# outside ctest, outside the drift gates, and outside CI, so a harness left to
# be remembered is a harness that rots. It rides on `test` rather than standing
# alone for that reason. The cases read only the tracked capture definitions
# and reference profiles -- no rendered corpus, no plugin, no built library --
# so they pass on a fresh clone in about half a minute.
test-voicematch:
	$(RYE) run --pyproject bindings/python/pyproject.toml python -m pytest tools/voicematch -q

# Heavy cases (>~2 s each) are tagged [.][slow] and hidden from the default
# ctest run; this runs just those. Must run from the repo root (librosa
# fixtures load by relative path).
test-slow: build fixtures
	./$(BUILD_DIR)/bin/sonare_tests "[slow]"

# Golden regressions are hidden from the default Catch2 run so they can be
# invoked explicitly in local development and CI.
test-golden: build fixtures
	./$(BUILD_DIR)/bin/sonare_tests "[golden]"

test-optional-fixtures:
	$(CMAKE) -B $(OPTIONAL_FIXTURE_BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug -DSONARE_ENABLE_OPTIONAL_FIXTURE_TESTS=ON
	$(CMAKE) --build $(OPTIONAL_FIXTURE_BUILD_DIR) -j
	ctest --test-dir $(OPTIONAL_FIXTURE_BUILD_DIR) --output-on-failure -R "optional|fixture|EBU R128" --parallel

# Measures musical accuracy against whatever corpus the music_eval manifests
# point at and rolls it up into a publishable table. Distinct from
# test-optional-fixtures, which gates: only report_only manifest rows produce
# the observations this reads. Reports "unmeasured" rather than a score for a
# dimension with no rows. See tools/eval/README.md.
accuracy-report:
	$(CMAKE) -B $(OPTIONAL_FIXTURE_BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DSONARE_ENABLE_OPTIONAL_FIXTURE_TESTS=ON
	$(CMAKE) --build $(OPTIONAL_FIXTURE_BUILD_DIR) -j
	python3 tests/fixtures/run_optional_fixture_report.py --suite music \
	        --sonare-tests $(OPTIONAL_FIXTURE_BUILD_DIR)/bin/sonare_tests \
	        --output $(ACCURACY_REPORT_JSON)
	python3 tools/eval/summarize_accuracy.py $(ACCURACY_REPORT_JSON) --markdown

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
	git ls-files -z --cached --others --exclude-standard -- '*.h' '*.hpp' '*.c' '*.cpp' '*.mm' ':!:third_party/**' | python3 -c 'import os, sys; paths = [p for p in sys.stdin.buffer.read().split(b"\0") if p and os.path.exists(os.fsdecode(p))]; sys.stdout.buffer.write(b"\0".join(paths) + (b"\0" if paths else b""))' | xargs -0 clang-format -i
	cd bindings/wasm && yarn lint:fix
	cd bindings/node && yarn lint:fix
	UV_CACHE_DIR=$(UV_CACHE_DIR) $(RYE) sync --pyproject bindings/python/pyproject.toml
	UV_CACHE_DIR=$(UV_CACHE_DIR) $(RYE) run --pyproject bindings/python/pyproject.toml ruff format bindings/python/src bindings/python/tests
	UV_CACHE_DIR=$(UV_CACHE_DIR) $(RYE) run --pyproject bindings/python/pyproject.toml ruff check --fix bindings/python/src bindings/python/tests
	$(MAKE) lint

# `test:types` type-checks the Node binding's tests against src (biome does not
# type-check, and the build tsconfig excludes tests). It reads sources only —
# no built addon or dist/ needed — so it belongs with the other static gates.
lint:
	cd bindings/wasm && yarn lint
	cd bindings/node && yarn lint
	cd bindings/node && yarn test:types
	UV_CACHE_DIR=$(UV_CACHE_DIR) $(RYE) sync --pyproject bindings/python/pyproject.toml
	UV_CACHE_DIR=$(UV_CACHE_DIR) $(RYE) run --pyproject bindings/python/pyproject.toml ruff check bindings/python/src bindings/python/tests

format-check:
	git ls-files -z -- '*.h' '*.hpp' '*.c' '*.cpp' '*.mm' ':!:third_party/**' | xargs -0 clang-format --dry-run --Werror
	$(MAKE) lint

# Binding targets
build-shared:
	$(CMAKE) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED=ON
	$(CMAKE) --build $(BUILD_DIR) --target sonare_shared -j
	cp -L $(SHARED_LIB) $(PYTHON_SHARED_LIB)
ifeq ($(UNAME_S),Darwin)
	-install_name_tool -id @loader_path/libsonare.dylib $(PYTHON_SHARED_LIB)
endif

# Regenerate the checked-in runtime processor and preset catalog from the C
# ABI. The check variant leaves the worktree untouched and fails on drift.
capability-catalog: build-shared
	python3 tools/generate_capability_catalog.py --library $(SHARED_LIB)

capability-catalog-check: build-shared
	python3 tools/generate_capability_catalog.py --library $(SHARED_LIB) --check

# The instrument bank's own version registry: one generation per voice, per drum
# note and per group of shared calibration constants (the engines, the GS effect
# scales, the fallback send weights). Read from the library's own knob dump, so
# it cannot drift from what the render uses -- which needs a BUILD_TUNING build,
# in its own directory, since it neither retargets the Debug `build/` that ctest
# reads nor disturbs the shared `build-python-shared/`.
BANK_BUILD_DIR ?= build-tuning
ifeq ($(UNAME_S),Darwin)
BANK_SHARED_LIB := $(CURDIR)/$(BANK_BUILD_DIR)/lib/libsonare.dylib
else
BANK_SHARED_LIB := $(CURDIR)/$(BANK_BUILD_DIR)/lib/libsonare.so
endif

build-bank-shared:
	$(CMAKE) -S . -B $(BANK_BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED=ON -DBUILD_TUNING=ON
	$(CMAKE) --build $(BANK_BUILD_DIR) --target sonare_shared -j

# NOTE is what the bump is recorded as; a run without one records "unrecorded",
# which the version can never recover.
bank-versions: build-bank-shared
	$(RYE) run --pyproject bindings/python/pyproject.toml python tools/generate_bank_versions.py \
		--library $(BANK_SHARED_LIB) --note "$(NOTE)"

bank-versions-check: build-bank-shared
	$(RYE) run --pyproject bindings/python/pyproject.toml python tools/generate_bank_versions.py \
		--library $(BANK_SHARED_LIB) --check

# Render the per-binding processor-name declarations from the tracked catalog,
# so no surface carries a hand-maintained copy of the shipped name set. Reads
# only the committed catalog, so neither target needs a build.
processor-types:
	python3 tools/generate_processor_types.py

processor-types-check:
	python3 tools/generate_processor_types.py --check

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

# The WASM tests type-check against dist/, so unlike the Node one this gate
# cannot live in `lint` — it needs the build prerequisite this target already
# carries. Left unwired it would decay into a script nobody runs.
test-wasm: build-wasm-binding
	cd bindings/wasm && yarn test
	cd bindings/wasm && yarn test:types

# Focused security-hardening gates. Each test command writes its complete log
# under the matching build directory, and --no-tests=error prevents a renamed
# or accidentally undiscovered regression test from passing silently.
# `halt_on_error=1` is load-bearing -- without it UndefinedBehaviorSanitizer
# diagnoses and continues, the process exits 0, and the target passes with
# undefined behaviour present.
test-hardening-asan:
	CC=clang CXX=clang++ $(CMAKE) -B build-hardening-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DBUILD_CLI=OFF -DSONARE_WITH_FFMPEG=OFF -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined"
	$(CMAKE) --build build-hardening-asan --target sonare_tests --parallel $(HARDENING_JOBS)
	ASAN_OPTIONS=$(HARDENING_ASAN_OPTIONS) UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ctest --test-dir build-hardening-asan --output-on-failure --no-tests=error --output-log build-hardening-asan/test-hardening.log -R "public input corpus|set_markers rejects an invalid list|duplicate parameter rejection|offline results reject shapes|default Audio exposes a valid empty iterator"

# The TSan filter selects every test that starts a second thread, matched on the
# naming vocabulary those tests share. Three threaded cases stay out of reach
# because they are `[.]`-hidden and ctest never discovers them: the GS EFX render
# race and the two seqlock tearing soaks.
test-hardening-tsan:
	CC=clang CXX=clang++ $(CMAKE) -B build-hardening-tsan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DBUILD_CLI=OFF -DSONARE_WITH_FFMPEG=OFF -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"
	$(CMAKE) --build build-hardening-tsan --target sonare_tests --parallel $(HARDENING_JOBS)
	TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-hardening-tsan --output-on-failure --no-tests=error --output-log build-hardening-tsan/test-hardening.log -R "concurrent|producer consumer stress|control/audio threads|captured samples before captured_frames|polls safely during processing|reclaims retired pages|cannot lap an audio-held snapshot|race with process_block"

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

# Feature-gate build matrix. Configure + compile only, no ctest: what it catches
# is the defect class where an always-compiled translation unit references a
# symbol that only exists inside a feature gate, which shows up as a compile or
# link failure and never as a test failure. It catches no behaviour at all -- a
# stub returning wrong values, or a feature-off build answering SONARE_OK with
# zeroes, passes this gate.
#
# The all-off entry is not redundant with the single-option rows. A gated symbol
# can be reachable through a second enabled feature, so a break that needs two
# options off together is invisible to a matrix that only turns one off at a
# time. Tests are excluded so a failure points at the shipped library and the
# CLI rather than at test code that assumes a full-feature build.
FEATURE_MATRIX_OPTIONS := BUILD_MASTERING BUILD_MIXING BUILD_MIXING_ASSISTANT BUILD_GRAPH \
       BUILD_FX BUILD_ACOUSTIC_SIM BUILD_PITCH_EDITOR BUILD_VOICE_CHANGER BUILD_ARRANGEMENT \
       BUILD_ASSIST
FEATURE_MATRIX_ALL_OFF := $(foreach opt,$(FEATURE_MATRIX_OPTIONS),-D$(opt)=OFF)

build-feature-matrix:
	@set -e; \
	for opt in $(FEATURE_MATRIX_OPTIONS); do \
	  dir="build-feature-$$(echo $$opt | tr 'A-Z_' 'a-z-')-off"; \
	  echo "=== $$opt=OFF ($$dir) ==="; \
	  $(CMAKE) -B "$$dir" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF -DSONARE_WITH_FFMPEG=OFF -D$$opt=OFF > "$$dir.log" 2>&1 || { cat "$$dir.log"; exit 1; }; \
	  $(CMAKE) --build "$$dir" --parallel $(HARDENING_JOBS) >> "$$dir.log" 2>&1 || { cat "$$dir.log"; exit 1; }; \
	done; \
	echo "=== all features OFF (build-feature-all-off) ==="; \
	$(CMAKE) -B build-feature-all-off -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF -DSONARE_WITH_FFMPEG=OFF $(FEATURE_MATRIX_ALL_OFF) > build-feature-all-off.log 2>&1 || { cat build-feature-all-off.log; exit 1; }; \
	$(CMAKE) --build build-feature-all-off --parallel $(HARDENING_JOBS) >> build-feature-all-off.log 2>&1 || { cat build-feature-all-off.log; exit 1; }

# Cross-binding parity gate (C API is canonical). Stdlib-only, no build needed:
# it reads the binding sources directly and exits non-zero on active drift.
parity: conformance
	python3 tools/parity/check_parity.py

# Tracked per-runtime capability matrix, derived from the same reachability
# decision the parity checker makes. The check variant leaves the worktree
# untouched and fails when the table no longer matches the surfaces.
surface-coverage:
	python3 tools/parity/surface_coverage.py

surface-coverage-check:
	python3 tools/parity/surface_coverage.py --check

# Shared public-input schema plus public streaming field/flag/default snapshot.
# Also gates request-object coverage: every one-shot facade export keeps a
# *Request overload, and every *Request a public function accepts stays exported
# from the package entry (both are invisible to the C-ABI parity checker).
conformance:
	python3 tools/conformance/check_public_contracts.py
	python3 tools/api/check_request_object_coverage.py
	python3 tools/conformance/check_cli_contract.py --schema
	python3 -m unittest tests/conformance/test_cli_contract.py
	python3 -m unittest tests/conformance/test_wasm_exception_scope.py
	python3 -m unittest tests/conformance/test_bank_versions.py
	python3 tools/parity/test_handle_gating.py
	python3 tools/parity/test_record_shape.py
	python3 tools/parity/test_ts_reexport.py
	python3 tools/parity/test_surface_coverage.py
	python3 tools/parity/test_allowlist_audit.py
	python3 tools/eval/test_summarize_accuracy.py
	python3 tools/audition/test_serve.py
	python3 tools/parity/surface_coverage.py --check
	python3 tools/parity/check_parity.py --audit-allowlist
	@if test -x "$(BUILD_DIR)/bin/sonare-cli" && test -x "bindings/python/.venv/bin/python"; then \
		python3 tools/conformance/check_cli_contract.py \
			--native "$(BUILD_DIR)/bin/sonare-cli" \
			--python "bindings/python/.venv/bin/python"; \
	else \
		echo "conformance: live CLI check skipped (build/bin/sonare-cli or bindings/python/.venv/bin/python is unavailable)"; \
	fi

# Opt-in GM-program project bounce acceptance across the C, Python, Node, and
# WASM public surfaces. The check deliberately does not build the bindings: it
# is an acceptance run over already-built artifacts, with an explicit preflight
# so a clean tree fails with the exact build targets to run first.
test-gm-cross-surface:
	@command -v "$(RYE)" >/dev/null 2>&1 || { \
		echo "test-gm-cross-surface: required command not found: $(RYE)" >&2; \
		exit 1; \
	}
	@set -eu; \
	for artifact in \
		"$(PYTHON_SHARED_LIB)" \
		"bindings/node/dist/index.js" \
		"bindings/node/build/Release/sonare-node.node" \
		"bindings/wasm/dist/index.js" \
		"bindings/wasm/dist/sonare.js" \
		"bindings/wasm/dist/sonare.wasm"; do \
		if test ! -f "$$artifact"; then \
			echo "test-gm-cross-surface: missing required artifact: $$artifact" >&2; \
			echo "test-gm-cross-surface: run 'make build-shared build-node build-wasm-binding' first" >&2; \
			exit 1; \
		fi; \
	done
	SONARE_LIB_PATH=$(PYTHON_SHARED_LIB) PYTHONPATH=$(CURDIR)/bindings/python/src \
	$(RYE) run --pyproject bindings/python/pyproject.toml python tests/conformance/check_gm_project_surfaces.py

# Opt-in mixing-assistant scene acceptance across the C, Python, Node, and WASM
# public surfaces: one synthetic multi-track fixture in, one scene JSON out of
# each facade. Request-object shapes and per-binding option-name tables are
# invisible to the parity checker, so this runs the surfaces instead of reading
# them. Like the GM check, it builds nothing and preflights the artifacts.
test-mix-assistant-cross-surface:
	@command -v "$(RYE)" >/dev/null 2>&1 || { \
		echo "test-mix-assistant-cross-surface: required command not found: $(RYE)" >&2; \
		exit 1; \
	}
	@set -eu; \
	for artifact in \
		"$(PYTHON_SHARED_LIB)" \
		"bindings/node/dist/index.js" \
		"bindings/node/build/Release/sonare-node.node" \
		"bindings/wasm/dist/index.js" \
		"bindings/wasm/dist/sonare.js" \
		"bindings/wasm/dist/sonare.wasm"; do \
		if test ! -f "$$artifact"; then \
			echo "test-mix-assistant-cross-surface: missing required artifact: $$artifact" >&2; \
			echo "test-mix-assistant-cross-surface: run 'make build-shared build-node build-wasm-binding' first" >&2; \
			exit 1; \
		fi; \
	done
	SONARE_LIB_PATH=$(PYTHON_SHARED_LIB) PYTHONPATH=$(CURDIR)/bindings/python/src \
	$(RYE) run --pyproject bindings/python/pyproject.toml python tests/conformance/check_mix_assistant_surfaces.py

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

# What each calibrated voice has and what its next round needs — the entry
# point of the calibration loop, and the one target here that neither builds
# nor renders anything. Read-only and always exit 0: a loop reads it to decide
# where to start, which a target that failed on "there is work to do" could not
# be used for.
voice-status:
	@$(RYE) run --pyproject bindings/python/pyproject.toml python tools/voicematch/profile.py \
		status --all

# Hold every calibrated GM fallback voice to the bounds recorded beside its
# reference profile. One target rather than one per instrument: a gate exists
# for an instrument exactly when `tools/voicematch/reference/<id>_gate.json`
# does, so calibrating a new voice adds it here by writing that file and
# nothing else. Each gate names the timbre it was recorded against, and a bound
# only means anything against that one, so the timbre is read back out of the
# gate rather than left to `compare`'s first-timbre default. A voice outside
# its bounds does not stop the ones after it: each run costs minutes, so
# aborting on the first failure would hide every later instrument behind it and
# turn one drift into several round trips.
#
# Deliberately outside `ci-local` and outside CI. It renders the full grid of
# every gated instrument through the library (minutes, not seconds) and it is a
# listening-and-measuring tool: the bounds are re-recorded by whoever makes the
# trade, in the change that justifies it, which is a judgement no CI job can
# make. Its own build dir, so it neither retargets the Debug `build/` that ctest
# reads nor disturbs the shared `build-python-shared/`.
VOICE_BUILD_DIR ?= build-autofit
ifeq ($(UNAME_S),Darwin)
VOICE_SHARED_LIB := $(CURDIR)/$(VOICE_BUILD_DIR)/lib/libsonare.dylib
else
VOICE_SHARED_LIB := $(CURDIR)/$(VOICE_BUILD_DIR)/lib/libsonare.so
endif
voice-gate:
	$(CMAKE) -S . -B $(VOICE_BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED=ON
	$(CMAKE) --build $(VOICE_BUILD_DIR) --target sonare_shared -j
	@found=0; failed=""; \
	for gate in tools/voicematch/reference/*_gate.json; do \
		test -e "$$gate" || continue; \
		found=1; \
		id=$$(basename "$$gate" _gate.json); \
		timbre=$$($(RYE) run --pyproject bindings/python/pyproject.toml python -c \
			"import json,sys; print(json.load(open(sys.argv[1]))['timbre'])" "$$gate"); \
		echo "=== $$id (timbre $$timbre)"; \
		SONARE_LIB_PATH=$(VOICE_SHARED_LIB) \
		$(RYE) run --pyproject bindings/python/pyproject.toml python tools/voicematch/profile.py \
			compare --config tools/voicematch/capture/$$id.json \
			--timbre "$$timbre" --gate "$$gate" || failed="$$failed $$id"; \
	done; \
	test "$$found" = 1 || { echo "no *_gate.json under tools/voicematch/reference/"; exit 1; }; \
	if test -n "$$failed"; then \
		echo; echo "voices outside their recorded bounds:$$failed"; \
		exit 1; \
	fi; \
	echo; echo "every gated voice held its bounds"

# Aggregate the fast, non-modifying mechanical gates so a pre-commit run can't
# silently skip one. Check-only (run `make format` first to auto-fix); excludes
# the heavy build + ctest (`make test`) by design. Ordered build-independent
# checks first, then the compiler-backed layout snapshot check (needs a C++
# compiler, not a full build).
ci-local:
	$(MAKE) format-check
	$(MAKE) parity
	$(MAKE) processor-types-check
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
