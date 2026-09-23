# Runtime capability matrix

One C++ core, four hand-written runtimes. This table is what "the same engine everywhere" means concretely: per domain, how many of the C ABI's entry points each runtime can reach.

**Generated — do not edit.** Run `make surface-coverage` to regenerate; `make surface-coverage-check` fails on a stale copy. The reachability decision is the parity checker's, so class methods, handle-prefix renames and verified aliases all count as reached; see [README.md](README.md) for how that decision is made.

A gap here is a statement about reach, not about quality: both CLIs are a curated subset by design, and WASM cannot expose the host filesystem or anything that needs threads. An allowlisted divergence still counts as a gap, because a reviewed absence is still an absence.

The two command-line front-ends get a column each because they are two binaries: the Python `sonare` CLI and the native `sonare-cli`. The parity checker compares their union against the C ABI, so a capability only one of them ships is not drift there — this table is where that difference is visible. Which commands the two must keep identical is a separate contract, in `tests/conformance/cli_contract_v2.json`.

| domain | C entry points | Python | Node | WASM | CLI (python) | CLI (native) |
|---|---:|---:|---:|---:|---:|---:|
| assist | 3 | 0/3 | 0/3 | 0/3 | 0/3 | 0/3 |
| core (analysis, IO, conversion) | 64 | 47/64 | 47/64 | 44/64 | 16/64 | 16/64 |
| creative effects | 45 | 43/45 | 43/45 | 43/45 | 17/45 | 17/45 |
| feature extraction | 140 | 123/140 | 123/140 | 123/140 | 25/140 | 49/140 |
| mastering | 105 | 95/105 | 95/105 | 97/105 | 11/105 | 9/105 |
| metering | 40 | 40/40 | 38/40 | 40/40 | 7/40 | 7/40 |
| mixing & routing | 57 | 55/57 | 55/57 | 55/57 | 2/57 | 2/57 |
| polyphony | 13 | 13/13 | 13/13 | 13/13 | 4/13 | 4/13 |
| project & arrangement | 147 | 139/147 | 138/147 | 138/147 | 11/147 | 8/147 |
| realtime engine | 144 | 142/144 | 142/144 | 142/144 | 5/144 | 5/144 |
| room acoustics | 5 | 5/5 | 5/5 | 5/5 | 5/5 | 5/5 |
| sample bank | 6 | 6/6 | 6/6 | 6/6 | 2/6 | 2/6 |
| streaming | 34 | 32/34 | 32/34 | 32/34 | 8/34 | 8/34 |
| transcription | 4 | 3/4 | 3/4 | 3/4 | 2/4 | 2/4 |
| voice changer | 20 | 20/20 | 20/20 | 19/20 | 3/20 | 3/20 |
| **all domains** | **827** | **763/827** | **760/827** | **760/827** | **118/827** | **137/827** |
