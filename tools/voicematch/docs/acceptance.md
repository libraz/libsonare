# Voicematch acceptance

Acceptance separates source policy, numerical quality, regression gates, bank status, signoff, and listening. The policy and reports describe the current bank; they do not turn an approximation into a fidelity claim.

## Targets

The bank implements GM/GS addresses and aims to sound like the instrument or machine-defined voice named by each slot. Physical modelling and FM are implementation techniques; source policy decides the sound target.

One usable reference is sufficient to define a target and start coverage. A second reference can show agreement or spread, but agreement is informational and never promotes a candidate. Source selection follows the policy in [`policy.json`](../policy.json): a dedicated modern direct product is preferred, then a general modern library, with a machine source where the policy classifies the program, variation, kit, or incompatible instrument as machine. Approximation and no-reference cases are declared by policy and remain visible in reports.

The policy has independent timbre and behaviour answers. Ordinary real-instrument entries use an instrument reference. Machine-defined programs, GS variations, kits, and the policy's different-instrument branch use machine references. Modern drum captures supply the velocity-dependent peak-level range (`vel_range`) that the machine reference cannot establish; that axis uses a modern multi-velocity reference. See [`policy.py`](../policy.py) for the layer and answer rules.

- A slot naming a real instrument aims at that instrument as recorded today, and being unlike the sampled original is not a defect; a slot naming a sound the machine invented (synth leads, pads, brass, strings and basses, the orchestra hit, sound effects) aims at the machine, timbre included, because nothing stands behind it to record and a substitute with another envelope and spectrum stops the part doing its job in the mix.
- Kits follow the machine on both axes because a modern library's kit is a different layout, so following its colour changes which instrument a note reaches; the one crossing is `vel_range`, read from a modern multi-velocity kit because the module holds one recording per note.
- Which slots are machine-defined, `machine_fallback`, `no_reference` and `approximated` are data in `policy.json`, never decided in prose or per tool.
- `machine_fallback` has three cases: no modern source exists, a modern source holds a different instrument at the address, or a modern source's layout does not line up with the map; for a slot not yet enumerated, which case holds is measured by comparing the modern reference against the machine's tone at the same address, never judged.
- An `approximated` slot is answered by the nearest voice the bank has, declared in the `approximated` block with the slot, the answering voice and what the model cannot reach; it is never silence and never a purpose-built stand-in, and two programs sharing one patch owe an entry unless the map calls them one instrument.
- A machine capture is taken with its effects off (reverb, chorus, delay, insertion effect) and with the tone map selected explicitly; the audio stays under the scratch root and only the measurement is committed.
- Diversity beyond GM/GS lives in the patch API and preset catalogue and is not a GS extension, because GS offers only a choice among presets plus eight tone-modify parameters; a voice built that way carries no GM or GS program number.

## Numerical quality

Numerical quality is reported in fixed engineering units. [`fit_quality.py`](../fit_quality.py) uses absolute weighted mean residuals over aggregate terms, reports the worst row/condition, and treats one declared unit as the default target. This is a measurement target, not a perceptual similarity assertion. Missing coverage leaves the report unvalidated.

The quality report and the relative optimizer loss are different views. The relative loss is useful for fitting and may be normalized to its starting score; quality remains in the fixed term units. Gain-normalized shape fields and raw held-level fields are reported separately.

## Regression gates

`profile.py compare --gate` checks measured dimension residuals against recorded regression bounds. A passing regression gate may still have a large absolute residual. The bank's `fitted` stage additionally requires a current gate record, complete canonical coverage, and an existing reference/profile. Each canonical dimension must be bounded or explicitly excluded with a nonempty reason. Capture `dimensions_na` and gate `_excluded` supply exclusions; gate `_unbounded` dimensions remain open and keep coverage incomplete.

Gate agreement compares multiple references with their spread when that comparison is possible. It is a diagnostic for disagreement, not a promotion rule. One reference therefore remains sufficient for target definition and fitting; it does not produce an agreement verdict.

Data goals are a report only. The `_goals_do_not_gate` policy keeps goals out of release, merge, and CI decisions. The normal status report is read-only; `status.py --check` validates the generated status artifact and may exit 1 when it is missing or stale.

## Promotion and work order

- A number never promotes a voice; measurement has two jobs, getting a voice near the target cheaply and stopping a good voice from being broken later, and the ear decides acceptance.
- When calibration cannot reach the target, treat it as a missing mechanism and change the model (`autofit.py --diagnose` reads whether any knob moves a measurement at all); never add a parameter because it lowers an error, and justify a mechanism by the physics first and the measurement second.
- Work in order of frequency of use: `policy.json` carries the tiers and goals and `make voice-status` reads them, and a variation waits for its capital because a capital that moves invalidates every variation under it.
- A variation carries its capital's tier (`variations_follow_capital`), and a goal names capital tones and kits only.
- `kits` and `programs` in a tier or goal are two number spaces over the same integers (kit 8 is the Room set, program 8 the celesta), resolved apart; a kit number must be a program some GS map defines a set at and must have a bank row.
- Tiers and goals are resolved when the table is printed and never written into `tools/voice-status.json`, which holds only what the library and the references reported.

## Bank status

[`status.py`](../status.py) reports the highest sequential stage whose predicates hold:

| stage | evidence |
|---|---|
| `untouched` (0.0) | no deliberate engine/patch assignment |
| `voiced` (0.2) | a deliberate engine/patch assignment |
| `targeted` (0.4) | at least one reference timbre and measured profile row |
| `fitted` (0.6) | a current gate and complete canonical coverage |
| `heard` (0.8) | a current music claim |
| `settled` (1.0) | current music and structure claims, with no open unreachable terms |

These labels summarize evidence; `fitted` does not imply absolute numerical target attainment.

- A stage is a floor, not a score: an open write-back candidate is a badge and never a demotion.
- `untouched` needs the patch as well as the engine, because subtractive is both a deliberate choice for a synth lead and the default for every `famN` fallback.
- `toneclass.canonical_dimensions` is the coverage denominator: the dimensions a class can be judged on, listed when the measurement means something for that excitation.
- A spread the model has no axis for (such as two fingerings of one string instrument) is a tolerance to sit inside, never a target to close.
- `make voice-status` shows the bank past the oracle step, `make voice-status-all` every voice, and `make voice-readiness` every captured instrument in profile-column detail; `status.py --goal <version>` lists one goal's members and what each still needs.
- Recording is not adoption: a candidate in `calibrations.json` stays until it is written back (deleted from the file in the same change) or judged and deleted.
- Each `calibrations.json` entry carries `title` and `desc` in both `en` and `ja` and `calibration.py` refuses the file without them; an ad hoc `--variant` carries none and its button shows its name.

Use [`reference.md`](reference.md) for captures and probes, [`fitting.md`](fitting.md) for fitting and gate commands, and [`development.md`](development.md) for recovery or probe changes. The generated status file is an output of those workflows, not a replacement for their source evidence.

## Signoff

[`signoff.json`](../signoff.json) contains two manually maintained claims: `music` records the listening decision and `structure` summarizes a scoped `autofit.py --diagnose` result. Neither is written automatically from audition feedback. Diagnosis samples `2n+1` points and can add interior or joint points up to `4n+3`; an `unreachable` term is a sampled hypothesis, not an exhaustive proof.

An accepted term must name an existing unreachable term and include a nonempty reason. Every unreachable term needs an accepted reason before structure can be settled. Both claims must be present and current; a moving own patch or drum unit makes its claim stale, while a moving shared unit makes it unverified. Either state blocks settled. Structure records its spec/probe scope; music records its listening take. Both are keyed by voice slug and carry provenance from the bank's version registry. Accepting a structural limitation closes that recorded item and does not establish fidelity.

## Listening

Use [`make_audition.py`](../make_audition.py) to build the bank-indexed audition page and [`serve.py`](../../audition/serve.py) to serve it; page details are in [`tools/audition/README.md`](../../audition/README.md). The compact manifest contract is:

| source | role and current fields |
| --- | --- |
| baseline | model render, with `role`, `path`, `title`, and `desc` metadata |
| policy reference | the selected capture reference when policy supplies one; comparison captures are separate sources |
| candidate | named entries from [`calibrations.json`](../calibrations.json) via `--calibrations`, or ad hoc `--variant`; direct-input variants retain their path and label |
| take | current/adopted/candidate role and version, with one shared gain across versions of a take so level differences remain audible |

The page can select `--model-sends auto|gs|dry`; `auto` follows the reference room-send evidence, `gs` keeps GS sends, and `dry` removes them. Full flags and take selection belong to `make_audition.py --help`. Listening feedback is untracked JSONL under `<scratch>/feedback/<slug>.jsonl`; grades and tags are evidence for the manual music claim and do not promote a bank automatically.
