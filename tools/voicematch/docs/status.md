# Bank status — where every voice stands, as one number

The bank is the master. A capture, a reference profile, a gate, a rendered page and an unadopted calibration setting are all attachments to a bank entry, and each used to be readable only through the tool that produced it. `profile.py status --all` covers the instruments a capture exists for and says nothing about the rest of the 128, which is the half of the bank where the next round's work is.

```sh
make voice-status              # the whole bank, only what is past the oracle step
make voice-status-all          # every voice
make voice-status-refresh      # regenerate tools/voice-status.json  (needs BUILD_TUNING)
make voice-status-check        # fail if it is stale
make voice-readiness           # every captured instrument, in profile-column detail
python3 tools/voicematch/status.py --goal 1.8.0   # one release goal's members and what each still needs
```

## Priority and goals come from `policy.json`, and are read rather than stored

`tools/voicematch/policy.json` carries the working-priority tiers and the goals; [objective.md](objective.md) argues them. Each printed row shows its tier as `t1`–`t3`, and the summary carries one line per goal.

**A goal is where attention goes and never a condition on shipping.** Calibration is open-ended analog work and a version whose date arrives with the set unmet ships with it unmet, the goal carrying over. Nothing here may grow into a gate: `status.py` exits 0 whatever it finds, and `make voice-status` is documented as read-only for the same reason — a target that failed on "there is work to do" could not be the thing a loop reads to decide where to start.

**They are resolved when the table is printed, never written into `tools/voice-status.json`.** That file holds what the library and the references reported, and a tier is a decision — baking one in would make reordering the bank need a `-DBUILD_TUNING=ON` rebuild to take effect, and would mark the generated file stale every time the policy moved with no voice having changed. The two kinds of fact are kept in different files for the same reason the bank registry keeps a shared calibration unit apart from the patches that use it.

**A variation carries its capital's tier rather than one of its own.** It is that capital copied and then narrowed, so it cannot be worked first; `variations_follow_capital` records that the shared rank is deliberate rather than an omission.

**A goal names capital tones and kits only.** A goal listing a variation would be blocked on a capture nobody has a source for, which is a fact about the reference market rather than about the voice.

**A kit is named by the rhythm-part program a file selects it with, so `kits` and `programs` are two number spaces over the same integers.** Kit 8 is the Room set and program 8 is the celesta; a tier's two lists are resolved apart, because read as one a tier naming the Room kit also ranks the celesta. `kGsDrumKits` defines 26 rhythm sets while the bank carries one kit row — its kit rows come from the captures that exist rather than from the GS map — so a kit number is held to both: it has to be a program some GS map defines a set at, and it has to have a row before anything can report on it.

**A voice's next action is generated from the bank and then resolved against the policy when it is printed**, for the same reason the rank is. Two things the generated answer cannot say. An **approximated** slot — one whose mechanism this bank does not have and is deliberately answering with the nearest voice it does — reads as uncaptured, so its next action says `capture an oracle`, which is the one instruction that will never be carried out for it; resolved, it is terminal and carries the reason. And a **variation** is its capital copied and then narrowed, so the honest answer is one of two that need different work — *the capital is not accepted yet, and starting this buys a round of rework*, or *the capital is heard, so capture this one from the module*. The summary counts each split rather than printing fifty-nine copies of one sentence, and approximations are counted apart from the voices still waiting for an oracle, because adding the two together names the wrong task list.

**`make conformance` holds the policy to the bank it ranks** (`tests/conformance/check_bank_policy.py`). Everything the policy names is read through `.get()` chains that answer empty for a key that moved, so a program outside the GM range, a goal naming a voice the bank has no row for, two tiers claiming one rank, or a key nothing reads each degrades into a different but plausible ranking rather than into an error. Nothing it asserts is about how far a voice has got: the one stage it reads is a goal's declared step, which has to be a rung of the ladder rather than a number between two. It also puts `status.py`'s own renderer through every declared goal, which is the only class that catches a policy that parses, satisfies every field rule and still kills the tool.

`tools/voice-status.json` is **committed**, so reading it needs nothing. Generating it needs a `-DBUILD_TUNING=ON` library, because the engine voicing each patch is reported by the library rather than parsed out of it.

## The stage

One number per voice, in fifths. Each step is a predicate over facts already on disk, not a weighting anyone chose:

| stage | name | predicate |
|---|---|---|
| 0.0 | untouched | no deliberate patch: a `famN` family fallback on the subtractive engine |
| 0.2 | voiced | a deliberate engine and patch answer it |
| 0.4 | targeted | a reference exists and a profile has been measured from it |
| 0.6 | fitted | a gate that is current, over every canonical dimension |
| 0.8 | heard | somebody listened and it is the instrument |
| 1.0 | settled | and calibration reaches everything the model is asked for |

**The ladder implements [`objective.md`](objective.md), and two of its steps used to say something else.** `measured` demanded two reference timbres and `agreeing` demanded that most gated dimensions sit inside those two references' mutual disagreement. Both are retired: one reference is the target rather than a draw from the distribution of real instruments, so a second is not required, and a green gate is not acceptance because voices have passed every recorded bound while sounding wrong. What replaced them is the ear, moved from a footnote at the top of the ladder to the step that promotes a voice.

**A stage is a floor, not a score.** A voice sits at the highest step whose predicate holds, and an open write-back candidate is a badge rather than a demotion: a candidate nobody has adopted means there may be more to gain, not that what shipped is worse than it was.

**Untouched needs the patch as well as the engine.** Subtractive is the right engine for a synth lead and the default everywhere else, so `tremolo_strings` and `orchestra_hit` are deliberate while `fam10` through `fam15` are eight synth programs sharing one patch nobody has voiced apart.

**Agreement is still computed and printed, and it decides nothing.** Where a capture happens to carry several timbres, each gated bound is compared against how far those references sit from *each other* — a reading of how much the target itself wobbles on that dimension, which is worth knowing before spending an afternoon closing a gap the references do not agree exists. Where a capture has one timbre there is no spread and the answer is unjudgeable, which is a different answer from "none of them agree" and, since [`objective.md`](objective.md), no longer holds anything back. A single dimension whose spread is zero gets the same answer for the same reason: the references agree to finer than the metric resolves, so the ratio has no denominator. The tonewheel organ's arrival is one, both registrations speaking inside a single envelope hop.

**A spread the model has no axis for is a tolerance and never a target.** The timbres of a capture usually differ by something the bank could in principle express — an instrument, a registration, a microphone position — so a voice outside the spread has somewhere to move. The sampled electric guitar's two are the ends of a *fingering* choice on one instrument, and the bank plays one string per note: the band is a real ambiguity in what a note number means on a fretted instrument, which makes sitting inside it a legitimate claim and makes moving toward either end something no fit can be asked to do. Where a capture's timbres differ that way, its `_timbres` note says so, and the printed agreement for that voice is read as a band to sit inside rather than a gap to close.

**The top two steps' claims are the two nothing on disk implies**, so they are recorded by hand in `signoff.json` — see [the two claims 1.0 needs](#the-two-claims-10-needs) below. They are read one per step rather than together: the musical one promotes a voice to `heard`, and the structural one carries it the rest of the way.

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

**Both claims expire, and the two ways they expire are not the same.** Each record carries the `bank_generation` it was taken at and, for a voice that has one, the version of its own patch unit — both from `tools/bank-versions.json`. The patch version moving makes it `stale`: the voice itself changed. A *shared* unit moving makes it `unverified`: one of the 17 engine and fallback-table entries carries values this voice may rest on, and shared calibration constants are their own unit precisely because nothing can attribute them to the patches that use them. That is read from the registry's `kind` rather than from the generation, which moves for any of the 314 units and would retire every voice in the bank whenever one patch is touched — while saying a shared unit had moved when none did. Both block `settled`, and they are named apart so the next action can say which happened. A kit has no single patch unit — its voices are its drum notes — so the `kind: drum` entries stand in for one, and one of them moving makes its record `stale` exactly as a patch bump does. Read separately from the shared units rather than as one generation folding both together: merged, a fit of forty-three of the kit's own notes reported as `unverified` and said a shared unit had moved when none had.

**The bank summary counts both claims, and counts the ones that raise nothing apart.** A record is expensive and a stage is a floor, so a voice's stage says where it stopped and never what has been recorded past the step it stopped at — a structural diagnosis taken before anyone listened sits behind the musical claim, raises no stage at all, and goes on ageing against the bank it was measured on with nothing showing that it exists. So the summary prints how many of each claim are recorded, how many are still current, and how many diagnoses are waiting on the other claim. It is reported and never required: which of the two halves is worth doing next is a decision about time, and `status.py` exits 0 whatever it finds.
