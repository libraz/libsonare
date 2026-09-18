# Runtime capability matrix

One C++ core, four hand-written runtimes. This table is what "the same engine everywhere" means concretely: per domain, how many of the C ABI's entry points each runtime can reach.

**Generated — do not edit.** Run `make surface-coverage` to regenerate; `make surface-coverage-check` fails on a stale copy. The reachability decision is the parity checker's, so class methods, handle-prefix renames and verified aliases all count as reached; see [README.md](README.md) for how that decision is made.

A gap here is a statement about reach, not about quality: both CLIs are a curated subset by design, and WASM cannot expose the host filesystem or anything that needs threads. An allowlisted divergence still counts as a gap, because a reviewed absence is still an absence.

The two command-line front-ends get a column each because they are two binaries: the Python `sonare` CLI and the native `sonare-cli`. The parity checker compares their union against the C ABI, so a capability only one of them ships is not drift there — this table is where that difference is visible. Which commands the two must keep identical is a separate contract, in `tests/conformance/cli_contract_v2.json`.

| domain | C entry points | Python | Node | WASM | CLI (python) | CLI (native) |
|---|---:|---:|---:|---:|---:|---:|
| assist | 3 | 0/3 | 0/3 | 0/3 | 0/3 | 0/3 |
| core (analysis, IO, conversion) | 63 | 47/63 | 47/63 | 44/63 | 16/63 | 16/63 |
| creative effects | 45 | 43/45 | 43/45 | 43/45 | 17/45 | 17/45 |
| feature extraction | 139 | 122/139 | 122/139 | 122/139 | 25/139 | 49/139 |
| mastering | 105 | 95/105 | 95/105 | 97/105 | 11/105 | 9/105 |
| metering | 40 | 40/40 | 38/40 | 40/40 | 7/40 | 7/40 |
| mixing & routing | 57 | 55/57 | 55/57 | 55/57 | 2/57 | 2/57 |
| polyphony | 13 | 13/13 | 13/13 | 13/13 | 4/13 | 4/13 |
| project & arrangement | 142 | 134/142 | 133/142 | 133/142 | 10/142 | 7/142 |
| realtime engine | 126 | 124/126 | 124/126 | 124/126 | 5/126 | 5/126 |
| room acoustics | 5 | 5/5 | 5/5 | 5/5 | 5/5 | 5/5 |
| sample bank | 6 | 6/6 | 6/6 | 6/6 | 2/6 | 2/6 |
| streaming | 34 | 32/34 | 32/34 | 32/34 | 8/34 | 8/34 |
| transcription | 4 | 3/4 | 3/4 | 3/4 | 2/4 | 2/4 |
| voice changer | 20 | 20/20 | 20/20 | 19/20 | 3/20 | 3/20 |
| **all domains** | **802** | **739/802** | **736/802** | **736/802** | **117/802** | **136/802** |
