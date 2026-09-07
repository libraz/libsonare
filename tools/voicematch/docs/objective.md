# What the bank is for, and what decides that it is done

This page exists because the harness's own criteria kept being mistaken for the goal. Everything else in `docs/` describes a measurement; this one says what the measurements are in service of, and which of them are allowed to decide anything.

## The objective

**Reproduce the sounds the SC-8850's GS map names.** The map says which slots exist and what each one is — an acoustic grand at program 0, a darker one at bank 2, a standard kit on channel 10. That is the specification, and it is the only thing the machine supplies.

**It does not say what any of them should sound like.** The samples are a quarter-century old and were degraded to fit the hardware of their day; a recording made now is a better instrument than the one they hold. So the map is followed and the timbre is not, which is the same split `src/midi/synth/docs/gs.md` already states from the other side: "sounding like an SC-88Pro is explicitly out of scope."

**Physical modelling and FM are the technique, not the product.** An exact match to any sampled source is impossible by construction and is not what is being attempted. What is being attempted is that a listener hears the instrument the slot names.

**Building a correct general-purpose physical model is not the objective.** It is welcome where it happens, and it is often the cheapest route to a voice that sounds right, but no voice is finished because a model is principled and none is unfinished because a model is ad hoc.

## One reference is enough, and it is a target rather than a sample

**A reference is the thing being aimed at, not an estimate of a hidden truth.** This is the point the harness had backwards. A statistical reading of a reference treats it as one draw from the distribution of real instruments, which makes a second draw necessary before anything can be said — and makes fitting closely to the first one *overfitting*. Under the objective above that reading is simply wrong: a good modern recording of a grand piano is a legitimate thing to sound like, and matching it closely is the goal rather than a failure mode.

So:

- **A voice needs one reference. Two are not required and must not be demanded.** The cost of a second is a plugin authored by hand per instrument, which across the bank is out of all proportion to what it buys.
- **The reference-spread criterion — "inside the two references' own disagreement" — is retired as a promotion condition.** Where a capture happens to carry several timbres the spread is still worth printing, as information about how much the target itself wobbles. It decides nothing.
- **Where a voice's design needs a different signal than the obvious reference carries, the reference is wrong, not the design.** The electric guitars are the standing case: the rig is a separate per-part stage here (`voicing.md`), so the voice must be fitted against a direct-injected instrument and never against a sample with an amplifier baked into it.

**Reference source priority**, in order, and the first that covers the instrument wins:

1. A product dedicated to that instrument, played direct — the closest thing to the instrument alone.
2. A general instrument library.
3. The SC-8850 itself, as the oracle of last resort, for slots nothing else reaches.

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
