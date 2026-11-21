# Runtime capability matrix

One C++ core, four hand-written runtimes. This table is what "the same engine everywhere" means concretely: per domain, how many of the C ABI's entry points each runtime can reach.

**Generated — do not edit.** Run `make surface-coverage` to regenerate; `make surface-coverage-check` fails on a stale copy. The reachability decision is the parity checker's, so class methods, handle-prefix renames and verified aliases all count as reached; see [README.md](README.md) for how that decision is made. A cell's `(n core)` counts entry points that runtime reaches by calling the core function the C entry wraps, skipping the entry's own validation.

A gap here is a statement about reach, not about quality: both CLIs are a curated subset by design, and WASM cannot expose the host filesystem or anything that needs threads. An allowlisted divergence still counts as a gap, because a reviewed absence is still an absence.

The two command-line front-ends get a column each because they are two binaries: the Python `sonare` CLI and the native `sonare-cli`. The parity checker compares their union against the C ABI, so a capability only one of them ships is not drift there — this table is where that difference is visible. Which commands the two must keep identical is a separate contract, in `tests/conformance/cli_contract_v2.json`.

| domain | C entry points | Python | Node | WASM | CLI (python) | CLI (native) |
|---|---:|---:|---:|---:|---:|---:|
| assist | 3 | 0/3 | 0/3 | 0/3 | 0/3 | 0/3 |
| core (analysis, IO, conversion) | 73 | 49/73 | 54/73 | 48/73 | 17/73 | 17/73 |
| creative effects | 60 | 48/60 | 59/60 | 49/60 | 20/60 | 20/60 |
| feature extraction | 148 | 127/148 | 131/148 | 128/148 | 52/148 | 50/148 |
| mastering | 142 | 128/142 | 135/142 (3 core) | 128/142 (3 core) | 24/142 | 24/142 |
| metering | 40 | 40/40 | 40/40 | 40/40 | 7/40 | 7/40 |
| mixing & routing | 62 | 59/62 | 60/62 | 60/62 | 2/62 | 2/62 |
| playback | 25 | 23/25 | 23/25 | 23/25 | 7/25 | 7/25 |
| polyphony | 14 | 13/14 | 14/14 | 13/14 | 4/14 | 4/14 |
| project & arrangement | 180 | 166/180 | 171/180 | 171/180 | 12/180 | 9/180 |
| project part rig | 3 | 3/3 | 3/3 | 3/3 | 0/3 | 0/3 |
| realtime engine | 198 | 188/198 | 196/198 | 188/198 | 5/198 | 5/198 |
| room acoustics | 13 | 8/13 | 8/13 | 8/13 | 6/13 | 6/13 |
| sample bank | 6 | 6/6 | 6/6 | 6/6 | 2/6 | 2/6 |
| streaming | 34 | 32/34 | 32/34 | 32/34 | 8/34 | 8/34 |
| transcription | 4 | 3/4 | 4/4 | 4/4 | 2/4 | 2/4 |
| vocal edit | 57 | 43/57 | 56/57 | 56/57 | 11/57 | 11/57 |
| vocal project | 10 | 6/10 | 10/10 | 10/10 | 2/10 | 2/10 |
| voice changer | 27 | 26/27 | 26/27 | 26/27 | 3/27 | 3/27 |
| **all domains** | **1099** | **968/1099** | **1028/1099 (3 core)** | **993/1099 (3 core)** | **184/1099** | **179/1099** |
