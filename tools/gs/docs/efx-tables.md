# The insertion-effect conversion tables

`src/midi/synth/docs/gs.md` says the manual is a proxy for the machine and that where the two disagree the machine decides. The GS insertion-effect parameter block is where that bites: a Rate byte is not a rate, a Time byte is not a time, a Level byte is not a level, and a Freq byte does not even have 128 values. Until these tables existed libsonare stored every one of those bytes faithfully and then handed the insert its compile-time default, so the quantity a file asked for never reached the audio.

`derive_efx_tables.py` is what closes that. It reads an archive of what one individual SC-8850 answered and writes down, once, what each byte is worth — the eleven shared conversions, and which (type, slot) pairs the archive gives one to. The result is committed as `efx-tables.json` and rendered into `src/midi/synth/gs_efx_tables.h`, so a fresh clone builds against the tables without fetching anything.

**It decides nothing.** A slot the archive does not reach is left unreached and counted rather than given a plausible conversion; a record that falls outside the claim it rests on is reported rather than dropped or kept quietly. What the tool does is make the list finite — the same arrangement `check_unit.py` next to it has, and for the same reason: a conversion written without a measurement behind it is worse than the default it replaces, because nothing downstream can tell the two apart.

## Why this is not the unit diff

They read different parts of the same archive and neither substitutes for the other.

`unit-diff.json` compares the **address table** against the machine: which addresses exist, what they accept, what they power up holding. It is blind to what a byte *means*, because an address map has nowhere to say so.

`efx-tables.json` says what the bytes at those addresses are worth. It is blind to whether the address exists at all, since it never looks at the map. Together they bound the block from both sides: the diff says the address is real, the tables say the byte is a frequency.

## The three reach numbers

The design this implements splits coverage into three, because `gs.md`'s own rule — an engine that does not read a byte owes it `STATE` and not `AUDIBLE` — makes "translate everything" impossible by contract, and one number would hide which of the three walls a slot stopped at.

- **reached** — the archive gives this (type, slot) a conversion column or an explicit range. **This is the only one this tool can compute**, and it is a statement about the archive rather than about libsonare.
- **translatable** — reached, and the insert the type maps to has a control of the same physical unit. Needs the insert side, so it is `null` here.
- **translated** — translatable, and the GS layer actually emits the JSON key. Needs the layer, so it is `null` here.

The header also carries a named count per class, `kGsEfxReach*`. **A class reaching zero is a failure of the derivation and not a property of the unit**: a class that stopped being fed produces exactly what a class that was never wired produces, and both look like a clean run. The script exits non-zero on it, and the C++ side checks the same thing without an archive by enumerating the eleven classes by hand — which is why the header offers the eleven counts as named constants and not as an array the test could iterate. A derivation that dropped a class would drop its count too, and a test reading the list would pass by not looking.

## How a slot gets a class

From structured record fields, never from a file name. Names here carry real exceptions — nine records elsewhere in the archive have no (type, address) in their name at all and ten spell their hex in lower case — and the fields do not.

1. **The printed spelling picks the table.** Every parameter record carries `printed_values`: either a pointer into the unit's own conversion grid (`*1`–`*14`), or an explicit byte range (`00–7F`, `34–4C`, `00/01/02/03/04`). A pointer names one column and therefore one table. 163 of the unit's 1297 parameters carry a pointer, 604 carry a range, and 530 carry neither — those last are simply not reached.
2. **The class is intersected with its claim's own type list.** A claim's `rests_on` list holds the records that **refuted its rivals** as well as the ones that carry it: the frequency claim cites a reading taken on the phaser's corner byte, and its own `about.types` excludes the phaser, because that byte is swept by a modulator and has no sixteen-entry table on it. Deriving from `rests_on` without the intersection hands the phaser a table the claim says in as many words that it does not have.
3. **An address outside the claim's list is kept and reported.** Records legitimately sit outside one — the chorus's pre-filter corner is at `40 03 04` and the frequency claim's address list does not name it — so dropping them loses reach, and keeping them quietly loses the one place a reader can see that the claim was widened here rather than by its author. Four slots are in that state today, two frequency and two gain.

**Where the spelling is the bare byte range, the address is part of the selector rather than a report line.** `00–7F` is the whole byte and is printed against 321 parameters that have nothing to do with each other, so it cannot pick a table on its own; for output level, output pan and effect balance the claim's single address is what is left to select on. The trade is stated rather than hidden: a slot of one of those types carrying the same quantity at some other address is invisible here instead of reported.

## Suffixed records are a detector, not a source

The parameter stage holds 109 records over 65 types. Sixty-five of them measured the power-on machine; the other forty-four measured it under some other condition — a modulator parked, an amp switched on, a stage mixed in, a disc raised.

Which is which is read from each record's own `prepared` list, not from the suffix on its name: a power-on record prepares exactly two things, the type and the part routing, and anything further is a condition. The name is checked against that and a disagreement is reported, so a change in the archive's naming is a finding rather than a silent change of population.

**The suffixed records are then compared against the power-on ones at every slot they overlap, and a disagreement is fatal.** They are not a second source for a default. They are the detector that the power-on records were read as what they are — the failure this guards against is a defaults table that silently ships one type's parked modulator, which nothing downstream could distinguish from a measurement.

Sixty-two of the sixty-five types carry a measured byte at all twenty slots. Three carry nineteen: on each, one slot's comparison ran and refused, because something was already sounding when the take began. Those bytes are emitted as zero with their bit clear in `GsEfxTypeDefaults::measured`, so a consumer can tell "not measured" from "measured as zero".

## A table no reached slot uses

**A class holds more than one table** — two rate ranges, five delay ladders, three frequency columns — so a table that stops being fed leaves its class's count untouched, which is exactly the shape the class guard was written against. The count is therefore taken per table as well, into `reach_by_table` in the JSON and into `kGsEfxTableUse*` in the header, so a check can be held to it later. Named constants rather than a list, for the reason the class counts are: a check that reads a list the generator wrote passes by not looking.

One of the five delay ladders is at zero today. The unit's grid carries five delay-time columns; two of them share both printed ends — both spell `200ms` to `990ms/1sec` — and differ only in where the step coarsens from 5 ms to 10, which is how the archive established that the printed range cannot be what picks a ladder. The one slot citing the second of those two sits on a type the delay claim's `about.types` does not name, so the intersection leaves that ladder with nothing pointing at it.

**The intersection is corroborated there rather than merely applied.** That slot has been read, at 35 settings, and it is the one slot of the six this class was read on that does not return its cited column: the archive's own figure for it is 0.96917 of every printed value with a spread of 0.05882, where the other five return 1.0 with a spread of 0.0. Re-measured here against the ladder this file stores, it is 35.96 ms short at the median and misses at **all 35** admitted settings, not at two or three. So the type list and the measurement agree, and shipping the ladder onto that slot would be a conversion with no measurement behind it — worse than leaving the slot alone, for the reason stated at the top of this page.

**What it returns instead has a shape, and the shape is recorded rather than acted on.** A residual in milliseconds cannot tell a constant fraction of the column from a constant number of milliseconds short, so both are reported, split at the byte the slot powers up holding — the threshold the archive names, taken from the parameter record rather than from the look of the readings, since a split chosen by eye finds a break in anything. At or below that byte the slot returns a median **0.992208** of the column over 16 settings, spread 0.992188 to 0.992235, where 127/128 is 0.992188; above it, 0.952526 over 19 settings, a further median 37.67 ms short. So a scaling plus a constant subtraction past a threshold, and not the single constant the raw residual reports.

**Nothing here decides whether that is a second law of this slot's or a shape a few points fall into.** The archive's own claim reads the shortfall as an artifact of which direction the sweep approached the setting from, and separating the two wants a reading of the unit no derivation can produce. It is recorded — in `notes`, and as `as_a_fraction_of_the_ladder` beside `ms_short_of_the_ladder` on the record — so the observation survives; it changes nothing, because the column *unscaled* is wrong on that slot under either reading and the slot stays unreached either way.

The ladder is kept, because it is measured and it is part of the class. The run reports it under `tables_no_reached_slot_uses` with every slot whose printed spelling names its column and which filter let each go, since "the unit has no slot for this table" and "the intersection removed the only one" are different findings; and the record itself is carried under `delay_records_of_slots_not_reached`, which is what the intersection costs, measured rather than argued.

## What the entry flags mean

Every entry in the file carries both, from the first version — adding a flag later would be a format change, and the two cases they are for were known before the first run.

- **`unit_specific`** — this unit returned a value the class's own law does not account for, by more than the reading separates. A second machine is free to differ here, and an entry so flagged can be replaced on its own when a second one is measured. Two entries carry it: the `200 - 6.3k` frequency table's third entry, read at 279.1 Hz where its own third-octave series says 315 (0.17 octaves, two bands, and confirmed on three types); and the rotary acceleration table's last entry, measured at 168.41 where the doubling sequence the other fifteen follow says 256 — five times what that take could resolve.
- **`approximate`** — no reading placed this entry and it is carried on the law alone. The level table's setting 0 (too close to the floor to read), the two lowest acceleration entries (no multiplier published), and the frequency entries of the two ranges whose readouts are partial.

`unit_specific` being false is not a claim that a second machine would agree. It is a claim that this one did not visibly disagree.

## What the derivation checks about itself

Printed at the end of every run, because a derivation that reached nothing prints the same thing a clean one does unless it says how far it got.

- **The ladders against the records.** Every `efx-time` record the delay claim rests on, for a slot these tables reach, read setting by setting against the ladder this file stores. Thirteen of fourteen records agree to a median of 0.0000 ms against a reading quantised to 0.0208 ms; the fourteenth to 0.0417 ms. **A residual is the raw difference and nothing is subtracted from it** — no per-record baseline, no offset, no fitted constant. A ladder beginning at 200 ms begins at 200 ms in the stored breakpoints, which is why a slot returning `200 + 5·setting` shows a residual of a twenty-fifth of a millisecond rather than of two hundred.
- **The cut, floor against round.** A delay-time entry is a round decimal millisecond cut back to a whole sample of the unit's 32000 Hz clock, and which way it is cut is read off the records rather than asserted. **Only the settings where the two predictions differ at all carry any information** — on an entry of five milliseconds or more they agree exactly, and counting those would drown the answer in agreement neither reading earned. Over the 61 settings that do separate them, **floor is closer at 61 and round at 0**, by a median 0.0104 ms against 0.0312. Two failures rather than a report: round winning stops the run, because these tables and the conversion layer both ship floor and one of the three would then be wrong; and *nothing* separating them stops it too, because a check that reaches no verdict reports exactly what a check that agrees reports.
- **Where the cut happens.** Once, at evaluation, after interpolating between the stored knots — the design's arrangement for a piecewise-linear table. **The stored knots are the uncut ladder**, and every one of them is checked to be a whole sample already, so the runtime cannot cut a value twice. That check is not idle bookkeeping: an interpolation between two *cut* endpoints is not the cut of the interpolation between the uncut ones, so a knot quietly pre-cut would move every value between it and its neighbour.
- **The three settings that do miss.** Two on the four-tap delay's first tap, one on its fourth, all three reading within a millisecond of 951 ms. That is the stimulus and not the effect: the note is a held sample, a sample that loops puts a copy of itself in the output at its loop length, and a cepstrum cannot tell that from a delay — the archive names the figure and names these three readings as the ones it does not cite. The record's own second and third peaks hold the ladder's figure at each (679.96 against 680, 750.06 against 750, 750.02 against 750). They are listed in full under `the_settings_over_1ms` with the record's alternative reading beside each, and counted, rather than excused: a median cannot say which settings they are and a worst says it wrongly. The four taps of that type agree with each other — taps 2 and 3 are clean at every setting, and taps 1 and 4 are clean at every setting the reading did not put on the loop.
- **The rate tables against the records.** Same arrangement over the `efx-rate` stage; nine records, medians from 0.0005 to 0.0047 Hz against the archive's own 0.0028 Hz floor.
- **The third-octave series against the claim's.** The frequency entries are the named third octaves between the two ends a slot's own record prints. The series is derived here from the preferred-number decade and compared with the one the claim names; a mismatch stops the run rather than shipping a table built on a series the archive does not recognise.
- **The azimuth rule against the claim's column.** The rule is evaluated over all 128 settings and every position's setting span is compared with the archive's published column. A rule agreeing at the ends and not in the middle would otherwise ship.
- **The state of every claim a class rests on.** All eleven stand today. An entry resting on a **parked** claim is listed one entry at a time rather than one claim at a time: a claim is parked over a specific residual question, and whether that question is one these tables lean on is decided per entry.

## What it cannot see

Carried in `efx-tables.json` itself, under `what_this_cannot_see`, so a reader of the file never has to come here for the caveats. In short: one unit is one unit; a slot with no printed spelling is unreached and nothing here says whether it carries a quantity at all; reach is a statement about the archive and not about libsonare; a slot whose spelling matches a class it is not on would be assigned to it and nothing here would notice; a class selected by the bare byte range cannot see the same quantity at another address; the defaults are the bytes the unit powered up holding and not the manual's printed defaults, which the archive does not transcribe; and an entry flagged `approximate` is one no reading placed.

## What a consumer of these tables owes

Two properties are the tables' and not the reader's, so they are stated here rather than left to be rediscovered.

- **The defaults are a function of the type, not of the address.** Twenty bytes arrive when a type is selected, and they are almost never zero. A consumer that treats byte 0 as "unset, use the default" is reading a value the unit holds as an absence. The cost of the other reading is real and is the machine's: a file that writes a parameter *before* its type loses that byte, because selecting the type loads the type's own twenty.
- **A structural field cannot be automated.** A conversion that changes a filter's order, a window's length or a hold's frequency rebuilds the processor rather than being applied in place, so editing that byte mid-note cuts the tail. A field that is only a gain, a rate or a corner does not have that problem. Which is which is a property of the insert and not of these tables, but the tables are what make the difference reachable from a file.

## Running it

```sh
export GS_EFX_ARCHIVE=<an archive root>  # no default; see below
make gs-efx-tables                       # regenerate the committed tables and header
make gs-efx-tables-check                 # fail if the committed ones are stale
```

The archive is external and holds measurements of one named individual machine under its own licence. `efx-tables.json` and `gs_efx_tables.h` are committed so that a clone builds without fetching anything — the same arrangement the census and the unit diff next to them have, which is also why `GS_EFX_ARCHIVE` carries no default: a path into a tree a clone does not have is a dead pointer rather than a convenience.

It reads two directories and no others: `data/units/<unit>/efx-params/`, taken from the archive's own `index.json` rather than by globbing, and the sibling stages the two self-checks cite; and `inferences/<unit>/` plus the one model file the pan readout lives in. Both fall under the archive's CC0 dedication. **Nothing under its `documents/` tree is read**, and that is a licence boundary rather than a preference: the extracted structuring there is dedicated but the document content is not, and every input this derivation needs turned out to be present in the measurements anyway.

The header is rendered from the committed JSON by the same script, so it has one source:

```sh
python3 tools/gs/derive_efx_tables.py --from-json tools/gs/efx-tables.json --header <path>
```

`clang-format` runs over the header as the last step of rendering, with the repository's style named explicitly rather than found by searching upwards. The header is a `.h` under `src/`, so `make format` owns its layout; without this the committed file and a freshly rendered one would differ over line breaks and the check target would report it as drift in the tables. A scratch copy rendered outside the tree would pick up a different style for the same reason, which is why the style is named.

## `archive_revision`

Every run records what identifies its inputs: the archive's git revision where it is a working copy, and a hash over the files actually read where it is not. Which of the two it was is recorded beside it.

**`archive_inputs_dirty` is the field that says whether the revision means anything.** A revision names a commit; it names the *inputs* only if the working copy was clean over them, so the flag is taken as `git status --porcelain` over the exact list of files the run read — the stage index, every parameter record, every claim file, the pan model and the records the two self-checks cite. A stale table and a table whose inputs are unidentified are different problems and only one of them can be fixed by re-running:

- **`dirty: false` and the revision behind the archive's HEAD** — the table is *stale*. It was derived from a state that can be named and fetched, so what it says can be checked and the fix is to re-derive.
- **`dirty: true`** — the table is *unidentified*, whatever its revision says. It was derived from files that existed only in somebody's working copy, and no amount of reading the history recovers them. Re-derive from a clean tree; do not reason about the difference.

The archive is a live repository and is worked on while these tables sit still, so the revision going behind is the normal case rather than a fault. **`gs-efx-tables-check` reports a revision mismatch on its own line, separate from any diff of the tables, and that is the target working.** The two are different findings — "the archive moved" and "the script changed" both produce a diff, and reading one as the other sends the next person to the wrong place. Nothing here retries, pins or tolerates the difference; hiding it would cost exactly the distinction the field was added for.
