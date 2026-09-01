# Bank status — where every voice stands, as one number

The bank is the master. A capture, a reference profile, a gate, a rendered page and an unadopted calibration setting are all attachments to a bank entry, and each used to be readable only through the tool that produced it. `profile.py status --all` covers the instruments a capture exists for and says nothing about the rest of the 128, which is the half of the bank where the next round's work is.

```sh
make voice-status              # the whole bank, only what is past the oracle step
make voice-status-all          # every voice
make voice-status-refresh      # regenerate tools/voice-status.json  (needs BUILD_TUNING)
make voice-status-check        # fail if it is stale
make voice-readiness           # the four captured instruments, in profile-column detail
```

`tools/voice-status.json` is **committed**, so reading it needs nothing. Generating it needs a `-DBUILD_TUNING=ON` library, because the engine voicing each patch is reported by the library rather than parsed out of it.

## The stage

One number per voice, in fifths. Each step is a predicate over facts already on disk, not a weighting anyone chose:

| stage | name | predicate |
|---|---|---|
| 0.0 | untouched | no deliberate patch: a `famN` family fallback on the subtractive engine |
| 0.2 | voiced | a deliberate engine and patch answer it |
| 0.4 | measured | two or more reference timbres, a measured profile, a gate that is current |
| 0.6 | covered | every canonical dimension gated, or excused with a reason |
| 0.8 | agreeing | most gated dimensions sit inside the reference's own spread |
| 1.0 | settled | no structural residual, and the musical take signed off |

**A stage is a floor, not a score.** A voice sits at the highest step whose predicate holds, and an open write-back candidate is a badge rather than a demotion: a candidate nobody has adopted means there may be more to gain, not that what shipped is worse than it was.

**Untouched needs the patch as well as the engine.** Subtractive is the right engine for a synth lead and the default everywhere else, so `tremolo_strings` and `orchestra_hit` are deliberate while `fam10` through `fam15` are eight synth programs sharing one patch nobody has voiced apart.

**Agreement is measured against the references' own spread**, not against zero. A voice inside that spread is as close to the instrument as two presets of the instrument are to each other, which is the strongest claim this harness can make. Where a capture has one reference timbre there is no spread and no dimension can be adjudicated at all — reported as unjudgeable, which is a different answer from "none of them agree".

**The last step's two claims are the two nothing on disk implies**, so they are recorded by hand in `signoff.json` — see [unadopted settings and signed claims](#unadopted-settings-and-signed-claims) below.

## Coverage is all-or-nothing

A canonical dimension is gated, or it is named in the capture's `dimensions_na` with a reason, or it is a gap. There is no fraction to tune and no majority to argue about.

`toneclass.canonical_dimensions` is the denominator: the dimensions a class can be judged on, listed for the class when the measurement means something for that excitation rather than when some instrument happened to be measured on it. A sustained voice is not judged on a free decay it does not have; a bar or a bell is not judged against equal temperament; a kit uses the percussion vocabulary and no ladder.

**An exclusion argued only in prose reads as a gap.** The piano's `damper` and `tnr` and the organ's `tnr`, `damper` and `vel_range` were each argued with a measurement in the capture's `_dimensions` note and were invisible to anything mechanical. `dimensions_na` is that argument as data:

```json
"dimensions_na": {
  "damper": "the three grands disagree by 120-195 ms at the median, wider than the model's own error"
}
```

A dimension with no reason is not excusable. That is the whole of the discipline here — the same rule the parity allowlist runs on, for the same reason.

## What the engine column comes from

`#mode<TAB><patch><TAB><engine>` in the `SONARE_TUNING_DUMP` catalogue, recorded by `apply_patch_tuning` as the fallback tables are built. Keyed by patch rather than by program, since one patch commonly voices several programs and the engine belongs to the patch.

The page groups the sixteen engines into three methods — physical model, FM, and subtractive/additive — because what the eye is asked for is which of the three, and sixteen hues would answer nothing. The engine's own name is on the row.

**A kit is not its program's melodic patch.** On channel 10 the program selects the kit and the note selects the instrument, so the engine belongs to the drum notes; asking the program map gives whatever melodic voice shares the number, and program 0 answers `piano`.

<a id="unadopted-settings-and-signed-claims"></a>

## Unadopted settings

`calibrations.json` holds a candidate that has been heard and not written back. It is the only place such a thing can live: an override string kept in a render directory under the scratch root goes with the directory, and one harpsichord round's seven candidates were lost exactly that way.

Recording is not adoption. A setting stays there until it is either written back — deleted from the file in the same change — or judged and deleted. The bank view counts them per voice; `make voice-status` prints the total.

## The two claims 1.0 needs

Every step below `settled` is a predicate over a file some tool already wrote. The last one is not, because it is the two questions a comparison cannot ask, and `signoff.json` is where their answers go — keyed by voice slug, like `calibrations.json`.

```json
"p000-acoustic-grand-piano": {
  "structure": {
    "provenance": { "date": "2026-09-01", "bank_generation": 19, "patch_version": 1 },
    "spec": "specs/piano_corpus.json",
    "probe": "the corpus pattern over 15 notes and 4 velocities",
    "unreachable": [],
    "accepted": {},
    "note": "…"
  }
}
```

- **`structure`** is one `autofit --diagnose` run reduced to the part that outlives it. `unreachable` is the terms it reported no knob moves at all, which is the only verdict that is a structural claim; `spent`, `partial` and `reachable` are values, weights or budget and belong to the next fit. `spec` and `probe` are recorded because a knob whose axis the probe holds fixed reads inert and is not.
- **An unreachable term is accepted with a reason, or it is open.** Same discipline as a capture's `dimensions_na` and the parity allowlist, for the same reason. A term accepted with an empty reason is refused, and so is one the diagnosis never reported. An open term blocks `settled` and is what the voice's next action names — which is the point of recording a diagnosis that found something: it turns "nobody has looked" into a named measurement with no mechanism behind it.
- **`music`** is somebody's word that a take is the instrument. No metric produces it and none ever will; the rest of the harness exists to make it a smaller question.

**Both claims expire, and the two ways they expire are not the same.** Each record carries the `bank_generation` it was taken at and, for a voice that has one, the version of its own patch unit — both from `tools/bank-versions.json`. The patch version moving makes it `stale`: the voice itself changed. Only the generation moving makes it `unverified`: some unit's values moved, and shared calibration constants are their own unit precisely because nothing can attribute them to the patches that use them. Both block `settled`, and they are named apart so the next action can say which happened. A kit has no single patch unit — its voices are its drum notes — so only the generation dates a claim about it.
