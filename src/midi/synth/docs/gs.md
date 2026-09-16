# GS — the compatibility contract and the extensions on top of it

The synth answers Roland GS. This page holds what the manual cannot: which device defines the target, what an address is promised to do, where libsonare diverges deliberately, and the rules that keep the divergences and the extensions from contaminating each other.

Per-address detail — offsets, ranges, defaults — is not restated here. It comes from the **Roland SC-8850 Owner's Manual, Appendices, "Parameter Address Map"** (p.235 onward; `cdn.roland.com/assets/media/pdf/SC-8850_OM.pdf`), and the reasoning for an individual row lives beside that row in `gs_address_table.h`. A second copy of either here would be free to drift from the one that decides.

**The manual is a proxy for the machine, and where the two disagree the machine decides.** Which of the two manuals a row was written from no longer changes what it says: the maps have been compared address by address and agree on size, data range and power-on default everywhere they overlap, so the only rows where the document matters are the nine points below. What is left is document against machine, and `make gs-unit-diff` closes that by measurement against an archive of what an individual SC-8850 answered, and produces the work list (`tools/gs/unit-diff.json`; `tools/gs/docs/unit-diff.md` is how to read it). The corpus census beside it (`tools/gs/docs/census.md`) bounds the table from the other side: it says what real files reach and is blind to every address they never send.

**This page is normative and says nothing about progress.** It describes what must be true, not what is true today; an item here the code does not yet do is work outstanding rather than a documentation error. Coverage is a number the address table and its tests produce, not a status section that would drift the moment it was written.

## What is being made compatible is the control protocol, not the sound

**GS is implemented here as a way to control physical-model and FM instruments, and the resemblance to a sound module stops at the wire.** What is owed by this page is that a file's messages arrive, are understood, and move the parameter they name in the direction and by the amount the manual gives. **What any slot should sound like is not this page's business at all** — a report that a program's timbre differs from the hardware's is never a defect against this page, whichever way it is meant to differ. Which reference a slot is aimed at is decided in `tools/voicematch/docs/objective.md`, and it is not one answer for the whole bank: a slot naming a real instrument is aimed at a modern recording of it, while a slot naming a sound the machine invented is aimed at the machine, timbre included. Do not read either rule off this page, and do not restate them here — two copies of that boundary would part company on the first slot anybody argued about.

Two things follow, and both are load-bearing:

- **`AUDIBLE` is a relative claim.** Its test is that changing the value produces a measurable difference of the right sign and rough magnitude — never that the resulting audio matches a reference recording. There is no reference recording, and there is not going to be one.
- **A parameter with no physical-model counterpart still has to arrive.** Where the hardware's mechanism has no analogue in the model, the address takes the closest control the model does have, and where there is none at all it takes a row with a reason. It does not take silence.

## The target is the SC-8850, and that includes the SC-88Pro

The two Parameter Address Maps agree on every address they share — size, data range, power-on default and alias annotation alike — and part company at ten points. At nine of them the SC-8850 is the superset, so targeting it *includes* SC-88Pro compatibility rather than trading against it: an SC-88Pro file selects the SC-88Pro tone map (`40 4x 00` = `03`) and plays. The tenth runs the other way, which is why the superset is stated as a count of points rather than as a principle. The bulk-dump space is a separate address space and has its own three differences, below.

| | SC-88Pro | SC-8850 |
|---|---|---|
| `00 00 7F` SYSTEM MODE SET | `00`/`01` — Mode-1 / Mode-2 | Range `00`–`01`, but **only `00` acts**: "the same processing will be carried out as when GS Reset is received. Other values are ignored." Mode-1, single module, Rx only |
| Mode-2 restrictions | seven parameters unusable in Mode-2 | none |
| `00 01 xx` CHANNEL MSG RX PORT | 32 blocks, ports A/B | 64 blocks, ports A–D |
| `20 b0 pp` SOURCE TONE# (MAP) | `01`–`03` | `01`–`04` |
| `21 dA rr` SOURCE DRUM SET# (MAP) | `01`–`03` | `01`–`04` |
| `22 ** **`–`27 ** **` user effect types and stored patches | 64 stored effect types; 16 patches, each holding a patch common and two patch parts | **absent** |
| `40 1x 30`–`37` TONE MODIFY 1–8 | reachable from the address and NRPN `01 08`/`09`/`0A`/`20`/`21`/`63`/`64`/`66` | the same, **plus CC#71–78** — the map prints the controller in the row's own alias |
| `40 4x 00` TONE MAP NUMBER | `00`–`03` | `00`–`04` |
| `40 4x 01` TONE MAP-0 NUMBER | `01`–`03`, default `03` | `01`–`04`, default `04` |
| `50 ** **` / `51 ** **` | the opposite group's blocks | absent — the port selects the group |

**Two consequences worth stating because they save work rather than cost it.** There is no double-module mode to implement, and the 64 parts are four ports of sixteen rather than a second address space — so a part is still addressed by one block nibble and the port carries the group.

**CC#71–78 are listed above rather than left to the GM2 paragraph because that is where they would be argued away.** They are the third entry point to `40 1x 30`–`37` and the one-storage rule's table carries them unconditionally, which is right for the target and reads as an error against the SC-88Pro's map, where the same rows name only the NRPN. A part that answers CC#74 with a filter sweep is the SC-8850 behaving as documented; narrowing it to match the older machine would be a regression dressed as a correction.

**The tenth point is the one that costs something, and it costs a row rather than a mode.** A stored patch and a stored effect type are a front panel's memory, which a renderer does not have and would not gain by being told about; so `22 ** **` through `27 ** **` are `IGNORE`, one row per block, for the reason `50 ** **` is — the statement being made is that a whole layer is absent, and that is one statement however many parameters would have sat inside it. What this rules out is reading "the SC-8850 is the superset" as licence to leave the region unrowed: an address the target machine dropped is still an address a real SC-88Pro file sends, and an unrowed address is a defect.

`20 bn pp` USER INSTRUMENT is not in the table above, because both machines have it: two banks of stored tone edits reachable as GS variations 64 and 65. It is `IGNORE` on the same grounds as the stored patches — a program here selects a bank preset or the model floor directly, so there is no stored tone for an edit to sit on — and it is a gap in what libsonare implements rather than a difference between the machines.

A tone map is audible exactly where it fails to reach a kit: sixteen of the twenty-six rhythm sets were introduced by a later map, while every melodic variation voiced so far is an SC-55 tone that all maps reach. Both voice banks read it through the same rule, because a parameter must not do something different depending on which bank answered.

The SC-8850 also answers GM2, which the SC-88Pro does not. GM2's additions are all second addresses onto GS mechanisms already present (Controller Destination onto `40 2x`, Key-Based Instrument Control onto `41 mn rr`, Scale/Octave Tuning onto `40 1x 40`–`4B`, Modulation Depth Range onto `40 2x 04` and RPN `00 05`). They are covered by the one-storage rule below rather than by new state. CC#71–78 arrive with the same machine but are not GM2's doing — the SC-8850's own GS map prints them in the `40 1x 30`–`37` rows, which is why they are a line in the table above.

## The bulk-dump space is a second address space, and none of it is rowed

Bulk dump moves packed state rather than parameters: a `DT1` to one of these addresses carries a block of the machine's memory, not a value at a named offset, and the request that asks for one is an `RQ1` to `0C 00 00` whose size field selects content instead of length. It travels on the same command as an individual write, so real files reach it — the corpus touches `29`, `48` and `49` — and the address table has a row for none of it.

| | SC-88Pro | SC-8850 |
|---|---|---|
| `08` SETUP | 1 packet (`08 00 00`–`08 00 7F`) | 2 packets (`08 00 00`–`08 01 7F`) |
| `28` USER TONE BANK #64 / #65 | 11 packets each | 11 packets each |
| `29` USER DRUM SET #65 / #66 | 12 packets each | 12 packets each |
| `2A` USER EFX #1-64 | 16 packets | **absent** |
| `2B` USER PATCH #1-16 | 96 packets | **absent** |
| `48` / `49` group A, `58` / `59` group B | present | present |
| `68` / `69` group C, `78` / `79` group D | **absent** | present |

**The three differences are the parameter map's own three, restated in the other space**, which is the useful thing to know about them: `2A` and `2B` are the bulk form of the user effect types and stored patches the SC-8850 dropped, groups C and D follow from four ports against two, and SETUP grew a packet carrying them. Nothing here is an independent decision a machine switch would have to make.

**Leaving the space unrowed is deliberate, and the census ceiling is what keeps it visible.** A bulk dump is state to unpack into the storage the individual addresses already own, so an `IGNORE` row would record a reason that stops being true the moment it is unpacked, and it would move the coverage ratchet by sixty addresses without one file being better understood — the move `gs_address_census_test.cpp`'s third ceiling exists to expose. These addresses get rows in the change that reads them, not before.

## Every address is assigned, and "not in the table" is not an assignment

Each address in the space carries exactly one of four levels. **An address with no row is a defect, not a silence** — that is the property this scheme exists to make checkable.

| level | meaning | how it is verified |
|---|---|---|
| `AUDIBLE` | received, held, and reflected in the audio | changing the value produces a measurable difference |
| `STATE` | received and held, not reflected in the audio | reads back through a round-trip test |
| `ACCEPT` | received and discarded | does not disturb the interpretation of what follows |
| `IGNORE` | deliberately not implemented | the reason is written in the table |

The address table is one `constexpr` array, and a decoder walks it. An address the table does not name increments `unknown_writes` rather than being dropped, and the unit tests pin that counter at zero for every message they construct. Against a corpus it is a ratchet rather than a zero: `gs_address_census_test.cpp` counts the distinct census addresses no row claims and the corpus file-touches they carry, and both ceilings only ever move down. The difference matters — the space is not covered today and the number says by how much. This is the same discipline as the parity allowlist: nothing is silently discarded, and a gap is a number rather than an absence.

**Undefined regions get rows too.** A multi-byte write starting before such a region runs through it, and real files carry SC-55-era addresses the SC-8850 dropped, so they are `ACCEPT` and the unknown counter keeps meaning something. A range covers one mid byte only: one spanning two would step through low bytes above `7F`, which are not addresses.

**The controller-destination block is a matrix, and libsonare models it as one.** `40 2x xx` gives each part six controller sources — modulation, bend, channel and polyphonic aftertouch, and two assignable — each routed to the same eleven destinations. The low byte carries the source in its high nibble and the destination in its low one, so a table row names the **destination** while the address names the source: one conversion per destination, one stored set per source, and **a destination takes the sum over sources of what each is worth where it presently sits**. A row is `AUDIBLE` exactly when libsonare both routes that destination and receives that source, and where a sum can pass an end the end is an identity rather than a bound — two sources at full render exactly as one at full does.

**A destination row's `ACCEPT` reason has to say why *that* row is unreadable.** A blanket reason for the whole block stopped being true the moment anything in it was routed, and a reason that would still be printed unchanged after the thing it excuses was built is not a reason.

**The two assignable sources are a controller named by number**, and `40 1x 1F`/`20` CC1/CC2 CONTROLLER NUMBER is where the name lives. A part therefore has to know where an arbitrary controller *sits*, not only what it means, so every controller's position is recorded by number in one place and the source reads through the number rather than storing a value beside it. Reading through is what makes the order a file writes in stop mattering.

**The level is measured rather than declared, and on both voice banks.** Every row is probed with a value it accepts, and a render carrying that write is compared against one without it: `AUDIBLE` requires the audio to move and the three levels below it require it not to, so a row cannot sit at the wrong level in either direction. `STATE` additionally requires the byte to reach a mirror the player exposes and `ACCEPT` / `IGNORE` require it not to — the difference between a value held for a consumer that does not exist yet and a value dropped. A part plays on the SoundFont bank or on the physical-model floor depending on whether a preset answered its program, and the two apply a parameter through different code, so a probe on one bank alone measures half the implementation. A dimension one bank genuinely cannot answer is excused beside the gate with its reason and is required to keep failing, so the exclusion cannot outlive the gap.

**A byte the engine never asks for is `STATE`, not `AUDIBLE`.** An effect bus takes a fixed set of fields off its config, and a value outside that set is held faithfully, converted faithfully, and never read. Raising such a row means giving the engine the control, not editing the row.

**`AUDIBLE` is conditional on the build, and the gate refuses to run without it.** The system-effect, master-EQ and EFX blocks reach the audio only inside `SONARE_MIDI_WITH_FX`, and an EFX chain only when the host supplies an insert factory. In a build without either, those rows are received and silent — so the gate fails rather than passing them dry, because a green run in that configuration would be evidence of nothing.

## One parameter has one storage location

GS reaches the same parameter from up to three directions. **Holding a second copy is the defect this rule exists to prevent**: write through the CC, read through the SysEx, and the two disagree.

| parameter | CC | SysEx | NRPN |
|---|---|---|---|
| Tone number | 0 + program change | `40 1x 00`–`01` | — |
| Tone map | 32 | `40 4x 00` | — |
| Part level | 7 | `40 1x 19` | — |
| Part panpot | 10 | `40 1x 1C` | — |
| Reverb send | 91 | `40 1x 22` | — |
| Chorus send | 93 | `40 1x 21` | — |
| Delay send | 94 | `40 1x 2C` | — |
| Vibrato rate | 76 | `40 1x 30` | `01 08` |
| Vibrato depth | 77 | `40 1x 31` | `01 09` |
| Vibrato delay | 78 | `40 1x 37` | `01 0A` |
| TVF cutoff | 74 | `40 1x 32` | `01 20` |
| TVF resonance | 71 | `40 1x 33` | `01 21` |
| EG attack | 73 | `40 1x 34` | `01 63` |
| EG decay | 75 | `40 1x 35` | `01 64` |
| EG release | 72 | `40 1x 36` | `01 66` |
| Pitch fine tune | — | `40 1x 2A`–`2B` | RPN `00 01` |
| Bend range | — | `40 2x 10` | RPN `00 00` |
| Mono/poly mode | 126 / 127 | `40 1x 13` | — |
| Drum level | — | `41 m2 rr` | `1A rr` |
| Drum panpot | — | `41 m4 rr` | `1C rr` |
| Drum reverb send | — | `41 m5 rr` | `1D rr` |
| Drum chorus send | — | `41 m6 rr` | `1E rr` |
| Drum delay send | — | `41 m9 rr` | `1F rr` |

**Coinciding ranges are not an alias, and the map is what settles it.** It annotates an alias inside the row that has one — `(=CC# 7)`, `(= RPN#1)`, `(=NRPN# 8/CC#76)` — and annotates `40 1x 16` PITCH KEY SHIFT with nothing, however exactly its range coincides with RPN `00 02` Master Coarse Tuning's. Where the manual does document such a pair it says the two "are added together to determine the actual pitch sounded by each Part": two locations that add, not one that overwrites. The controller-destination pitch and cutoff controls stand in the same adding relation to the fields they land on and are likewise not in the table.

`41 m1 rr` PLAY NOTE NUMBER is **not** NRPN `18 rr` Drum Instrument Pitch Coarse: the first replaces which sample a note plays, the second shifts the pitch of the one it already plays. They are adjacent in the map and mean different things.

**`40 1x 13`'s annotation `(=CC# 126 01/CC# 127 00)` carries the controllers' data bytes rather than parameter values** — Mono Mode On's data byte is a voice count and Poly Mode On's is always `00`. Read as parameter values they would invert the row's own default. The two controllers also do something the SysEx does not: both are All Notes Off. One storage location, two entry points, one of which silences the part on the way in.

**The map number is written two ways and only one of them is one-based.** The `m` nibble of `41 mn rr` is zero-based — `41 0n rr` is MAP1 — while the *value* of `40 1x 15` USE FOR RHYTHM PART is one-based. Both numberings are the manual's own, and an address written as though it carried the value's numbering lands in the other map's setup, where every note still sounds.

The mapping is verified by a round trip over every pair: written from either side, read back from either side, equal. That shows the two entry points agree; it cannot show the pairing is the right one, since a table pairing the wrong two addresses round-trips just as cleanly. What can is a measured unit's alias scan, which sends each controller and reports which address moved.

## Where a parameter is applied is part of what it means

- **`40 1x 40`–`4B` SCALE TUNING is indexed by the key that was struck**, which is the whole difference between a temperament and a part-wide detune: one byte per pitch class, so a byte written for C moves every C in every octave and nothing else. A note a substitution redirected keeps the tuning of the key that asked for it.
- **`40 1x 17`–`18` PITCH OFFSET FINE is the one pitch parameter measured in hertz** — the manual states outright that the shift "will be identical no matter which note is played". The cents it works out to are a function of the key, so folding it into the part's constant offset the way MASTER TUNE and the two key shifts are folded would turn it into the parameter it is printed to be distinguished from. A downward shift past a low note's own frequency names no pitch, so the result is bounded at the lowest key's: a finiteness bound rather than a modelled behaviour, since no source says what the hardware does there.
- **`40 1x 1D`/`1E` KEY RANGE is a refusal and not a mute.** A key outside the range is not received: it takes no voice, does not choke what the part is already sounding, and does not spend an armed portamento — so the test precedes all three, and the choice of voice bank with them. A low above the high names no key at all rather than a range to be corrected.
- **`40 1x 02` RX CHANNEL is a layering parameter, not a permutation.** Nothing stops two parts naming one channel, and that is what it is for: a channel message reaches every part claiming its channel, and a part claiming none is not reached at all. Routing a message to at most one part would be wrong for most of the files that use it.
- **The rhythm-part exclusions are decoded at the note-on, not at the write.** A part can take a key shift and *become* a rhythm part afterwards, and it then has to stop being transposed; a value already folded into a cents offset cannot.

## The parameters a rhythm part does not take

MASTER TUNE, PITCH FINE TUNE and the coarse tuning all reach a rhythm part. `40 00 05` MASTER KEY-SHIFT, `40 1x 16` PITCH KEY SHIFT and `40 1x 13` MONO/POLY MODE do not, each because the manual prints the exclusion beside it — "Even if you adjust Key Shift for all Parts, the pitch of the Drum Part will not be affected" for the first two, "For a Drum Part, changing the Mono/Poly Mode setting will not affect the sound" for the third. Fine Tune sits on the same page as the part-level Key Shift with no such note, and that asymmetry is what makes each of these a statement rather than an omission.

`40 1x 14` ASSIGN MODE is deliberately not in this list — the manual gives it no exemption, and its SC-55-map default is SINGLE *for the drum part specifically*, which is the opposite of an exemption.

**A rhythm part's chorus send is stored and not sounded on a measured unit, and libsonare sounds it.** The part accepts `40 1x 21`, reads it back, and the recording finds nothing where the reverb send on the same part in the same run is plainly audible. It is rhythm mode rather than part 10: part 2 hears the chorus while melodic and stops the moment `40 1x 15` USE FOR RHYTHM PART is set, nothing else changed. Why the send is inert — ignored, the part off the bus, or the return muted — is not established, so there is no mechanism to reproduce, only an outcome; and this page's own rule is that a parameter with no counterpart still has to arrive rather than take silence. So the divergence is stated and the send keeps working. It is worth knowing when reading the per-program CC93 weighting the fallback bank applies: on the hardware that weight does nothing for a kit.

## The drum setup block and the user drum sets

`41 mn rr` is the per-note edit on a map's kit; `21 dn rr` is two kits a file assembles note by note, selected by **program 64 or 65** — numbers `kGsDrumKits` leaves free, which is what lets them mean this.

- **The set is the stored kit and `41 mn rr` is the edit on top of it.** A note resolves its source first, then reads one merged set of per-note edits: the set's stored ones with the map's written ones over them, field by field behind the flag that owns each. Layered rather than one re-initialising the other, which is what lets both sides hold the value that changes nothing. Both voice banks read the merged struct, so a note cannot be edited differently depending on which answered it.
- **The drum setup parameters have no power-on value, and that is what fixes their defaults.** The manual gives them none because a drum set change re-initialises them to what the kit specifies, so each is held at the value that changes nothing — `7F` for LEVEL and the three multiplicand sends, `40` for PANPOT — and read only once written. A default that was not the identity would make an unwritten parameter overrule the kit. A note number has no such value at all, so nothing but the written flag distinguishes it from an unwritten one.
- **A note-number substitution moves the voiced note, not the key.** The written note selects the SoundFont zone or the model floor's kit piece *and* is the note the voice is started at; the struck note keeps the per-note edit slab, the exclusive-group choke and the voice, because that is what a note-off matches. Leaving the voiced note behind gives the right kit piece at the wrong pitch, which still moves the render — a per-parameter identity assertion's to catch, not an audibility probe's.
- **`41 m3 rr` ASSIGN GROUP replaces the kit piece's own exclusive class rather than adding to it**, which is what makes `00` the value that says something: a group is a name, so writing `00` takes the note out of every group. Both banks read the same value, the zone's `exclusiveClass` and the model kit piece's group being one quantity from two directions. A group is also the one drum parameter that says nothing about a single note — it is a relation.

## The receive switches

`40 1x 03`–`12` plus `40 1x 23`–`24` is eighteen per-part switches saying whether the part receives one class of message at all. Each is `00`/`01` and powers on `01`. They are one row each rather than one folded row, which would have to claim one level for all of them.

- **A switch that is off is an absence, not a neutral value.** The part keeps what the last received message left, and no later message of that class corrects it until the switch comes back on. That is why the gate is at the message rather than at the value: a control change the part refuses does not even record where its controller sits.
- **`40 1x 24` RX BANK SELECT LSB is the one that breaks that rule, and the map says so.** With it off, CC#32 is read as `00` rather than not read at all, so the part goes back to whatever an LSB of zero means for it instead of staying where the last accepted message left it. It is the only switch in the set that writes something while it is off.
- **`40 1x 23` Rx. BANK SELECT covers both halves of the bank number**, since the map states it against the Bank Select message as a whole — and a MIDI 2.0 program change that carries its bank inside itself with them: one storage location, and a second transport to it does not get to arrive past a switch that closed the first. It does not cover `40 1x 00` or `40 4x 00`, which are SysEx addresses rather than a controller.
- **Three of the eighteen have a default that depends on which reset arrived, and the three do not agree with each other.** A GS Reset leaves all eighteen on. A System On of either level clears `Rx. NRPN`, the NRPNs it would carry being Roland's rather than either GM specification's. The two bank-select switches are cleared by a GM1 System On alone and left on by GM2 — which is what keeps a GM1 file on the plain program it names while still letting a GM2 file reach a variation. So the two System On messages are not interchangeable.
- **Controllers 120–127 are outside `Rx. CONTROL CHANGE`, because they are outside control change.** MIDI calls them channel mode messages, and the map gives them no switch while giving one to each named controller below 120. So CC126/CC127's alias onto `40 1x 13` stays unconditional, and All Notes Off still stops a note on a part that has stopped taking controllers.
- **Two readings are deliberate rather than the map's.** CC5 PORTAMENTO TIME and CC84 PORTAMENTO CONTROL are left ungated, `Rx. PORTAMENTO` being named for the controller that turns portamento on. Data entry belongs to whichever parameter number is selected rather than to a switch of its own, so `Rx. RPN` and `Rx. NRPN` decide it at the value.

## Deliberate divergences from the manual

These are decisions, not gaps. Each one is here because it would otherwise be re-litigated or re-discovered.

- **`40 1x 1C` = `00` and `41 m4 rr` = `00` mean random pan on the hardware. libsonare treats them as centre.** Randomness breaks the bit-identical bounce contract, and a deterministic substitute would not match the hardware either, so it would buy divergence without buying fidelity. This is also the one value at which `40 1x 1C` and CC10 part company, and they are still one storage location — the manual's own note reads "(=CC# 10, except RANDOM)", so `00` is hard left through the controller and centre through the address.
- **A drum note's reverb, chorus and delay sends are multiplicands of what the note sends into that unit, not additions to it.** The manual is explicit (`0.0 – 1.0`, "Multiplicand of the part reverb level"). One rule covers all three: the zone's own send and the part's CC91 / CC93 / CC94 are summed and clamped, and the multiplicand attenuates that whole — so a note's own send taken to zero takes it out of that bus however loud its part is sending, which adding does not. Delay is the degenerate case rather than a second rule, SoundFont having no delay generator. The one thing the multiplicand cannot reach is a difference the hardware does not have: a kit zone carries a send of its own, so a part taken to zero still leaves the zone's share in the bus.
- **A drum instrument uses its chorus and delay sends at the same time, which the hardware cannot.** The manual prints the restriction beside the block and names neither a winner nor a rule for choosing one, which is what a restriction rather than a behaviour reads like. It is a resource limit under the rule below, so libsonare is free of it.
- **"Ignored, never clamped" is libsonare's rule and not the machine's, and a measured unit disagrees at two addresses.** `40 01 32` REVERB PRE-LPF and `40 01 39` CHORUS PRE-LPF clamp an out-of-range value to `07` where `40 01 30`, `31` and `38` — same block, same `00`–`07` range — leave the byte untouched. Both PRE-LPFs are `STATE` here, so nothing audible turns on it; what does turn on it is the mirror a `STATE` row promises, which reports the value libsonare ignored where the machine would report `07`. Stated rather than adopted: one rule over the whole space is worth more than matching an inconsistency two addresses deep, and the row that changes this is the one that gives the engine the parameter.
- **An out-of-range `40 1x 15` USE FOR RHYTHM PART reads as drum map 1, where every other address ignores a value it does not accept.** A file asking for a map the machine does not have still means the part to be drums, and map 1 sounds it as drums where ignoring the write leaves it melodic. This is libsonare's one departure from its own "ignored, never clamped", and it is one because the parameter selects a resource rather than sets a value.
- **`40 1x 1A`/`1B` VELOCITY SENSE DEPTH and OFFSET get a curve the manual does not give, and the depth pivots on the centre of the velocity axis rather than on zero.** The map gives them a range and a power-on `40` and no mapping, as it does MASTER VOLUME. A depth read as a plain multiply would make `00` silence the part; read as a slope through the centre it makes every key sound alike, which is what "velocity sense" names — the part stops answering velocity rather than stopping. Offset then moves the whole curve, and the result is clamped to 1–127 because a shaped `00` is a note-off on the wire and this is a note that was struck. The shaping is applied to the struck velocity ahead of both voice banks *and* ahead of the zone velocity ranges, so a part made insensitive picks the layer the shaped velocity names rather than the one the wire carried.
- **`40 00 06` MASTER PAN is a balance on the finished mix, not a re-pan of the parts.** Each part already carries its own position, and a master that re-positioned them would collapse the image rather than move it. Attenuating only the far leg is also what keeps the power-on `40` at exactly unity on both legs, which a constant-power law would not: a master that is not bit-exact at its default moves every bounce in the repository. `40 00 04` MASTER VOLUME takes the square law CC7, velocity and the drum-note level already use, since the manual gives a range and no curve.
- **`40 1x 14` ASSIGN MODE `01` LIMITED-MULTI and `02` FULL-MULTI are one behaviour here, and only `00` SINGLE branches.** The manual separates the two by how long a repeated note's predecessor survives, which is the machine deciding when to start stealing — a voice budget, so a resource limit rather than behaviour owed fidelity. SINGLE is a different kind of statement: the previous note is stopped because it was asked to be, not because there was nowhere to put the new one. Its stop is a short release rather than a cut, since a cut clicks and the voice's own release would leave audible the note it was told to silence; both voice pools take the same fade, because a part moves between them on a program change. The default is `01` for every part; the per-part split the manual prints alongside it belongs to the SC-55 map, which is not the target.
- **`40 03 1B`–`1E` EFX CONTROL SOURCE / DEPTH 1 and 2 are `IGNORE`, and that is a structural statement rather than a gap.** They let a controller move an insertion-effect parameter while the effect runs. The chain here is realised from its type and its twenty parameters and is not re-parameterised afterwards, so there is nothing for a source to drive; implementing them means giving the chain that hook first, and the rows come off `IGNORE` in the same change. `40 03 1F` SEND EQ SWITCH is next to them for a smaller reason: there is one EQ stage and it is bypassed per part at `40 4x 20`, so an EFX return has no separate one to switch.
- **The eight part edits at `40 1x 30` name a sampler's stages, and the physical-model bank applies them to the stages it has.** TVF cutoff and resonance are a filter after the voice, the three EG times scale the amplitude envelope, and the three vibrato controls move the pitch LFO. Two consequences follow from the model voice being a model rather than a sample: an edited filter is engaged even where the patch had left it wide open, since the manual offers no way to ask for a filter and then not hear it; and the vibrato-delay edit cannot be a plain multiply, because a model voice's LFO has no onset delay of its own and scaling zero would leave the parameter inert on exactly the bank that needs it most.
- **`40 4x 21` OUTPUT ASSIGN is `IGNORE` because libsonare has one output pair, and the second pair is not a quieter version of the first.** The map gives `00` OUTPUT-1, `01` OUTPUT-2, `02`/`03` the second pair's legs alone. A measured unit moved to `01` leaves the insertion effect and the system reverb behind and arrives dry, with a volume controller still reaching it — so the row is a jack rather than a signal-path setting, and there is no approximation of it on a single pair worth making. Routing every part to one output regardless is what a renderer with one output can honestly do; a part the file meant to send elsewhere is heard rather than lost.
- **`40 4x 01` TONE MAP-0 NUMBER survives every reset.** Power-on, GS Reset, GM System On and System Mode Set all leave it alone. It is the one part field a blanket re-initialisation must skip.
- **A drum set change re-initialises that map's setup parameters.** Program change on a rhythm part clears the `41 mn rr` state for that map.
- **`50 ** **` and `51 ** **` are the opposite group's part and drum blocks, and they are `IGNORE` for the same reason `00 01 xx` is: one port.** On the SC-88Pro a file reaches the other sixteen parts by addressing `50` where it would address `40`; the SC-8850 has neither address, the port deciding which group `40` and `41` mean. Both readings agree here, because libsonare receives one port and has one group of sixteen. Each is one row over its whole block rather than a mirror of the block it shadows: the statement being made is that a group is absent, and that is one statement however many parameters would have sat inside it.

## The extensions libsonare adds

The hardware allows exactly one insertion effect for the whole module — "you can select one Insertion effect, and specify for each Part whether or not the sound will be routed through the effect", and turning it on for two parts mixes them into that one unit. That is a limit of the machine, not a property of GS worth preserving, and libsonare lifts it.

**The extension is reachable only from addresses a spec-compliant file cannot send.** That is what makes it safe without a feature flag: a GS file is inert against it by construction, and the behaviour it gets is exactly the hardware's — one unit, with every part that switches it on summing into it.

- **Unit 0 stays at `40 03 xx`, unchanged.** Its semantics, defaults and parameter layout are the manual's, including the two control sources and the send EQ switch, which are the spec block's alone: a unit that cannot be re-parameterised while it runs does not gain the ability by being numbered.
- **A unit's number is its own address nibble.** Units live at `40 3u xx`, each carrying the same `00`–`1F` layout, and `40 30 xx` is unit 0 — a second way into the storage `40 03 xx` writes rather than a hole in the space. `40 30`–`40 3F` carries no row in either map, so nothing collides, and the whole block is checked against a machine rather than against a document: `make gs-unit-diff` reports how many of its 512 addresses a read reached, and it is zero.
- **Routing extends the value range of an existing address rather than adding one.** `40 4x 22` PART EFX ASSIGN is specified as `00` = BYPASS, `01` = EFX; `02`–`10` select units 1–15. `00` and `01` keep their exact meanings, and a value outside `00`–`10` is ignored like any other out-of-range value rather than read as some unit.
- **A part reaches its unit after its own insert.** The part's own stage — a host insert or the bank's default rig — runs on the part's bus, and what the unit receives is the sum of the parts routed to it.
- **Parts assigned to the same unit sum into that unit's single instance**, as on the hardware: the unit runs once over the sum, so two parts through one overdrive intermodulate. The unit count changes; the summing does not.
- **A unit's output is one signal, so what is downstream of it is the unit's and no longer the part's.** Its send to the system effects is the unit's own (`40 3u 17`–`19`), and its master-EQ routing follows the parts that feed it only where they agree — a bypass holds when every part asked for it, and otherwise the unit takes the EQ, which is what every part powers on with. This is the price of the summing rather than a separate decision.
- Real hardware ignores an unknown address and treats an out-of-range `40 4x 22` as its own business, so a file using the extension still plays there, with one insertion effect.

**Out-of-range `40 4x 22` values exist in real files, and the extension is what they now reach.** They are not spec-compliant — the manual gives `00`/`01` — so the rule below is not broken, but it is worth stating that it is not vacuous either. That is accepted rather than guarded: such files already diverge from the hardware today, since any non-zero switches the part on; the values look like authoring slips; and a part hearing the module's own EFX is closer to the intent than silence. Gating the extension behind a flag to protect them would cost the property that makes it usable.

**Two different things wear the phrase "what the hardware does", and collapsing them is the failure this paragraph exists to prevent.** *One unit for the whole module* is a resource limit — a property of the machine that was built, not of GS — and it is lifted. *Parts on one unit sum into it* is not a limit at all: it is what an effect is. Two guitars into one distortion pedal intermodulate because that is what a pedal does, and a file that routes two parts through one unit was written for that sound. So "restore the summing" never means "cap the units", and any change that reduces the reachable unit count to satisfy a compatibility argument has misread this. The reachable range lives in the `40 4x 22` row of the address table, not in prose, so narrowing it is a diff on a row rather than a reading.

## The per-part insert slot is a different thing from the EFX, and both reach the part

`Sf2PartInsert` is the host's own insert on a part, built through an injected factory. It is not GS and does not appear in the address space.

**A part may carry both** — its own insert (a guitar amp, `voicing.md`) and the file's EFX — and they run **in series**, the part's own first, on opposite sides of the merge: the insert is the part's and runs on the part's bus, the EFX is the unit's and runs on the sum. The slot is a chain, not a choice: a guitar with an amplifier still gets the file's chorus. The bank's default rig is the third thing that can occupy the part's stage and the only one that yields, since a configured insert or a route into a unit is the host or the file speaking and either outranks a default.

What the route changes beyond the chain is the part's send to the system effects. A part routed into a unit feeds reverb, chorus and delay from that unit's **post**-effect bus by the unit's own sends, and its pre-effect CC91 / CC93 / CC94 send is suppressed so the wet tail follows the processed signal without double-sending. A part running only its own insert or the bank's rig has no EFX sends to answer to and keeps the CC-driven send — carrying a chain is not the same question as carrying the file's effect.

## Rules

- **An address is added to the table in the same change that implements it.** A parameter that works but has no row is invisible to the coverage test, which is the only thing that can say the space is covered.
- **Never widen the space to silence an unknown.** An address that turns up in real data and is not in the manual is either a row with a reason or a defect to investigate; it is not an `ACCEPT` added to make a counter go to zero.
- **An extension must be unreachable from a spec-compliant file.** If a proposed extension can be triggered by a message a real GS sequencer emits, it is a divergence rather than an extension and does not belong here.
- **A resource limit is not a behaviour, and only behaviour is owed fidelity.** Before matching the hardware, say which of the two a rule is: could the machine have done otherwise with more voices, more units, more memory? Then it is a limit and libsonare is free of it. Does it follow from what the signal path *is*? Then it is behaviour and it is binding. The one-insertion-effect ceiling is the first kind; parts summing into the unit they share is the second.
- **Behavioural fidelity beats convenience when the two conflict.** The double-amp a file gets by selecting a guitar multi over an already-amped part is what the hardware does; suppressing it automatically would make the sound depend on a hidden rule.
- **Reset defaults are part of the contract.** They are not implementation detail: a file that sends GS Reset and then nothing else is entitled to the hardware's state. Changing one moves the goldens and belongs in its own change.
- **A reset default may also depend on which reset arrived, and that axis has no place in the table at all.** `def` is defined as the GS Reset state, so a row that a GM1 or GM2 System On leaves elsewhere is saying something the table cannot hold and `gs_reset_default` cannot answer — it is a function of the message, not of the address. What holds it is a test pinning every such bit after each of the three resets; a row of this kind is added there in the same change or it is unguarded.
- **A reset default may be a function of the address, and where it is, the function is the contract rather than the row's `def` byte.** Two parameters power on differently per part — `40 1x 02` RX CHANNEL, where every part listens to its own, and `40 1x 15` USE FOR RHYTHM PART, which only part 10 powers on holding. Sixteen instances cannot fit in one byte, so `gs_reset_default` answers per address and `def` is defined as its answer at the row's base, a definition the table's own self-check enforces. Stating such an exception in a comment beside the row instead would leave it invisible to that check and to the unit comparison, which is the same failure a prose-only parity exclusion is.
