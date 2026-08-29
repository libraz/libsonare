# GS — the compatibility contract and the extensions on top of it

The synth answers Roland GS. This page is the specification of what that means here: which device defines the target, what every address in the space is promised to do, which parts of the space libsonare adds to, and the rules that keep the two from contaminating each other.

Per-address detail — offsets, ranges, defaults — is not restated here. It comes from the **Roland SC-8850 Owner's Manual, Appendices, "Parameter Address Map"** (`cdn.roland.com/assets/media/pdf/SC-8850_OM.pdf`), which is the source of record. What this page holds is what the manual cannot: the decisions, the places libsonare diverges from it deliberately, and the extensions.

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
| `00 00 7F` SYSTEM MODE SET | `00`/`01` — Mode-1 / Mode-2 | **`00` only**, treated as GS Reset. **No Mode-2** |
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

**Undefined regions get rows too.** `40 01 10`–`2F`, `40 01 36`, `40 01 41`–`4F`, `40 03 02`, `40 03 1A`, `40 1x 25`–`29` and the rest have no definition in the manual, but a multi-byte write starting before them runs through them, and real files carry SC-55-era addresses the SC-8850 dropped. They are `ACCEPT` so that the unknown counter keeps meaning something.

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
| Drum level | — | `41 m2 rr` | `1A rr` |
| Drum panpot | — | `41 m4 rr` | `1C rr` |
| Drum reverb send | — | `41 m5 rr` | `1D rr` |
| Drum chorus send | — | `41 m6 rr` | `1E rr` |
| Drum delay send | — | `41 m9 rr` | `1F rr` |

`41 m1 rr` (Play Note Number) is **not** the same parameter as NRPN `18 rr` (Drum Instrument Pitch Coarse). The first replaces which sample a note plays; the second shifts the pitch of the one it already plays. They are adjacent in the map and mean different things.

The mapping is verified by a round-trip test over every pair: written from either side, read back from either side, equal.

## Deliberate divergences from the manual

These are decisions, not gaps. Each one is here because it would otherwise be re-litigated or re-discovered.

- **`40 1x 1C` = `00` and `41 m4 rr` = `00` mean random pan on the hardware. libsonare treats them as centre.** Randomness breaks the bit-identical bounce contract, and a deterministic pseudo-random substitute would not match the hardware either, so it would buy divergence without buying fidelity. Centre matches what the overwhelming majority of files that never write the address get, and it fails quietly rather than loudly.
- **EFX type 47 (Rotary Multi) is accepted at both `02 0C` and `03 00`.** The SC-88Pro manual contradicts itself — its Effect list table says `02 0C` while its Insertion Effect List says `03 00` — and the SC-8850 says `03 00` in both places. Files written against either table exist.
- **A drum note's reverb, chorus and delay sends are multiplicands of the part's send, not additions to it.** The manual is explicit (`0.0 – 1.0`, "Multiplicand of the part reverb level"). Multiplying gives the hardware behaviour that taking a part's send to zero silences its drum notes' sends too; adding does not.
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
