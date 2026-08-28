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

The same shape appears in GS itself: a drum note's send is a multiplicand of the part's send, not an addition to it (`gs.md`), for exactly this reason. Additive would mean a part taken to silence still sends.

An amplifier is not expressible as a weight — there is no file-controlled "amp amount" for it to scale — so it takes the other form: a stage with a binding the host can clear. Both are bindings; they differ in what the file already had a handle on.

## The RT contract for a binding that changes mid-song

A program change on a part changes which rig that part should carry, and building a rig allocates. This is a new mechanism rather than a variation on an existing one: `part_inserts` is configuration today and never moves once the player is prepared.

- **The audio thread never builds.** A program change marks the part dirty; the control thread rebuilds and publishes, and the render side swaps a snapshot wait-free. This is the path the insertion effect already uses, and the rig joins it rather than growing a second one.
- **A rig swapped under a sounding note loses that note's tail through the old rig.** The same is true of an EFX change on the hardware, and making it seamless would mean running both rigs during the crossover for an effect nobody asked for. Accepted, and stated so it is not rediscovered as a bug.
- **Offline renders resolve inline.** The bounce path already realises a pending change at the top of a block rather than waiting for a control-thread pump, so a program change mid-render takes effect in the same block it arrived in — and the render stays deterministic, which is what the bit-identical contract needs.

## Fitting

**Never let one optimiser move both sides.** The instrument's brightness and the rig's tone control reach the same measurement; a fit given both will trade them against each other and settle somewhere that transfers to neither.

- Pin the rig to a named preset, then fit the instrument against it.
- Where the reference library offers the instrument dry and the rig switchable, capture **both**: the same notes rendered with the rig bypassed and engaged. The difference is the rig's transfer function in isolation, which is what an amplifier model should be fitted to and which no single amplified reference can give.
- Where it does not, the rig is a design choice rather than a fitted one, and is recorded as such.

This is the same discipline the room already uses: an external reference has its room baked in and cannot be dried, so the room is measured and the model convolved to match before any timbre metric is taken.

## Versioning

**A rig binding is bank data.** Changing which amplifier a program defaults to changes what a consumer hears from the same file, exactly as changing a patch field does — and neither the semver release version nor the ABI versions can move for it.

So a binding is reported by the library in the tuning dump and carries a unit in `tools/bank-versions.json`, on the same terms as a patch or an engine's constants. A rig swapped without a version moving is a voice that changed with nothing saying why.

## Rules

- **The voice produces the instrument. The rig is a stage.** A patch field that models something downstream of the instrument's boundary is misplaced.
- **A default binding is not a baked-in effect.** It must be visible, addressable and removable, or it is the thing this design replaces.
- **A spec-compliant file must come out sounding complete without asking.** That is the acceptance criterion for any change to how a rig is bound.
- **One implementation per rig component.** Reuse the calibrated one; do not write a lightweight second copy inside the synth.
- **Capture the instrument at its own boundary.** A reference that bakes in a rig is fitted as instrument-and-rig together, and the model's own boundary is then unmeasured.
