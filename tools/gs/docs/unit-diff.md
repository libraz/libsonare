# The address table against a measured unit

`src/midi/synth/docs/gs.md` names the SC-8850 as the target and then declares a gap in its own provenance: a row in the address table came from the **SC-88Pro** manual unless it says otherwise, and the SC-8850's own map has been checked only where a row names a difference. That is not a known error — the sampled points agree — but it means "the manual says" about an untouched row means the wrong manual's.

`check_unit.py` closes that gap by measurement rather than by reading a second document. It takes the table as the code holds it and an archive of what one individual machine answered, and reports where the two disagree.

**It decides nothing.** A disagreement is a question — a row transcribed from the wrong manual, a real difference between the two machines, or a limit of what the probe could see — and which of those it is comes from reading the row and the record. What the tool does is make the list finite.

## Why this is not the census

The two answer different questions and neither substitutes for the other.

`address-census.json` says what **real files reach**, and it is blind to any address they never send. `40 1x 03`–`12` is the sharpest case: the machine answers all 256 of them and the corpus of 2 233 files touches exactly one, in one file. A gate anchored on the corpus can be green while a whole block of the map has no row.

The unit diff says what **the machine answers**, and it is blind to everything the machine will not read back. Together they bound the table from both sides.

## Reading it

The table is read by including its header and printing it (`dump_address_table.cpp`), never by parsing the source: a parse would have to keep up with the row layout and the enumerator spellings, and would go wrong quietly the first time a field was added.

- **`addresses_with_no_row`** — the machine returned a value and neither a row nor an undefined range claims the address. Grouped by block, since a gap is normally a run.
- **`rows_no_read_reached`** — **not an absence claim.** A single-byte read that answers nothing leaves the address unproven, because a block read starting earlier reaches addresses a direct read does not, which is the archive's own finding about this unit. The list is for aiming: the candidates for an SC-88Pro-only row are inside it.
- **`default_disagreements`** — the machine holds one value across every instance of the parameter and it is not what the table expects there.
- **`defaults_one_row_cannot_express`** — the machine's instances disagree with **each other** and the table names no exception for the row, so no single `def` byte can be right for all of them. A statement about the row's shape rather than its value, which is why it is reported apart. The two rows that were here — a part's power-on receive channel, the rhythm assignment only part 10 holds — are now a function of the address rather than a byte, and the expected value is taken per instance from `reset_not_def`, which the dump fills by calling `gs_reset_default`. Deriving those here instead would be a second copy of the rule, in another language, free to drift from the one the synth resets from.
- **`range_disagreements`** — a value the write probe sent and the machine accepted from outside `[lo, hi]` (the row is too narrow), or one inside it the machine would not take (too wide). Values the probe never sent say nothing either way, which is why both halves are drawn from what it actually wrote.
- **`ranges_the_probe_could_not_decide`** — the probe's verdict on that byte was `unchanging`, its own words for "a clamp and a refusal cannot be told apart". The accepted set is then the one value the byte already held, not a range, and comparing a row to it manufactures a disagreement the width of the row. A whole-parameter-only address is the usual reason: MASTER TUNE's four bytes are one nibble-packed value, so writing the first alone is not a write the machine has anywhere to put. Reported rather than dropped, because an address whose range no probe can reach is a gap in the measurement and not agreement.
- **`blanket_rows_not_compared`** — a row standing for a whole high-byte block rather than for a parameter. gs.md: "the statement being made is that a group is absent, and that is one statement however many parameters would have sat inside it." Its `lo`, `hi` and `def` are not claims about any parameter.
- **`extension_at_40_3u_xx`** — the extra insertion-effect units are safe without a feature flag because a spec-compliant file cannot reach them, which rests on the hardware having nothing at `40 3u xx`. gs.md asserts that from the manual; this checks it against the machine. Any address reached here is a collision.

## The exclusions carry their reasons

Some rows hold a default that is deliberately **not** the machine's. The drum setup parameters have no power-on value at all — a drum set change re-initialises them to what the kit specifies — so each is held at the value that changes nothing, and comparing that identity to a machine holding a loaded kit is comparing two different things.

Those rows are named in `DEFAULT_IS_NOT_THE_MACHINES` with the decision behind each, rather than left to prose. Same discipline as the parity allowlist and for the same reason: an exclusion argued only in a document is invisible to anything mechanical and reads as an oversight. **An entry that stops suppressing anything is reported as `stale_default_exclusions`** rather than sitting unexamined, so the list cannot outlive what it excuses.

## What it cannot see

Carried in the diff itself, under `what_the_comparison_cannot_see`, so a reader of the file never has to come here for the caveats. In short: no row is shown absent from the machine; the reach of a read is taken as contiguous because a count is all the record keeps; the window blocks are excluded entirely; levels are libsonare's promises about its own implementation and are never compared; and one unit is one unit.

## Running it

```sh
make gs-unit-diff                                    # regenerate the committed diff
make gs-unit-diff SOUNDINGS_UNIT=<a unit directory>  # against another machine
make gs-unit-diff-check                              # fail if the committed diff is stale
```

The archive is external and holds measurements of a named individual machine under its own licence. `unit-diff.json` is committed so that a clone reads the work list without fetching anything, the same arrangement the census next to it has.

The four records it reads are `meta.json`, `power-on-state.json`, `boundary-probe.json` and `write-probe-wholemap.json`, and the diff records what each of them says about itself under `derived_from` — including that it says nothing, for a record written before that archive carried provenance of its own.
