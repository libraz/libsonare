# Runtime capability matrix

One C++ core, four hand-written runtimes. This table is what "the same engine everywhere" means concretely: per domain, how many of the C ABI's entry points each runtime can reach.

**Generated — do not edit.** Run `make surface-coverage` to regenerate; `make surface-coverage-check` fails on a stale copy. The reachability decision is the parity checker's, so class methods, handle-prefix renames and verified aliases all count as reached; see [README.md](README.md) for how that decision is made. A cell's `(n core)` counts entry points that runtime reaches by calling the core function the C entry wraps, skipping the entry's own validation.

A gap here is a statement about reach, not about quality: both CLIs are a curated subset by design, and WASM cannot expose the host filesystem or anything that needs threads. An allowlisted divergence still counts as a gap, because a reviewed absence is still an absence.

The two command-line front-ends get a column each because they are two binaries: the Python `sonare` CLI and the native `sonare-cli`. The parity checker compares their union against the C ABI, so a capability only one of them ships is not drift there — this table is where that difference is visible. Which commands the two must keep identical is a separate contract, in `tests/conformance/cli_contract_v2.json`.

| domain | C entry points | Python | Node | WASM | CLI (python) | CLI (native) |
|---|---:|---:|---:|---:|---:|---:|
| assist | 3 | 0/3 | 0/3 | 0/3 | 0/3 | 0/3 |
| core (analysis, IO, conversion) | 61 | 43/61 | 50/61 | 43/61 | 14/61 | 14/61 |
| creative effects | 57 | 40/57 | 52/57 | 41/57 | 16/57 | 16/57 |
| feature extraction | 132 | 115/132 | 124/132 | 116/132 | 49/132 | 47/132 |
| mastering | 134 | 118/134 | 125/134 (1 core) | 118/134 (1 core) | 25/134 | 25/134 |
| metering | 38 | 38/38 | 38/38 | 38/38 | 7/38 | 7/38 |
| mixing & routing | 61 | 58/61 | 60/61 | 60/61 | 2/61 | 2/61 |
| playback | 26 | 24/26 | 24/26 | 24/26 | 7/26 | 7/26 |
| polyphony | 14 | 13/14 | 14/14 | 13/14 | 4/14 | 4/14 |
| project & arrangement | 167 | 154/167 | 163/167 | 163/167 | 12/167 | 9/167 |
| project part rig | 3 | 3/3 | 3/3 | 3/3 | 0/3 | 0/3 |
| realtime engine | 190 | 183/190 | 190/190 | 181/190 | 5/190 | 5/190 |
| room acoustics | 12 | 7/12 | 7/12 | 7/12 | 5/12 | 5/12 |
| sample bank | 6 | 6/6 | 6/6 | 6/6 | 2/6 | 2/6 |
| streaming | 31 | 29/31 | 29/31 | 29/31 | 8/31 | 8/31 |
| transcription | 4 | 3/4 | 4/4 | 4/4 | 2/4 | 2/4 |
| vocal edit | 57 | 43/57 | 56/57 | 56/57 | 11/57 | 11/57 |
| vocal project | 10 | 6/10 | 10/10 | 10/10 | 2/10 | 2/10 |
| voice changer | 24 | 22/24 | 23/24 | 23/24 | 2/24 | 2/24 |
| **all domains** | **1030** | **905/1030** | **978/1030 (1 core)** | **935/1030 (1 core)** | **173/1030** | **168/1030** |
