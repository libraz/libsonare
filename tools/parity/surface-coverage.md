# Runtime capability matrix

One C++ core, four hand-written runtimes. This table is what "the same engine everywhere" means concretely: per domain, how many of the C ABI's entry points each runtime can reach.

**Generated — do not edit.** Run `make surface-coverage` to regenerate; `make surface-coverage-check` fails on a stale copy. The reachability decision is the parity checker's, so class methods, handle-prefix renames and verified aliases all count as reached; see [README.md](README.md) for how that decision is made.

A gap here is a statement about reach, not about quality: the CLI is a curated subset by design, and WASM cannot expose the host filesystem or anything that needs threads. An allowlisted divergence still counts as a gap, because a reviewed absence is still an absence.

| domain | C entry points | Python | Node | WASM | CLI |
|---|---:|---:|---:|---:|---:|
| assist | 3 | 0/3 | 0/3 | 0/3 | 0/3 |
| core (analysis, IO, conversion) | 56 | 46/56 | 46/56 | 44/56 | 16/56 |
| creative effects | 39 | 38/39 | 38/39 | 38/39 | 16/39 |
| feature extraction | 135 | 119/135 | 119/135 | 119/135 | 48/135 |
| mastering | 100 | 90/100 | 90/100 | 92/100 | 8/100 |
| metering | 40 | 40/40 | 38/40 | 40/40 | 7/40 |
| mixing & routing | 50 | 50/50 | 50/50 | 50/50 | 1/50 |
| polyphony | 13 | 13/13 | 13/13 | 13/13 | 4/13 |
| project & arrangement | 141 | 134/141 | 133/141 | 133/141 | 10/141 |
| realtime engine | 126 | 124/126 | 124/126 | 124/126 | 5/126 |
| room acoustics | 5 | 5/5 | 5/5 | 5/5 | 5/5 |
| sample bank | 5 | 5/5 | 5/5 | 5/5 | 1/5 |
| streaming | 33 | 31/33 | 31/33 | 31/33 | 7/33 |
| transcription | 3 | 3/3 | 3/3 | 3/3 | 2/3 |
| voice changer | 20 | 20/20 | 20/20 | 19/20 | 3/20 |
| **all domains** | **769** | **718/769** | **715/769** | **716/769** | **133/769** |
