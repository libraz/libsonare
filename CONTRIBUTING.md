# Contributing to libsonare

Contributions are welcome. Before writing code, please read the next two sections — this is a large project with a small number of maintainers, and the shape of a change matters more here than the amount of it.

## What you are contributing to

Roughly 835,000 tracked lines: a C++17 DSP core, four hand-written API surfaces over it (the C ABI, a Node N-API addon, a Python ctypes binding, a WASM embind module), a Python CLI, a native C++ CLI, and a test suite of about 5,400 cases. The core covers analysis, mastering, mixing, editing, a GM instrument bank and a DAW engine, behind nineteen `BUILD_*` options.

Two consequences worth knowing up front:

- **A change usually reaches further than it looks.** A single new capability can mean an edit to the C ABI, three bindings, two type-declaration files, and a drift gate — see [Invariants](#invariants). The surfaces are hand-written, not generated, so nothing stops them from disagreeing except the checks described below.
- **The project moves quickly and is developed on `develop`.** A fork goes stale in days. For anything beyond a typo or a one-line fix, **open an issue first** and describe what you want to change; a short exchange there saves more work than it costs, and it avoids a pull request that has to be rewritten because it landed on the wrong side of a boundary.

Reporting a bug you do not intend to fix is a genuinely useful contribution and needs none of the setup below.

## What CI requires

**Every check blocks.** There is no advisory gate: the workflows contain no `continue-on-error`, so a red job is a red pull request. Pull requests targeting `main` run `ci.yml`; those targeting `develop` run `develop-ci.yml`, which adds an examples build and a Python wheel build.

| Job | What it checks |
| --- | --- |
| `clang-format` | `clang-format --dry-run --Werror` over every tracked `.h` / `.hpp` / `.c` / `.cpp` / `.mm` outside `third_party/` |
| `lint` | Biome over the WASM and Node bindings, a `tsc` type-check of the Node binding, `ruff check .` over the whole repository, and four drift gates: the ABI layout snapshot, the ABI version mirrors, the generated processor types, and cross-surface parity |
| `Calibration harness` | The instrument-bank calibration harness under `tools/voicematch` |
| `Native C++` | A Debug build and the default ctest run, on Linux and macOS |
| `Installed package` | Installs into a scratch prefix and links each exported archive on its own — the only gate that sees the library the way an external C++ consumer does |
| `ASan/UBSan` | The suite under the address and undefined-behaviour sanitizers |
| `Coverage + WASM` | Coverage, plus the emscripten build and the WASM binding's tests |
| `FFmpeg decode` | The optional FFmpeg decode path, which every other job compiles out |

`ruff` runs over the repository rather than a path list, so a Python error anywhere — `tools/`, `tests/`, `benchmarks/` — fails the job, not only one under `bindings/python/`.

## Development setup

What you need depends on what you are changing. Nothing here requires the full toolchain.

**C++ core only** — a C++17 compiler, CMake 3.16 or newer, `clang-format`, and `python3` (standard library only):

```bash
make build            # Debug build into build/
make fixtures         # generates the K-weighting reference fixture; needed once
ctest --test-dir build --output-on-failure --parallel
```

Eigen and Catch2 are resolved with `find_package` first and fetched only if the host has neither, so an installed `libeigen3-dev` and Catch2 3 make configuring work offline.

Note that `make test` also runs the calibration harness, which needs [rye](https://rye.astral.sh/); `make test-cxx` is the C++ suite on its own and is what a core-only change needs.

**A binding** — additionally Node 22 with Corepack, and rye for Python:

```bash
cd bindings/node   && yarn install --immutable && yarn build && yarn test
cd bindings/wasm   && yarn install --immutable && yarn build && yarn test
make test-python
```

Tests import built output, so build before testing. The Node and WASM tests import from `dist/`, and the WASM test run refuses to start against a `dist/` older than its sources.

**The WASM module** — additionally emsdk 5.0.2:

```bash
source /path/to/emsdk/emsdk_env.sh
make wasm
```

A WASM build overwrites `bindings/wasm/dist/`, which every other surface's tests import, wherever its build directory lives. Use a separate `git worktree` if you need to build a deliberately modified module.

### Running the gates locally

The drift gates need only `python3`, so a core-only change can clear them without Node or rye:

```bash
git ls-files -z -- '*.h' '*.hpp' '*.c' '*.cpp' '*.mm' ':!:third_party/**' | xargs -0 clang-format --dry-run --Werror
make parity                                      # cross-surface drift, runs the conformance gates first
make processor-types-check
make check-abi-version
python3 tools/abi/gen_abi_layout.py --check      # needs a C compiler for a generated probe
```

`make ci-local` runs all of these together with `make lint`, but it does need Node and rye: it reaches the layout snapshot through rye rather than calling the script directly, and the binding linters through yarn. `make format` applies formatting across every language present, and needs the same tools for the same reason.

`make lint` type-checks both typed surfaces, not only the linters: `tsc` for Node and `mypy --strict` for Python, each with the non-vacuity guard CI uses. **A Python stub is the only place a type error can hide from every other gate** — ruff does not read it, no test imports it, and the module answers at runtime whatever the stub says — so a `.pyi` edited without running this target is unverified.

## Invariants

These are the rules a reviewer will check a change against, and most of them are things no compiler can catch.

### Across the surfaces

- **The C ABI is the reference.** For a signature, enum or struct change, change `include/sonare/` first and bring the bindings to it, rather than the reverse.
- **Type declarations move in the same change as the code.** The `.pyi` files for Python, `bindings/node/src/types.ts` and `bindings/wasm/src/public_types.ts` for Node and WASM. Parity cannot see type drift.
- **Find every caller before a rename, in two passes.** A language server resolves the C++ callers; in the bindings the symbol survives only as a ctypes string or an N-API registration name, which nothing follows, so grep the whole tree. `benchmarks/` and `tools/` are caller trees too and neither is compiled by default.
- **Python uses keyword arguments; the high-level JS/TS entry points take a request object.** These are deliberate per-surface idioms, not drift. Keep field names and defaults matching across surfaces without making the call shapes identical.
- **Fixing a parity divergence means deleting its allowlist entry in the same change.** An entry that excuses nothing fails the audit, and a stale one keeps blessing the name for whatever symbol inherits it next.

### The command-line front-ends

- **A new subcommand is unfinished until `tests/conformance/cli_contract_v2.json` classifies it.** The manifest is the ledger for both front-ends — the native `sonare-cli` and the Python `sonare` — and `shared` is the expensive classification, because it also pins an option contract in `inventory.expected_options` and is compared against both binaries. Implementation and ledger rows move together.
- **A command on one front-end only states why, in the ledger.** `reason_kind` is `by_design` for a decision the project stands behind and `unported` for a gap nobody has closed, with a sentence saying which. A `by_design` reason names the route the other side already has, so it can be checked; an `unported` one expires with the port, the way an allowlist entry expires with its divergence. Without the field a classification and a decision are spelled the same and the difference has to be re-derived from commit messages.
- **"Reach it from the library instead" justifies a native-only command and never a Python-only one.** It is the argument most of the native-only set rests on, and it holds because a Python CLI user has the library in the same language and the same process — the librosa mirrors and the metering figures are entry points they already call over the array in hand. The native binary's user has a binary and no toolchain, so a capability absent there has no fallback at all. A Python-only command therefore needs a named native route to be `by_design`, and is `unported` without one, whatever the two front-ends' relative sizes suggest.
- **CLI stdout is snake_case throughout.** The core's JSON producers emit camelCase for the object surfaces, so a command that passes such a document through re-keys it — `analysis_json_for_cli()` on the native side, `_json_keys_to_snake_case()` on the Python side. The exception is a document fed back to the library verbatim, whose names belong to the schema that will read it rather than to the CLI.
- **The live comparison is skipped when either binary is missing, and says so rather than failing.** A ledger row is only checked against reality on a run that has both `build/bin/sonare-cli` and the Python virtualenv, so build both before trusting a green `make conformance` on a manifest change.

### The C ABI

- **Never bump an ABI version as part of a change.** The versions live in several mirrors that must agree, and `make check-abi-version` verifies the hand-written ones against the C source of truth. A bump belongs to a release that changes a distributed binary's layout.
- **ctypes has no `static_assert`.** The C side guards its struct layouts; the Python mirror does not, so changing a C struct field without updating the matching `ctypes.Structure` segfaults pytest instead of failing a test. Run `make abi-layout` and commit the regenerated snapshot in the same change.
- **Adding a translation unit under `src/c_api/` means editing several source lists, and which ones depends on the unit.** Copy the row of the closest existing sibling rather than assuming a fixed set.
- **Moving a C-ABI translation unit between library targets can delete an exported function from the shared library while everything still builds and passes.** It surfaces late, as a `dlsym` failure in the parity run. Check `nm -gU` on the built shared library after any such move.
- **Every pointer in a public header owes its lifecycle in its own doc comment** — who frees it, which `sonare_free_*` releases it, or what ends the library's retention of a caller's buffer. A conformance gate enforces this.

### WASM

- **A `catch` is deleted at compile time unless its translation unit carries `-fexceptions` on its own compile line.** Nothing announces the loss: no compile error, no link error, no failing test — the throw walks past its handler and reaches JavaScript raw. The flag is set per file, per target, in `src/CMakeLists.txt`. Run the exception-scope check after a WASM build.

### Build options

- **Feature-gate breakage appears only in a build, never in a test.** `make build-feature-matrix` compiles each option off on its own plus one all-off row. The all-off row is not redundant: a gated symbol can stay reachable through a second enabled feature. No CI job configures a feature-off build, so run this after touching a source list or adding a translation unit outside a gate.
- **A new library target must be named in the export list at the foot of `src/CMakeLists.txt`**, or it builds and links in tree and is silently missing from `find_package`.
- **Eigen is compiled with `EIGEN_MPL2_ONLY`.** A handful of headers under `unsupported/` are LGPL rather than MPL-2.0, and libsonare ships Apache-2.0 with no copyleft in any distributed artifact, so including one fails the build with `Including non-MPL2 code in EIGEN_MPL2_ONLY mode`. That error means the header has to go, not the definition.

### Tests

- **`tests/CMakeLists.txt` is an explicit list, not a glob.** Add a case to an existing file or register the new file.
- **Run `sonare_tests` from the repository root**; it loads reference fixtures by relative path.
- **Cases over roughly two seconds take the `[.][slow]` tag** (`make test-slow`), and Python uses `@pytest.mark.slow`. Most pipelines validate fine on a 0.25–0.5 s signal, so prefer shrinking the input.
- **Golden tests are excluded from the default run and are environment-sensitive.** Their hashes cannot match across architectures, so they are a same-environment check. Confirm a golden failure is not pre-existing before blaming your change, and never move a golden without the behavioural change that justifies it in the same commit.
- **A test that cannot fail is worse than no test.** Where a change fixes a defect, show the assertion failing before the fix — a guard that can never fire, or an assertion on a compile-time constant, will be asked about in review.

### Numeric constants

- **Use `sonare::constants::*` from `src/util/constants.h`; do not hardcode a universal numeric literal.** In a `.cpp`, include the header and add a `using` declaration inside the file's namespace; in a header, fully qualify. No `using namespace` in a header, ever.
- **The same value is not the same constant.** A literal that merely looks like a named constant stays a literal: the `12.0f` in a polynomial divisor, as a dB limit, and as a delay in milliseconds are none of them semitones. Replace only where the literal genuinely means what the constant means.

## Style

- **C++17.** No C++20 or later features, including `<numbers>` — which is why the project carries its own constants header.
- **Comments and documentation in English**, in the language's standard doc-comment format: Doxygen for C and C++, TSDoc for TypeScript, PEP 257 docstrings for Python.
- **Comments in the C++ tree are terse.** A justification above a constant is three lines: the measurement that set the value and the direction it moved. Inside a function, one line above the step it explains — if a step needs a paragraph, the paragraph is describing something the code should have named. Prefer deleting a sentence to rewording it. A binding's public API documentation is the exception, since a consumer reads it in a tooltip.
- **Markdown in this repository is never hard-wrapped.** One bullet is one logical line however long it runs.
- **No per-file licence headers.** The licence lives in `LICENSE` and `NOTICE`.
- Documentation describes the current state, not the change history.
- Where a function mirrors an established analysis API, match its defaults (`sr=22050`, `n_fft=2048`, `hop_length=512`).

## Commit messages

Conventional Commits, in English: `type(scope): summary`, lowercase after the colon, no trailing period. A body, when there is one, is a blank line followed by `- ` bullets. Describe the change itself rather than the process that produced it.

## Reporting issues

Search the existing issues first, then include: what you did, what happened, what you expected, the version and which binding, and a minimal reproduction if you have one. For an analysis result that looks wrong, the input file matters more than anything else in the report.

Security vulnerabilities go through the process in [SECURITY.md](SECURITY.md), not the public issue tracker.

## Licence

By contributing, you agree that your contributions are licensed under the Apache License 2.0.
