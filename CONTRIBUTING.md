# Contributing to libsonare

Contributions are welcome, including small and unfinished ones. A maintainer finishes every change before it is merged, so you do not need to get everything in this document right, or even read all of it.

## What a pull request needs

- A description of what the change does and why.
- If you can, a build and the tests for the part you changed.

That is all. A red CI job, a missing type declaration, or a binding you did not update is fine: say what you know in the pull request, and the maintainer will finish it or tell you what is left. Target `develop` if you can; a pull request against `main` is retargeted.

For a larger change, such as a new capability or one that crosses several surfaces, opening an issue first to describe the idea can save you work. It is not required.

Reporting a bug you do not intend to fix is a useful contribution too, and needs none of the setup below.

## What you are contributing to

A C++17 DSP core with four hand-written API surfaces over it (the C ABI, a Node N-API addon, a Python ctypes binding, a WASM embind module), a Python CLI and a native C++ CLI. The core covers analysis, mastering, mixing, editing, a GM instrument bank and a DAW engine, behind a set of `BUILD_*` options.

A change often reaches further than it looks. A single new capability can mean an edit to the C ABI, three bindings and two type-declaration files, because the surfaces are hand-written rather than generated. You can stop at the part you know; the rest is the maintainer's to finish.

The rest of this document has two parts. [Checklist](#checklist) describes what a finished change looks like. [Reference](#reference) explains why the checklist items exist and how the checks behave.

## Development setup

What you need depends on what you are changing. Nothing here requires the full toolchain.

**C++ core only** — a C++17 compiler, CMake 3.16 or newer, `clang-format`, and `python3` (standard library only):

```bash
make build            # Debug build into build/
make fixtures         # generates the K-weighting reference fixture; needed once
make test-cxx         # the C++ suite on its own
```

Eigen and Catch2 are resolved with `find_package` first and fetched only if the host has neither. `make test` also runs the calibration harness, which needs [rye](https://rye.astral.sh/).

**A binding** — additionally Node 22 with Corepack, and rye for Python:

```bash
cd bindings/node   && yarn install --immutable && yarn build && yarn test
cd bindings/wasm   && yarn install --immutable && yarn build && yarn test
make test-python
```

**The WASM module** — additionally emsdk 6.0.10:

```bash
source /path/to/emsdk/emsdk_env.sh
make wasm
```

### Running the gates locally

```bash
git ls-files -z -- '*.h' '*.hpp' '*.c' '*.cpp' '*.mm' ':!:third_party/**' | xargs -0 clang-format --dry-run --Werror
make parity                                      # cross-surface drift, runs the conformance gates first
make processor-types-check
make check-abi-version
python3 tools/abi/gen_abi_layout.py --check      # needs a C compiler for a generated probe
make ci-local                                    # all of the above plus make lint; needs Node and rye
make format                                      # applies formatting across every language present
```

## What CI requires

Every check blocks merging, but a red check on your pull request is not a reason to hold it back; the maintainer can look at it with you. A pull request to `develop` runs `develop-ci.yml`. The heavier jobs run in `ci.yml` on pull requests and pushes to `main`, as the pre-release gate.

| Job | Runs on | What it checks |
| --- | --- | --- |
| `clang-format` | both | `clang-format --dry-run --Werror` over every tracked `.h` / `.hpp` / `.c` / `.cpp` / `.mm` outside `third_party/` |
| `lint` | both | Biome over the WASM and Node bindings, a `tsc` type-check of the Node binding, `ruff check .` over the whole repository, and four drift gates: the ABI layout snapshot, the ABI version mirrors, the generated processor types, and cross-surface parity |
| `Calibration harness` | both | The instrument-bank calibration harness under `tools/voicematch` |
| `Native C++` | both | A Debug build and the default ctest run; Linux on `develop`, Linux and macOS on `main` |
| `Installed package` | both | Installs into a scratch prefix and links each exported archive on its own |
| `Examples` | `develop` | The Python and C++ examples |
| `Python wheel` | `develop` | A manylinux x86_64 wheel build |
| `ASan/UBSan` | `main` | The suite under the address and undefined-behaviour sanitizers |
| `Coverage + WASM` | `main` | Coverage, plus the emscripten build and the WASM binding's tests |
| `FFmpeg decode` | `main` | The optional FFmpeg decode path, which every other job compiles out |

## Checklist

This is what a change looks like by the time it is merged. The maintainer checks it and fills in what is missing, so you are not expected to know all of it. Following the items for the area you touched saves a round of review.

### Building and checking

- Build before testing; the Node, WASM and Python tests import built output.
- Build the Node and WASM packages through their package scripts (`yarn build`, `yarn build:ts`), never with a bare `tsc`.
- Build a deliberately modified WASM module in a separate `git worktree` at the same path depth as the repository.
- Run `make lint` after editing any `.pyi` stub.

### Across the surfaces

- For a signature, enum or struct change, change `include/sonare/` first and bring the bindings to it.
- Update the type declarations in the same change as the code: the `.pyi` files, `bindings/node/src/types.ts` and `bindings/wasm/src/public_types.ts`.
- Before a rename, find every caller in two passes: a language server for the C++ tree, and a grep of the whole tree for the bindings, `benchmarks/` and `tools/`.
- In the Node addon, read every argument and options key through the shared readers in `sonare_wrap_options.h` (`*Property`, `Optional*Arg`, `Required*Arg`). Never substitute a default for a value of the wrong type.
- Report a failure that comes from the library or from an object's state as a `SonareError` carrying the C-ABI code; report an argument the binding refuses itself as a `TypeError`, a `RangeError`, or in Python a `SonareValueError`.
- Read a null-handle constructor's error code from `sonare_last_error_code()`; never hard-code one.
- Give Python APIs keyword arguments and high-level JS/TS entry points a request object, with field names and defaults matching across surfaces.
- Express "not given" as absence (omitted on Node and WASM, `None` in Python). Refuse a negative position rather than reading it as a sentinel.
- Give edit-range requests a `*Sec` (`*_sec`) spelling beside `*Sample`, and refuse a request that gives both spellings of one bound.
- Never take a relative threshold against the bare maximum of the whole input: use a sliding maximum for local contrast, a maximum that sets aside the top few events for body or summary decisions, and an absolute floor beside either.
- Hold a high-level analyzer's window lengths as time quantities and rescale them once at its entry; librosa-named functions keep plain sample counts.
- When you fix a parity divergence, delete its allowlist entry in the same change.

### The command-line front-ends

- Classify a new subcommand in `tests/conformance/cli_contract_v2.json` in the same change as its implementation.
- Give a command that exists on one front-end only a `reason_kind` (`by_design` or `unported`) and a sentence saying which; a `by_design` reason names the route the other front-end already has.
- Classify a Python-only command as `by_design` only when a named native route exists; otherwise it is `unported`.
- Emit snake_case keys on CLI stdout, except in a document fed back to the library verbatim.
- Build both `build/bin/sonare-cli` and the Python virtualenv before trusting `make conformance` on a manifest change.

### The C ABI

- Bump the ABI version when a shipped struct layout or a shipped C function signature changes, and not otherwise. Check whether a struct has shipped with `git grep -l <StructName> <last-tag> -- include/`, and run `make check-abi-version`.
- When a C struct changes, update its `ctypes.Structure` mirror, run `make abi-layout`, and commit the regenerated snapshot in the same change.
- When adding a translation unit under `src/c_api/`, copy the source-list rows of its closest existing sibling.
- After moving a C-ABI translation unit between library targets, check `nm -gU` on the built shared library.
- Open every exported C function with `SONARE_C_API_ENTRY` and put everything that can throw inside `SONARE_C_TRY` / `SONARE_C_CATCH*`; write out-params only after every allocation has succeeded. `make conformance` runs the entry-guard scan.
- Document the lifecycle of every pointer in a public header in its own doc comment: who frees it, which `sonare_free_*` releases it, or what ends the library's retention of a caller's buffer.

### WASM

- Give every translation unit that contains a `catch` the `-fexceptions` flag in `src/CMakeLists.txt`, and run the exception-scope check after a WASM build.

### Build options

- Run `make build-feature-matrix` after touching a source list or adding a translation unit outside a feature gate.
- Name a new library target in the export list at the foot of `src/CMakeLists.txt`.
- Do not include Eigen headers outside the MPL-2.0 set.

### Tests

- Register a new test file in `tests/CMakeLists.txt`.
- Run `sonare_tests` from the repository root.
- Tag C++ cases over roughly two seconds `[.][slow]` and slow Python tests `@pytest.mark.slow`; prefer shrinking the input.
- Run golden tests through `make test-golden` only, and confirm a golden failure is not pre-existing before attributing it to your change.
- Never move a golden without the behavioural change that justifies it, in the same commit.
- For any change under `src/midi/synth/`, run `make test-golden` locally and commit both synth manifests' diffs (and the regenerated value registry, if a patch field or calibration constant moved) with it.
- Hold MIDI values at MIDI 2.0 width (`Velocity16` / `Control32` / `Bend32`); read `f7()` / `f14()` for a float and `u7()` / `u14()` for an integer decision. Keep MIDI 1.0 renders bit-identical (`tests/midi/golden/midi1_control_hashes.tsv`).
- Compare float results across code paths within a tolerance derived from the values' scale, never with `==`.
- A test showing that a parameter changed a result compares every field the operation measures.
- Where a change fixes a defect, show the assertion failing before the fix.

### Numeric constants

- Use `sonare::constants::*` from `src/util/constants.h` instead of a universal numeric literal. In a `.cpp`, add a `using` declaration inside the file's namespace; in a header, fully qualify. No `using namespace` in a header.
- Replace a literal only where it means what the constant means; the same value is not the same constant.

### Sample-rate-dependent values

- Store the physical quantity (hertz, milliseconds, seconds, a ratio) and build the coefficient in `prepare()`. Never carry a coefficient as a constant or as a linear rescaling of itself.
- Keep both assertions per quantity in `tests/effects/insert_sample_rate_test.cpp`: each rate against the requested value, and the two rates against each other.

### The GS insertion effects

- Keep the byte-to-physical-quantity conversions in `src/midi/synth/gs_efx_convert.{h,cpp}`, as pure functions over a wire byte plus an enum naming the table.
- Regenerate `src/midi/synth/gs_efx_tables.h` in the same change as `tools/gs/efx-tables.json`; never hand-edit either.
- Give every printed (type, slot) pair exactly one form in `tools/gs/efx-bindings/*.json` under the rules in its `SCHEMA.md`, and run `make gs-efx-coverage`.
- Express an EFX parameter's reset default through `gs_efx_parameter_reset_default`, not through a row's `def` byte or a comment.

### Style

- C++17. No C++20 or later features, including `<numbers>`.
- Comments and documentation in English, in the language's standard doc-comment format: Doxygen for C and C++, TSDoc for TypeScript, PEP 257 docstrings for Python.
- Keep C++ comments terse: at most three lines above a constant (the measurement that set it), one line inside a function. A binding's public API documentation is the exception.
- Never hard-wrap Markdown. One bullet is one logical line.
- No per-file licence headers; the licence lives in `LICENSE` and `NOTICE`.
- Documentation describes the current state, not the change history.
- Where a function mirrors an established analysis API, match its defaults (`sr=22050`, `n_fft=2048`, `hop_length=512`).

### Commit messages

Conventional Commits, in English: `type(scope): summary`, lowercase after the colon, no trailing period. A body, when there is one, is a blank line followed by `- ` bullets. Describe the change itself rather than the process that produced it. Commit messages can be rewritten at merge, so do not let them hold up a pull request.

## Reference

Nothing in this section asks anything of you; read it when a checklist item or a check puzzles you. It explains why the checklist items above exist and how the checks behave.

### Builds and gates

- `ruff` runs over the repository rather than a path list, so a Python error in `tools/`, `tests/` or `benchmarks/` fails the `lint` job too.
- The WASM test run refuses to start against a `dist/` older than its sources.
- A TypeScript declaration emit only adds to `dist/`, so a deleted or renamed source leaves its `.d.ts` behind, and the npm tarball ships `dist/` whole. The package scripts clear it; a bare `tsc` does not, and nothing fails while the orphan is there.
- A WASM build overwrites `bindings/wasm/dist/` wherever its build directory lives, and every surface's tests import it. Emscripten compiles its system libraries through a path relative to the build directory, so a worktree under `/tmp` fails to link for a reason unrelated to your change.
- The drift gates need only `python3`. `make ci-local` and `make format` need Node and rye, because they reach the layout snapshot through rye and the binding linters through yarn.
- `make lint` type-checks both typed surfaces (`tsc` for Node, `mypy --strict` for Python). A Python stub is invisible to every other gate: ruff does not read it, no test imports it, and the module answers at runtime whatever the stub says.
- `Installed package` is the only CI job that sees the library the way an external C++ consumer does. Every in-tree target links the whole set, so an archive missing a declared dependency still links in tree.

### Parity

- Read a category's finding count beside its `compared` column. Zero findings out of zero comparisons renders like zero out of five thousand. `default` reaches a verdict on few candidates, because a default spelled inside a request-object normalizer is not in the signature the extractor reads. A category that reaches no verdict at all fails the run.
- An allowlist entry that excuses nothing fails the audit, and a stale one keeps excusing whatever symbol later takes its name.
- A helper is on the surface only if a value re-export reaches it. `export type { T } from './_helpers'` publishes `T` and nothing else.
- Full documentation: `tools/parity/README.md`.

### Errors and arguments

- `InvalidState` covers a released or uninitialised handle, `NotSupported` a feature compiled out of the build, and `AbiMismatch` a native module whose ABI version disagrees with the binding, checked when the module loads.
- An out-of-domain value includes two buffers whose lengths must match.
- A negative position is refused because a sentinel means different things on different surfaces, and a negative typed by mistake must not edit nothing. A value whose `-1` is a real quantity (a −1 dBTP ceiling) is not a sentinel. The C ABI keeps samples and its own `-1` conventions, which the facades map to.
- `*Sec` values are converted at the call's sample rate and rounded to the nearest sample.
- Keyword arguments and request objects are deliberate per-surface idioms, not drift.

### Thresholds and analysis windows

- A threshold against the bare maximum lets a single dominant event set the bar for everything else. Boundary detection's `referenceWindow` is the sliding instance; beat trimming's third-highest onset peak is the set-aside instance.
- A sliding reference also sets aside a lone dominant event, one no other event in the window comes within a fixed ratio of. That ratio is held apart from the threshold, so raising the threshold never lowers the reference. With too few events to tell an outlier from the body, nothing is set aside (the assistant sets aside one per 10 s of audio, up to two).
- An analyzer's `n_fft` is samples at 22050 Hz, rescaled to the input rate, and its hop stays in input samples. The polyphony chain and the streaming analyzer state `n_fft` (and, for polyphony, the hop) as samples at 44100 Hz. The mastering and mixing assistants read window and hop as samples at 48 kHz.

### The command-line front-ends

- The manifest is the ledger for both front-ends, the native `sonare-cli` and the Python `sonare`. `shared` is the expensive classification, because it also pins an option contract in `inventory.expected_options` and is compared against both binaries.
- "Reach it from the library instead" holds for a native-only command because a Python CLI user has the library in the same language and process. The native binary's user has no toolchain, so a capability missing there has no fallback.
- An `unported` reason expires with the port, the way an allowlist entry expires with its divergence.
- The core's JSON producers emit camelCase; `analysis_json_for_cli()` (native) and `_json_keys_to_snake_case()` (Python) re-key for the CLI.
- The live comparison is skipped, and says so, when either binary is missing.

### The C ABI

- A struct that has never appeared in a release can grow a field freely. A shipped one cannot: a consumer built against the released header still holds the old size and offsets. A shipped function signature changed under the same symbol lets an old caller pass shifted arguments.
- General C function contracts use `SONARE_FEATURE_ABI_VERSION`; subsystem contracts use their own counter. There are seven counters, mirrored in several places. `make check-abi-version` checks the hand-written mirrors; `src/arrangement/edit_compiler.h` is pinned by a `static_assert` instead, so a missed bump there arrives as a compile error.
- ctypes has no `static_assert`, so a C struct changed without its Python mirror segfaults pytest instead of failing a test.
- The Python loader takes `SONARE_LIB_PATH`, then `build/lib`, then the package-adjacent copy. `conftest.py` refuses to start when the resolved in-tree library is older than the newest file under `src/` (outside `src/wasm/`) and `include/`, since a rebuild-less edit keeps the same ABI version. A library outside the checkout is not judged; `SONARE_ALLOW_STALE_LIB=1` opts out. `tests/conformance/test_playback_cli.py` takes `sonare-cli` from the same build unless `SONARE_NATIVE_CLI` names one.
- Moving a C-ABI translation unit between library targets can remove an export from the shared library while everything builds and passes; it surfaces later as a `dlsym` failure in the parity run.
- A conformance gate enforces the pointer-lifecycle doc comments.
- An exception crossing the C boundary is undefined behaviour, and a last error left from an earlier call is read as this call's failure. `tests/conformance/check_c_api_entry_guard.py` classifies every exported name; the arithmetic-only converters it exempts are listed in the script, and the audio-thread process entries use `SONARE_C_RT_API_ENTRY` because touching the thread-local diagnostics can allocate.

### WASM

- Without `-fexceptions` on its own compile line, a translation unit's `catch` is deleted at compile time: no compile error, no link error, no failing test, and the throw reaches JavaScript raw.

### Build options

- Feature-gate breakage appears only in a build, and no CI job configures a feature-off build. `make build-feature-matrix` compiles each option off on its own plus one all-off row; the all-off row catches a gated symbol that stays reachable through a second enabled feature.
- A non-zero exit from the matrix does not mean a build failed. The summary names one of three cases: a row that failed to compile, a row that never configured because another default-on option requires it, or a row whose value a dependency rule forced back. The last two describe the option set, not the code. A row the summary does not name compiled clean.
- A library target missing from the export list builds and links in tree and is silently absent from `find_package`.
- Eigen is compiled with `EIGEN_MPL2_ONLY`, because a few headers under `unsupported/` are LGPL and libsonare ships Apache-2.0 with no copyleft in any distributed artifact. Including one fails with `Including non-MPL2 code in EIGEN_MPL2_ONLY mode`; the fix is to drop the header.

### Tests

- `sonare_tests` loads reference fixtures by relative path.
- Golden tests are excluded from the default run, and their hashes cannot match across architectures. The digests are recorded at Release, so `sonare_tests "[golden]"` from a Debug tree compares two optimization levels and fails on a small, stable subset. Each case's failure output says which kind of run produced it.
- The instrument bank's value registry fingerprints tunable values, so an engine-code change moves no version and only the synth goldens see it. `gm_program_hashes.tsv` covers each program with no controller input; `gm_gesture_hashes.tsv` covers each engine, and each patch-gated branch, under one controller gesture per row. No CI job runs them.
- CI runs on x86_64 and aarch64 under GCC and clang, whose float rounding differs; a single field can come out bit-identical on one platform.
- No test or tool reads documentation. README examples, counts and stated ranges are transcribed by hand, so a prose edit never turns a build red.

### Sample-rate-dependent values

- `b1 = exp(-2*pi*fc/SR)` is not linear in `SR`, so a coefficient rescaled by the rate ratio is as wrong as one copied.
- `make lint` refuses a `*Config` field under `src/effects/` or `src/mastering/` named for a coefficient (`coeff`, `b1`, `a1`, `alpha`). That is a naming rule and catches only the obvious form; `tests/effects/insert_sample_rate_test.cpp` measures the behaviour at 44100 and 48000 Hz.
- The two assertions catch different defects. One defect makes both rates agree with each other and with neither requested value; another makes each rate right on its own while the two disagree. Which one sees a given defect is not knowable in advance.
- The classic GS realisation runs at a fixed 32 kHz internal clock with a resampler to the host rate, because its coefficients, delay steps and band limit are functions of that clock. The coefficient grep does not look under `src/midi/synth/gs_classic/`; `tests/midi/gs_classic_conformance_test.cpp` requires the same drawing at 44.1 and 48 kHz instead.

### The GS insertion effects

- The conversions stay pure so they can be unit-tested against raw measured readings. A conversion that resolved its own table from a type and slot would make its test check one derivation against another.
- The tables are committed so a clone builds without fetching anything. `make gs-efx-tables-check` re-derives them from an external measurement archive (`GS_EFX_ARCHIVE`, no default) and reports a content difference separately from an archive that has moved on. A (type, slot) pair the archive does not reach gets no conversion; its stand-in is a `designed` binding row whose basis (`carried` or `invented`) is counted. Reading guide: `tools/gs/docs/efx-tables.md`.
- The three binding forms are `translated` (a measured law), `designed` (a law the library states, basis `carried` or `invented`) and `enables` (a byte that switches a stage or selects between stages). `make gs-efx-coverage` prints `GS EFX coverage: printed=<n> translated=<n> designed=<n> enables=<n>` and `GS EFX basis: carried=<n> invented=<n>`, and fails while they do not add up.
- The tally balances with any classification, so the tests also drive each row's type through the chain and require the behaviour its form claims, and require the named control to be one the insert publishes as realtime-automatable or to sit on a reasoned exception list.
- `tools/gs/marks_from_manual.py` transcribes the manual's `+`/`#` marks and printed names onto the rows; it is the only tool that opens the archive's `documents/` tree.
- The insertion effect has two realisations over one protocol layer. Modern, the default, maps each parameter onto the library's own inserts; classic runs the archive's whole-type model of each effect under `src/midi/synth/gs_classic/`. A host selects one per instrument (`gs_efx_realization` in the C ABI and Python, `gsEfxRealization` on Node and WASM); the native CLIs do not expose it. `tools/gs/docs/classic.md` lists the per-type differences.
- Writing an EFX type loads that type's own parameter bytes, which is measured behaviour, so the reset default is a function of the type. The address table's self-check holds the rows to `gs_efx_parameter_reset_default`; an exception stated in a comment would be invisible to it.

## Reporting issues

Search the existing issues first, then include: what you did, what happened, what you expected, the version and which binding, and a minimal reproduction if you have one. For an analysis result that looks wrong, the input file matters more than anything else in the report.

Security vulnerabilities go through the process in [SECURITY.md](SECURITY.md), not the public issue tracker.

## Licence

By contributing, you agree that your contributions are licensed under the Apache License 2.0.
