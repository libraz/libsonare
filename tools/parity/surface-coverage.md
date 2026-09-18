# Runtime capability matrix

One C++ core, four hand-written runtimes. This table is what "the same engine everywhere" means concretely: per domain, how many of the C ABI's entry points each runtime can reach.

**Generated — do not edit.** Run `make surface-coverage` to regenerate; `make surface-coverage-check` fails on a stale copy. The reachability decision is the parity checker's, so class methods, handle-prefix renames and verified aliases all count as reached; see [README.md](README.md) for how that decision is made.

A gap here is a statement about reach, not about quality: both CLIs are a curated subset by design, and WASM cannot expose the host filesystem or anything that needs threads. An allowlisted divergence still counts as a gap, because a reviewed absence is still an absence.

The two command-line front-ends get a column each because they are two binaries: the Python `sonare` CLI and the native `sonare-cli`. The parity checker compares their union against the C ABI, so a capability only one of them ships is not drift there — this table is where that difference is visible. Which commands the two must keep identical is a separate contract, in `tests/conformance/cli_contract_v2.json`.

| domain | C entry points | Python | Node | WASM | CLI (python) | CLI (native) |
|---|---:|---:|---:|---:|---:|---:|
| assist | 3 | 0/3 | 0/3 | 0/3 | 0/3 | 0/3 |
| core (analysis, IO, conversion) | 57 | 47/57 | 47/57 | 44/57 | 16/57 | 16/57 |
| creative effects | 41 | 40/41 | 40/41 | 40/41 | 16/41 | 16/41 |
| feature extraction | 137 | 121/137 | 121/137 | 121/137 | 24/137 | 49/137 |
| mastering | 100 | 90/100 | 90/100 | 92/100 | 8/100 | 6/100 |
| metering | 40 | 40/40 | 38/40 | 40/40 | 7/40 | 7/40 |
| mixing & routing | 50 | 50/50 | 50/50 | 50/50 | 1/50 | 1/50 |
| polyphony | 13 | 13/13 | 13/13 | 13/13 | 4/13 | 4/13 |
| project & arrangement | 141 | 134/141 | 133/141 | 133/141 | 10/141 | 7/141 |
| realtime engine | 126 | 124/126 | 124/126 | 124/126 | 5/126 | 5/126 |
| room acoustics | 5 | 5/5 | 5/5 | 5/5 | 5/5 | 5/5 |
| sample bank | 5 | 5/5 | 5/5 | 5/5 | 1/5 | 1/5 |
| streaming | 33 | 31/33 | 31/33 | 31/33 | 7/33 | 7/33 |
| transcription | 3 | 3/3 | 3/3 | 3/3 | 2/3 | 2/3 |
| voice changer | 20 | 20/20 | 20/20 | 19/20 | 3/20 | 3/20 |
| **all domains** | **774** | **723/774** | **720/774** | **720/774** | **109/774** | **129/774** |
