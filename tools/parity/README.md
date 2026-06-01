# Cross-binding parity checker

libsonare exposes one C++ core through four **hand-written** language surfaces —
the C ABI (`src/sonare_c.h` + `src/sonare_c_<domain>.{h,cpp}`), Node N-API
(`bindings/node/`), Python ctypes (`bindings/python/`) and WASM embind
(`src/wasm/bindings.cpp` + `bindings/wasm/`) — plus a curated Python CLI. Because
those surfaces are maintained by hand (there is no code generation; the YAML
codegen experiment was rolled back), they drift. This tool finds the drift.

It is **stdlib-only** (Python 3.11+ for `tomllib`), **read-only** (never edits
sources), and **opt-in** (run on demand; it is not wired into CI).

The **C API is the canonical reference**: it defines the authoritative function
name, parameter order and types. Every other surface is compared against it.

## Running

```bash
make parity                                  # the normal entry point
python3 tools/parity/check_parity.py         # markdown report (same thing)
python3 tools/parity/check_parity.py --json  # machine-readable findings
python3 tools/parity/check_parity.py --surface c,python   # limit surfaces (c always included)
```

Flags: `--json`, `--surface <csv>`, `--root <path>`, `--allowlist <path>`,
`--core-map <path>`. Exit code is **0** when there is no active drift, **1**
otherwise (CI-gate friendly).

## What it reports — seven categories

Categories 1–6 compare each facade against the **C API**. Category 7 is a
different axis: it compares the WASM binding against **itself** (its own three
files), catching a wiring break the C-anchored checks structurally cannot see.

1. **coverage** — a canonical C free function missing from a surface, or a
   surface symbol with no C counterpart. Handle/class APIs, `free_*` memory
   helpers and the curated CLI are reported as *informational* (see below).
2. **default** — Node / WASM / Python disagree on a parameter's default
   (facade-vs-facade; C carries no defaults).
3. **core_default** — a facade default diverges from the C++ **core** design
   default: the field initializer of the config struct, or the default argument
   of the free function, that backs the call. This catches the case the
   facade-vs-facade check is blind to — every facade agreeing on a value the
   core never intended, or one facade silently drifting from core. Driven by
   [`core_map.toml`](#extending-core_maptoml).
4. **order** — a surface's config parameter order/name/count diverges from the C
   canonical order (after stripping the leading audio-input group).
5. **input** — the audio-input params (`samples` / `sr` / `left` / …) are named
   inconsistently across surfaces.
6. **enum** — the accepted enum / string-union value set for a param differs
   across surfaces.
7. **wasm_internal** — the WASM binding is inconsistent across its own three
   files: an embind free-function registration (`src/wasm/bindings.cpp`), the
   `SonareModule` TS interface (`bindings/wasm/src/sonare.js.d.ts`) and the
   `index.ts` facade. *Active* when a registered free function is missing from
   the `SonareModule` type (TypeScript can't call it) or `index.ts` calls a
   `module.X` the type never declares; *informational* when a registered + typed
   function has no `index.ts` wrapper (often a raw entry superseded by a richer
   variant). This is the leg that would have caught **P0-4** (`analyzeSections`
   registered in embind but absent from both the type and the facade) — the
   other six checks read `index.ts` alone and so model "exposed in WASM" =
   "exported from `index.ts`", blind to a break upstream of it. Only
   FREE-function registrations are checked; class methods (`.function(...)`
   inside a `class_<T>()` chain) belong to bound class types, not `SonareModule`.

### Finding states

- **active** — counts toward the non-zero exit code (real, un-triaged drift).
- **informational** — reported but non-gating: handle/class methods, `free_*`
  helpers, CLI-only gaps, ergonomic facade-only methods.
- **allowlisted** — suppressed (and counted) via `allowlist.toml`; an
  intentional, reviewed divergence.

## Extending `core_map.toml`

`core_map.toml` links a canonical key (the C function name minus the `sonare_`
prefix) to where its **core** default lives. Two entry kinds:

```toml
# A: defaults live in a C++ config STRUCT's field initializers.
[map.analyze_melody]
header = "src/analysis/melody_analyzer.h"
struct = "MelodyConfig"

# B: defaults live in a free FUNCTION's default arguments.
[func.amplitude_to_db]
header = "src/core/db_convert.h"
cpp_func = "amplitude_to_db"

# Optional rename when the facade param name != the core field/param name.
[map.hpss.rename]
kernel_harmonic = "kernel_size_harmonic"
```

Workflow to add coverage for a function:
1. Find the C++ struct (or free function) that supplies its defaults.
2. Add a `[map.<key>]` (struct) or `[func.<key>]` (free function) block.
3. `make parity` and triage any new findings.

What the extractor compares (and deliberately skips):
- **Numeric / boolean** literals — compared.
- **Named constants** (`constants::kC1Hz`, `chord_constants::kFoo`) — resolved to
  their literal and compared. Only direct-literal `constexpr` constants resolve;
  computed ones are skipped.
- **Enum members** (`WindowType::Hann`, `TempogramMode::kAutocorrelation`) —
  folded to the member name (dropping a Google-style leading `k`) and compared
  to the facade's string spelling. If a facade spells the enum default as a bare
  **integer**, it is skipped (no enum int table — avoids false positives).
- **`std::nullopt` / `nullptr`** — folded to the `None` sentinel.
- **Strings, aggregates (`{…}`), unresolved names** — skipped.

A field a facade does not expose, and a sample-rate / buffer input param, are
never compared. Only overlapping, comparable defaults produce a finding.

## `allowlist.toml`

Intentional divergences, one section per category:
`[coverage]`, `[surface_only]`, `[order]` (each keyed by surface → list of
keys), `[default]` / `[core_default]` / `[enum]` (lists of `"key.param"`),
`[input_naming]` (list of keys), `[wasm_internal]` (list of `names` whose
intra-binding wiring inconsistency is intentional). `[tuning]` overrides the
central knobs (`input_roles`, `handle_prefixes`).

## Architecture

```
check_parity.py     entry point: load extractors + allowlist + core_map, build report
extractors/         one parser per surface:
  c_api.py            sonare_c.h + the sonare_c_<domain>.h headers it includes
  python_pyi.py       analyzer.pyi stubs
  node_ts.py / wasm_ts.py / ts_common.py   the TS facades (every export class body)
  cli.py              the curated CLI command surface
  cpp_struct.py       C++ core struct field inits + free-fn default args (+ constants)
  wasm_internal.py    the WASM binding's own 3 files (embind regs / SonareModule / index.ts)
core_defaults.py    loads core_map.toml, resolves each struct/func to its core defaults
model.py            FunctionSig / Param / Extraction data model (canonical keys)
normalize.py        name + default canonicalization (cross-surface equality)
compare.py          builds the matrix and the seven drift categories
report.py           markdown / JSON rendering
allowlist.py        loads allowlist.toml
```

## Limitations

It cannot verify: functions with no core-declared default (the facade adds one
the core lacks), method-based defaults (e.g. `Spectrogram::to_db`), enum defaults
a facade spells as a bare integer, and CLI defaults / argument names (the CLI is
a curated subset — coverage-only, informational). These are gaps in *coverage*,
not silent passes: an un-mapped function simply isn't checked for core-default
drift.

The `wasm_internal` check covers only WASM **free-function** wiring (embind
`function(...)` ↔ `SonareModule` ↔ `index.ts` `module.X`). It does NOT
cross-validate class-method registrations (`.function(...)` inside a
`class_<T>()` chain) against their bound-class interfaces, nor does it compare
embind argument *types/arity* — only the existence of the name across the three
layers. It also assumes the facade reaches the raw module via `module.X` or
`requireModule().X`; a method aliased through a differently-named local would
read as unwrapped (surfaced informationally, never as an active gap).
