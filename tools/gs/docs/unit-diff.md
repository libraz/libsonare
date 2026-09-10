# The address table against a measured unit

`src/midi/synth/docs/gs.md` names the SC-8850 as the target, and which manual a row was written from no longer decides what it says: the two maps have been compared address by address and agree on size, data range and power-on default everywhere they overlap, so only the nine points that page lists are model-dependent at all. What is left is the document against the machine.

`check_unit.py` is what closes that. It takes the table as the code holds it and an archive of what one individual machine answered, and reports where the two disagree.

**It decides nothing.** A disagreement is a question — a row transcribed wrongly, one of the nine points where the machines genuinely differ, or a limit of what the probe could see — and which of those it is comes from reading the row and the record. What the tool does is make the list finite.

## Why this is not the census

The two answer different questions and neither substitutes for the other.

`address-census.json` says what **real files reach**, and it is blind to any address they never send. The receive switches are the sharpest case: the machine answers all 288 of them — `40 1x 03`–`12` and `40 1x 23`–`24` — and the corpus of 2 233 files touches exactly one, in one file. A gate anchored on the corpus can be green while a whole block of the map has no row.

The unit diff says what **the machine answers**, and it is blind to everything the machine will not read back. Together they bound the table from both sides.

## Reading it

The table is read by including its header and printing it (`dump_address_table.cpp`), never by parsing the source: a parse would have to keep up with the row layout and the enumerator spellings, and would go wrong quietly the first time a field was added.

- **`addresses_with_no_row`** — the machine returned a value and neither a row nor an undefined range claims the address. Grouped by block, since a gap is normally a run.
- **`rows_no_read_reached`** — **not an absence claim.** A single-byte read that answers nothing leaves the address unproven, because a block read starting earlier reaches addresses a direct read does not, which is the archive's own finding about this unit. The list is for aiming. The address families the SC-8850 does not have are now named in `gs.md` rather than guessed at from here, so an entry falling inside one of them is expected and an entry outside them is the interesting kind.
- **`default_disagreements`** — the machine holds one value across every instance of the parameter and it is not what the table expects there.
- **`defaults_one_row_cannot_express`** — the machine's instances disagree with **each other** and the table names no exception for the row, so no single `def` byte can be right for all of them. A statement about the row's shape rather than its value, which is why it is reported apart. The two rows that were here — a part's power-on receive channel, the rhythm assignment only part 10 holds — are now a function of the address rather than a byte, and the expected value is taken per instance from `reset_not_def`, which the dump fills by calling `gs_reset_default`. Deriving those here instead would be a second copy of the rule, in another language, free to drift from the one the synth resets from.
- **`range_disagreements`** — a value the write probe sent and the machine accepted from outside `[lo, hi]` (the row is too narrow), or one inside it the machine would not take (too wide). Values the probe never sent say nothing either way, which is why both halves are drawn from what it actually wrote.
- **`power_on_reads_disagree`** — two captures of the power-on state answered the same address differently. Neither is preferred and the address takes no part in the default comparison; it still counts among the addresses a read answered, which is not what the two disagree about. Region reads and single-byte reads reach different bytes of a block on this unit, so where both answered they are two measurements rather than one repeated.
- **`ranges_the_probe_could_not_decide`** — the probe's verdict on that byte was `unchanging`, its own words for "a clamp and a refusal cannot be told apart". The accepted set is then the one value the byte already held, not a range, and comparing a row to it manufactures a disagreement the width of the row. A whole-parameter-only address is the usual reason: MASTER TUNE's four bytes are one nibble-packed value, so writing the first alone is not a write the machine has anywhere to put. Reported rather than dropped, because an address whose range no probe can reach is a gap in the measurement and not agreement.
- **`blanket_rows_not_compared`** — a row standing for a whole high-byte block rather than for a parameter. gs.md: "the statement being made is that a group is absent, and that is one statement however many parameters would have sat inside it." Its `lo`, `hi` and `def` are not claims about any parameter.
- **`extension_at_40_3u_xx`** — the extra insertion-effect units are safe without a feature flag because a spec-compliant file cannot reach them, which rests on the hardware having nothing at `40 3u xx`. gs.md asserts that from the manual; this checks it against the machine. Any address reached here is a collision.

## The exclusions carry their reasons

Some rows hold a default that is deliberately **not** the machine's. The drum setup parameters have no power-on value at all — a drum set change re-initialises them to what the kit specifies — so each is held at the value that changes nothing, and comparing that identity to a machine holding a loaded kit is comparing two different things.

Those rows are named in `DEFAULT_IS_NOT_THE_MACHINES` with the decision behind each, rather than left to prose. Same discipline as the parity allowlist and for the same reason: an exclusion argued only in a document is invisible to anything mechanical and reads as an oversight. **An entry that stops suppressing anything is reported as `stale_default_exclusions`** rather than sitting unexamined, so the list cannot outlive what it excuses.

`RANGE_WIDENED_DELIBERATELY` is the same arrangement for a row whose range is wider than the machine's on purpose — presently the one row: `40 4x 22` PART EFX ASSIGN reaches libsonare's extra insertion units at `02`–`10` and the hardware refuses them, which is the very property that keeps the extension unreachable from a spec-compliant file. **Only the widening is excused.** A row here found too *narrow* is still reported, an extension being no licence for a value the hardware takes and libsonare does not, and `stale_range_exclusions` reports an entry that suppressed nothing.

**Stale means compared and agreed, never not compared.** A row every one of whose instances landed in `power_on_reads_disagree` produced no verdict for its exclusion to suppress, and retiring a reviewed decision on that would be reading a withheld measurement as agreement — the same shape as scoring an empty set as perfect. Such a row is left excused and neither list mentions it.

## What it cannot see

Carried in the diff itself, under `what_the_comparison_cannot_see`, so a reader of the file never has to come here for the caveats. In short: the records are not shown to be one state of the machine; where two captures of that state disagree neither is preferred and no default verdict is taken; no row is shown absent from the machine; the reach of a read is taken as contiguous because a count is all the record keeps; the window blocks are excluded entirely; levels are libsonare's promises about its own implementation and are never compared; and one unit is one unit.

## Running it

```sh
export GS_UNIT_ARCHIVE=<a unit directory>  # no default; see below
make gs-unit-diff                          # regenerate the committed diff
make gs-unit-diff-check                    # fail if the committed diff is stale
```

The archive is external and holds measurements of a named individual machine under its own licence. `unit-diff.json` is committed so that a clone reads the work list without fetching anything, the same arrangement the census next to it has — which is also why `GS_UNIT_ARCHIVE` carries no default: a path into a tree a clone does not have would be a dead pointer rather than a convenience, and the target says so instead of failing on a missing file.

It reads `meta.json`, the `boundary` stage's whole-map record, and the **whole of** the `power-on` and `write-probe` stages, taken from the archive's own `index.json`. Each record's envelope is copied whole into `derived_from`, never summarised: which fields a record cannot fill differs by record — one kept by hand has no stage that produced it, a run predating the envelope has no arguments and no moment — and restating that here would be a second copy of the archive's account, free to drift from it. Several hold no measurement time, so nothing here places those in order; the published date each carries bounds it from above and is not when it was taken.

**A stage is read whole rather than by one file's name, because a stage is free to be more than one run and this one is.** An archive takes a capture two ways where neither way reaches what the other does: this unit's power-on state is read once region by region and once an offset at a time, and only the second reaches a live address sitting past a silent gap. Naming one file reads whichever half the name landed on and reports the other half's addresses as absent from the machine, silently — which is how `40 4x 21` PART OUTPUT ASSIGN sat unrowed while the diff read clean. A write-probe record declaring a `prepared` state is left out of the accepted map instead: its values are that state's rather than the power-on machine's.

An archive is free to file its records differently, and to word a finding differently. Both are read rather than assumed: the stage listing comes from `index.json`, and the blocks that are a window come from the `blocks-that-are-a-window` finding by its kind, searched across the stage rather than expected in a particular file. Nothing about this unit's map is compiled in.
