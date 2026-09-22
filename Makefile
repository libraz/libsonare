.PHONY: all build fixtures release install test-install test test-cxx test-voicematch test-slow test-golden test-optional-fixtures test-librosa-live clean rebuild format format-check lint wasm coverage \
       coverage-build coverage-clean build-shared build-node build-wasm-binding \
       test-python test-python-slow test-node test-wasm parity conformance test-gm-cross-surface test-mix-assistant-cross-surface abi-layout abi-layout-check check-abi-version \
       capability-catalog capability-catalog-check processor-types processor-types-check ci-local \
       build-bank-shared bank-versions bank-versions-check \
       surface-coverage surface-coverage-check \
       gs-census gs-census-header gs-census-check gs-program-census gs-address-table-json gs-unit-archive-set gs-unit-diff gs-unit-diff-check \
       gs-efx-archive-set gs-efx-tables gs-efx-tables-check gs-efx-coverage \
       gs-efx-bindings gs-efx-bindings-check gs-efx-join gs-efx-join-check \
       test-hardening test-hardening-asan test-hardening-tsan test-hardening-host test-hardening-wasm \
       build-feature-matrix accuracy-report voice-gate voice-status voice-status-all \
       voice-readiness voice-status-refresh voice-status-check spec-check \
       voicematch-substitution voicematch-substitution-all voicematch-determinism \
       voicematch-reextract-check voicematch-sf2-corpus voicematch-sustain-check \
       voicematch-loss-sensitivity voicematch-loss-cells \
       spec-liveness spec-liveness-census spec-liveness-census-check \
       excerpts excerpts-check test-voicematch \
       check-c-api-out-param-init check-c-api-pointer-contracts check-c-api-header-self-contained \
       check-c-api-type-home check-binding-warning-flags

BUILD_DIR ?= build
OPTIONAL_FIXTURE_BUILD_DIR := build-optional-fixtures
GOLDEN_BUILD_DIR := build-golden
ACCURACY_REPORT_JSON ?= $(CURDIR)/build-optional-fixtures/accuracy-report.json
INSTALL_PREFIX_DIR := $(CURDIR)/build-install-prefix
RYE ?= rye
CMAKE ?= cmake
# Compile workers for every target that builds into a private -B directory.
# An explicit --parallel outranks CMAKE_BUILD_PARALLEL_LEVEL, so honour that
# variable here or the documented way to cap a shared machine silently loses.
HARDENING_JOBS ?= $(if $(CMAKE_BUILD_PARALLEL_LEVEL),$(CMAKE_BUILD_PARALLEL_LEVEL),2)
# Compile workers for the ordinary build targets. A bare `-j` outranks
# CMAKE_BUILD_PARALLEL_LEVEL the same way an explicit --parallel does, and it
# means "unbounded" to the underlying make, so pass the level through when one
# is set and fall back to the native default when it is not.
BUILD_PARALLEL := $(if $(CMAKE_BUILD_PARALLEL_LEVEL),--parallel $(CMAKE_BUILD_PARALLEL_LEVEL),-j)
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
	$(CMAKE) --build $(BUILD_DIR) $(BUILD_PARALLEL)

release:
	$(CMAKE) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build $(BUILD_DIR) $(BUILD_PARALLEL)

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
	$(CMAKE) --build build-install $(BUILD_PARALLEL)
	$(CMAKE) --install build-install
	$(CMAKE) -S tests/cmake/consumer -B build-install-consumer \
	  -DCMAKE_PREFIX_PATH=$(INSTALL_PREFIX_DIR)
	$(CMAKE) --build build-install-consumer $(BUILD_PARALLEL)
	ctest --test-dir build-install-consumer --output-on-failure --no-tests=error

# Delegates instead of configuring a tree of its own, because the feature
# flags, the output name and the analysis-only switch that decide WHICH module
# gets built live in bindings/wasm/package.json, while the post-build step
# copies whatever was built over the dist/ every surface's tests import. It
# also covers both bundles: the freshness guard checks each, so building one
# leaves the other stale and the suite refuses to start.
wasm:
	cd bindings/wasm && yarn build

# The K-weighting cases compare against a reference this script computes, and
# the file it writes is gitignored, so a fresh checkout has no copy of it. The
# cases fail rather than skip when it is absent, so every target that runs them
# generates it first and CI invokes this same target rather than repeating the
# command.
fixtures:
	python3 tools/scripts/k_weighting_reference.py

# The C++ suite on its own: cmake, a C++17 compiler and python3, with no Node
# and no rye. A change confined to the core can be verified with this, which is
# what CONTRIBUTING points a contributor at.
test-cxx: build fixtures
	ctest --test-dir $(BUILD_DIR) --output-on-failure --parallel

# The full local run. The calibration harness rides on it because `tools/` is
# outside ctest and outside the drift gates, so a harness left to be remembered
# is a harness that rots -- but it needs rye, which a contributor touching only
# C++ has no other reason to install, so its absence reports itself and does not
# fail a green C++ run. CI covers the harness in a build-free job of its own, so
# nothing goes unchecked when this branch is taken.
test: test-cxx
	@if command -v $(RYE) >/dev/null 2>&1; then \
	  $(MAKE) test-voicematch; \
	else \
	  echo "note: skipping the calibration harness -- $(RYE) is not installed."; \
	  echo "      run 'make test-voicematch' once it is, or let CI cover it."; \
	fi

# The cases read only the tracked capture definitions and reference profiles --
# no rendered corpus, no plugin, no built library -- so they pass on a fresh
# clone in about half a minute. Invoked directly, this reports a missing rye as
# the error it is; `test` is the target that treats it as a skip.
test-voicematch:
	$(RYE) run --pyproject bindings/python/pyproject.toml python -m pytest tools/voicematch -q

# Heavy cases (>~2 s each) are tagged [.][slow] and hidden from the default
# ctest run; this runs just those. Must run from the repo root (librosa
# fixtures load by relative path).
test-slow: build fixtures
	./$(BUILD_DIR)/bin/sonare_tests "[slow]"

# Golden regressions are hidden from the default Catch2 run so they can be
# invoked explicitly in local development and CI.
#
# Release, in its own directory, for two independent reasons. The recorded
# digests come from a Release build, and comparing them against a Debug one asks
# whether two optimization levels agree rather than whether the code changed.
# And $(BUILD_DIR) is shared: depending on `build` would reconfigure whatever
# another session has in it, mid-run, as a side effect of running a test.
test-golden: fixtures
	$(CMAKE) -B $(GOLDEN_BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build $(GOLDEN_BUILD_DIR) $(BUILD_PARALLEL) --target sonare_tests
	./$(GOLDEN_BUILD_DIR)/bin/sonare_tests "[golden]"

test-optional-fixtures:
	$(CMAKE) -B $(OPTIONAL_FIXTURE_BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug -DSONARE_ENABLE_OPTIONAL_FIXTURE_TESTS=ON
	$(CMAKE) --build $(OPTIONAL_FIXTURE_BUILD_DIR) $(BUILD_PARALLEL)
	ctest --test-dir $(OPTIONAL_FIXTURE_BUILD_DIR) --output-on-failure -R "optional|fixture|EBU R128" --parallel

# Measures musical accuracy against whatever corpus the music_eval manifests
# point at and rolls it up into a publishable table. Distinct from
# test-optional-fixtures, which gates: only report_only manifest rows produce
# the observations this reads. Reports "unmeasured" rather than a score for a
# dimension with no rows. See tools/eval/README.md.
accuracy-report:
	$(CMAKE) -B $(OPTIONAL_FIXTURE_BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DSONARE_ENABLE_OPTIONAL_FIXTURE_TESTS=ON
	$(CMAKE) --build $(OPTIONAL_FIXTURE_BUILD_DIR) $(BUILD_PARALLEL)
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
#
# Every writing step is fed a tracked file list from git, never a directory. A
# directory argument cannot tell a file you just wrote from one another worktree
# is still writing, and a file that exists in no commit is the one write class
# with nothing to diff against. Format a brand-new file by naming it, or stage it
# first -- `ls-files` reads the index, so `git add` opts it in. The lists are
# generated, so a new source tree is picked up without editing this file, and the
# two binding scripts own their own so `yarn lint:fix` is safe to run directly.
# `format-check` covers the same files and only reads; point CI and any caller
# that does not own the worktree at that instead.
LS_EXISTING = python3 -c 'import os, sys; paths = [p for p in sys.stdin.buffer.read().split(b"\0") if p and os.path.exists(os.fsdecode(p))]; sys.stdout.buffer.write(b"\0".join(paths) + (b"\0" if paths else b""))'

format:
	git ls-files -z -- '*.h' '*.hpp' '*.c' '*.cpp' '*.mm' ':!:third_party/**' | $(LS_EXISTING) | xargs -0 clang-format -i
	cd bindings/wasm && yarn lint:fix
	cd bindings/node && yarn lint:fix
	UV_CACHE_DIR=$(UV_CACHE_DIR) $(RYE) sync --pyproject bindings/python/pyproject.toml
	git ls-files -z -- 'bindings/python/src/*.py' 'bindings/python/src/*.pyi' 'bindings/python/tests/*.py' | $(LS_EXISTING) | xargs -0 env UV_CACHE_DIR=$(UV_CACHE_DIR) $(RYE) run --pyproject bindings/python/pyproject.toml ruff format
	git ls-files -z -- '*.py' '*.pyi' '*pyproject.toml' | $(LS_EXISTING) | xargs -0 env UV_CACHE_DIR=$(UV_CACHE_DIR) $(RYE) run --pyproject bindings/python/pyproject.toml ruff check --fix
	$(MAKE) lint

# `test:types` type-checks the Node binding's tests against src (biome does not
# type-check, and the build tsconfig excludes tests). It reads sources only —
# no built addon or dist/ needed — so it belongs with the other static gates.
#
# Ruff lints the repo rather than a path list. Python lives in ten trees here —
# the binding, `tools/`, `benchmarks/`, `examples/python/` and four under
# `tests/` — and a list of them is a hand-maintained index that a new tree drops
# out of silently. This target only reads, so it takes `.`; the auto-fix side
# derives the same population from `git ls-files` because it writes. `ruff
# format` deliberately stays on the binding: it would restyle 110 files
# elsewhere, whose line breaks are hand-set.
#
# The sample-rate rule, stated in src/midi/synth/docs/gs.md and CONTRIBUTING.md: a
# quantity measured at the machine's 32 kHz internal clock must be stored in
# physical units (Hz/ms/s/ratio/dB), never as a sample-rate-dependent
# coefficient. This is not a style preference -- it is the exact shape of a
# defect this tree already shipped once (a waveguide loop filter's brightness
# mapping with no `sr` term at all, correct only at 48 kHz). No test can
# enforce it (a coefficient and the physical quantity it came from both
# compile and both run), so this is a grep over src/effects/** and
# src/mastering/**: no `*Config` struct field named for a coefficient shape
# (coeff/b1/a1/alpha). (A second rule that flagged `std::exp(` combined with
# `sample_rate` was tried and dropped -- that combination is the CORRECT
# pattern, not the defective one, so the rule could only ever fire on code
# already doing the right thing. The two-sample-rate behavioural test,
# tests/effects/insert_sample_rate_test.cpp, is what actually catches this
# defect class.) One field predates this rule and is not a sample-rate
# coefficient despite the name match; allowlisted by exact field name with a
# reason, retired the day the field is renamed or becomes rate-derived.
define GS_EFX_SR_COEFFICIENT_LINT_PY
import pathlib, re, sys

ROOTS = [pathlib.Path("src/effects"), pathlib.Path("src/mastering")]
FILES = sorted(p for r in ROOTS for p in r.rglob("*") if p.suffix in (".h", ".hpp", ".cpp"))

FIELD_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(coeff|Coeff|b1|a1|alpha|Alpha)[A-Za-z0-9_]*\s*[;=]")
STRUCT_RE = re.compile(r"\bstruct\s+\w*Config\b")

ALLOWED_FIELDS = {
    ("src/mastering/repair/denoise_classical.h", "dd_alpha"):
        "Ephraim-Malah frame-to-frame smoothing factor, a literature constant "
        "with no sample_rate term -- not a filter coefficient.",
}

violations = []
scanned = 0

for path in FILES:
    scanned += 1
    posix = path.as_posix()
    depth = 0
    config_depth = None
    pending_config = False
    for lineno, line in enumerate(path.read_text().splitlines(), start=1):
        if config_depth is None and not pending_config and STRUCT_RE.search(line):
            pending_config = True
        for ch in line:
            if ch == "{":
                depth += 1
                if pending_config and config_depth is None:
                    config_depth = depth
                    pending_config = False
            elif ch == "}":
                depth -= 1
                if config_depth is not None and depth < config_depth:
                    config_depth = None
        in_config = config_depth is not None
        if in_config:
            fm = FIELD_RE.search(line)
            if fm:
                field_name = fm.group(0).rstrip(";= ").strip()
                if (posix, field_name) not in ALLOWED_FIELDS:
                    violations.append((path, lineno, line.strip(),
                        "field name carries a sample-rate-dependent coefficient shape "
                        "(coeff/b1/a1/alpha) inside a *Config struct"))

if scanned == 0:
    sys.exit("sample-rate coefficient rule: found no files under src/effects or "
             "src/mastering -- the glob is broken, this is not a clean result")

if violations:
    for path, lineno, code, why in violations:
        print(f"{path}:{lineno}: {why}")
        print(f"    {code}")
    sys.exit(f"sample-rate coefficient rule: {len(violations)} violation(s) across {scanned} files scanned")

print(f"sample-rate coefficient rule: clean ({scanned} files scanned)")
endef
export GS_EFX_SR_COEFFICIENT_LINT_PY

# The insertion-effect chain skeleton holds a few fixed quantities the archive
# never measured -- shelf corners, a mix ratio, a carrier rate -- and a binding
# row may not carry a constant at all (tools/gs/efx-bindings/SCHEMA.md). What is
# pinned is the count rather than the names: the rule being defended is that the
# measured table stays the place a new quantity lands, so a seventh is a law
# hand-written into the skeleton. Moving one the other way (into efx-tables.json,
# where its provenance becomes checkable) fails this too, on purpose -- the pin
# comes down in the same commit that earns it.
GS_EFX_SKELETON_CONSTANTS := 6
GS_EFX_SKELETON_CONSTANT_RE := constexpr float k[A-Za-z]*(Hz|DryWet|ChainHz) =

lint:
	cd bindings/wasm && yarn lint
	cd bindings/node && yarn lint
	cd bindings/node && yarn test:types
	UV_CACHE_DIR=$(UV_CACHE_DIR) $(RYE) sync --pyproject bindings/python/pyproject.toml
	UV_CACHE_DIR=$(UV_CACHE_DIR) $(RYE) run --pyproject bindings/python/pyproject.toml ruff check .
# The Python half of the type check that `yarn test:types` above already does for
# Node. CI has run it all along and nothing local did, so a stub that stopped
# describing its module -- invisible to ruff, to every test, and to the runtime
# that answers anyway -- reached develop and was found by a push that had not
# happened yet. Second invocation is the non-vacuity guard, as in CI: a file that
# must NOT type-check, so a configuration that silently checks nothing fails here
# instead of certifying everything.
	cd bindings/python && UV_CACHE_DIR=$(UV_CACHE_DIR) $(RYE) run --pyproject pyproject.toml \
		mypy --strict src/libsonare ../../tests/typing/python_smoke.py
	@cd bindings/python && if UV_CACHE_DIR=$(UV_CACHE_DIR) $(RYE) run --pyproject pyproject.toml \
		mypy --strict ../../tests/typing/python_catalog_invalid.py; then \
		echo "python_catalog_invalid.py unexpectedly passed mypy" >&2; exit 1; \
	fi
	python3 -c "$$GS_EFX_SR_COEFFICIENT_LINT_PY"
# The test-independence rule, stated in CONTRIBUTING.md: the byte-to-physical-unit
# conversion functions are tested against the archive's raw measured
# readings, hand-transcribed, never against gs_efx_tables.h -- the table the
# same derivation script generates from the same archive. A test that
# imported the table instead would be comparing the derivation to itself and
# could never go red. `-n` prints the offending line so a future false
# positive (a comment mentioning the header, say) is visible immediately
# rather than needing a second run to see what matched; a bare mention
# outside an #include is deliberately not this rule's concern.
	@test -f tests/midi/gs_efx_convert_test.cpp || { \
		echo "lint: tests/midi/gs_efx_convert_test.cpp is missing -- the include-scope check has nothing to read" >&2; \
		exit 1; }
	@if grep -n '#include.*gs_efx_tables\.h' tests/midi/gs_efx_convert_test.cpp; then \
		echo "tests/midi/gs_efx_convert_test.cpp includes gs_efx_tables.h -- its expectations"; \
		echo "would come from the same derivation that built the table it is meant to check"; \
		exit 1; \
	fi
	@found=$$(grep -cE '$(GS_EFX_SKELETON_CONSTANT_RE)' src/midi/synth/gs_layer.cpp); \
	if [ "$$found" != "$(GS_EFX_SKELETON_CONSTANTS)" ]; then \
		echo "gs_layer.cpp holds $$found hand-placed EFX constants, pinned at $(GS_EFX_SKELETON_CONSTANTS):" >&2; \
		grep -nE '$(GS_EFX_SKELETON_CONSTANT_RE)' src/midi/synth/gs_layer.cpp >&2 || true; \
		echo "  a new one is a measured law written into the skeleton by hand; one fewer means the" >&2; \
		echo "  pin moves in the same commit. Neither is decided here." >&2; \
		exit 1; \
	fi; \
	echo "gs_layer.cpp hand-placed EFX constants: $$found (pinned)"
# Both headers are rendered from committed inputs alone, so a clone can re-run
# them and a drifted one is a static fact rather than something a build reports.
	$(MAKE) gs-efx-bindings-check
	$(MAKE) gs-efx-join-check

format-check:
	git ls-files -z -- '*.h' '*.hpp' '*.c' '*.cpp' '*.mm' ':!:third_party/**' | xargs -0 clang-format --dry-run --Werror
	$(MAKE) lint

# Binding targets
build-shared:
	$(CMAKE) -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED=ON
	$(CMAKE) --build $(BUILD_DIR) --target sonare_shared $(BUILD_PARALLEL)
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

FIELD_COVERAGE := $(CURDIR)/tools/voicematch/field-coverage.json

build-bank-shared:
	$(CMAKE) -S . -B $(BANK_BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED=ON -DBUILD_TUNING=ON
	$(CMAKE) --build $(BANK_BUILD_DIR) --target sonare_shared $(BUILD_PARALLEL)

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

# This is the only configuration that compiles the AU adapters at all, so the
# filter below decides whether any of their tests ever run. It matches the whole
# `AU ` family rather than naming cases: an enumerated list silently stops
# covering the next probe added beside them.
test-hardening-host:
ifeq ($(UNAME_S),Darwin)
	$(CMAKE) -B build-hardening-host -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DBUILD_CLI=OFF -DSONARE_WITH_FFMPEG=OFF -DBUILD_COREAUDIO=ON -DBUILD_COREMIDI=ON -DBUILD_AU_HOST=ON
	$(CMAKE) --build build-hardening-host --target sonare_tests --parallel $(HARDENING_JOBS)
	ctest --test-dir build-hardening-host --output-on-failure --no-tests=error --output-log build-hardening-host/test-hardening.log -R "CoreAudio oversize callback|CoreMIDI (input|output|scripted)|AU "
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
#
# Each row checks that its option actually went off before spending a build on
# it. A dependency rule in CMakeLists.txt may force an option back ON -- which
# is a legitimate thing for it to do -- and the row then configures, compiles
# and passes while being a byte-for-byte duplicate of the default build. Green,
# expensive, and evidence of nothing. Reading the resolved value back out of the
# cache is what separates the two, and it costs one `cmake -L` rather than a
# full core build: a forced row is reported and skipped, and the target exits
# non-zero at the end naming every option that is not currently switchable.
#
# A dependency rule that REFUSES to configure is the same fact stated the other
# way round, so it takes the same reported-and-skipped path rather than ending
# the run. Aborting there costs every row after it, including the all-off one
# that no single-option row can stand in for -- which is how a matrix meant to
# cover ten options covered six.
#
# Every failure is therefore carried to the summary instead of exiting where it
# happens, so one kind of failure can never hide the report of another.
FEATURE_MATRIX_OPTIONS := BUILD_MASTERING BUILD_MIXING BUILD_MIXING_ASSISTANT BUILD_GRAPH \
       BUILD_FX BUILD_ACOUSTIC_SIM BUILD_PITCH_EDITOR BUILD_VOICE_CHANGER BUILD_ARRANGEMENT \
       BUILD_ASSIST
FEATURE_MATRIX_ALL_OFF := $(foreach opt,$(FEATURE_MATRIX_OPTIONS),-D$(opt)=OFF)

build-feature-matrix:
	@set -e; \
	forced=""; \
	blocked=""; \
	broken=""; \
	for opt in $(FEATURE_MATRIX_OPTIONS); do \
	  dir="build-feature-$$(echo $$opt | tr 'A-Z_' 'a-z-')-off"; \
	  echo "=== $$opt=OFF ($$dir) ==="; \
	  if ! $(CMAKE) -B "$$dir" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF -DSONARE_WITH_FFMPEG=OFF -D$$opt=OFF > "$$dir.log" 2>&1; then \
	    echo "    SKIPPED: configuring with $$opt=OFF failed, so this row has nothing to build"; \
	    sed 's/^/    /' "$$dir.log"; \
	    blocked="$$blocked $$opt"; \
	    continue; \
	  fi; \
	  resolved=$$($(CMAKE) -L -N "$$dir" 2>/dev/null | sed -n "s/^$$opt:BOOL=//p"); \
	  case "$$resolved" in \
	    OFF|0|FALSE|NO|N|IGNORE|NOTFOUND|"") ;; \
	    *) echo "    SKIPPED: a dependency rule forced $$opt back to $$resolved, so this row would rebuild the default configuration"; \
	       grep -i "enabling $$opt" "$$dir.log" | sed 's/^/    /' || true; \
	       forced="$$forced $$opt"; \
	       continue;; \
	  esac; \
	  if ! $(CMAKE) --build "$$dir" --parallel $(HARDENING_JOBS) >> "$$dir.log" 2>&1; then \
	    cat "$$dir.log"; \
	    broken="$$broken $$opt"; \
	  fi; \
	done; \
	echo "=== all features OFF (build-feature-all-off) ==="; \
	if ! $(CMAKE) -B build-feature-all-off -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF -DSONARE_WITH_FFMPEG=OFF $(FEATURE_MATRIX_ALL_OFF) > build-feature-all-off.log 2>&1 \
	   || ! $(CMAKE) --build build-feature-all-off --parallel $(HARDENING_JOBS) >> build-feature-all-off.log 2>&1; then \
	  cat build-feature-all-off.log; \
	  broken="$$broken ALL_OFF"; \
	fi; \
	rc=0; \
	if test -n "$$broken"; then \
	  echo; \
	  echo "feature-off builds that failed:$$broken"; \
	  rc=1; \
	fi; \
	if test -n "$$blocked"; then \
	  echo; \
	  echo "cannot be switched off alone, so the matrix cannot cover them:$$blocked"; \
	  echo "Each one is required by another option that is ON by default, so its row"; \
	  echo "never configures. Express the requirement as a forced value rather than a"; \
	  echo "refusal, so the row reports as forced and the default stays buildable, or"; \
	  echo "pair the two options in FEATURE_MATRIX_OPTIONS and vary them together."; \
	  rc=1; \
	fi; \
	if test -n "$$forced"; then \
	  echo; \
	  echo "not switchable, so the matrix cannot cover them:$$forced"; \
	  echo "Either gate the subsystem that forces each one (the NOT_SUPPORTED stub"; \
	  echo "pattern in src/c_api/sonare_c_daw.cpp is how the tree already does it),"; \
	  echo "or drop the option and make the subsystem unconditional. Leaving it in"; \
	  echo "FEATURE_MATRIX_OPTIONS buys a row that can never differ from default."; \
	  rc=1; \
	fi; \
	exit $$rc

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

# The GS address census: which addresses real Standard MIDI Files reach, and how
# many files reach each. The corpus is not in the repository (see
# tools/gs/docs/census.md), so `gs-census` needs one fetched first and
# `gs-census-header` rerenders the committed test input from the committed JSON.
# The coverage gate itself is a C++ case, gs_address_census_test.cpp.
# The whole corpus root, not the loose `mid/` half: the committed census was
# taken over both that and the unpacked archives, and pointing this at one of
# them regenerates a census a third smaller — which reads as coverage
# regressing and lowers the gate's ceilings, the one direction a refresh must
# not be able to move them by accident.
GS_CORPUS ?= .cache/gs-corpus
GS_CENSUS_SOURCE ?= regenerated locally

gs-census:
	python3 tools/gs/extract_addresses.py --corpus $(GS_CORPUS) \
	    --out tools/gs/address-census.json --source "$(GS_CENSUS_SOURCE)"
	$(MAKE) gs-census-header

# Which programs and which variation banks real files select, as a sibling
# histogram. Nothing gates on it: it is evidence for the bank's working order
# (tools/voicematch/policy.json), which was a reasonable guess until it ran.
gs-program-census:
	python3 tools/gs/extract_programs.py --corpus $(GS_CORPUS) \
	    --out tools/gs/program-census.json --source "$(GS_CENSUS_SOURCE)" --top 20

gs-census-header:
	python3 tools/gs/gen_census_header.py --census tools/gs/address-census.json \
	    --out tests/midi/gs_address_census.inc

gs-census-check:
	python3 tools/gs/gen_census_header.py --census tools/gs/address-census.json \
	    --out /tmp/gs_address_census_check.inc
	diff -u tests/midi/gs_address_census.inc /tmp/gs_address_census_check.inc

# The address table against a measured unit. The census above says what real
# files reach and is blind to any address they never send; this says what the
# machine itself answers, which is the other half and the one that closes the
# provenance gap gs.md declares. The archive is external and its licence is its
# own, so the diff is committed and a fresh clone reads the work list without
# fetching it -- and GS_UNIT_ARCHIVE has no default for the same reason, a path
# into a tree a clone does not have being a dead pointer rather than a
# convenience. See tools/gs/docs/unit-diff.md.
GS_UNIT_ARCHIVE ?=
GS_TABLE_JSON := .cache/gs-address-table.json

gs-address-table-json:
	@mkdir -p .cache
	$(CXX) -std=c++17 -I src -o .cache/gs_dump_address_table tools/gs/dump_address_table.cpp
	.cache/gs_dump_address_table > $(GS_TABLE_JSON)

gs-unit-archive-set:
	@test -n "$(GS_UNIT_ARCHIVE)" || { \
	    echo "GS_UNIT_ARCHIVE is unset: point it at a unit directory of an external"; \
	    echo "measurement archive (tools/gs/docs/unit-diff.md)."; exit 1; }

gs-unit-diff: gs-unit-archive-set gs-address-table-json
	python3 tools/gs/check_unit.py --table $(GS_TABLE_JSON) \
	    --unit $(GS_UNIT_ARCHIVE) --out tools/gs/unit-diff.json

gs-unit-diff-check: gs-unit-archive-set gs-address-table-json
	python3 tools/gs/check_unit.py --table $(GS_TABLE_JSON) \
	    --unit $(GS_UNIT_ARCHIVE) --out /tmp/gs_unit_diff_check.json
	diff -u tools/gs/unit-diff.json /tmp/gs_unit_diff_check.json

# The GS insertion-effect byte-to-physical-unit conversion tables, derived
# from the same external archive as gs-unit-diff but read differently: that
# script's --unit wants a unit DIRECTORY (GS_UNIT_ARCHIVE, documented as such
# above), while derive_efx_tables.py's --archive wants the archive ROOT and
# resolves the unit itself. The two are not interchangeable, so this gets its
# own variable rather than reading GS_UNIT_ARCHIVE -- doing so would silently
# break whichever of the two callers is already relying on its documented
# shape. No default, for the reason GS_UNIT_ARCHIVE has none: a path into a
# tree a clone does not have is a dead pointer rather than a convenience. See
# tools/gs/docs/efx-tables.md.
GS_EFX_ARCHIVE ?=

gs-efx-archive-set:
	@test -n "$(GS_EFX_ARCHIVE)" || { \
	    echo "GS_EFX_ARCHIVE is unset: point it at the root of an external"; \
	    echo "measurement archive of what an individual SC-8850 answered"; \
	    echo "(tools/gs/docs/efx-tables.md)."; exit 1; }

gs-efx-tables: gs-efx-archive-set
	python3 tools/gs/derive_efx_tables.py --archive $(GS_EFX_ARCHIVE) \
	    --out tools/gs/efx-tables.json --header src/midi/synth/gs_efx_tables.h

# Regenerates into a scratch directory and diffs against the committed pair.
# The scratch directory is a fresh mktemp -d rather than a fixed /tmp path,
# because two sessions running this target at once would otherwise clobber
# each other's output. A revision mismatch is reported on its own line
# before the diff runs: "the archive moved on" and "the script changed" both
# show up as a diff, and folding the two into one report sends whoever reads
# it to the wrong place.
gs-efx-tables-check: gs-efx-archive-set
	@scratch=$$(mktemp -d) || exit 1; \
	trap 'rm -rf "$$scratch"' EXIT; \
	set -e; \
	python3 tools/gs/derive_efx_tables.py --archive $(GS_EFX_ARCHIVE) \
	    --out "$$scratch/gs_efx_tables_check.json" --header "$$scratch/gs_efx_tables_check.h"; \
	committed=$$(python3 -c "import json; print(json.load(open('tools/gs/efx-tables.json'))['archive_revision'])"); \
	regenerated=$$(python3 -c "import json; print(json.load(open('$$scratch/gs_efx_tables_check.json'))['archive_revision'])"); \
	if [ "$$committed" != "$$regenerated" ]; then \
	    echo "archive_revision differs: committed $$committed, regenerated from \$$GS_EFX_ARCHIVE $$regenerated"; \
	    echo "  (this is the archive moving on, not necessarily the derivation changing --"; \
	    echo "  the diff below says whether the tables themselves moved too)"; \
	fi; \
	diff -u tools/gs/efx-tables.json "$$scratch/gs_efx_tables_check.json"; \
	diff -u src/midi/synth/gs_efx_tables.h "$$scratch/gs_efx_tables_check.h"

# The binding table the insert chain walks, rendered from the hand-written
# tools/gs/efx-bindings/*.json. Both inputs are committed, so this one needs no
# archive and a clone can regenerate it -- which is why it is a script of its
# own rather than another output of derive_efx_tables.py.
gs-efx-bindings:
	python3 tools/gs/bindings_header.py

gs-efx-bindings-check:
	python3 tools/gs/bindings_header.py --check

# The same rows as gs-efx-bindings, but all five forms rather than the
# assigned ones alone: what the tests need in order to tell a documented
# state from an unmapped type, which a table of reached controls cannot say.
gs-efx-join:
	python3 tools/gs/join_header.py

gs-efx-join-check:
	python3 tools/gs/join_header.py --check

# The GS EFX coverage equation: how many of the 770 printed (type, slot)
# parameters are translated, documented as a state, unmapped, unreadable or
# built by hand, and how many nobody has adjudicated yet. See
# tools/gs/coverage.py.
gs-efx-coverage: gs-efx-archive-set
	python3 tools/gs/coverage.py --archive $(GS_EFX_ARCHIVE)

# Shared public-input schema plus public streaming field/flag/default snapshot.
# Also gates request-object coverage: every one-shot facade export keeps a
# *Request overload, and every *Request a public function accepts stays exported
# from the package entry (both are invisible to the C-ABI parity checker).
#
# `check_lint_scope` gates the static gates themselves. CI does not call this
# Makefile for lint, so the ruff target, the clang-format pathspec and every
# pinned tool version each live in three files at once; widening one and not the
# others leaves the gate that runs on a push as narrow as it was, and green.
conformance:
	python3 tools/conformance/check_public_contracts.py
	python3 tools/api/check_request_object_coverage.py
	python3 tools/conformance/check_cli_contract.py --schema
	python3 tools/conformance/check_repair_cli_param_keys.py
	python3 tools/conformance/check_lint_scope.py
	python3 -m unittest tests/conformance/test_cli_contract.py
	python3 -m unittest tests/conformance/test_wasm_exception_scope.py
	python3 -m unittest tests/conformance/test_wasm_feature_gate_scope.py
	python3 -m unittest tests/conformance/test_binding_warning_flags.py
	python3 tests/conformance/check_wasm_narrowing_scope.py
	python3 -m unittest tests/conformance/test_wasm_narrowing_scope.py
	python3 tests/conformance/check_python_narrowing_scope.py
	python3 -m unittest tests/conformance/test_python_narrowing_scope.py
	python3 tests/conformance/check_public_integer_domains.py
	python3 -m unittest tests/conformance/test_public_integer_domains.py
	python3 tests/conformance/check_zero_sentinel_filter.py
	python3 -m unittest tests/conformance/test_zero_sentinel_filter.py
	python3 tests/conformance/check_goniometer_capacity_mirrors.py
	python3 -m unittest tests/conformance/test_goniometer_capacity_mirrors.py
	python3 tests/conformance/check_chord_quality_tables.py
	python3 -m unittest tests/conformance/test_chord_quality_tables.py
	python3 tests/conformance/check_documented_bound_mirrors.py
	python3 -m unittest tests/conformance/test_documented_bound_mirrors.py
	python3 tests/conformance/check_rir_diagnostic_codes.py
	python3 -m unittest tests/conformance/test_rir_diagnostic_codes.py
	python3 tests/conformance/check_synth_param_surface.py
	python3 -m unittest tests/conformance/test_synth_param_surface.py
	python3 -m unittest tests/conformance/test_ts_surface_walk.py
	python3 tests/conformance/check_mastering_param_surfaces.py
	python3 -m unittest tests/conformance/test_mastering_param_surfaces.py
	python3 tests/conformance/check_result_schema_surfaces.py
	python3 -m unittest tests/conformance/test_result_schema_surfaces.py
	python3 -m unittest tests/conformance/test_bank_versions.py
	python3 tests/conformance/check_bank_policy.py
	python3 -m unittest tests/conformance/test_bank_policy.py
	python3 -m unittest tests/conformance/test_gs_program_census.py
	python3 -m unittest tests/conformance/test_c_api_out_param_init.py
	python3 -m unittest tests/conformance/test_c_api_pointer_contracts.py
	python3 -m unittest tests/conformance/test_c_api_header_self_contained.py
	python3 -m unittest tests/conformance/test_c_api_type_home.py
	python3 tests/conformance/check_c_api_out_param_init.py --floor 250
	python3 tests/conformance/check_c_api_pointer_contracts.py --floor 250
	python3 tests/conformance/check_c_api_header_self_contained.py --floor 20
	python3 tests/conformance/check_c_api_type_home.py --floor 150
	python3 -m unittest tests/conformance/test_error_code_mapping.py
	python3 tools/conformance/test_lint_scope.py
	python3 tools/parity/test_handle_gating.py
	python3 tools/parity/test_record_shape.py
	python3 tools/parity/test_ts_reexport.py
	python3 tools/parity/test_surface_coverage.py
	python3 tools/parity/test_allowlist_audit.py
	python3 tools/parity/test_comparison_reach.py
	python3 tools/parity/test_c_return_type_coverage.py
	python3 tools/eval/test_summarize_accuracy.py
	python3 tools/audition/test_serve.py
	python3 tools/audition/test_page.py
	python3 tools/parity/surface_coverage.py --check
	python3 tools/parity/check_parity.py --audit-allowlist
	@if test -x "$(BUILD_DIR)/bin/sonare-cli" && test -x "bindings/python/.venv/bin/python"; then \
		python3 tools/conformance/check_cli_contract.py \
			--native "$(BUILD_DIR)/bin/sonare-cli" \
			--python "bindings/python/.venv/bin/python"; \
	else \
		echo "conformance: live CLI check skipped (build/bin/sonare-cli or bindings/python/.venv/bin/python is unavailable)"; \
	fi

# C-ABI contract scans, kept as named targets for running one on its own;
# `conformance` runs all four, plus the self-tests that carry the non-vacuity
# break each is calibrated against.
#
# The floors are the point of the invocation: each scan reports nothing when its
# pattern stops matching, so the count of declarations it resolves at all is
# pinned separately from the count of findings, and falling under it exits 2.
check-c-api-out-param-init:
	python3 tests/conformance/check_c_api_out_param_init.py --floor 250

check-c-api-pointer-contracts:
	python3 tests/conformance/check_c_api_pointer_contracts.py --floor 250

# Compiles each public header as a TU's only include, in C and in C++. No build
# tree: it drives the compiler on a probe file, so it costs about a second.
check-c-api-header-self-contained:
	python3 tests/conformance/check_c_api_header_self_contained.py --floor 20

# Reports a type a surface header defines but none of its own declarations
# take, while a sibling's do. Text-only, no build tree.
check-c-api-type-home:
	python3 tests/conformance/check_c_api_type_home.py --floor 150

# Holds both hand-written binding layers to the core's warning bar, read off the
# compilation database each layer's own build wrote. It therefore needs the WASM
# module and the Node addon built, and refuses an absent database rather than
# passing on it, so it stays out of `conformance`: the gate that runs on a push
# builds neither. The unittest beside it does stay in `conformance` — it works
# on synthetic databases and skips a layer this tree has not built.
check-binding-warning-flags:
	python3 tests/conformance/check_binding_warning_flags.py

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

# Where every voice in the bank stands and what its next round needs — the entry
# point of the calibration loop, and the one target here that neither builds nor
# renders anything. Reads the committed `tools/voice-status.json`, so a plain
# clone sees the whole bank. Read-only and always exit 0: a loop reads it to
# decide where to start, which a target that failed on "there is work to do"
# could not be used for.
voice-status:
	@$(RYE) run --pyproject bindings/python/pyproject.toml python tools/voicematch/status.py

# The same, every voice rather than only those past the oracle step.
voice-status-all:
	@$(RYE) run --pyproject bindings/python/pyproject.toml python tools/voicematch/status.py --all

# The readiness of the four instruments a capture exists for, in the detail the
# bank view does not carry: which profile columns back which dimension, which
# phrase takes have an archived reference.
voice-readiness:
	@$(RYE) run --pyproject bindings/python/pyproject.toml python tools/voicematch/profile.py \
		status --all

# What a gate does NOT say. Its bounds come from whatever the voice measured on
# the day, so passing them means "no worse than when this was written"; feeding
# another instrument's reference rows through the same comparison counts how
# many of those bounds are not identifying the instrument at all. Reads only the
# committed captures, profiles and gates — no render, no build, seconds — and it
# exits 0 whatever it finds, for the same reason `voice-status` does: a target
# that failed on "a gate is loose" could not be read to decide which to
# re-record. Deliberately outside `ci-local` and outside CI.
voicematch-substitution:
	@$(RYE) run --pyproject bindings/python/pyproject.toml python \
		tools/voicematch/substitution.py

# The same over every grid two or more captures share, not only the largest.
voicematch-substitution-all:
	@$(RYE) run --pyproject bindings/python/pyproject.toml python \
		tools/voicematch/substitution.py --all-grids

# Whether a repeated note comes back as the same waveform, one program per
# engine. Renders, so it needs the Python dylib current — `render_model` warns
# when it is not. Exits 0 whatever it finds, and stays out of CI: a voice that
# repeats itself is a voicing finding rather than a build failure.
voicematch-determinism:
	@$(RYE) run --pyproject bindings/python/pyproject.toml python \
		tools/voicematch/determinism_check.py

# Re-measures every committed reference from its cached corpus and diffs the
# result against `reference/<id>.json` (its `measured_utc` stamp excluded --
# see the module docstring for why that field always changes). Answers
# whether the tree a measurement-code change is about to touch already
# disagrees with the code sitting in it today, before the change lands and
# takes the blame for drift that predates it. Needs the cached corpus under
# `.cache/voicematch/` (or `SONARE_VOICEMATCH_ROOT`), so an id with none is
# reported rather than measured. Read-only over `reference/` and `capture/` --
# every measurement goes to a scratch `--profile` path. Exits 0 whatever it
# finds, on the same terms as `voicematch-substitution`, and stays out of CI.
voicematch-reextract-check:
	@$(RYE) run --pyproject bindings/python/pyproject.toml python \
		tools/voicematch/reextract_check.py

# Whether a voice still sounds at the end of a held note, and for as long as
# its reference does. `decay_db_s` is measured on every captured note and is
# not a canonical dimension for the sustained class, so it is neither gated nor
# excusable and nothing reports it; a voice whose column stops oscillating then
# renders a note that falls away under a compare table of ordinary numbers,
# because every other column is a ratio the signal keeps producing on its way
# to the noise floor. Reads the reference for the target rather than holding
# every voice to a constant -- a pad is meant to evolve -- and is two-sided,
# since holding where the reference decays is the same kind of defect. Renders
# the model, writes nothing, exits 0 whatever it finds, and stays out of CI on
# the same terms as `voicematch-substitution`.
voicematch-sustain-check:
	@$(RYE) run --pyproject bindings/python/pyproject.toml python \
		tools/voicematch/sustain_check.py

# Whether the loss charges for a change a listener would name. Perturbs a
# captured REFERENCE by a known amount -- raising partials 15-30, slowing the
# attack, adding tremolo, negating the velocity response, adding hiss, cutting
# the output -- and reads the result against the same capture's own timbre
# spread, so a score under 1.0 means the change sits inside the distance two
# recordings of the instrument already are apart. The perturbation is applied to
# audio rather than to a knob, because a null through a knob cannot say which of
# the two was blind. Every perturbation is also run at amplitude zero, and a
# zero that does not score exactly 0.0 is plumbing. Needs the cached corpus
# under `.cache/voicematch/` (or `SONARE_VOICEMATCH_ROOT`); reads nothing else,
# renders nothing, builds nothing, exits 0 whatever it finds, and stays out of
# CI on the same terms as `voicematch-substitution`.
voicematch-loss-sensitivity:
	@$(RYE) run --pyproject bindings/python/pyproject.toml python \
		tools/voicematch/loss_sensitivity.py

# What happened to every cell the loss aggregates, over the rendered probes in
# `tools/voicematch/out/`. Half the terms are sums over a ladder, a band profile
# or a slice grid, and a raw value cannot say whether its cells were
# comparisons, caps standing in for a model that produced nothing, or skips
# where the reference offered nothing -- and the last two read as the term's
# worst and its best respectively, so neither shows up in a results table.
# Renders nothing, builds nothing, exits 0 whatever it finds, and stays out of
# CI for the reason `voicematch-loss-sensitivity` does: a gate built on this
# would be a gate on how short the probes happen to be.
voicematch-loss-cells:
	@$(RYE) run --pyproject bindings/python/pyproject.toml python \
		tools/voicematch/loss_cells.py

# Rebuild the corpus of every capture whose untracked overlay names a SoundFont.
# Which captures those are is a question only the overlay can answer, so a clone
# without one imports nothing and says so rather than reporting a clean sweep
# over an empty set. Each import is refused rather than approximated -- a font
# needing a player, a grid asking for a velocity layer the font has not got, a
# gate longer than the shortest recording -- and a refusal is printed and moved
# past, because one capture's overlay pointing at the wrong file says nothing
# about the next. Writes only under the scratch root, read-only over `capture/`
# and `reference/`, exits 0 whatever it finds, and stays out of CI on the same
# terms as `voicematch-substitution`.
voicematch-sf2-corpus:
	@ids=$$(grep -l '"sf2"' tools/voicematch/capture/*.local.json 2>/dev/null \
		| sed 's|.*/||; s|\.local\.json$$||'); \
	if [ -z "$$ids" ]; then \
		echo "voicematch-sf2-corpus: no capture overlay names a SoundFont, so there is nothing to import"; \
		exit 0; \
	fi; \
	for id in $$ids; do \
		echo "== $$id"; \
		$(RYE) run --pyproject bindings/python/pyproject.toml python \
			tools/voicematch/import_sf2.py "tools/voicematch/capture/$$id.json" || true; \
	done

# Regenerate the bank view. Needs the tuning build, because the engine voicing
# each patch is reported by the library rather than parsed out of it — the same
# reason `bank-versions` needs it, and the same build directory.
voice-status-refresh: build-bank-shared
	$(RYE) run --pyproject bindings/python/pyproject.toml python tools/voicematch/status.py \
		--write --lib $(BANK_SHARED_LIB)

voice-status-check: build-bank-shared
	$(RYE) run --pyproject bindings/python/pyproject.toml python tools/voicematch/status.py \
		--check --lib $(BANK_SHARED_LIB)

# A fit spec outlives the mechanism it was written for. Nothing asks whether its
# knobs still exist until a run resolves the corpus and then dies on the first
# dead name, so this asks first, against the same catalogue the fit validates on.
spec-check: build-bank-shared
	$(RYE) run --pyproject bindings/python/pyproject.toml python tools/voicematch/check_specs.py \
		--lib $(BANK_SHARED_LIB)

# The other half of the same question: `spec-check` asks whether a knob exists,
# this asks whether it moves anything. A render per range end per note, so it is
# minutes rather than seconds and stays out of `ci-local`; the answer needs no
# reference, which is why it covers every spec instead of the few with an oracle.
spec-liveness: build-bank-shared
	$(RYE) run --pyproject bindings/python/pyproject.toml python tools/voicematch/liveness.py \
		--lib $(BANK_SHARED_LIB)

# The same probe pointed at the whole bank instead of the 17 specs: per patch,
# which of its own fields cannot move the render it voices. Needs no reference,
# so it answers for the 117 voices that have no oracle too. Tens of minutes: the
# cost is interpreter spawns rather than renders, and a whole grid shares one.
# Informational rather than a gate -- a patch is free not to use a field its
# engine offers, so it always exits 0.
spec-liveness-census: build-bank-shared
	$(RYE) run --pyproject bindings/python/pyproject.toml python -u tools/voicematch/liveness.py \
		--census --drums --lib $(BANK_SHARED_LIB) --out $(FIELD_COVERAGE)

# The census is committed, so a plain clone can read it without the hour. What
# keeps it honest is the bank generation it was stamped with: a voice fitted or
# a family rebalanced moves that, and a census taken against an older one
# describes a bank nobody runs. This compares the two and needs no library.
spec-liveness-census-check:
	$(RYE) run --pyproject bindings/python/pyproject.toml python tools/voicematch/liveness.py \
		--census-check $(FIELD_COVERAGE)

# Re-cut the committed Bach excerpts a musical take plays. Needs the sibling
# corpus ($SONARE_BACH_ROOT); rendering one needs nothing, which is why the note
# data is committed rather than resolved on demand. Deliberately outside
# `ci-local`: no CI checkout has the corpus, so a check there would fail on an
# absent sibling rather than on a stale excerpt.
excerpts:
	$(RYE) run --pyproject bindings/python/pyproject.toml python tools/voicematch/extract_excerpt.py

excerpts-check:
	$(RYE) run --pyproject bindings/python/pyproject.toml python tools/voicematch/extract_excerpt.py --check

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
	$(CMAKE) --build $(VOICE_BUILD_DIR) --target sonare_shared $(BUILD_PARALLEL)
	@attempted=0; compared=0; failed=""; unreached=""; log=$$(mktemp); \
	for gate in tools/voicematch/reference/*_gate.json; do \
		test -e "$$gate" || continue; \
		attempted=$$((attempted + 1)); \
		id=$$(basename "$$gate" _gate.json); \
		timbre=$$($(RYE) run --pyproject bindings/python/pyproject.toml python -c \
			"import json,sys; print(json.load(open(sys.argv[1]))['timbre'])" "$$gate" 2>/dev/null); \
		if test -z "$$timbre"; then \
			echo "=== $$id: its timbre could not be read"; \
			unreached="$$unreached $$id"; continue; \
		fi; \
		echo "=== $$id (timbre $$timbre)"; \
		SONARE_LIB_PATH=$(VOICE_SHARED_LIB) \
		$(RYE) run --pyproject bindings/python/pyproject.toml python tools/voicematch/profile.py \
			compare --config tools/voicematch/capture/$$id.json \
			--timbre "$$timbre" --gate "$$gate" > "$$log" 2>&1; \
		status=$$?; cat "$$log"; \
		if grep -q '^gate: ' "$$log"; then \
			compared=$$((compared + 1)); \
			test "$$status" = 0 || failed="$$failed $$id"; \
		else \
			unreached="$$unreached $$id"; \
		fi; \
	done; \
	rm -f "$$log"; \
	echo; echo "gates attempted: $$attempted, compared against their bounds: $$compared"; \
	test "$$attempted" != 0 || { echo "no *_gate.json under tools/voicematch/reference/"; exit 1; }; \
	if test -n "$$unreached"; then \
		echo "never reached their bounds:$$unreached"; \
		echo "  Not a verdict on these voices. A comparison that could not run exits"; \
		echo "  non-zero for reasons that are not a voice -- a missing interpreter, an"; \
		echo "  unreadable capture, a crash -- and the bound-exceeded line is the only"; \
		echo "  thing that separates them."; \
	fi; \
	if test -n "$$failed"; then \
		echo; echo "voices outside their recorded bounds:$$failed"; \
	fi; \
	if test -n "$$unreached" || test -n "$$failed"; then exit 1; fi; \
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
	$(CMAKE) --build $(BUILD_DIR) $(BUILD_PARALLEL)

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
