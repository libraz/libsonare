# Contributing to libsonare

Contributions are welcome. Before writing code, please read the next two sections — this is a large project with a small number of maintainers, and the shape of a change matters more here than the amount of it.

## What you are contributing to

Roughly 860,000 lines of hand-written code, and about as much again in tracked JSON fixtures and goldens: a C++17 DSP core, four hand-written API surfaces over it (the C ABI, a Node N-API addon, a Python ctypes binding, a WASM embind module), a Python CLI, a native C++ CLI, and a C++ suite of some 5,600 cases with the Python and TypeScript suites beside it. The core covers analysis, mastering, mixing, editing, a GM instrument bank and a DAW engine, behind nineteen `BUILD_*` options.

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

Build through the package scripts rather than calling `tsc` yourself. A declaration emit only ever adds to `dist/`, so a deleted or renamed source leaves its `.d.ts` behind, and the npm tarball ships `dist/` whole — `yarn build` is what removes it, and nothing fails while the orphan is there.

**The WASM module** — additionally emsdk 5.0.2:

```bash
source /path/to/emsdk/emsdk_env.sh
make wasm
```

A WASM build overwrites `bindings/wasm/dist/`, which every other surface's tests import, wherever its build directory lives. Use a separate `git worktree` if you need to build a deliberately modified module. Cut it at the same path depth as the repository and run the package's own scripts inside it: emscripten compiles its system libraries through a path relative to the build directory, so a worktree under `/tmp` fails to link for a reason that looks like your change and is not.

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
- **Read a category's finding count beside its `compared` column.** Zero findings out of zero comparisons renders exactly like zero out of five thousand, and the difference is the whole meaning: `default` reaches a verdict on about one candidate in twenty-five, because a default spelled inside a request-object normalizer is not in the signature the extractor reads. A category that reaches no verdict at all fails the run — an extractor that stops feeding a check produces what a clean tree produces.
- **A helper is on the surface only if a value re-export reaches it.** `export type { T } from './_helpers'` publishes `T` and nothing else, so the module's functions stay sibling-only and are not compared. Re-export one for real and it becomes a coverage gap, which is the case the check exists for.

### The command-line front-ends

- **A new subcommand is unfinished until `tests/conformance/cli_contract_v2.json` classifies it.** The manifest is the ledger for both front-ends — the native `sonare-cli` and the Python `sonare` — and `shared` is the expensive classification, because it also pins an option contract in `inventory.expected_options` and is compared against both binaries. Implementation and ledger rows move together.
- **A command on one front-end only states why, in the ledger.** `reason_kind` is `by_design` for a decision the project stands behind and `unported` for a gap nobody has closed, with a sentence saying which. A `by_design` reason names the route the other side already has, so it can be checked; an `unported` one expires with the port, the way an allowlist entry expires with its divergence. Without the field a classification and a decision are spelled the same and the difference has to be re-derived from commit messages.
- **"Reach it from the library instead" justifies a native-only command and never a Python-only one.** It is the argument most of the native-only set rests on, and it holds because a Python CLI user has the library in the same language and the same process — the librosa mirrors and the metering figures are entry points they already call over the array in hand. The native binary's user has a binary and no toolchain, so a capability absent there has no fallback at all. A Python-only command therefore needs a named native route to be `by_design`, and is `unported` without one, whatever the two front-ends' relative sizes suggest.
- **CLI stdout is snake_case throughout.** The core's JSON producers emit camelCase for the object surfaces, so a command that passes such a document through re-keys it — `analysis_json_for_cli()` on the native side, `_json_keys_to_snake_case()` on the Python side. The exception is a document fed back to the library verbatim, whose names belong to the schema that will read it rather than to the CLI.
- **The live comparison is skipped when either binary is missing, and says so rather than failing.** A ledger row is only checked against reality on a run that has both `build/bin/sonare-cli` and the Python virtualenv, so build both before trusting a green `make conformance` on a manifest change.

### The C ABI

- **An ABI version moves only when a layout that has already shipped changes — and then it must.** Do not bump to mark progress: a struct that has never appeared in a release can grow a field freely, because nothing was compiled against the old shape. A struct that HAS shipped cannot — a consumer built against the released header still holds the old size and offsets, so adding a field there is a bump, and leaving the version behind lets that consumer memcpy a struct that no longer matches and read the wrong field. Decide which case you are in by measuring rather than by how new the surface feels: `git grep -l <StructName> <last-tag> -- include/`. There are five counters, not one, and the versions live in several mirrors that must agree; `make check-abi-version` verifies the hand-written ones against the C source of truth. One mirror sits outside that check — `src/arrangement/edit_compiler.h` is pinned by a `static_assert` instead, so a bump missed there arrives as a compile error partway through a build rather than as a named mismatch.
- **ctypes has no `static_assert`.** The C side guards its struct layouts; the Python mirror does not, so changing a C struct field without updating the matching `ctypes.Structure` segfaults pytest instead of failing a test. Run `make abi-layout` and commit the regenerated snapshot in the same change.
- **Adding a translation unit under `src/c_api/` means editing several source lists, and which ones depends on the unit.** Copy the row of the closest existing sibling rather than assuming a fixed set.
- **Moving a C-ABI translation unit between library targets can delete an exported function from the shared library while everything still builds and passes.** It surfaces late, as a `dlsym` failure in the parity run. Check `nm -gU` on the built shared library after any such move.
- **Every pointer in a public header owes its lifecycle in its own doc comment** — who frees it, which `sonare_free_*` releases it, or what ends the library's retention of a caller's buffer. A conformance gate enforces this.

### WASM

- **A `catch` is deleted at compile time unless its translation unit carries `-fexceptions` on its own compile line.** Nothing announces the loss: no compile error, no link error, no failing test — the throw walks past its handler and reaches JavaScript raw. The flag is set per file, per target, in `src/CMakeLists.txt`. Run the exception-scope check after a WASM build.

### Build options

- **Feature-gate breakage appears only in a build, never in a test.** `make build-feature-matrix` compiles each option off on its own plus one all-off row. The all-off row is not redundant: a gated symbol can stay reachable through a second enabled feature. No CI job configures a feature-off build, so run this after touching a source list or adding a translation unit outside a gate. **A non-zero exit does not mean a build failed** — every row runs to the end, and the summary names which of three things happened: a row that failed to compile, a row that never configured because another default-on option requires that one, or a row whose value a dependency rule forced back so it would only rebuild the default. The last two describe the option set rather than the code. A row the summary does not name compiled clean.
- **A new library target must be named in the export list at the foot of `src/CMakeLists.txt`**, or it builds and links in tree and is silently missing from `find_package`.
- **Eigen is compiled with `EIGEN_MPL2_ONLY`.** A handful of headers under `unsupported/` are LGPL rather than MPL-2.0, and libsonare ships Apache-2.0 with no copyleft in any distributed artifact, so including one fails the build with `Including non-MPL2 code in EIGEN_MPL2_ONLY mode`. That error means the header has to go, not the definition.

### Tests

- **`tests/CMakeLists.txt` is an explicit list, not a glob.** Add a case to an existing file or register the new file.
- **Run `sonare_tests` from the repository root**; it loads reference fixtures by relative path.
- **Cases over roughly two seconds take the `[.][slow]` tag** (`make test-slow`), and Python uses `@pytest.mark.slow`. Most pipelines validate fine on a 0.25–0.5 s signal, so prefer shrinking the input.
- **Golden tests are excluded from the default run and are environment-sensitive.** Their hashes cannot match across architectures, so they are a same-environment check. Confirm a golden failure is not pre-existing before blaming your change, and never move a golden without the behavioural change that justifies it in the same commit.
  - **Run them through `make test-golden`, which builds Release in its own directory.** The digests are recorded at Release, so `sonare_tests "[golden]"` from a Debug tree compares two optimization levels rather than two states of the code — and it fails on a small, stable subset that reads convincingly as a regression in two synth engines. Each case says which of the two runs produced it in its failure output.
- **A change under `src/midi/synth/` needs `make test-golden` run locally, with both synth manifests' diffs in the same commit.** No CI job can do this for you, for the reason above. The instrument bank's value registry fingerprints tunable *values*, so a change to engine code moves no version and the synth goldens are the only thing that can see it: `gm_program_hashes.tsv` covers each program with no controller input, and `gm_gesture_hashes.tsv` covers each engine — and each patch-gated branch within an engine — under one controller gesture per row. A change reachable only from a controller moves the second and not the first. Where the same change also moves a patch field or a calibration constant, the regenerated value registry belongs in that commit too.
- **A test that cannot fail is worse than no test.** Where a change fixes a defect, show the assertion failing before the fix — a guard that can never fire, or an assertion on a compile-time constant, will be asked about in review.

### Numeric constants

- **Use `sonare::constants::*` from `src/util/constants.h`; do not hardcode a universal numeric literal.** In a `.cpp`, include the header and add a `using` declaration inside the file's namespace; in a header, fully qualify. No `using namespace` in a header, ever.
- **The same value is not the same constant.** A literal that merely looks like a named constant stays a literal: the `12.0f` in a polynomial divisor, as a dB limit, and as a delay in milliseconds are none of them semitones. Replace only where the literal genuinely means what the constant means.

### Sample-rate-dependent values

- **Store the physical quantity; derive the coefficient in `prepare()`.** A filter coefficient is a function of the sample rate, so a value measured at one rate may be carried neither as a constant nor as a linear rescaling of itself: `b1 = exp(-2*pi*fc/SR)` is not linear in `SR`. A corner is held in hertz, a delay in milliseconds, a time constant in seconds, a ratio as a ratio — and the coefficient is built where the rate is known. This tree shipped the violation once, in a waveguide loop filter whose brightness mapping had no `sr` term at all and so was correct only at 48 kHz.
- **`make lint` greps for it and a behavioural test measures it, and the two look at different things.** The grep refuses a `*Config` field under `src/effects/` or `src/mastering/` named for a coefficient shape (`coeff`, `b1`, `a1`, `alpha`), which is a naming rule and catches only the obvious form. `tests/effects/insert_sample_rate_test.cpp` is the instrument: each insert names one physical quantity it preserves, and the quantity is read off the audio at 44100 Hz and at 48000 Hz.
- **That test asserts twice per quantity and needs both halves.** Reading each rate against the value its configuration asked for says the quantity is right *there*; reading the two rates against each other says it survives a rate change. Neither subsumes the other — a defect can make both rates agree with each other and with neither asked-for value, and a different defect can make each rate right on its own while the two disagree. Which one will see a given defect is not knowable in advance, so deleting either half as redundant stops the pair being an instrument.

### The GS insertion effects

- **The byte-to-physical-quantity conversions live in `src/midi/synth/gs_efx_convert.{h,cpp}` and nowhere else.** They are pure functions over a wire byte plus an enum naming which table applies, with no dependency on the GS layer or the insert factory, which is what lets them be unit-tested against raw measured readings. Keep them that way: a conversion that takes a type and a slot and resolves the table itself would depend on the generated table, and its test would then be checking one derivation against another.
- **The tables are committed, and the tool that derives them decides nothing.** `tools/gs/efx-tables.json` and the generated `src/midi/synth/gs_efx_tables.h` are in the tree so a clone builds without fetching anything; `make gs-efx-tables-check` re-derives them from an external measurement archive and reports a content difference separately from an archive that has simply moved on. `GS_EFX_ARCHIVE` has no default, because a path into a tree a clone does not have is a dead pointer. A (type, slot) pair the archive does not reach is left on the insert's own default and counted, never given a plausible conversion — nothing downstream can tell an invented conversion from a measured one. `tools/gs/docs/efx-tables.md` is the reading guide.
- **Regenerate the header in the same change as the JSON.** It is generated and carries a do-not-edit banner; hand-editing either one alone is what the check exists to catch.
- **An EFX parameter's reset default is a function of the type, so the contract is the function and not the row's `def` byte.** Writing an EFX type loads that type's own twenty parameter bytes, which is measured behaviour rather than an implementation convenience, and `gs_efx_parameter_reset_default` answers for all sixty-five types. The address table's self-check holds the rows to it. Stating the exception in a comment beside the row instead would leave it invisible to that check.

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
