# GS — the compatibility contract and the extensions on top of it

The synth answers Roland GS. This page is the specification of what that means here: which device defines the target, what every address in the space is promised to do, which parts of the space libsonare adds to, and the rules that keep the two from contaminating each other.

Per-address detail — offsets, ranges, defaults — is not restated here. It comes from the **Roland SC-8850 Owner's Manual, Appendices, "Parameter Address Map"** (p.235 onward; `cdn.roland.com/assets/media/pdf/SC-8850_OM.pdf`), which is the source of record. What this page holds is what the manual cannot: the decisions, the places libsonare diverges from it deliberately, and the extensions.

**A row in the address table came from the SC-88Pro manual unless it says otherwise, and the two are not the same document.** The transcription the table was built from is the SC-88Pro Owner's Manual, and the SC-8850's own map has been checked only where a row of this page names a difference. That is a gap in provenance rather than a known error — the sampled points agree — but "the manual says" about an untouched row means the SC-88Pro's, and the SC-8850's is the one that decides.

Two blocks have since been read out of the SC-8850's own map rather than inherited: the **part parameters `40 1x 00`–`4B`**, where the two machines turn out to list address for address the same set, and the six difference points above. Closing the gap elsewhere is reading the map, not re-deriving anything.

**This page is normative and says nothing about progress.** It describes what must be true, not what is true today; an item here that the code does not yet do is work outstanding rather than a documentation error. Coverage is a number the address table and its test produce, not a status section that would drift the moment it was written.

## What is being made compatible is the control protocol, not the sound

**GS is implemented here as a way to control physical-model instruments, and the resemblance to a sound module stops at the wire.** The voices are physical models fitted against modern references (`voicing.md`, `tools/voicematch/`); sounding like an SC-88Pro is explicitly out of scope, and a report that a program's timbre differs from the hardware's is not a defect against this page. What is owed is that a file's messages arrive, are understood, and move the parameter they name in the direction and by the amount the manual gives.

Two things follow, and both are load-bearing:

- **`AUDIBLE` is a relative claim.** Its test is that changing the value produces a measurable difference of the right sign and rough magnitude — never that the resulting audio matches a reference recording. There is no reference recording, and there is not going to be one.
- **A parameter with no physical-model counterpart still has to arrive.** Where the hardware's mechanism has no analogue in the model, the address takes the closest control the model does have, and where there is none at all it takes a row with a reason. It does not take silence.

## The target is the SC-8850, and that includes the SC-88Pro

The GS address space is identical between the two machines except at six points, and the SC-8850 is the superset. Targeting it therefore *includes* SC-88Pro compatibility rather than trading against it: an SC-88Pro file selects the SC-88Pro tone map (`40 4x 00` = `03`) and plays.

Where the two differ:

| | SC-88Pro | SC-8850 |
|---|---|---|
| `00 00 7F` SYSTEM MODE SET | `00`/`01` — Mode-1 / Mode-2 | Range `00`–`01`, but **only `00` acts**: "the same processing will be carried out as when GS Reset is received. Other values are ignored." Mode-1, single module, Rx only |
| Mode-2 restrictions | seven parameters unusable in Mode-2 | none |
| `00 01 xx` CHANNEL MSG RX PORT | 32 blocks, ports A/B | 64 blocks, ports A–D |
| `40 4x 00` TONE MAP NUMBER | `00`–`03` | `00`–`04` |
| `40 4x 01` TONE MAP-0 NUMBER | `01`–`03`, default `03` | `01`–`04`, default `04` |
| `50 ** **` / `51 ** **` | the opposite group's blocks | absent — the port selects the group |

**Two consequences worth stating because they save work rather than cost it.** There is no double-module mode to implement, and the 64 parts are four ports of sixteen rather than a second address space — so a part is still addressed by one block nibble and the port carries the group.

The SC-8850 also answers GM2, which the SC-88Pro does not. GM2's additions are all second addresses onto GS mechanisms already present (Controller Destination onto `40 2x`, Key-Based Instrument Control onto `41 mn rr`, Scale/Octave Tuning onto `40 1x 40`–`4B`, Modulation Depth Range onto `40 2x 04`), and CC71–78 are the Tone Modify parameters. They are covered by the one-storage rule below rather than by new state.

## Every address is assigned, and "not in the table" is not an assignment

Each address in the space carries exactly one of four levels. **An address with no row is a defect, not a silence** — that is the property this scheme exists to make checkable.

| level | meaning | how it is verified |
|---|---|---|
| `AUDIBLE` | received, held, and reflected in the audio | changing the value produces a measurable difference |
| `STATE` | received and held, not reflected in the audio | reads back through a round-trip test |
| `ACCEPT` | received and discarded | does not disturb the interpretation of what follows |
| `IGNORE` | deliberately not implemented | the reason is written in the table |

The address table is one `constexpr` array, and a decoder walks it. An address the table does not name increments `unknown_writes` rather than being dropped, and tests assert that counter is zero over a corpus of real files. This is the same discipline as the parity allowlist: nothing is silently discarded, and a gap is a number rather than an absence.

**Undefined regions get rows too.** `40 01 10`–`2F`, `40 01 36`, `40 01 41`–`4F`, `40 03 02`, `40 03 1A`, `40 1x 25`–`29`, the tail of each controller-destination source at `40 2x 0B`–`0F` and its five siblings, and the rest have no definition in the manual, but a multi-byte write starting before them runs through them, and real files carry SC-55-era addresses the SC-8850 dropped. They are `ACCEPT` so that the unknown counter keeps meaning something. A range covers one mid byte only: one spanning two would step through low bytes above `7F`, which are not addresses, so a region that crosses a mid byte is written as two rows.

**The controller-destination block is `ACCEPT` in full, because it is a matrix and there is no matrix.** `40 2x xx` gives each part six controller sources — modulation, bend, channel and polyphonic aftertouch, and two assignable — each routed to the same eleven destinations: pitch, TVF cutoff, amplitude, and rate plus pitch, TVF and TVA depth for each of two LFOs. libsonare routes its controllers directly to the quantities they move, so no byte in the block has a reader, and rows are split where the accepted range or the power-on default changes rather than one per destination. Two of them name a quantity that already exists under another address and are the ones worth raising first: `40 2x 10` BEND PITCH CONTROL is the part's bend range, which RPN `00 00` already owns, and `40 2x 04` MODULATION LFO1 PITCH DEPTH is the modulation depth, which is presently a constant. Raising either means making it the one storage location for that quantity, not editing the row.

**The level is measured rather than declared.** Every row is probed with a value it accepts, and a render carrying that write is compared against one without it: `AUDIBLE` requires the audio to move and the three levels below it require the audio not to move, so a row cannot sit at the wrong level in either direction. `STATE` additionally requires the byte to reach a mirror the player exposes and `ACCEPT` / `IGNORE` require it not to, which is the difference between a value held for a consumer that does not exist yet and a value dropped. `handle_sysex()` answers the same split: it returns true when an apply layer took the write, so an `ACCEPT` or `IGNORE` address is decoded, counted as known, and still returns false.

**Nineteen addresses claimed `AUDIBLE` while nothing read them, and this is what found them.** The effect bus takes ten fields off its config, and a system-effect byte outside that set reaches `GsSystemEffects` and stops there — held faithfully, converted faithfully, and never asked for. Fourteen of them are `STATE` now: reverb pre-LPF, delay feedback and predelay; chorus pre-LPF, feedback, and the two unit-to-unit sends; delay pre-LPF, the two time ratios, the three per-leg levels, and its send to reverb. Raising one back to `AUDIBLE` means giving the engine the control, not editing the row. The count is a floor rather than a total: a row nothing reads is invisible to the corpus census, which counts an address as answered once it has a row at all.

**`AUDIBLE` is measured on both voice banks, because a row can reach one and not the other.** A part plays on the SoundFont bank or on the physical-model floor depending on whether a preset answered its program, and the two apply a parameter through different code — so a probe on one bank alone can watch the audio move and still be measuring half the implementation. Two defects had already shipped that way: a drum note's delay multiplicand reached only the SoundFont voices, and every one of the eight part edits at `40 1x 30` reached only them, which in a build with no SoundFont at all meant the whole block was inert for all 128 programs. Both are fixed by giving the banks one conversion of the offsets to share rather than one application each, which is what keeps them from drifting apart again.

**`AUDIBLE` is conditional on the build, and the gate refuses to run without it.** The system-effect, master-EQ and EFX blocks reach the audio only inside `SONARE_MIDI_WITH_FX`, and an EFX chain only when the host supplies an insert factory. In a build without either, those rows are received and silent — so the gate fails rather than passing them dry, because a green run in that configuration would be evidence of nothing.

## One parameter has one storage location

GS reaches the same parameter from up to three directions. **Holding a second copy is the defect this rule exists to prevent**: write through the CC, read through the SysEx, and the two disagree.

| parameter | CC | SysEx | NRPN |
|---|---|---|---|
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
| Mono/poly mode | 126 / 127 | `40 1x 13` | — |
| Drum level | — | `41 m2 rr` | `1A rr` |
| Drum panpot | — | `41 m4 rr` | `1C rr` |
| Drum reverb send | — | `41 m5 rr` | `1D rr` |
| Drum chorus send | — | `41 m6 rr` | `1E rr` |
| Drum delay send | — | `41 m9 rr` | `1F rr` |

`41 m1 rr` (Play Note Number) is **not** the same parameter as NRPN `18 rr` (Drum Instrument Pitch Coarse). The first replaces which sample a note plays; the second shifts the pitch of the one it already plays. They are adjacent in the map and mean different things.

**The map number is written two ways and only one of them is one-based.** The `m` nibble of `41 mn rr` is zero-based — `41 0n rr` is MAP1 — while the *value* of `40 1x 15` USE FOR RHYTHM PART is one-based, `01` being MAP1. Both numberings are the manual's own. An address written as though it carried the value's numbering lands in the other map's setup, and every note still sounds, so nothing about the failure is loud. The mid byte holds `m` and the parameter nibble in seven bits, which also means the highest map a file can address is 7; the machine has two.

**The drum setup parameters have no power-on value, and that is what fixes their defaults.** The manual gives them none because a drum set change re-initialises them to what the kit itself specifies. So each is held at the value that changes nothing — `7F` for LEVEL and for the three multiplicand sends, `40` for PANPOT — and read only once the parameter has actually been written. A default that was not the identity would make an unwritten parameter overrule the kit.

`40 1x 16` PITCH KEY SHIFT is likewise **not** RPN `00 02` Master Coarse Tuning, however exactly their ranges coincide — both `28`–`58` around a centred `40`, both ±24 semitones, both MSB-only. The map annotates an alias inside the row that has one, which is where every entry in the table above comes from: `(=CC# 7)`, `(= RPN#1)`, `(=NRPN# 8/CC#76)`. It annotates this row with nothing. The manual settles the reading one step up, for the pair that is documented: RPN `00 01` and `40 00 00` MASTER TUNE "are added together to determine the actual pitch sounded by each Part". Two locations that add, then, not one that overwrites — and the same for the coarse pair.

The mapping is verified by a round-trip test over every pair: written from either side, read back from either side, equal.

Two rows in that table carry a reading that is not obvious from the map and is worth writing down once. `40 1x 13`'s annotation is `(=CC# 126 01/CC# 127 00)`, and those trailing bytes are the **controllers' data bytes rather than parameter values** — MIDI's Mono Mode On carries a voice count, of which `01` is the ordinary monophonic case, and Poly Mode On's data byte is always `00`. Read as parameter values they would invert the row's own default of `01` Poly. And the two controllers do something the SysEx does not: both are also All Notes Off. One storage location, two entry points, one of which silences the part on the way in.

## The parameters a rhythm part does not take

MASTER TUNE, PITCH FINE TUNE and the coarse tuning all reach a rhythm part. `40 00 05` MASTER KEY-SHIFT, `40 1x 16` PITCH KEY SHIFT and `40 1x 13` MONO/POLY MODE do not, each because the manual prints the exclusion beside it: "Even if you adjust Key Shift for all Parts, the pitch of the Drum Part will not be affected" for the first two, "For a Drum Part, changing the Mono/Poly Mode setting will not affect the sound" for the third. Fine Tune sits on the same page as the part-level Key Shift with no such note, and that asymmetry is what makes each of these a statement rather than an omission.

`40 1x 14` ASSIGN MODE is deliberately not in this list — the manual gives it no exemption, and its SC-55-map default is SINGLE *for the drum part specifically*, which is the opposite of an exemption.

Each is held as the byte that was written and decoded at the note-on or the render rather than at the write. A part can take a key shift and *become* a rhythm part afterwards, and it then has to stop being transposed; a value already folded into a cents offset cannot.

Real files reach past both rows. `40 00 05` is written by 102 corpus files with values up to `6F`, against an accepted `28`–`58`; `40 1x 13` is written by 92 files with a value of `64` on parts 1–4, against an accepted `00`–`01`. Both are single-byte writes rather than a run overshooting, and both are ignored like any other out-of-range value.

## Deliberate divergences from the manual

These are decisions, not gaps. Each one is here because it would otherwise be re-litigated or re-discovered.

- **`40 1x 1C` = `00` and `41 m4 rr` = `00` mean random pan on the hardware. libsonare treats them as centre.** Randomness breaks the bit-identical bounce contract, and a deterministic pseudo-random substitute would not match the hardware either, so it would buy divergence without buying fidelity. Centre matches what the overwhelming majority of files that never write the address get, and it fails quietly rather than loudly. This is also the one value at which `40 1x 1C` and CC10 part company, and they are still one storage location: the manual's own note on the address reads "(=CC# 10, except RANDOM)", so `00` is hard left through the controller and centre through the address.
- **EFX type 47 (Rotary Multi) is accepted at both `02 0C` and `03 00`.** The SC-88Pro manual contradicts itself — its Effect list table says `02 0C` while its Insertion Effect List says `03 00` — and the SC-8850 says `03 00` in both places. Files written against either table exist.
- **A drum note's reverb, chorus and delay sends are multiplicands of what the note sends into that unit, not additions to it.** The manual is explicit (`0.0 – 1.0`, "Multiplicand of the part reverb level"). One rule covers all three: the SoundFont zone's `reverbEffectsSend` / `chorusEffectsSend` and the part's CC91 / CC93 / CC94 are summed and clamped, and the multiplicand attenuates that whole — so a note's own send taken to zero takes it out of that bus however loud its part is sending, which adding does not. Delay is the degenerate case rather than a second rule: SoundFont has no delay generator, so the part's send is all there is to scale. Both voice banks answer it alike, for the reason the ASSIGN MODE bullet below gives. The one thing the multiplicand cannot reach is a difference the hardware does not have: a kit zone carries a send of its own, so a part taken to zero still leaves the zone's share in the bus where the hardware would leave nothing.
- **A drum instrument uses its chorus and delay sends at the same time, which the hardware cannot.** The manual prints the restriction beside the block — "It is not possible to simultaneously use both Chorus Send Level and Delay Send Level for a single Drum Instrument" — and names neither a winner nor a rule for choosing one, which is what a restriction rather than a behaviour reads like. It is the first kind under the rule below — a second simultaneous send is exactly what more of the machine would have bought — so libsonare is free of it. A file writing `41 m6 rr` and `41 m9 rr` on one note gets both sends here, where the hardware honours one and the manual does not say which.
- **An out-of-range `40 1x 15` USE FOR RHYTHM PART reads as drum map 1, where every other address ignores a value it does not accept.** The row accepts `00`–`02`, and the corpus reaches `03` at `40 1A 15` — an address 1 059 files write. A file asking for a map the machine does not have still means the part to be drums, and map 1 sounds it as drums where ignoring the write leaves it melodic. This is the one departure from "ignored, never clamped", and it is one because the parameter selects a resource rather than sets a value.
- **`40 00 06` MASTER PAN is a balance on the finished mix, not a re-pan of the parts.** Each part already carries its own position, and a master that re-positioned them would collapse the image rather than move it. Attenuating only the far leg is also what keeps the power-on `40` at exactly unity on both legs, which a constant-power law would not: a master that is not bit-exact at its default moves every bounce in the repository. `40 00 04` MASTER VOLUME takes the square law CC7, velocity and the drum-note level already use, since the manual gives a range and no curve.
- **`40 1x 14` ASSIGN MODE `01` LIMITED-MULTI and `02` FULL-MULTI are one behaviour here, and only `00` SINGLE branches.** The manual separates the two by how long a repeated note's predecessor survives — "continued to a certain extent" against "for their natural length" — which is the machine deciding when to start stealing, and stealing is what a voice budget is. That makes it a resource limit under the rule below rather than behaviour owed fidelity, and libsonare is free of it. SINGLE is a different kind of statement: the previous note is stopped because it was asked to be, not because there was nowhere to put the new one, so it is implemented. Its stop is a five-millisecond release rather than a cut — silent inside about 15 ms — since a cut clicks and the voice's own release, seconds on a pad and over a second on a sustaining model, would leave audible the note it was told to silence. Both voice pools take the same fade: a part moves between the SoundFont and the model bank on a program change, so a shortened release on one of them only would make the parameter's effect depend on which bank happened to answer. The default is `01` for every part; the per-part split the manual prints alongside it belongs to the SC-55 map, which is not the target.
- **`40 03 1B`–`1E` EFX CONTROL SOURCE / DEPTH 1 and 2 are `IGNORE`, and that is a structural statement rather than a gap.** They let a controller move an insertion-effect parameter while the effect runs. The chain here is realised from its type and its twenty parameters and is not re-parameterised afterwards, so there is nothing for a source to drive; implementing them means giving the chain that hook first, and the rows come off `IGNORE` in the same change. `40 03 1F` SEND EQ SWITCH is next to them for a smaller reason: there is one EQ stage and it is bypassed per part at `40 4x 20`, so an EFX return has no separate one to switch.
- **The eight part edits at `40 1x 30` name a sampler's stages, and the physical-model bank applies them to the stages it has.** TVF cutoff and resonance are a filter after the voice, the three EG times scale the amplitude envelope, and the three vibrato controls move the pitch LFO — all of which the model voices already carry, so the offsets land on the same quantities either bank would have used. Two consequences follow from the model voice being a model rather than a sample. An edited filter is engaged even where the patch had left it wide open, since the manual offers no way to ask for a filter and then not hear it; and the vibrato-delay edit is the one that cannot be a plain multiply, because a model voice's LFO has no onset delay of its own and scaling zero would leave the parameter inert on exactly the bank that needs it most.
- **`40 4x 01` TONE MAP-0 NUMBER survives every reset.** Power-on, GS Reset, GM System On and System Mode Set all leave it alone. It is the one part field that a blanket re-initialisation must skip.
- **A drum set change re-initialises that map's setup parameters.** Program change on a rhythm part clears the `41 mn rr` state for that map.

## The extensions libsonare adds

The hardware allows exactly one insertion effect for the whole module — "you can select one Insertion effect, and specify for each Part whether or not the sound will be routed through the effect", and turning it on for two parts mixes them into that one unit. That is a limit of the machine, not a property of GS worth preserving, and libsonare lifts it.

**The extension is reachable only from addresses a spec-compliant file cannot send.** That is what makes it safe without a feature flag: a GS file is inert against it by construction, and the behaviour it gets is exactly the hardware's.

- **Unit 0 stays at `40 03 xx`, unchanged.** Its semantics, defaults and parameter layout are the manual's.
- **Units 1–15 live at `40 3u xx`** (`u` = `1`–`F`), each with the identical `00`–`1F` layout. `40 30`–`40 3F` carries no row in either the SC-88Pro or the SC-8850 map, so nothing collides.
- **Routing extends the value range of an existing address rather than adding one.** `40 4x 22` PART EFX ASSIGN is specified as `00` = BYPASS, `01` = EFX; `02`–`10` select units 1–15. `00` and `01` keep their exact meanings.
- **Parts assigned to the same unit sum into that unit's single instance**, as on the hardware. The unit count changes; the summing does not.

**Two different things wear the phrase "what the hardware does", and collapsing them is the failure this paragraph exists to prevent.** *One unit for the whole module* is a resource limit — a property of the machine that was built, not of GS, and it is lifted. *Parts on one unit sum into it* is not a limit at all: it is what an effect is. Two guitars into one distortion pedal intermodulate because that is what a pedal does, and a file that routes two parts through one unit was written for that sound. So "restore the summing" never means "cap the units", and any change that reduces the reachable unit count to satisfy a compatibility argument has misread this. The reachable range lives in the `40 4x 22` row of the address table (`00`–`10`), not in prose, so narrowing it is a diff on a row rather than a reading.
- Real hardware ignores an unknown address and will treat an out-of-range `40 4x 22` as its own business, so a file using the extension still plays there, with one insertion effect.

**Out-of-range `40 4x 22` values exist in real files, and the extension is what they now reach.** A census of 2 233 SC-55/SC-88/SC-88Pro files (`tools/gs/docs/census.md`) finds 1 678 writes of `00` or `01` and **six writes of `02`, `03`, `04`, `06` and `11`** across five files. Those files are not spec-compliant — the manual gives `00`/`01` — so the rule below is not broken, but it is worth stating that it is not vacuous either: the five will route a part to a unit instead of bypassing it. That is accepted rather than guarded. They already diverge from the hardware today (any non-zero switches the part on), the values look like authoring slips, and a part hearing the module's own EFX is closer to the intent than silence. Gating the extension behind a flag to protect five files would cost the property that makes it usable.

**Restoring the summing is a prerequisite, not an afterthought.** The current implementation builds one chain per EFX-enabled part and runs each on that part's own bus, which is already past the hardware's limit — accidentally, and in a way that diverges: two parts through one distortion intermodulate on the hardware and do not here, and one shared delay line becomes N independent ones. Until the spec behaviour is the default there is no baseline to describe an extension against. Fixing it also reduces the instance count, which is where the headroom for extra units comes from.

## The per-part insert slot is a different thing from the EFX, and both must reach the part

`Sf2PartInsert` is the host's own insert on a part, built through an injected factory. It is not GS and does not appear in the address space. **A part may carry both**: its own insert (a guitar amp, see `voicing.md`) and the file's EFX. They run in series.

This is stated because the code currently makes them exclusive — a part with a static insert never receives the EFX chain — which would mean a guitar with an amp loses the file's chorus. The slot is a chain, not a choice.

## Rules

- **An address is added to the table in the same change that implements it.** A parameter that works but has no row is invisible to the coverage test, which is the only thing that can say the space is covered.
- **Never widen the space to silence an unknown.** An address that turns up in real data and is not in the manual is either a row with a reason or a defect to investigate; it is not an `ACCEPT` added to make a counter go to zero.
- **An extension must be unreachable from a spec-compliant file.** If a proposed extension can be triggered by a message a real GS sequencer emits, it is a divergence rather than an extension and does not belong here.
- **A resource limit is not a behaviour, and only behaviour is owed fidelity.** Before matching the hardware, say which of the two a rule is: could the machine have done otherwise with more voices, more units, more memory? Then it is a limit and libsonare is free of it. Does it follow from what the signal path *is*? Then it is behaviour and it is binding. The one-insertion-effect ceiling is the first kind; parts summing into the unit they share is the second.
- **Behavioural fidelity beats convenience when the two conflict.** The double-amp a file gets by selecting a guitar multi over an already-amped part is what the hardware does; suppressing it automatically would make the sound depend on a hidden rule.
- **Reset defaults are part of the contract.** They are not implementation detail: a file that sends GS Reset and then nothing else is entitled to the hardware's state. Changing one moves the goldens and belongs in its own change.
