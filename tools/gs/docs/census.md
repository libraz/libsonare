# The GS address census

`src/midi/synth/docs/gs.md` promises that every address in the GS space is assigned, and that an address with no row is a defect rather than a silence. That promise needs data somebody else wrote to be worth anything: a hand-authored corpus contains only the addresses whoever wrote it already knew about, so it proves the table covers the table.

`extract_addresses.py` walks a directory of Standard MIDI Files and writes `address-census.json` — per address, how many messages landed on it, how many distinct files used it, the payload lengths, and the range of values seen.

## What is committed and what is not

**The census is committed; the corpus is not.** The census carries no filenames, no note data, no timing and no ordering — a histogram of 24-bit integers, which is not a derivative work of any composition. The files themselves are arrangements under their authors' own terms and cannot go in the repository, so a fresh clone gets the census and no way to regenerate it without fetching a corpus first.

The corpus lives under `.cache/gs-corpus/` (gitignored). The census records where it came from in its `source` field, so a number in it can be traced to a corpus rather than to a claim.

## Refreshing it

```sh
python3 tools/gs/extract_addresses.py \
  --corpus .cache/gs-corpus/mid \
  --out tools/gs/address-census.json \
  --source "<where the corpus came from, and when>"
```

The corpus used so far is a public collection of SMFs written for the SC-55, SC-88 and SC-88Pro by the Japanese DTM community of the 1990s and early 2000s, distributed as loose `.mid` entries plus `.lzh` / `.zip` archives holding about half as many again. Where to fetch it is a local note beside the corpus under `.cache/gs-corpus/`, not a repository fact: what this repository asserts is the census, and the distribution is somebody else's.

**The distribution ships many tunes both loose and inside an archive, so the extractor deduplicates by content hash.** Counting a file twice would weight the coverage ratchet by how a collection was packaged rather than by how many files reach an address; 1 382 of 5 408 paths were the same bytes as another.

**Both of the gate's ceilings are relative to one census.** Refreshing it over a larger corpus raises them by surfacing addresses nobody had seen — that is the census working, not coverage regressing. Re-record them in the same change, and compare the **ratio** across a refresh, never the count.

## Reading it

- **`messages`** counts every SysEx in the corpus by kind, not just the GS ones. It is where a message the census *declines* shows up: a bad checksum, a data byte with its high bit set, another Roland model, another manufacturer. **A census that skipped the checksum would invent addresses** — 238 messages in the corpus fail it — and the coverage gate would then demand a table row for each fiction.
- **`addresses`** is every concrete address, as `[address, count, files, len_min, len_max, value_min, value_max]`. Concrete rather than folded, because the gate resolves each one through the address table's own lookup and a wildcard has nothing to look up.
- **`groups`** folds the variable nibbles (part, EFX unit, drum map, drum note) for reading. Presentation only.

**`files` ranks better than `count`.** One file automating master volume over a fade contributes tens of thousands of writes to a single address; that says the address is hot, not that it is widely used. Sorting by `files` is what says which addresses a listener actually meets.

## What it is for

Two things, and the second is the one that pays immediately:

1. **The coverage gate.** Every address in the census must resolve to a table row. The count of those that do not is the number `gs.md` says must exist.
2. **Ordering the work.** The manual's own layout says nothing about what files send. The census does, and it disagrees with the layout: the system-effect block at `40 01 30`–`5A` is the densest region in real data by a wide margin, and the master EQ at `40 02 00`–`03` — four addresses — is reached by more files than most part parameters.
