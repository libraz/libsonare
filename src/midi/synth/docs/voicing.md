# Voicing — the instrument is separate from the rig it is heard through

A GS module's electric guitar is a recording of an amplified guitar: the cabinet, the power stage and the room are inside the sample, and no message can take them out. libsonare models the instrument instead — the string, the body, the pickup — and puts the amplifier after it as a stage of its own.

This page is the rule that follows from that, and the mechanism that lets it hold without breaking a file written for the module it replaces.

## The principle

**The model's boundary is the boundary the instrument has.** For an electric guitar that is the jack: string, body and pickup are the instrument; cable, preamp, tone stack, power stage and cabinet are the rig. For an organ it is the pipe mouth: the pipe and the wind supply are the instrument, the building is not.

Everything downstream of that boundary is a *stage*, addressable and replaceable, not a property of the patch.

The gain is not tidiness. A model whose boundary matches its reference can be fitted against that reference directly; one that bakes in a rig has to be fitted against instrument-and-rig together, where the string's brightness and the amplifier's treble move the same measurement and no optimiser can separate them.

## The compatibility problem this creates, and how it is solved

**A file written for a GS module selects program 30 and expects distortion.** It sends no insertion effect, and on many files it uses the insertion effect for something else entirely. If the amplifier is something the file has to ask for, that file plays a clean string and the module is broken.

The two requirements are only in conflict if the rig is baked into the patch. They are compatible if it is a **default binding**:

- the **voice** produces the instrument alone (a DI, for a guitar)
- the **bank** carries, per program, a default rig — which rig, at what settings
- the **host** may override it, replace it, or clear it

So program 30 with no SysEx at all comes out amplified, because the bank said so; and the DI is still available, because nothing was baked. **The distinction that makes both true is between where the DSP sits and who decided it is there.**

## Why the rig cannot live in the insertion effect

GS has exactly one insertion effect for the whole module. A song with a distorted guitar and a rotary organ can only have one. If the guitar's amplifier is the EFX, then the moment a file uses the EFX for the organ, the guitar loses its amplifier.

libsonare lifts the one-unit limit (`gs.md`), but that does not change the conclusion: the extension is unreachable from a spec-compliant file, and the whole point is that a spec-compliant file must come out sounding complete. The rig belongs to the instrument's own signal path.

**The hardware agrees, in its own way.** Every guitar and bass multi-effect on the SC-8850 carries `OD Amp <type>` together with an independent `OD Amp Sw`, and the effect types for overdrive and distortion carry the same pair. Drive and amplifier are separate stages there too — the machine simply had the amplifier's result already inside the sample as well.

## Where the rig sits

The rig is a **per-part** stage on the part bus, not per-voice. A cabinet convolution per voice would be forty-eight instances; per part it is at most one per part in use, which is the same shape as the per-part physical-model state the player already holds.

It is built through the injected insert factory, so `sonare_midi` does not depend on the mastering tree — the host wires it, and all three production paths already do. **A host that does not wire the factory gets the instrument without its rig**, which for a guitar means a bare DI. That is the failure this whole design exists to avoid, so the wiring is part of the contract rather than an optional nicety, and it is checked rather than assumed.

**There is one amplifier.** `src/mastering/saturation/` already holds a circuit-level one with a generated cabinet impulse and calibrated rigs in physical units. A second implementation inside the synth would mean two calibrated amplifiers to keep in agreement, which is the failure mode the cabinet generator's own history warns about.

## The instrument must not carry a shadow of the rig

**A drive control inside the voice is not an amplifier and must not be used as one.** The synth patch's `drive` is a pre-filter saturation for synth character. Using it to make program 30 sound distorted produces the sound of clipping rather than the sound of a guitar — a cabinet is what makes distortion sound like an instrument — and it puts a second gain-like control in the fit, degenerate with the amplifier's.

For the electric programs the voice's `drive` is zero, or small and justified as pickup nonlinearity. The distortion is the rig's.

The same rule generalises: **no voice carries a baked-in room, cabinet or ambience.** Where a family is only ever heard through something — a pipe organ through a building — that something is a stage with a default binding, on the same terms.

## The test that separates a weight from a baked-in effect

The bank already leans on this and it is easy to misread as the thing the rule forbids. `gm_fallback_sends()` weights each program's reverb and chorus send — a church organ 2.2×, a bass 0.4× — so an instrument that is only ever heard in a room carries one by default.

**That is a binding, not a bake, and the property that makes it one is that it multiplies.** The weight scales a send the file controls, so CC91 at zero is fully dry and the controller keeps its meaning. A baked-in room is inside the voice, survives the controller, and cannot be removed by any message.

So the test is: **take the file's own control for that stage to zero. If the stage disappears, it is a weight. If it survives, it is baked in and the rule has been broken.**

The same shape appears in GS itself: a drum note's send is a multiplicand of what that note sends into the unit, not an addition to it (`gs.md`), for exactly this reason. Additive would mean a note told to send nothing still sends.

An amplifier is not expressible as a weight — there is no file-controlled "amp amount" for it to scale — so it takes the other form: a stage with a binding the host can clear. Both are bindings; they differ in what the file already had a handle on.

## What binds, and what does not

`gm_fallback_rig(bank, program)` is the table, and `Sf2Player::build_realized_efx()` is where a binding becomes a processor. Three conditions gate it, and each is the answer to a way the rule could be broken:

- **Only where the note plays the model floor.** A SoundFont's electric guitar is a recording of an amplified one, so a bank rig on top of it would be two amplifiers, which is the bake this page removes. The part re-resolves the question at every program, bank and rhythm-part change.
- **Only where the part carries nothing of its own.** A configured insert or a live GS EFX chain is the host or the file speaking, and either outranks a default.
- **Only where the host wired an insert factory**, since the rig is built through it. A host that wires none gets the instrument alone, and that is stated in the config rather than left to be discovered.

The six GM electric guitars bind today. Everything else the split would cover is left unbound rather than guessed at: an electric piano and a drawbar organ each want a component that is not an amplifier, and a module's electric bass is close enough to a direct signal that binding one would be a preference rather than a repair.

**Clearing it is part of the contract, not a debug switch.** `clear_bank_rig` on the SF2 instrument config — `clearBankRig` on the JS surfaces — renders the instrument alone, and it exists on the live path as well as the bounce. Without it "the DI is still available" would be a claim no caller could act on, and a calibration harness comparing a rigged model against a direct reference would be measuring the amplifier as if it were the string, with both sides audio and both plausible. That is what the capture's `rig` field now decides on the model side too, and not only on whether a fit may run.

## The RT contract for a binding that changes mid-song

A program change on a part changes which rig that part should carry, and building a rig allocates.

- **The audio thread never builds.** It publishes which rig each part wants — one word, four bits per part, so the builder reads all sixteen as they stood at one instant — and the control thread builds and publishes the chains for the render side to swap wait-free. This is the path the insertion effect already uses, and the rig joins it rather than growing a second one.
- **Offline renders resolve inline.** The bounce path realises a pending change at the top of a block rather than waiting for a control-thread pump, so a program change mid-render takes effect in the same block it arrived in — and the render stays deterministic, which is what the bit-identical contract needs.
- **A live program change keeps the rig the part already had**, until the control thread comes past — `prepare`, `set_soundfont`, a reset, or a SysEx the host pushed. That is the same reach a *sequenced* insertion-effect SysEx has, for the same reason: a live program change arrives entirely on the audio thread and the control-thread hook on `MidiInstrument` is SysEx-only. A binding that rebound itself live would need a signal from the audio thread to the control thread, which no queue in the engine runs in that direction.
- **A rig swapped under a sounding note loses that note's tail through the old rig.** The same is true of an EFX change on the hardware, and making it seamless would mean running both rigs during the crossover for an effect nobody asked for. Accepted, and stated so it is not rediscovered as a bug.
- **A program change within one rig rebuilds nothing.** A binding carries an id as well as a name, so moving a part from program 27 to 28 keeps the amplifier it is running instead of replacing it with an identical one and discarding its state.

## A reference is on one side of the boundary or the other, and which one decides what it may be used for

**A dry reference is not a DI one.** Dryness is measured by looking for a tail, and a cabinet has none — it is a filter, not a space. So a close-mic'd amplified guitar passes every dryness test there is and is still on the far side of the boundary, with the whole rig inside it.

The distinction has to be recorded per capture, because nothing downstream can infer it: a reference that carries a rig looks exactly like one that does not, until a model fitted against it acquires a cabinet it was never supposed to have.

**Absence of that record means unclassified, never "no rig".** The dangerous reading is the one a missing field would otherwise default to, and captures predate the question — so a capture that does not say is one nobody has answered for yet. Comparing and auditioning proceed unchanged; **fitting is what the answer gates**, and only for a family where a rig is possible at all. A wind or a piano is not waiting on anyone.

**A reference that carries a rig is an acceptance target. It is never a fit target.**

- **Fit the instrument against a reference at the instrument's own boundary.** For an electric string that means a DI.
- **Check the instrument-plus-rig against a reference that carries one.** This is the only measurement that says the separation closed end to end, and it is worth more than a second same-side reference: the two are measuring different quantities, so they cannot form a spread, but together they bracket the whole chain.
- **Fitting the instrument directly against a rigged reference is the failure this whole page exists to prevent.** It produces a model that reproduces an amplifier with string parameters — the fit closes, every metric improves, and the work is lost the moment the rig is put back where it belongs. The hazard is not hypothetical or rare: the rigged reference is usually the one that already exists, and the fitter will run against it without complaint.

This is the same shape as the room rule and the opposite conclusion, for a reason worth stating: a room can be measured and convolved onto the model, so a wet reference is usable after correction. **A rig cannot be undone that way** — it is nonlinear, so there is no inverse to apply and nothing to correct with.

## Fitting

**Never let one optimiser move both sides.** The instrument's brightness and the rig's tone control reach the same measurement; a fit given both will trade them against each other and settle somewhere that transfers to neither.

- Pin the rig to a named preset, then fit the instrument against it.
- Where the reference library offers the instrument dry and the rig switchable, capture **both**: the same notes rendered with the rig bypassed and engaged. The difference is the rig's transfer function in isolation, which is what an amplifier model should be fitted to and which no single amplified reference can give.
- Where it does not, the rig is a design choice rather than a fitted one, and is recorded as such.

**Recorded, for the six that bind today.** No product here renders an electric guitar both ways, so which amplifier each program is heard through is chosen rather than fitted. Programs 29 and 30 are one amplifier at two gains, which is also what separates them on a module: a brighter preset for 30 put its 5 kHz band 12.6 dB over its reference where 29's sat 3.6 out, and no drive value moved that band — the difference was the preset's tone stack, since all three bindings share one cabinet. What a measurement can still do is bound the choice rather than make it. **Both sides of that comparison are instrument-plus-rig, so an excess belongs to the chain and not to either stage**, and separating them needs a direct capture of the same instrument, which is exactly what a rig-switchable product is for.

**Where the three measured guitars stand, and what a binding can still do for them.** 29's chain already falls at the reference's own rate from the reference's own knee (−40.2 dB/oct from 4 kHz against −40.5 from 4 kHz) and what is left is two-sided rather than a tilt: 9 dB hot at 6.35 kHz and 20 short at 12.7. Drive does not reach it — across 0.45 to 0.95 the error from 2 to 10 kHz stays inside 0.8 dB, and every dB the wider band appears to gain comes from the top two thirds alone. 30 runs the same amplifier at its own gain and carries the same residual, so the two are one shape measured twice; with the rig off they render identically, which is this section's claim that they are one instrument rather than two, measured. 31 is the one whose chain is short of its reference — 13 dB at 4 kHz — and no drive the clean binding accepts closes more than 5 of it, so what that program needs is not a different binding.

This is the same discipline the room already uses: an external reference has its room baked in and cannot be dried, so the room is measured and the model convolved to match before any timbre metric is taken.

## Versioning

**A rig binding is bank data.** Changing which amplifier a program defaults to changes what a consumer hears from the same file, exactly as changing a patch field does — and neither the semver release version nor the ABI versions can move for it.

So a binding is reported by the library in the tuning dump and carries a unit in `tools/bank-versions.json`, on the same terms as a patch or an engine's constants. A rig swapped without a version moving is a voice that changed with nothing saying why.

The drive and the trim are ordinary tunables and appear as keys. The **preset name** is the one part of a binding that is not a float, so it gets a `#rig` line of its own in the dump and is folded into the table's unit as `rig.NNN` — without that, changing which amplifier program 30 runs would move no fingerprint at all.

## Rules

- **The voice produces the instrument. The rig is a stage.** A patch field that models something downstream of the instrument's boundary is misplaced.
- **A default binding is not a baked-in effect.** It must be visible, addressable and removable, or it is the thing this design replaces.
- **A spec-compliant file must come out sounding complete without asking.** That is the acceptance criterion for any change to how a rig is bound.
- **One implementation per rig component.** Reuse the calibrated one; do not write a lightweight second copy inside the synth.
- **Capture the instrument at its own boundary.** A reference that bakes in a rig is fitted as instrument-and-rig together, and the model's own boundary is then unmeasured.
- **A capture records whether its reference carries a rig, and dryness does not answer that.** A rigged reference is an acceptance target and never a fit target; a rig has no inverse, so unlike a room it cannot be corrected for.
