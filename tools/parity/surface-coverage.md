# Runtime capability matrix

One C++ core, four hand-written runtimes. This table is what "the same engine everywhere" means concretely: per domain, how many of the C ABI's entry points each runtime can reach.

**Generated — do not edit.** Run `make surface-coverage` to regenerate; `make surface-coverage-check` fails on a stale copy. The reachability decision is the parity checker's, so class methods, handle-prefix renames and verified aliases all count as reached; see [README.md](README.md) for how that decision is made.

A gap here is a statement about reach, not about quality: both CLIs are a curated subset by design, and WASM cannot expose the host filesystem or anything that needs threads. An allowlisted divergence still counts as a gap, because a reviewed absence is still an absence.

The two command-line front-ends get a column each because they are two binaries: the Python `sonare` CLI and the native `sonare-cli`. The parity checker compares their union against the C ABI, so a capability only one of them ships is not drift there — this table is where that difference is visible. Which commands the two must keep identical is a separate contract, in `tests/conformance/cli_contract_v2.json`.

| domain | C entry points | Python | Node | WASM | CLI (python) | CLI (native) |
|---|---:|---:|---:|---:|---:|---:|
| assist | 3 | 0/3 | 0/3 | 0/3 | 0/3 | 0/3 |
| core (analysis, IO, conversion) | 69 | 50/69 | 50/69 | 47/69 | 16/69 | 16/69 |
| creative effects | 48 | 45/48 | 45/48 | 45/48 | 17/48 | 17/48 |
| feature extraction | 145 | 126/145 | 126/145 | 126/145 | 25/145 | 49/145 |
| mastering | 118 | 114/118 | 114/118 | 116/118 | 14/118 | 12/118 |
| metering | 40 | 40/40 | 38/40 | 40/40 | 7/40 | 7/40 |
| mixing & routing | 57 | 55/57 | 55/57 | 55/57 | 2/57 | 2/57 |
| playback | 22 | 22/22 | 22/22 | 22/22 | 8/22 | 8/22 |
| polyphony | 13 | 13/13 | 13/13 | 13/13 | 4/13 | 4/13 |
| project & arrangement | 167 | 159/167 | 159/167 | 159/167 | 12/167 | 9/167 |
| project part rig | 3 | 3/3 | 3/3 | 3/3 | 0/3 | 0/3 |
| realtime engine | 185 | 182/185 | 182/185 | 182/185 | 5/185 | 5/185 |
| room acoustics | 8 | 6/8 | 6/8 | 6/8 | 5/8 | 5/8 |
| sample bank | 6 | 6/6 | 6/6 | 6/6 | 2/6 | 2/6 |
| streaming | 34 | 32/34 | 32/34 | 32/34 | 8/34 | 8/34 |
| transcription | 4 | 3/4 | 3/4 | 3/4 | 2/4 | 2/4 |
| vocal edit | 58 | 44/58 | 44/58 | 44/58 | 11/58 | 11/58 |
| vocal project | 10 | 5/10 | 5/10 | 5/10 | 2/10 | 2/10 |
| voice changer | 20 | 20/20 | 20/20 | 19/20 | 3/20 | 3/20 |
| **all domains** | **1010** | **925/1010** | **923/1010** | **923/1010** | **143/1010** | **162/1010** |
