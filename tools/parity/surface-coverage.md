# Runtime capability matrix

One C++ core, four hand-written runtimes. This table is what "the same engine everywhere" means concretely: per domain, how many of the C ABI's entry points each runtime can reach.

**Generated — do not edit.** Run `make surface-coverage` to regenerate; `make surface-coverage-check` fails on a stale copy. The reachability decision is the parity checker's, so class methods, handle-prefix renames and verified aliases all count as reached; see [README.md](README.md) for how that decision is made.

A gap here is a statement about reach, not about quality: the CLI is a curated subset by design, and WASM cannot expose the host filesystem or anything that needs threads. An allowlisted divergence still counts as a gap, because a reviewed absence is still an absence.

| domain | C entry points | Python | Node | WASM | CLI |
|---|---:|---:|---:|---:|---:|
| core (analysis, IO, conversion) | 56 | 46/56 | 46/56 | 44/56 | 17/56 |
| creative effects | 39 | 38/39 | 38/39 | 38/39 | 15/39 |
| feature extraction | 135 | 119/135 | 119/135 | 119/135 | 48/135 |
| mastering | 78 | 68/78 | 68/78 | 70/78 | 8/78 |
| metering | 28 | 28/28 | 26/28 | 28/28 | 7/28 |
| mixing & routing | 48 | 48/48 | 48/48 | 48/48 | 2/48 |
| polyphony | 12 | 12/12 | 12/12 | 12/12 | 4/12 |
| project & arrangement | 141 | 134/141 | 133/141 | 133/141 | 10/141 |
| realtime engine | 126 | 124/126 | 124/126 | 124/126 | 5/126 |
| room acoustics | 5 | 5/5 | 5/5 | 5/5 | 5/5 |
| sample bank | 5 | 5/5 | 5/5 | 5/5 | 1/5 |
| streaming | 33 | 31/33 | 31/33 | 31/33 | 7/33 |
| voice changer | 19 | 19/19 | 19/19 | 18/19 | 3/19 |
| **all domains** | **725** | **677/725** | **674/725** | **675/725** | **132/725** |
