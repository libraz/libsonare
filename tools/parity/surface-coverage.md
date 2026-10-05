# Runtime capability matrix

One C++ core, four hand-written runtimes. This table is what "the same engine everywhere" means concretely: per domain, how many of the C ABI's entry points each runtime can reach.

**Generated — do not edit.** Run `make surface-coverage` to regenerate; `make surface-coverage-check` fails on a stale copy. The reachability decision is the parity checker's, so class methods, handle-prefix renames and verified aliases all count as reached; see [README.md](README.md) for how that decision is made.

A gap here is a statement about reach, not about quality: both CLIs are a curated subset by design, and WASM cannot expose the host filesystem or anything that needs threads. An allowlisted divergence still counts as a gap, because a reviewed absence is still an absence.

The two command-line front-ends get a column each because they are two binaries: the Python `sonare` CLI and the native `sonare-cli`. The parity checker compares their union against the C ABI, so a capability only one of them ships is not drift there — this table is where that difference is visible. Which commands the two must keep identical is a separate contract, in `tests/conformance/cli_contract_v2.json`.

| domain | C entry points | Python | Node | WASM | CLI (python) | CLI (native) |
|---|---:|---:|---:|---:|---:|---:|
| assist | 3 | 0/3 | 0/3 | 0/3 | 0/3 | 0/3 |
| core (analysis, IO, conversion) | 64 | 47/64 | 47/64 | 44/64 | 16/64 | 16/64 |
| creative effects | 46 | 44/46 | 44/46 | 44/46 | 17/46 | 17/46 |
| feature extraction | 140 | 123/140 | 123/140 | 123/140 | 25/140 | 49/140 |
| mastering | 113 | 109/113 | 109/113 | 111/113 | 14/113 | 12/113 |
| metering | 40 | 40/40 | 38/40 | 40/40 | 7/40 | 7/40 |
| mixing & routing | 57 | 55/57 | 55/57 | 55/57 | 2/57 | 2/57 |
| playback | 22 | 22/22 | 22/22 | 22/22 | 8/22 | 8/22 |
| polyphony | 13 | 13/13 | 13/13 | 13/13 | 4/13 | 4/13 |
| project & arrangement | 165 | 157/165 | 156/165 | 156/165 | 12/165 | 9/165 |
| project part rig | 3 | 3/3 | 3/3 | 3/3 | 0/3 | 0/3 |
| realtime engine | 173 | 170/173 | 170/173 | 170/173 | 5/173 | 5/173 |
| room acoustics | 5 | 5/5 | 5/5 | 5/5 | 5/5 | 5/5 |
| sample bank | 6 | 6/6 | 6/6 | 6/6 | 2/6 | 2/6 |
| streaming | 34 | 32/34 | 32/34 | 32/34 | 8/34 | 8/34 |
| transcription | 4 | 3/4 | 3/4 | 3/4 | 2/4 | 2/4 |
| vocal edit | 58 | 44/58 | 44/58 | 44/58 | 11/58 | 11/58 |
| vocal project | 10 | 5/10 | 5/10 | 5/10 | 2/10 | 2/10 |
| voice changer | 20 | 20/20 | 20/20 | 19/20 | 3/20 | 3/20 |
| **all domains** | **976** | **898/976** | **895/976** | **895/976** | **143/976** | **162/976** |
