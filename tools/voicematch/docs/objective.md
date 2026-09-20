# What the bank is for, and what decides that it is done

This page exists because the harness's own criteria kept being mistaken for the goal. Everything else in `docs/` describes a measurement; this one says what the measurements are in service of, and which of them are allowed to decide anything.

## The objective

**Reproduce the sounds the SC-8850's GS map names.** The map says which slots exist and what each one is — an acoustic grand at program 0, a darker one at bank 2, a standard kit on channel 10. That is the specification, and it is the only thing the machine supplies.

**It does not say what any of them should sound like.** The samples are a quarter-century old and were degraded to fit the hardware of their day; a recording made now is a better instrument than the one they hold. So the map is followed and the timbre is not — for a slot naming an instrument that exists outside the machine. It inverts for one naming a sound the machine invented, and the next section draws the line.

**This page is the only place that line is drawn.** `src/midi/synth/docs/gs.md` is the protocol contract and says explicitly that what a slot should sound like is not its business, which is what keeps a timbre decision from existing in two documents that can part company.

**Physical modelling and FM are the technique, not the product.** An exact match to any sampled source is impossible by construction. For a slot naming a real instrument it is also not what is being attempted — what is attempted there is that a listener hears the instrument the slot names. For a slot naming a sound the machine invented the same impossibility holds and the aim is the opposite one: get as close to it as the engines reach, because there the sampled source *is* the instrument and there is nothing else to hear.

**Building a correct general-purpose physical model is not the objective.** It is welcome where it happens, and it is often the cheapest route to a voice that sounds right, but no voice is finished because a model is principled and none is unfinished because a model is ad hoc.

## Two kinds of slot, and the timbre rule is not the same for both

Follow the map and ignore the timbre is right for an instrument that exists outside the machine and wrong for one that does not, so the line is drawn exactly there.

**A slot naming a real instrument is aimed at that instrument as it is recorded today.** A grand, a harpsichord, a church organ, an electric bass. The map says which slot exists; a modern reference says what it sounds like; the sampled original is not the target and being unlike it is not a defect.

**A slot naming a machine's own sound is aimed at the machine, timbre included.** The synth leads and pads, synth brass, synth strings, synth bass, the orchestra hit, the sound effects. Nothing stands behind these for a recording to be made of — the sound *is* the module's, and a file that plays one is playing that sound rather than referring to something else through it. Here reproduction is the goal, as closely as the engines reach, and the machine is the reference rather than the oracle of last resort.

**The reason is that the arrangement stops working, not that the timbre is missed.** A pad or a synth brass line carries harmonic and rhythmic weight, and a substitute with a different envelope and a different spectrum does not carry it — the part stops doing its job in the mix. A better grand piano never breaks a piano part that way. This is the one place where sounding like the hardware is the requirement rather than a nostalgia.

**Drums follow the machine on both axes, for a third reason: the kit is a layout and not just a set of sounds.** A kit's slots do name real instruments, so the colour rule would send them to a modern reference like any other. What stops it is that a modern library's kit is a *different kit* — a different set of drums in a different arrangement of slots, not the same one recorded better — so following its colour changes which instrument a note reaches, and the groove a file was written to play stops being that groove. The behaviour half was always the machine's anyway: how long each piece rings, how it damps and where the members sit against each other are properties of the module rather than of a snare drum, and they are the first thing that stops a file sounding like itself.

**The single crossing is how a piece's colour moves with the stroke**, and it is the machine's one structural blind spot rather than an exception granted for convenience: the module holds one recording per note, so nothing in it says what a harder stroke does. That dimension alone is read from a modern multi-velocity kit, and `capture/drums.json` argues it at the point it is recorded.

**Which rule a slot falls under is data.** `tools/voicematch/policy.json` names the machine-defined set; everything unlisted is an instrument. Deciding it per case in prose would leave the boundary somewhere different in each tool that needs it.

## A slot the model cannot reach falls back to its nearest neighbour

Some slots name a mechanism this bank does not have and is not going to grow one for — a massed choir is the shape of the problem, where what a listener hears is dozens of uncorrelated singers and the model has one tract.

**The answer is the nearest voice the bank already has: never silence, and never a stand-in built to fill the hole.** A neighbour is chosen because it exists and is maintained, so it improves when the voice it borrows from improves; a purpose-built approximation is a voice nobody fits and nobody retires.

**An approximation is declared as data, in `policy.json`'s `approximated` block, naming the slot, the voice answering it and what the model cannot reach.** One declared in prose alone reads as an oversight — the same discipline as a capture's `dimensions_na` and the parity allowlist, for the same reason. Nothing downstream can tell an approximation from an unfinished voice, and the difference is whether anyone should spend an afternoon on it.

**Two programs sharing one patch is not automatically an approximation, and it is not automatically fine either — the question is whether the map calls them one instrument.** Where it does, one patch with two numbers is correct. Where it does not, one patch is an approximation and owes an entry here.

**The electric pianos are the case in the block, and they were long cited as the case that proved the block should be empty.** The map separates a tine instrument at program 4 from an FM one at program 5, and the machine holds them as independently recorded tones whose variation families name that difference — a Rhodes and a Wurlitzer hang under the first, FM tones under the second. The bank gives both one patch, voiced as the tine instrument, so program 5 is answered by its neighbour. The engine is not what is missing: both are FM and the second instrument is reachable on it. What is missing is a patch written for it, and the reason that has to be declared rather than left to be noticed is that program 5 has a capture, a stage and a gate of its own, so nothing downstream can see that the voice behind them was written for a different instrument.

## Diversity beyond GM/GS is not a GS extension

Two wants get confused here, so they are separated: **playing a file written for the hardware**, and **making sounds the hardware could not**. The first is what GS is for and everything above is its contract. The second has nothing to do with GS.

**GS cannot express modern synth voicing, and adding addresses would not change that.** The entire per-part sound-design surface is the eight TONE MODIFY parameters — vibrato rate, depth and delay, filter cutoff and resonance, amplitude attack, decay and release — plus the variation banks and the effect blocks. No oscillator selection, no modulation routing, no envelope past three segments, no unison, no wavetable position. What GS offers is a choice among presets, not a way to build one.

**So diversity lives in the patch API and the preset catalogue, and the GS address space is left alone.** A host names a preset or supplies a whole patch with fields overridden, and none of that travels as a controller. The tempting alternative — libsonare addresses inside the GS space, on the pattern the extra insertion units already use — is refused: the only sender that could reach them is a host that knows this library, and a host that knows this library can set the patch directly. It would be a narrower copy of an API that already exists, bought by putting sound design inside a document whose whole value is that it describes somebody else's standard.

**The one case that would earn a wire format is a self-contained file**, where the SMF is the deliverable and has to carry its own voices. That is a container question and not a GS one, and a patch in a meta event answers it without touching the address space.

**A voice built this way carries no GM or GS program number.** The bank answers those and is the only thing that does. A preset that wanted one would be a second answer to a question that has one correct answer, and which of the two won would depend on load order.

## What order the bank is worked in

**Frequency of use first.** Piano, the standard kit, the synths, the electric guitars and basses: a bank's worst voice costs nothing until a file plays it, and these are the ones files play.

**Pipe organ, harpsichord and classical guitar are worked ahead of their frequency.** That is this project's own interest rather than anything the corpus implies, and it is written down as such so the ordering is not later read as a measurement.

**A variation waits for its capital.** A variation patch is its capital copied and then changed by whatever its name claims is different, so a capital that moves invalidates every variation hanging under it. Fitting one first gains nothing and loses a round of rework — which is why all of them sit at the same step today, and the only ones worth starting are those whose capital is finished.

**The ordering is data.** `policy.json` carries the tiers and the goals and `make voice-status` reads it. A goal is a named set of voices and the step each is being worked toward — not a date, and not a fraction of the bank.

**A goal never gates a release.** Calibrating a voice is open-ended analog work: listen, try something, listen again, and it takes the time it takes. A version whose date arrives with the set unmet ships with it unmet and the goal carries over — that is the expected case rather than a failure, and a voice is never late. So nothing is to be built that blocks a release, a merge or a CI run on a voice's stage, and a goal named after a version is not thereby a due date. `status.py` is read-only and exits 0 whatever it finds, which is the mechanical half of the same statement.

## One reference is enough, and it is a target rather than a sample

**A reference is the thing being aimed at, not an estimate of a hidden truth.** This is the point the harness had backwards. A statistical reading of a reference treats it as one draw from the distribution of real instruments, which makes a second draw necessary before anything can be said — and makes fitting closely to the first one *overfitting*. Under the objective above that reading is simply wrong: a good modern recording of a grand piano is a legitimate thing to sound like, and matching it closely is the goal rather than a failure mode.

So:

- **A voice needs one reference. Two are not required and must not be demanded.** The cost of a second is a plugin authored by hand per instrument, which across the bank is out of all proportion to what it buys.
- **The reference-spread criterion — "inside the two references' own disagreement" — is retired as a promotion condition.** Where a capture happens to carry several timbres the spread is still worth printing, as information about how much the target itself wobbles. It decides nothing.
- **Where a voice's design needs a different signal than the obvious reference carries, the reference is wrong, not the design.** The electric guitars are the standing case: the rig is a separate per-part stage here (`voicing.md`), so the voice must be fitted against a direct-injected instrument and never against a sample with an amplifier baked into it.

**Reference source priority**, in order, and the first that covers the instrument wins:

1. A product dedicated to that instrument, played direct — the closest thing to the instrument alone.
2. A general instrument library.
3. The machine itself.

**A modern source outranks the machine because it is physically faithful, not because it is newer.** The engines here are physical models, so what a reference is worth is how much of the instrument's actual structure is legible in it — a recording of the real thing carries the coupling, the radiation and the nonlinearity a model has to grow; a quarter-century-old sample of it carries whatever survived that era's memory budget. Fitting a physical model to the second teaches it the compromise rather than the instrument.

**The machine is not the oracle of last resort, though — it is the authority on identity, and that never transfers.** The map says which slots exist and what each one *is*, and where the modern source cannot answer that question the machine answers it and keeps the slot. Three cases, enumerated as data in `policy.json`'s `machine_fallback`: nothing modern exists to record (the machine-defined slots, and the GS variation tones, which are the bulk — no library ships a "Piano 1w"); a modern source holds a different instrument at the address; or a modern source covers the family but its layout does not line up, so following it moves which instrument a note reaches (kits). The second of those has a terminal form worth keeping distinct: when it is the *machine's own* recordings that hold another instrument at an address, there is nothing further to fall back to and the slot has no reference at all — that is what `no_reference` records, and its eleven entries are the condition found rather than suspected.

**For a slot not already enumerated, which case holds is measured, not judged.** Compare the modern reference against the machine's tone at the same address and read the distance. A slot moved to the machine on an impression is indistinguishable afterwards from one moved on a reading, and the whole point of writing the fallback down as data was that the boundary stops being re-decided per tool.

**A capture taken from the machine is taken with its effects off** — reverb, chorus, delay and the insertion effect — and with the tone map selected explicitly rather than left at whatever the unit was last set to. A reference with the module's reverb inside it drives the room correction into matching a tank instead of a building, and a reference taken on an unknown tone map is a measurement of a tone nobody can name. The audio stays under the scratch root and is never committed, on the same terms as every other capture; only the measurement is.

Which product answered a given capture is recorded only in its untracked `capture/<id>.local.json`, as everywhere else.

## The ear decides, and nothing else is allowed to

**A voice is finished when someone has listened to it and it is the instrument.** A green gate is not that, and the gap between the two is not a rounding error — voices have passed every recorded bound while sounding wrong, which is the observation this page was written after.

Measurement has exactly two jobs and neither of them is acceptance:

1. **Get a voice into the neighbourhood cheaply**, so the ear is spent on judgement rather than on gross error.
2. **Stop a voice that is already good from being broken later.** This is what a gate is, and it is worth keeping precisely because it does not claim to be more.

A number therefore never promotes a voice. It only ever says *where to look next* or *something regressed*.

## When calibration cannot reach the target

**Treat it as a missing mechanism in the model, not as a limit to accept.** If every knob is at its own optimum and the voice still does not sound right, the model is not expressing something the instrument does. `autofit.py --diagnose` is the instrument for this and its useful half is connectivity — whether any knob moves a measurement at all — rather than improvement.

**The response is to change the model.** The bank's history is mostly this: a brass engine that computed bore pressure and never radiated it from the bell, a reed valve approximated by a clamped linear table where the physics is a Bernoulli pressure difference, a free reed with no slot flow, a steel pan modelled as a membrane when it is a dished shell.

**Each instrument is modelled by reasoning from its literature and its physical structure, one at a time.** Read what the mechanism is, decide what it means for this instrument's geometry and excitation, then implement that. Do **not** add a parameter because it reduces an error: a term with no mechanism behind it is a free parameter that will fit anything, and a fit that improves because of one has learned the reference's noise. A new mechanism is justified by the physics first and confirmed by the measurement second, in that order, and its citation goes on one line beside the code.

## What this page retires

Named so that a later reading of the older documents does not quietly restore them:

- **The requirement that every voice carry two reference timbres.** `status.md`'s ladder gates every step above `voiced` on it; that gate does not survive this page.
- **Agreement against the reference spread as a promotion criterion.**
- **Structural validation across an instrument family as a ladder step.** Comparing a whole family — how loop loss scales with pitch, whether a body resonance tracks — remains a good way to find out *why* a voice will not reach its target, and it is a debugging tool rather than a stage a voice passes.
- **The outstanding capture backlog that existed only to supply second timbres.** What remains is the much smaller set where the current reference is the wrong signal, plus the GS variations, whose slots only the machine can define.
- **"A modern recording is the target" as a rule over the whole bank.** It is the default branch of two, and the other branch — a slot naming a sound the machine invented — aims at the machine and reproduces its timbre deliberately. Reading the rule as universal would make the synth and effect programs look finished when they are aimed at nothing.
