# Contributing to libsonare

Contributions are welcome, including small and unfinished ones. A maintainer reviews every pull request, finishes what is missing, and merges it, so you do not need to get everything in this document right, or even read all of it.

## What a pull request needs

- A description of what the change does and why. If the intent is clear, the maintainer can verify the behaviour.
- If you can, a build and the tests for the part you changed.

That is all. A red check, a missing type declaration, a test you did not write, or a binding you did not update is fine: say what you know in the pull request, and the maintainer will finish it or tell you what is left. Target `develop` if you can; a pull request against `main` is retargeted.

For a larger change, such as a new capability or one that crosses several surfaces, opening an issue first to describe the idea can save you work. It is not required.

Reporting a bug you do not intend to fix is a useful contribution too, and needs none of the setup below.

### What the maintainer can verify

The maintainer tests on macOS; Linux is covered by CI. If your change or bug depends on something else (a Linux audio stack, a particular audio interface or MIDI device, WSL2), describe what you see there, with the output or a recording if you can, because it may not be reproducible on the maintainer's side.

## What you are contributing to

A C++17 DSP core with four hand-written API surfaces over it (the C ABI, a Node N-API addon, a Python ctypes binding, a WASM embind module), a Python CLI and a native C++ CLI. The core covers analysis, mastering, mixing, editing, a GM instrument bank and a DAW engine, behind a set of `BUILD_*` options.

A change often reaches further than it looks. A single new capability can mean an edit to the C ABI, three bindings and two type-declaration files, because the surfaces are hand-written rather than generated. You can stop at the part you know; the rest is the maintainer's to finish.

The rest of this document has two parts. [Checklist](#checklist) lists what a change can include without knowing the repository. [Reference](#reference) records what the maintainer brings each area to before merging, and why.

## Development setup

What you need depends on what you are changing. Nothing here requires the full toolchain.

**C++ core only** — a C++17 compiler, CMake 3.16 or newer, `clang-format` 18 (CI pins 18.1.8, and other major versions rewrap code differently), and `python3` (standard library only):

```bash
make build            # Debug build into build/
make fixtures         # generates the K-weighting reference fixture; needed once
make test-cxx         # the C++ suite on its own
```

Eigen and Catch2 are resolved with `find_package` first and fetched only if the host has neither. `make test` also runs the calibration harness, which needs [uv](https://docs.astral.sh/uv/).

**A binding** — additionally Node 22.12 or newer and uv for Python (CI pins uv 0.13.9, the version the locks are written with). The repo pins Node 26 and Yarn through `mise`; without it, enable Corepack, which Node 25 and later no longer bundle (`npm install -g corepack`):

```bash
(cd bindings/node && yarn install --immutable && yarn build && yarn test)
(cd bindings/wasm && yarn install --immutable && yarn build && yarn test)
make test-python
```

**The WASM module** — additionally emsdk at the release pinned in `bindings/wasm/.emscripten-version`:

```bash
source /path/to/emsdk/emsdk_env.sh
make wasm
```

### Running the checks locally

You do not have to; CI and the maintainer run them. If you want to see what CI will say first:

```bash
make format        # applies formatting across every language present
make ci-local      # formatting check and the drift gates; needs Node and uv
make parity        # cross-surface drift on its own, after the conformance gates
```

## What CI requires

A red check on your pull request is not a reason to hold it back; the maintainer can look at it with you. A pull request to `develop` runs `develop-ci.yml`. The heavier jobs run in `ci.yml` on pull requests and pushes to `main`, before a release.

| Job | Runs on | What it checks |
| --- | --- | --- |
| `clang-format` | both | `clang-format --dry-run --Werror` over every tracked `.h` / `.hpp` / `.c` / `.cpp` / `.mm` outside `third_party/` |
| `lint` | both | Biome over the WASM and Node bindings, a `tsc` type-check of both bindings and their tests, `ruff check .` over the whole repository, Biome and a type-check of the listening page, the drift gates (the ABI layout snapshot, the ABI version mirrors, the generated processor types, cross-surface parity), and `make lint-static` |
| `Python tools` | both | The Python tools' own tests: the instrument-bank calibration harness under `tools/voicematch` and `tools/audition`, and the SOFA-to-HRTF converter under `tools/playback` |
| `Native C++` | both | A Debug build and the default ctest run; Linux on `develop`, Linux and macOS on `main` |
| `Installed package` | both | Installs into a scratch prefix and links each exported archive on its own |
| `Examples` | `develop` | The Python and C++ examples |
| `Python wheel` | `develop` | A manylinux x86_64 wheel build |
| `ASan/UBSan` | `main` | The suite under the address and undefined-behaviour sanitizers |
| `Coverage` | `main` | The default ctest run under coverage instrumentation, uploaded to Codecov |
| `Python binding` | `main` | The Python binding's type checks and tests, and the capability catalog drift check |
| `WASM` | `main` | The emscripten build and the WASM binding's tests |
| `FFmpeg decode` | `main` | The optional FFmpeg decode path, which every other job compiles out |

## Checklist

This is what you can reasonably include without knowing the repository. The maintainer checks it and fills in what is missing. For an area-specific change, [Reference](#reference) lists what the maintainer brings it to before merging.

- Build before testing; the Node, WASM and Python tests import built output.
- For a signature, enum or struct change, start from `include/sonare/` and bring the bindings to it.
- When you can, update the type declarations alongside the code: the `.pyi` files, `bindings/node/src/types.ts` and `bindings/wasm/src/public_types.ts`.
- Register a new C++ test file in `tests/CMakeLists.txt`, and run `sonare_tests` from the repository root.
- For a defect fix, show the assertion failing before the fix when you can.
- Compare float results across code paths within a tolerance derived from the values' scale, not with `==`.
- Use `sonare::constants::*` from `src/util/constants.h` rather than a universal numeric literal.
- C++17 only; no C++20 or later features, including `<numbers>`.
- Comments and documentation in English, in the language's doc-comment format: Doxygen for C and C++, TSDoc for TypeScript, PEP 257 docstrings for Python.
- Markdown is not hard-wrapped; one bullet is one logical line.
- Run `make format` before sending if you have the tools.

### Commit messages

Conventional Commits, in English: `type(scope): summary`, lowercase after the colon, no trailing period. A body, when there is one, is a blank line followed by `- ` bullets. Describe the change itself rather than the process that produced it. Commit messages can be rewritten at merge, so do not let them hold up a pull request.

## Reference

Nothing in this section is asked of you. It records what a merged change looks like in each area and why; the maintainer applies it when merging. Read it if a check puzzles you, or if you want to finish an area-specific change yourself.

### Builds and gates

Before merging, the maintainer brings a change here to the following:

- The Node and WASM packages are built through their package scripts (`yarn build`, `yarn build:ts`) rather than a bare `tsc`.
- A deliberately modified WASM module is built in a separate `git worktree` at the same path depth as the repository.
- `make lint` has run after any edit to a `.pyi` stub or TypeScript type file.
- `ruff` runs over the repository rather than a path list, so a Python error in `tools/`, `tests/` or `benchmarks/` fails the `lint` job too.
- The WASM test run refuses to start against a `dist/` older than its sources.
- A TypeScript declaration emit only adds to `dist/`, so a deleted or renamed source leaves its `.d.ts` behind, and the npm tarball ships `dist/` whole. The package scripts clear it; a bare `tsc` does not, and nothing fails while the orphan is there.
- A WASM build overwrites `bindings/wasm/dist/` wherever its build directory lives, and every surface's tests import it. Emscripten compiles its system libraries through a path relative to the build directory, so a worktree under `/tmp` fails to link for a reason unrelated to your change.
- The drift gates need only `python3`. `make ci-local` and `make format` need Node and uv, because they reach the layout snapshot through uv and the binding linters through yarn.
- `make lint-static` holds the static and drift gates that read committed sources alone: the generated tables' drift checks, the GS EFX coverage equation and its include-scope and skeleton pins, the coefficient-name and enum-cast greps, and the numeric-domain rows. `make lint` and the CI `lint` job both run it, so a push is gated on the list a local lint runs.
- `make lint` type-checks the typed surfaces: `tsc` over the Node and WASM bindings and their tests (`make check-js-types`, against the declarations already in `dist/`), and `mypy --strict` for Python. A Python stub is invisible to every other gate: ruff does not read it, no test imports it, and the module answers at runtime whatever the stub says.
- `Installed package` is the only CI job that sees the library the way an external C++ consumer does. Every in-tree target links the whole set, so an archive missing a declared dependency still links in tree.

### Across the surfaces

Before merging, the maintainer brings a change here to the following:

- A JS/TS helper module that no entry point re-exports lives under `bindings/{node,wasm}/src/internal/`, not as a top-level `src/_name.ts`.
- A rename has found every caller in two passes: a language server for the C++ tree, and a grep of the whole tree for the bindings, `benchmarks/` and `tools/`.
- The Node addon reads every argument and options key through the shared readers in `sonare_wrap_options.h` (`*Property`, `Optional*Arg`, `Required*Arg`), which refuse a value of the wrong type rather than substitute a default.
- Python APIs take keyword arguments and high-level JS/TS entry points take a request object, with field names and defaults matching across surfaces.
- A JS/TS one-shot over samples takes `sampleRate` and refuses it outside the audio sample-rate domain even when its computation does not read it, so every one keeps the `(samples, sampleRate, options?)` shape.
- A helper's declaration ships in `dist/` even when no entry point re-exports it, and a leading underscore is a Python convention, not a TypeScript one.
- Keyword arguments and request objects are deliberate per-surface idioms, not drift.

### Parity

Before merging, the maintainer brings a change here to the following:

- A fixed parity divergence takes its allowlist entry with it in the same change.
- Read a category's finding count beside its `compared` column. Zero findings out of zero comparisons renders like zero out of five thousand. `default` reaches a verdict on few candidates, because a default spelled inside a request-object normalizer is not in the signature the extractor reads. A category that reaches no verdict at all fails the run.
- An allowlist entry that excuses nothing fails the audit, and a stale one keeps excusing whatever symbol later takes its name.
- A helper is on the surface only if a value re-export reaches it. `export type { T } from './_helpers'` publishes `T` and nothing else.
- Full documentation: `tools/parity/README.md`.

### Errors and arguments

Before merging, the maintainer brings a change here to the following:

- The cause determines the error class. Node and WASM raise `TypeError` for a wrong type, invalid shape or undeclared key and `RangeError` for an out-of-domain value. Python raises `TypeError` for a wrong type or shape and `SonareValueError` (a `ValueError`) for an out-of-domain value or unknown name.
- A call's shape is checked before its values: on Node and WASM a missing or wrong-typed field, an undeclared key or a surplus argument is a `TypeError` raised before any `RangeError`, whichever call form is used.
- A JSON input is a document only when the library writes that format or publishes its schema and the entry loads it (a project, a mixer scene, a preset, a playback configuration): the C parameter is `document` and its refusal is `InvalidFormat` at a document path. Any other JSON input is an argument: C spells it `<root>_json`, each facade `<root>`, and every refusal, a parse failure included, is `InvalidParameter` named from `<root>` — also when its shape is part of a written format (`overrides`, a strip's `settings`).
- A typed object a facade serializes into a document parameter is an argument. Refusals name the caller's argument path. Node and WASM choose `TypeError` or `RangeError` from the refusal kind. Python raises `SonareValueError` when the native reader refuses the serialized contents.
- A value in the library's own output that a binding's reader cannot accept (an ordinal or token outside the binding's table, a null that names no infinite end, a result failing the reader's check) is `InvalidFormat` on every surface. `Unknown` is only a failure whose cause has no code.
- A C-ABI `INVALID_PARAMETER` becomes an argument error. Python translates that code to `SonareValueError`. Other C-ABI failures (object state, unsupported feature, allocation, decoding) raise `SonareError` carrying the code.
- A loader refusing the content of a document it was handed (a form the library writes or ships a schema for, such as a project, scene or chain config, or HRTF bytes) reports `INVALID_FORMAT`.
- JSON that is only one call's arguments (insert `params`, `overrides`, a part rig's `inserts`, a MIDI-FX `config`) is an argument, so even malformed text there is `INVALID_PARAMETER`; a reader shared by both takes its code from the caller.
- A null-handle constructor reads its error code from `sonare_last_error_code()` rather than hard-coding one.
- "Not given" is absence (omitted on Node and WASM, `None` in Python), and a negative position is refused rather than read as a sentinel.
- Edit-range requests take a `*Sec` (`*_sec`) spelling beside `*Sample`, and a request giving both spellings of one bound is refused.
- `InvalidState` covers a released or uninitialised handle, `NotSupported` an unsupported operation or an unavailable feature, and `AbiMismatch` a native module whose ABI version disagrees with the binding, checked when the module loads. Every C failure code keeps its own name when a binding or the CLI rethrows it (`tests/conformance/c_error_codes.json`). The core's `ErrorCode` carries `AbiMismatch` and `Unknown` only for that, and nothing in the core raises either.
- An out-of-domain value includes two buffers whose lengths must match.
- A negative position is refused because a sentinel means different things on different surfaces, and a negative typed by mistake must not edit nothing. A value whose `-1` is a real quantity (a −1 dBTP ceiling) is not a sentinel. The C ABI keeps samples and its own `-1` conventions, which the facades map to.
- `*Sec` values are converted at the call's sample rate and rounded to the nearest sample.

### Thresholds and analysis windows

Before merging, the maintainer brings a change here to the following:

- A relative threshold is never taken against the bare maximum of the whole input: local contrast uses a sliding maximum, body or summary decisions use a maximum that sets aside the top few events, and an absolute floor sits beside either.
- A high-level analyzer holds its window lengths as time quantities and rescales them once at its entry; librosa-named functions keep plain sample counts.
- A threshold against the bare maximum lets a single dominant event set the bar for everything else. Boundary detection's `referenceWindowSec` is the sliding instance; beat trimming's third-highest onset peak is the set-aside instance.
- A sliding reference also sets aside a lone dominant event, one no other event in the window comes within a fixed ratio of. That ratio is held apart from the threshold, so raising the threshold never lowers the reference. With too few events to tell an outlier from the body, nothing is set aside (the assistant sets aside one per 10 s of audio, up to two).
- An analyzer's `n_fft` is samples at 22050 Hz, rescaled to the rate the analyzer runs at. Its hop is in samples of the signal it analyses: the caller's buffer for the direct `analyze_*` entry points (BPM, rhythm, chords, timbre, dynamics, melody), and the analysis-rate signal — the input resampled to 22050 Hz when it is above that rate, the input itself otherwise — for the entry points that resample first (`detect_*`, sections, boundaries, the full music analysis). The polyphony chain and the streaming analyzer state `n_fft` (and, for polyphony, the hop) as samples at 44100 Hz. The mastering and mixing assistants read window and hop as samples at 48 kHz.

### Public API changes

Before merging, the maintainer brings a change here to the following:

- Each operation has one C entry with its optional parameters in a `Sonare<Op>Options` struct, and no `_ex`, `_with_<thing>` or other suffixed twin, except the closed channel-form (`_stereo`, `_linked`, `_interleaved`) and sample-format (`_u8`, `_i16`) classes.
- A twin that adds an output is folded into an option on every surface, named after the output (`withMonitor`/`with_monitor`, `withTimeline`, `withResidual`, `db`), with one result type carrying the optional output.
- Every caller-filled C struct starts with `uint32_t struct_size` and has a `sonare_<struct>_init()`; a NULL options pointer reads as the defaults.
- An in-memory count is `size_t`, a sample position `int64_t`, a sample rate `int` and a boolean `int32_t`; the auto hop is spelled `0`.
- A running tally of events since start or arm (xruns, overruns, underruns, overflows, dropped frames or events) is `uint64_t`, in a status record or from a getter named for it, because it outgrows a 32-bit `size_t` in ordinary use.
- "Not given" is absence on every surface, not a magic value; an optional C float that has no default is NaN.
- A name carries its unit (`Sec`, `Hz`, `Db`, `Samples`); a bare `peak` or `rms` is a linear amplitude and is listed in `tools/api/dimensionless_names.toml` with class `magnitude`.
- A bank select is `{ msb, lsb }` on Node and WASM and `bank_msb`/`bank_lsb` in Python.
- A type that mirrors part of a platform interface carries `@mirrors <Interface>` (for example `@mirrors AudioContext`); the members the interface also declares keep the platform's names, and the surface check lists them instead of flagging them.
- A refusal names the real parameter (`options.<member>`, `config.<member>`). See [Errors and arguments](#errors-and-arguments) for the exception class.
- A cause two parameters share names both (`left and right have no measurable level`); `make test-python` fails on a refusal whose subject is no parameter of its entry.
- `make surface-check` returns rc=0 on a change that touches a public name, token, default or flag; CI runs it too.
- A renamed, removed or reshaped public name has its row in the surface's ledger under `tools/api/migration/`, rendered by `make upgrade-notes`.
- `UPGRADING.md` is generated from the ledgers, and `make lint` (through `make lint-static`, which CI runs) fails while the tracked file differs from the render, so a ledger edit and `make upgrade-notes` land in one commit. A row that quotes the member's new signature (`arm(id, state?)`) matches its declaration on Node, WASM and Python.
- A shipped struct, signature, constant or enumerator that changes bumps its owning ABI counter, measured against the last release tag with `python3 tools/abi/check_abi_bumps.py --tag <last release tag>`.
- A breaking change ships only in a major release, with its ledger row and the regenerated `UPGRADING.md`.
- A capability-catalog field change has run the `[catalog]` C++ cases, `make capability-catalog` and `python3 tests/conformance/check_result_schema_surfaces.py`.
- Every closed token field in `schemas/mixer-scene.schema.json` carries the loader's exact set as an `enum`, a shared set narrowed at one position with `not: { enum: [...] }`.
- `make scene-token-cases` generates corpus cases from those sets that load each member and refuse a non-member on every surface.
- A JSON result field that can be infinite has its path and the end a null stands for (`-Infinity` or `Infinity`) listed in that result's `<family>_null_means()` beside its path list, and `make result-null-means` has regenerated the table.
- Every facade reads the generated table and restores that end. A measurement not taken stays an omitted key (`None` in Python), `NaN` crosses no JSON record and no scalar result field (an unmeasured element of a numeric array measured as a whole is the one place it stays, at full length, with the field's doc saying so), and a C++ conformance case refuses a null at an undeclared path.
- One entry per operation keeps a caller from choosing between near-identical functions; the closed suffix classes are the only twins whose trailing parameters and result type match their base.
- A single `struct_size` lets every later field extend a struct without a second versioning scheme, and a size below `sizeof` is refused so the library never reads past the caller's struct.
- Absence, not a sentinel, is the only spelling that means the same thing on every surface; the C ABI keeps `0` or NaN as its own spelling and each facade maps to absence.
- `make surface-check` checks names, units, tokens, entry shape and the ledgers against the migration baseline; `make check-abi-bumps` needs release tags, so it runs in `make ci-local` and not on a shallow CI checkout.
- ABI counters are separate from the release version: a release never bumps one, and a struct absent from every release tag may still grow.
- A catalog field is emitted once, by the writer, and listed again in three literal path lists, the JSON Schema, the shared type shapes and each surface's types. The lists stay literal because the surface check reads them as text; the `[catalog]` cases hold each list to the writer.

### The command-line front-ends

Before merging, the maintainer brings a change here to the following:

- A new subcommand is classified in `tests/conformance/cli_contract_v2.json` in the same change as its implementation.
- A command that exists on one front-end only carries a `reason_kind` (`by_design` or `unported`) and a sentence saying which; a `by_design` reason names the route the other front-end already has.
- A Python-only command is `by_design` only when a named native route exists; otherwise it is `unported`.
- CLI stdout carries snake_case keys, except in a document fed back to the library verbatim.
- `make conformance` on a manifest change is trusted only once both `build/bin/sonare-cli` and the Python virtualenv are built.
- The manifest is the ledger for both front-ends, the native `sonare-cli` and the Python `sonare`. `shared` is the expensive classification, because it also pins an option contract in `inventory.expected_options` and is compared against both binaries.
- "Reach it from the library instead" holds for a native-only command because a Python CLI user has the library in the same language and process. The native binary's user has no toolchain, so a capability missing there has no fallback.
- An `unported` reason expires with the port, the way an allowlist entry expires with its divergence.
- The core's JSON producers emit camelCase; `analysis_json_for_cli()` (native) and `_json_keys_to_snake_case()` (Python) re-key for the CLI.
- The live comparison is skipped, and says so, when either binary is missing.

### The C ABI

Before merging, the maintainer brings a change here to the following:

- The ABI version moves when a shipped struct layout or a shipped C function signature changes, and not otherwise. `git grep -l <StructName> <last-tag> -- include/` shows whether a struct has shipped, and `make check-abi-version` checks the mirrors.
- A changed C struct has its `ctypes.Structure` mirror updated and the snapshot regenerated by `make abi-layout`, in the same change.
- A new translation unit under `src/c_api/` takes the source-list rows of its closest existing sibling.
- A C-ABI translation unit moved between library targets has been checked with `nm -gU` on the built shared library.
- Every exported C function opens with `SONARE_C_API_ENTRY`, keeps everything that can throw inside `SONARE_C_TRY` / `SONARE_C_CATCH*`, and writes out-params only after every allocation has succeeded; `make conformance` runs the entry-guard scan.
- An entry whose subsystem is compiled out refuses through `sonare_c_detail::refuse_not_compiled_in` (or its `_handle` / `refuse_value_` forms), which records `<feature> support is not compiled in`; `make conformance` flags a bare NOT_SUPPORTED in a feature-off branch.
- Every pointer in a public header has its lifecycle in its own doc comment: who frees it, which `sonare_free_*` releases it, or what ends the library's retention of a caller's buffer. A conformance gate enforces these comments.
- A struct that has never appeared in a release can grow a field freely. A shipped one cannot: a consumer built against the released header still holds the old size and offsets. A shipped function signature changed under the same symbol lets an old caller pass shifted arguments.
- General C function contracts use `SONARE_FEATURE_ABI_VERSION`; subsystem contracts use their own counter. There are seven counters, mirrored in several places. `make check-abi-version` checks the hand-written mirrors; `src/arrangement/edit_compiler.h` is pinned by a `static_assert` instead, so a missed bump there arrives as a compile error.
- ctypes has no `static_assert`, so a C struct changed without its Python mirror segfaults pytest instead of failing a test.
- The Python loader takes `SONARE_LIB_PATH`, then the in-tree builds (`build-python-shared/lib` before `build/lib`), then the package-adjacent copy, then the system lookup. A `SONARE_LIB_PATH` that names a missing file is refused rather than skipped, so a typo cannot fall through to a different build. `conftest.py` refuses to start when the resolved in-tree library is older than the newest file under `src/` (outside `src/wasm/`) and `include/`, since a rebuild-less edit keeps the same ABI version. A library outside the checkout is not judged; `SONARE_ALLOW_STALE_LIB=1` opts out. `tests/conformance/test_playback_cli.py` takes `sonare-cli` from the same build unless `SONARE_NATIVE_CLI` names one.
- Moving a C-ABI translation unit between library targets can remove an export from the shared library while everything builds and passes; it surfaces later as a `dlsym` failure in the parity run.
- An exception crossing the C boundary is undefined behaviour, and a last error left from an earlier call is read as this call's failure. `tests/conformance/check_c_api_entry_guard.py` classifies every exported name; the arithmetic-only converters it exempts are listed in the script, and the audio-thread process entries use `SONARE_C_RT_API_ENTRY` because touching the thread-local diagnostics can allocate.
- A C entry that refuses an argument names it in the last-error text. The exception is an entry its header documents as realtime-safe, which answers through the return code only because setting the text allocates. `tests/conformance/check_c_abi_bare_refusals.py` exempts a text-less refusal only where a public header says so; any other is counted and ratcheted.

### WASM

Before merging, the maintainer brings a change here to the following:

- Emscripten is raised by editing `bindings/wasm/.emscripten-version`; `build:wasm` refuses any other `emcc`, and a build tree first configured with another one, since CMake keeps the compiler it was first configured with — move that tree aside.
- No translation unit carries its own exception flag: the WASM build turns on native WebAssembly exceptions once, for every unit of both modules, in the top-level `CMakeLists.txt`.
- A thrown C++ exception reaches JavaScript as a `WebAssembly.Exception`; one function in `bindings/wasm/src/module_state.ts` turns it into the coded error, and no call site inspects a thrown value itself.

### Build options

Before merging, the maintainer brings a change here to the following:

- `make build-feature-matrix` has run after a source-list change or a new translation unit outside a feature gate.
- `make analysis-link-check` has run after a core or analysis source starts calling into another subsystem; it is part of `make ci-local`.
- A new library target is named in the export list at the foot of `src/CMakeLists.txt`.
- No Eigen header outside the MPL-2.0 set is included.
- Feature-gate breakage appears only in a build, and no CI job configures a feature-off build. `make build-feature-matrix` compiles each option off on its own plus one all-off row, linking shared libraries with undefined symbols refused on every platform; the all-off row catches a gated symbol that stays reachable through a second enabled feature.
- The analysis-only WASM module links `sonare_core`, `sonare_rt` and `sonare_pitch_editor` and nothing else, while every native target links the whole set, so a core symbol defined only in the engine, mixer, acoustic or arrangement archives links natively and fails only under Emscripten. `make analysis-link-check` whole-archive links those three into an empty executable so the undefined reference appears in a native build; `make analysis-link-check-wasm-options` repeats the link in a private tree configured with the module's own switched-off subsystems, read from its `build:wasm:analysis` script.
- A non-zero exit from the matrix does not mean a build failed. The summary names one of three cases: a row that failed to compile, a row that never configured because another default-on option requires it, or a row whose value a dependency rule forced back. The last two describe the option set, not the code. A row the summary does not name compiled clean.
- A library target missing from the export list builds and links in tree and is silently absent from `find_package`.
- Eigen is compiled with `EIGEN_MPL2_ONLY`, because a few headers under `unsupported/` are LGPL and libsonare ships Apache-2.0 with no copyleft in any distributed artifact. Including one fails with `Including non-MPL2 code in EIGEN_MPL2_ONLY mode`; the fix is to drop the header.

### Tests

Before merging, the maintainer brings a change here to the following:

- C++ cases over roughly two seconds are tagged `[.][slow]` and slow Python tests `@pytest.mark.slow`; shrinking the input is preferred.
- Golden tests run through `make test-golden` only, and a golden failure is confirmed not pre-existing before it is attributed to a change.
- A golden moves only in the same commit as the behavioural change that justifies it.
- A change under `src/midi/synth/` has run `make test-golden` locally and carries the synth manifests' diffs (program, gesture and drum kit) in the same commit.
- If a patch field or calibration constant moved, the regenerated value registry lands with it. `SONARE_UPDATE_KIT_GOLDEN=1` regenerates only `tests/midi/golden/gm_kit_hashes.tsv`, and `make test-golden` ends with `make bank-versions-check`.
- MIDI values are held at MIDI 2.0 width (`Velocity16` / `Control32` / `Bend32`); a float reads `f7()` / `f14()` and an integer decision `u7()` / `u14()`. MIDI 1.0 renders stay bit-identical (`tests/midi/golden/midi1_control_hashes.tsv`).
- A test showing that a parameter changed a result compares every field the operation measures.
- `sonare_tests` loads reference fixtures by relative path.
- Golden tests are excluded from the default run, and their hashes cannot match across architectures. The digests are recorded at Release, so `sonare_tests "[golden]"` from a Debug tree compares two optimization levels and fails on a small, stable subset. Each case's failure output says which kind of run produced it.
- The instrument bank's value registry fingerprints tunable values, so an engine-code change moves no version and only the synth goldens see it. `gm_program_hashes.tsv` covers each program with no controller input; `gm_gesture_hashes.tsv` covers each engine, and each patch-gated branch, under one controller gesture per row; `gm_kit_hashes.tsv` covers every voiced drum key of kit 0 at two velocities and each GS kit voiced apart from it on one key per piece class. No CI job runs them.
- CI runs on x86_64 and aarch64 under GCC and clang, whose float rounding differs; a single field can come out bit-identical on one platform.
- No test or tool reads documentation. README examples, counts and stated ranges are transcribed by hand, so a prose edit never turns a build red.

### Numeric constants

Before merging, the maintainer brings a change here to the following:

- In a `.cpp`, the constants come in through a `using` declaration inside the file's namespace; a header fully qualifies them and has no `using namespace`.
- A literal is replaced only where it means what the constant means; the same value is not the same constant.

### Sample-rate-dependent values

Before merging, the maintainer brings a change here to the following:

- A sample-rate-dependent value is stored as the physical quantity (hertz, milliseconds, seconds, a ratio) and its coefficient is built in `prepare()`, never carried as a constant or as a linear rescaling of itself.
- `tests/effects/insert_sample_rate_test.cpp` keeps both assertions per quantity: each rate against the requested value, and the two rates against each other.
- `b1 = exp(-2*pi*fc/SR)` is not linear in `SR`, so a coefficient rescaled by the rate ratio is as wrong as one copied.
- `make lint` refuses a `*Config` field under `src/effects/` or `src/mastering/` named for a coefficient (`coeff`, `b1`, `a1`, `alpha`). That is a naming rule and catches only the obvious form; `tests/effects/insert_sample_rate_test.cpp` measures the behaviour at 44100 and 48000 Hz.
- The two assertions catch different defects. One defect makes both rates agree with each other and with neither requested value; another makes each rate right on its own while the two disagree. Which one sees a given defect is not knowable in advance.
- The classic GS realisation runs at a fixed 32 kHz internal clock with a resampler to the host rate, because its coefficients, delay steps and band limit are functions of that clock. The coefficient grep does not look under `src/midi/synth/gs_classic/`; `tests/midi/gs_classic_conformance_test.cpp` requires the same drawing at 44.1 and 48 kHz instead.

### The GS insertion effects

Before merging, the maintainer brings a change here to the following:

- The byte-to-physical-quantity conversions live in `src/midi/synth/gs_efx_convert.{h,cpp}`, as pure functions over a wire byte plus an enum naming the table.
- `src/midi/synth/gs_efx_tables.h` is regenerated in the same change as `tools/gs/efx-tables.json`, and neither is hand-edited.
- Every printed (type, slot) pair has exactly one form in `tools/gs/efx-bindings/*.json` under the rules in its `SCHEMA.md`, and `make gs-efx-coverage` has run.
- An EFX parameter's reset default is expressed through `gs_efx_parameter_reset_default`, not through a row's `def` byte or a comment.
- The conversions stay pure so they can be unit-tested against raw measured readings. A conversion that resolved its own table from a type and slot would make its test check one derivation against another.
- The tables are committed so a clone builds without fetching anything. `make gs-efx-tables-check` re-derives them from an external measurement archive (`GS_EFX_ARCHIVE`, no default) and reports a content difference separately from an archive that has moved on. A (type, slot) pair the archive does not reach gets no conversion; its stand-in is a `designed` binding row whose basis (`carried` or `invented`) is counted. Reading guide: `tools/gs/docs/efx-tables.md`.
- The three binding forms are `translated` (a measured law), `designed` (a law the library states, basis `carried` or `invented`) and `enables` (a byte that switches a stage or selects between stages). `make gs-efx-coverage` prints `GS EFX coverage: printed=<n> translated=<n> designed=<n> enables=<n>` and `GS EFX basis: carried=<n> invented=<n>`, and fails while they do not add up.
- The tally balances with any classification, so the tests also drive each row's type through the chain and require the behaviour its form claims, and require the named control to be one the insert publishes as realtime-automatable or to sit on a reasoned exception list.
- `tools/gs/marks_from_manual.py` transcribes the manual's `+`/`#` marks and printed names onto the rows; it is the only tool that opens the archive's `documents/` tree.
- The insertion effect has two realisations over one protocol layer. Modern, the default, maps each parameter onto the library's own inserts; classic runs the archive's whole-type model of each effect under `src/midi/synth/gs_classic/`. A host selects one per instrument (`gs_efx_realization` in the C ABI and Python, `gsEfxRealization` on Node and WASM); the native CLIs do not expose it. `tools/gs/docs/classic.md` lists the per-type differences.
- Writing an EFX type loads that type's own parameter bytes, which is measured behaviour, so the reset default is a function of the type. The address table's self-check holds the rows to `gs_efx_parameter_reset_default`; an exception stated in a comment would be invisible to it.

### Style

Before merging, the maintainer brings a change here to the following:

- A TSDoc `@example` is one ```ts fence that compiles as written, declaring any input it assumes beyond the audio it works on; `make doc-examples` type-checks every one against the package sources.
- C++ comments are terse: at most three lines above a constant (the measurement that set it), one line inside a function. A binding's public API documentation is the exception.
- No per-file licence headers; the licence lives in `LICENSE` and `NOTICE`.
- Documentation describes the current state, not the change history.
- A function that mirrors an established analysis API matches its defaults (`sr=22050`, `n_fft=2048`, `hop_length=512`).

## Reporting issues

Search the existing issues first, then include what you can of: what you did, what happened, what you expected, the version and which binding, your OS and its display language, and a minimal reproduction. For an analysis result that looks wrong, the input file matters more than anything else in the report.

Security vulnerabilities go through the process in [SECURITY.md](SECURITY.md), not the public issue tracker.

## Licence

By contributing, you agree that your contributions are licensed under the Apache License 2.0.
