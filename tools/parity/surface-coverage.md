# Runtime capability matrix

One C++ core, four hand-written runtimes. This table is what "the same engine everywhere" means concretely: per domain, how many of the C ABI's entry points each runtime can reach.

**Generated — do not edit.** Run `make surface-coverage` to regenerate; `make surface-coverage-check` fails on a stale copy. The reachability decision is the parity checker's, so class methods, handle-prefix renames and verified aliases all count as reached; see [README.md](README.md) for how that decision is made. A cell's `(n core)` counts entry points that runtime reaches by calling the core function the C entry wraps, skipping the entry's own validation.

A gap here is a statement about reach, not about quality: both CLIs are a curated subset by design, and WASM cannot expose the host filesystem or anything that needs threads. An allowlisted divergence still counts as a gap, because a reviewed absence is still an absence.

The two command-line front-ends get a column each because they are two binaries: the Python `sonare` CLI and the native `sonare-cli`. The parity checker compares their union against the C ABI, so a capability only one of them ships is not drift there — this table is where that difference is visible. Which commands the two must keep identical is a separate contract, in `tests/conformance/cli_contract_v2.json`.

| domain | C entry points | Python | Node | WASM | CLI (python) | CLI (native) |
|---|---:|---:|---:|---:|---:|---:|
| assist | 3 | 0/3 | 0/3 | 0/3 | 0/3 | 0/3 |
| core (analysis, IO, conversion) | 67 | 50/67 | 50/67 | 47/67 | 16/67 | 16/67 |
| creative effects | 49 | 45/49 | 45/49 | 45/49 | 18/49 | 18/49 |
| feature extraction | 145 | 127/145 | 127/145 | 127/145 | 25/145 | 49/145 |
| mastering | 134 | 128/134 | 128/134 (5 core) | 126/134 (3 core) | 28/134 | 26/134 |
| metering | 40 | 40/40 | 40/40 | 40/40 | 7/40 | 7/40 |
| mixing & routing | 62 | 60/62 | 59/62 | 59/62 | 2/62 | 2/62 |
| playback | 23 | 23/23 | 23/23 | 23/23 | 8/23 | 8/23 |
| polyphony | 14 | 14/14 | 14/14 | 13/14 | 4/14 | 4/14 |
| project & arrangement | 172 | 164/172 | 164/172 | 164/172 | 12/172 | 9/172 |
| project part rig | 3 | 3/3 | 3/3 | 3/3 | 0/3 | 0/3 |
| realtime engine | 190 | 187/190 | 187/190 | 187/190 | 5/190 | 5/190 |
| room acoustics | 12 | 10/12 | 8/12 | 8/12 | 6/12 | 6/12 |
| sample bank | 6 | 6/6 | 6/6 | 6/6 | 2/6 | 2/6 |
| streaming | 34 | 32/34 | 33/34 | 33/34 | 8/34 | 8/34 |
| transcription | 4 | 3/4 | 3/4 | 3/4 | 2/4 | 2/4 |
| vocal edit | 57 | 49/57 | 56/57 | 56/57 | 11/57 | 11/57 |
| vocal project | 10 | 10/10 | 10/10 | 10/10 | 2/10 | 2/10 |
| voice changer | 26 | 26/26 | 26/26 | 26/26 | 3/26 | 3/26 |
| **all domains** | **1051** | **977/1051** | **982/1051 (5 core)** | **976/1051 (3 core)** | **159/1051** | **178/1051** |
