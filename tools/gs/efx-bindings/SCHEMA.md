# EFX binding files — what a row is and what it may say

Each file here answers one question the measurement archive cannot: **which physical quantity a given insertion-effect (type, slot) names, and which insert control receives it.** The archive measures laws — how a byte becomes a decibel, a hertz, a millisecond — and it measures which slots move the sound. It does not record what a slot is called, so nothing in it says that `01 42` slot 3 is a rate rather than a depth. That assignment is what these files carry.

## Why the assignment is hand-authored

The derivation (`tools/gs/derive_efx_tables.py`) reads only the archive's `data/` and `inferences/` trees, both CC0. Only the mark transcription script `tools/gs/marks_from_manual.py` opens the archive's `documents/` tree; the derivation and `coverage.py` do not. That is a licence boundary rather than a preference — `tools/gs/docs/efx-tables.md` states it, and the archive's own `documents/LICENSE` grants no licence over any document's contents. Copying a manual's conversion tables into a shipped artifact would hand downstream consumers a permission this project has no standing to give. The script copies two annotations per row and nothing else — the printed parameter name and the one-character mark beside it; a name or a mark is not a conversion table.

Reading a specification in order to implement against it is a different act from redistributing it. A person may read the parameter list, learn that a slot is a rate, and write that fact into a row here. What must not happen is the printed conversion *tables* being transcribed into this directory or into any generated header — a row names a law the archive already measured, never a column of numbers copied from a page.

The CC0 side is what keeps this honest. Every one of the 770 printed (type, slot) rows carries a `printed_values` field in `data/units/*/efx-params/*.json` — `34–4C`, `*6`, `00/01/02/03`, and so on. So a hand-written assignment is checked mechanically against a CC0 record: a row that claims a delay table for a slot whose printed values are a two-value enumeration fails the derivation. Authorship is human; verification is not.

## File naming

One file per type MSB: `01.json`, `02.json`, `04.json`, `05.json`, `11.json`.

There is deliberately **no `03.json`**. The manual prints the Rotary Multi effect under two type numbers, `02 0C` and `03 00`; this tree treats `02 0C` as canonical and `03 00` as its alias (`gs_layer.cpp` gives both `case` labels one handler). Those eighteen rows are written in `02.json` as type `02 0C`, and the coverage tool rewrites the archive's `03 00` to `02 0C` before matching. Writing them under `03 00` would leave them unmatched in both directions.

A file is a JSON array of row objects. Row order does not matter; sort by type then slot for a readable diff.

## Identifying a row

| field | form | meaning |
|---|---|---|
| `type` | `"MM LL"` — uppercase hex, one space | The effect type. Canonical spelling only (see `02 0C` above). |
| `slot` | integer `0`–`19` | The parameter position. `slot == address_lsb - 3`, so the block address `40 03 04` is slot 1. |

Every `(type, slot)` written here must be one of the 770 rows the archive records a `printed_values` for. A pair that is not printed does not exist on the machine, and naming one is an error rather than a harmless extra.

No `(type, slot)` may appear twice, in one file or across files.

## The forms

A row is written in **exactly one** form. Zero is an error and two is an error — the whole point of the file is that every printed parameter has been looked at and adjudicated once. Three forms make up the vocabulary: `translated` (a `stage` with a measured `class`), `designed` and `enables`.

```json
{"type":"01 42","slot":3,"class":"rate","table":"wide","stage":"effects.modulation.chorus","key":"rateHz"}
{"type":"01 30","slot":4,"designed":{"basis":"invented","law":"d.attack_ms","replaced_when":{"model_binding":["01 30","40 03 07"]}},"printed_values":"00–7F","stage":"dynamics.compressor","key":"attackMs"}
{"type":"11 03","slot":1,"designed":{"basis":"carried","law":"drive.gain","from":"0110-a-drive-byte-is-a-gain-in-front-of-a-fixed-curve","replaced_when":{"claim_names":["11 03","40 03 04"]}},"printed_values":"00–7F","stage":"saturation.ampSim","ordinal":0,"key":"inputDb","printed_mark":"+"}
{"type":"04 00","slot":3,"enables":{"stages":[{"stage":"dynamics.compressor"}],"on_states":[1],"basis":"invented","replaced_when":{"model_binding":["04 00","40 03 06"]}}}
{"type":"04 00","slot":11,"enables":{"select":[{"stage":"effects.modulation.chorus"},{"stage":"effects.modulation.flanger"}],"basis":"invented","replaced_when":{"model_binding":["04 00","40 03 0E"]}}}
```

Any other form field (`state`, `unmapped`, `unreadable`, `builder`) is refused by name: every printed byte drives something.

### `stage` — the quantity reaches a control (translated)

The row names a chain stage and the JSON key on it that receives the converted value.

- `stage` is the **insert name**, not an index into the chain: `"effects.modulation.chorus"`, `"eq.parametric"`, `"stereo.autoPan"`. Where a chain holds the same insert twice (the two halves of an overdrive pair, say), `ordinal` says which one, counting from 0; absent means 0. Name and ordinal together are the address. A translated or designed row whose byte reaches both — whichever of the pair a selector has switched on — lists them, `"ordinal": [0, 1]`; the slot is still one adjudicated row, and the generated header carries one row per ordinal.
- `key` is that insert's parameter key. Dotted keys are allowed where the insert nests them (`"band0.gainDb"`).
- **The key should also be one the insert publishes as realtime-automatable.** A key the insert reads at construction but offers no realtime descriptor for still works, at the price of rebuilding the whole chain on every wire edit of that byte — which zeroes the delay and reverb tails inside it. Where that is the right answer anyway, it is recorded with its reason in `tests/midi/gs_efx_send_routing_test.cpp`. The entries there are of two kinds: a length an insert sizes a buffer by in `prepare()` (the reverb's pre-delay, the pitch shifter's window), which a live write would allocate for on the audio thread; a mode or placement an insert applies on construction only (the reverb's character, the bit crusher's type ladder, the rotary's microphone spread); and the rotary's acceleration fields, which shape a glide rather than ride one.
- `keys` (array) replaces `key` where one slot drives several controls. The row is still one row.
- `alternatives` (array) — the same byte on the other stages an `enables` select chooses between: a CF Sel picks the chorus or the flanger, and the CF Rate, Depth, Fb and Mix bytes reach whichever is on. Each entry is `{"stage", "key" | "keys", "ordinal"?, "law"?}`; `law` is allowed on a designed row only and is held to the name rule of the entry's own stage (`CF Depth` is `d.chorus_depth_ms` on the chorus and `d.flanger_depth_ms` on the flanger). The slot is still one adjudicated row; the generated header carries one row per target. `make gs-efx-coverage` refuses a row that drives one stage a select chooses between and not the others, and a row whose alternatives are not stages one select names.
- `class` and `table` name the measured law. The pair spells the same `class.table` string the generated header uses — `"gain"` + `"tone"` is `gain.tone`.
- `via` (optional) names a fixed wrapper applied after the conversion, from a closed vocabulary rather than free text. Use it only where the wrapper already exists as a named function.

The fifteen classes and their tables, which are the whole vocabulary:

| class | tables |
|---|---|
| `ratio` | `percent`, `semitone`, `cent` |
| `rate` | `narrow`, `wide` |
| `delay_time` | `pre_delay`, `time1`, `time2`, `time3`, `time4` |
| `gain` | `tone` |
| `level` | `output` |
| `width` | `section` |
| `wave` | `modulator` |
| `pan` | `output` |
| `balance` | `effect` |
| `azimuth` | `placement` |
| `accel` | `rotor` |
| `freq` | `eq`, `pre_filter`, `damping` |
| `post_gain` | `makeup` |
| `window` | `splice` |
| `corner` | `low`, `high` |

A class outside this list means a law nobody measured for this slot. That is not something to invent in a translated row — the row becomes `designed`, and its law comes from the designed-law file below.

**`ratio` is the one class no measured table holds.** It reads a byte between the endpoints of a range printed with a unit, and it is admitted only where those endpoints force the step: `0F–71` against `-98%`–`+98%` is 98 bytes over 196 per cent, exactly 2 a byte, so the linear reading is the only one the page allows rather than a guess at the machine's law. A ratio row carries both endpoint pairs — `printed_values`, its copy of the archive's byte range, and `range`, the unit ends — and `bindings_header.py` refuses the header where the unit span is not a whole multiple of the byte span. The table names the unit the ends are printed in; `percent` reaches the control as the fraction, `semitone` and `cent` as they stand (`0E–72` against `-100`–`+100` cents is 2 a byte). Nothing makes `ratio` available to the bare `00–7F`: it prints no unit, and its law is measured not to be linear.

### `designed` — the quantity reaches a control through a law not measured for this slot

The row names `stage`, `key`/`keys`, `ordinal` and `via` exactly as a translated row does, and puts its law in `designed` instead of `class`/`table`:

- `basis: "carried"` — a law measured elsewhere, carried over as the same quantity. `law` is a measured `class.table` (`level.output`, `pan.output`, `balance.effect`, `gain.tone`, …) or one of the carried laws the designed-law file lists (`drive.gain`), and `from` names the claim the law rests on. `ratio.*` is never carried: it reads the row's own printed ends, so a ratio row is translated.
- `basis: "invented"` — a law this project designed. `law` is a `d.` id the designed-law file defines, or `d.enum<n>` for an enumeration of `n` states.
- `replaced_when` is required on both (see below).
- `printed_values` is required: the row's copy of the archive's spelling, held to the archive like a ratio row's. It gives the law its domain — the first and last printed byte, or the whole byte `00`–`7F` for a column pointer (`*11`, `*12`).

A designed row carries no number. The values live in `tools/gs/efx-designed-laws.json` and nowhere else; `bindings_header.py` copies the resolved law into the generated row.

### `enables` — the byte switches stages on

A switch or selector. The chain skeleton places every stage the byte can choose, and the byte only moves their enabled flags.

- `stages` + `on_states`: the listed stages are on at the listed bytes (0–127, each one the slot prints) and off elsewhere.
- `select`: state *i* of the printed list turns on the *i*-th stage and no other, so the list names exactly as many stages as the slot prints states.

Each stage is `{"stage": name, "ordinal": n}`, at most four of them. `basis` is `"invented"` and `replaced_when` is required. An enables row carries no `stage`, `key` or `class` of its own.

### `replaced_when` — how a carried or invented law is retired

An object naming at least one condition under which a measurement supersedes the law: `claim_names: ["MM LL", "40 03 XX"]` (a claim comes to name that pair on its recorded path), `model_binding: ["MM LL", "40 03 XX"]` (the archive's model binds that pair as a measured law), `stage_passed: "MM LL"` (the type's stage verdict passes), `covers: "<item>"` (the covered item's text changes), `table_reaches: ["MM LL", "40 03 XX"]` (the committed `efx-tables.json` map comes to reach that pair, at which point the row becomes translated; no archive is needed to see it). Any other key is refused. A carried row whose claim already stood on its pair when the row was written takes `table_reaches`, because `claim_names` would be met from the start; an invented row whose only evidence is a candidate model that has not passed its stage takes `stage_passed`.

### The designed-law file and its name rules

`tools/gs/efx-designed-laws.json` holds every invented law — `form` (`linear`, `log`, `db`, `bipolar`; `enum` through the `d.enum<n>` family), `lo` and `hi`, and on a `linear` or `log` law an optional `n_states` — and `name_rules`, which decide which law a printed parameter follows. A rule matches the printed parameter name (`name`, a full-match regex), the printed spelling (`printed`), or both, and `unless` excludes names. A rule may also name a receiving `stage`: it then applies only to a row driving that stage, and where it applies the rules naming no stage are set aside — `Depth` follows `d.unit` in general but `d.chorus_depth_ms` on the chorus and `d.flanger_depth_ms` on the flanger, where the control is a delay time. The row's own `stage` is the one looked up, so an enables row, which has none, only ever meets unqualified rules. A switch that toggles a setting rather than a stage — an amp switch (`Amp Sw`, `OD Amp Sw`, …) turning the cabinet on and off, and `Drive Sw` on the vowel filter — is excluded from the `Sw`/`Sel` enables rule and follows `d.enum2` into that setting's key. A rule's law is a measured `class.table`, a carried law, a `ratio.*` law, a `d.` law, `d.enum<n>` (taking `n` from the printed state count) or `enables`.

Every row written as `designed` or `enables` has to match **exactly one** rule, and its law has to be the one that rule gives — an enables row's rule gives `enables`. One exception: a `carried` row whose `from` claim is standing, measures a quantity, and stands on the row's own (type, address) is a measurement of that pair, and its law is accepted over the rule's. `coverage.py` reads the claims from the archive's `inferences/` tree (CC0) to decide this. No match or two matches stops the tool, so the author of a row never chooses a law. The name matched is the row's own `printed_name`; a `designed` or `enables` row without one is refused. `coverage.py --rule-report` prints the law every row not written as translated would be given.

A **stepped** law is a `linear` or `log` law carrying `n_states`: the byte is read as a state index and state *i* lands *i*/(`n_states` − 1) of the way from `lo` to `hi`. It is how a printed enumeration reaches a key that holds a physical quantity (`50Hz/60Hz` → `humHz`), where a bare state index would arrive in the wrong unit. Its `lo` and `hi` are the first and last printed state values, and its form is `log` where those values form a geometric series and `linear` otherwise. A key that classifies rather than measures, and whose 0 means "today's behaviour", takes the printed states one-based through a stepped `linear` 1→*n* law (`d.type_index6`, `d.type_index9`).

The law's form is also checked against the printed spelling, names or no names: `d.enum<n>` and a stepped law need a printed enumeration of exactly `n` (or `n_states`) states; a state list (`00/01/…`) refuses an invented law that is neither; a continuous range (more values than the enum family's `max_states`) or a column pointer refuses both.

## Optional fields

- **`range`: `[lo, hi]`** — the two unit endpoints of a slot whose printed values carry a unit (`0F–71` and its siblings), read by the `ratio` class and by nothing else; a row carrying it under any other class is refused. This is the one place a number is taken from the printed page, and it is a field of its own precisely so the exposure stays countable: two numbers per row, on at most the fifty-one rows printed as a unit range. An endpoint is the parameter's domain, which is a fact about the machine rather than a conversion table. Nothing else printed may be copied.
- **`printed_values`** — the row's copy of the archive's spelling for the slot, required on a `ratio` row and on a `designed` row so the header can be rendered without the archive. `make gs-efx-coverage` holds it equal to the archive's own.
- **`ordinal`** — which of two same-named stages a row drives, from 0; absent means 0. Allowed on any row; a translated or designed row may give a list of distinct ordinals.
- **`printed_name`** — the parameter's name as the SC-8850 parameter list prints it, on any row; the name rules are matched against it.
- **`printed_mark`: `"+"` or `"#"`** — the mark the same list puts beside a slot, on any row.

Both are transcribed, never written by hand: `marks_from_manual.py --archive <root> <row files>` matches the manual's rows to binding rows by address — never by name, whose spelling differs between manuals — folding `03 00` onto `02 0C`, and sets `printed_name` and sets or clears `printed_mark` on every row it is given. The manual's rows are the parser's output together with the hand-kept rows it refused (`by-hand.json`).
- **`named_by`: `"the parameter list"`** — where the row's identification of the slot came from, when it came from the printed parameter list rather than from the archive's CC0 records and inferences. Absent means the archive named it, or the row follows mechanically from its printed spelling. The field exists so the rows resting on a reading of the page stay countable, the way `range` keeps the copied numbers countable; `make gs-efx-coverage` refuses any other value.
- **`note`** — free text for a reader. Never load-bearing; nothing parses it.

## What a row may not contain

**No constants.** The corner frequency of a shelf, a fixed mix ratio, the number of bands: those are the skeleton's, meaning the per-type stage list and its fixed parameters. A row says only which law a byte follows — by class or by id — and where the result goes. This is what makes "regular" a decidable property rather than a judgement:

> **Regular** = one slot produces one or more keys, the value is determined by `(class, table)` alone, and no constant is involved.

**No conversion outside the designed-law file.** A slot with no measured law behind it is `designed`, and its law is the one the name rules give it — never one the row's author chose. Nothing downstream can tell a plausible conversion from a measured one, so every invented or carried law is marked by its `basis` and carries the `replaced_when` that retires it once a measurement exists.

**No general-purpose expression language.** A conditional output — a key emitted only below some byte value — is expressed as the state list of an enumeration class, which is data. A rule engine costs more to maintain than the rows it would cover.

## How a row is checked

- Every one of the 770 printed rows is adjudicated exactly once, so a row nobody adjudicated is visible as a shortfall rather than as silence. `make gs-efx-coverage` prints `GS EFX coverage: printed=… translated=… designed=… enables=…`, whose three forms add to 770, then where the designed and enables laws come from (`GS EFX basis: carried=… invented=…`) and the same form counts per MSB.
- `class`/`table`, or a carried law's class, must not contradict the row's CC0 `printed_values`, and a row's own `printed_values` must be the CC0 one.
- A designed or enables row passes the name rules and the form-against-spelling checks above.
- Every `(stage, key)` must be a key the named insert actually accepts, and one it publishes as realtime-automatable. The two failures are different: a key nothing reads is ignored in silence, while a key read without a realtime descriptor costs a chain rebuild per edit. `tests/midi/gs_efx_send_routing_test.cpp` asks the factory for both lists — the second half carries an excused list, the first does not, because no row has a reason to name a key its insert never reads.
- **The form itself is measured, not taken on the row's word.** `tests/midi/gs_efx_join.h` renders every row for `tests/midi/gs_efx_types_test.cpp`, and the chain decides whether the form is true: a translated or designed byte emits every target's key at each of its boundary bytes, on a stage switched on, carrying its law's reading and moving monotonically wherever the quantity has a direction; an enables byte switches a stage and moves no control; and no slot outside the files moves a chain at all. Without that the coverage equation would hold just as well with every row filed as whichever form is cheapest to defend.
