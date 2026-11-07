# Measuring musical accuracy

`libsonare` matches librosa where the two overlap, and that agreement is checked in CI. It is not an accuracy claim. Matching librosa means the two compute the same thing; whether the thing they compute names the right key, or finds the right chord, is a separate question that only annotated recordings can answer.

This directory holds the tooling for asking that question, and for turning the answer into figures a docs page can carry. Nothing here ships a corpus — the datasets below are licensed for research use and cannot be redistributed — so the numbers are produced locally against a corpus you hold, and only the numbers are published.

## The three pieces

| piece | what it is |
| --- | --- |
| `tests/fixtures/music_eval/*.tsv` | manifests naming the audio, the annotation and the expected answer |
| `tests/fixtures/run_optional_fixture_report.py` | runs the fixture tests and writes one observation per fixture per metric |
| `tools/eval/summarize_accuracy.py` | rolls those observations into per-dataset aggregates and a markdown table |

## The one measurement that needs no corpus

Multiple-F0 accuracy is measured against material this repo renders, so it is reproducible from a clean checkout. The chords are played through the GM fallback bank and the ground truth is the note list rather than an annotation — nothing is transcribed, so nothing can be transcribed wrong.

```sh
./build/bin/sonare_tests "[polyphony_eval]" | python3 tools/eval/summarize_polyphony.py --require 11
```

The figures are the frame-level precision, recall and F-measure at a 50-cent tolerance, plus the polyphony error — the mean absolute difference between the number of F0s reported and the number sounding. Matching is greedy nearest within the tolerance, one estimate per true F0, so two estimates on one note score a hit and a false alarm rather than two hits.

**The corpus is the subject, not the backdrop.** Sustained mid-register chords score a flat 1.000 whatever the program is, and a first version made of nothing else reported a perfect model — it was measuring its own difficulty. The items that separate one model from another are a voice count the polyphony cap cannot satisfy, intervals inside the separation rule, a register the default framing does not reach, and noise. The summarizer warns when every item lands within 0.05 of every other, because that is what the first run looked like.

**Two of the figures are not accuracy claims and should not be read as ones.** A four-voice chord rooted at C2 scores zero, because the default framing does not reach that register at all; the item is carried to document the limit, and a figure above zero there is a change worth reading rather than a pass. And the semitone cluster scores 0.667 with a polyphony error of zero — the third voice is not missing, the model reports three F0s and one of them stands where no note is.

**Noise at a quarter of the peak *raises* the low-register score** (F 0.734 to 0.855, polyphony error 1.68 to 1.02). The estimate is not what improves: at the edge of what the framing resolves, ridges break when their salience dips under a share of their own running peak, and a noise floor holds them above it. So the figure is reading the ridge-break rule's fragility rather than the estimator's accuracy, and a change that moves those two items together is probably a tracking change rather than an estimation one.

**Note-level polyphonic transcription is measured the same way, against rendered piano scores** (chords of five to eight voices with and without octave doubling, a sixteenth-note line, a melody over a held chord, repeated strikes of one key, a held note, and white and pink noise). Run it from a Release build — its floors were taken there — with `sonare_tests "[transcribe_eval]"`. Each item prints two `TRANSCRIBE_EVAL {json}` lines, `"config":"default"` (the transcription defaults) and `"config":"legacy"` (four voices, 140 ms, no re-strike split), with `tp`/`fp`/`fn`, precision, recall and F-measure under a match of onset within 50 ms and pitch within 50 cents, one-to-one; `cap_hit_share` is the share of frames whose estimated voice count reached the polyphony cap, `mean_polyphony` the mean count, `notes_per_second` the transcribed note rate (the figure to read on the noise items, where every note is false), and `wall_ms` the transcription's own time. `"[transcribe_eval_sweep]"` prints `TRANSCRIBE_SWEEP {json}` lines with a `param` and `value` for each candidate default varied alone from the defaults; it asserts nothing.

## Monophonic pitch editing

```sh
cmake --build build --target sonare_tests -j2
./build/bin/sonare_tests "[pitch_refinement]" -s
```

These regressions measure mean and maximum absolute pitch error in cents against generated tones with known frequencies, excluding windows that touch the signal boundaries. They exercise pitches between pYIN's 10-cent bins, including harmonic tones, and check that refinement preserves the decoder's voicing decisions and probabilities. The measurements cover synthetic signals; they do not establish accuracy on recorded singing.

The editing provider refines each voiced estimate within 25 cents using a fixed-window squared-difference minimum and parabolic interpolation. This shares the existing vocal-note measurement code. Public `pyin` output and persisted vocal analysis retain their original values; auto-tune and monophonic transcription receive the refined contour.

The method follows the difference-function and interpolation approach in [de Cheveigné and Kawahara's YIN paper](https://doi.org/10.1121/1.1458024), after the candidate tracking described in [Mauch and Dixon's pYIN paper](https://webspace.eecs.qmul.ac.uk/s.e.dixon/pub/2014/MauchDixon-PYIN-ICASSP2014.pdf). Reference implementations consulted were [QMUL's pYIN](https://github.com/c4dm/pyin) and [librosa's pitch implementation](https://github.com/librosa/librosa/blob/main/librosa/core/pitch.py). The local refinement retains the decoder's octave choice; it does not repair octave errors or missed voiced frames.

## Score-derived monophonic transcription

The optional Bach evaluation reads note events from external score JSON files. It generates audio with known note times, then calls `transcribe_notes` with its default pitch and segmentation settings. No scores or recordings are bundled with this repository.

```sh
cmake --build build --target sonare_tests -j2
SONARE_BACH_REFERENCE_DIR=/path/to/score-reference \
  ./build/bin/sonare_tests "[bach_transcribe_eval]"
```

The directory must contain `BWV1001_4.json`, `BWV1006_1.json`, `BWV1007_1.json`, and `BWV1003_4.json`. Each file supplies `tracks[0].notes`, an array of objects with `pitch` (MIDI note number), `onset` (quarter-note beats from the score's start), and `duration` (quarter-note beats). For example, a note event can be `{"pitch": 69, "onset": 0, "duration": 0.25}`. The selected passages must contain notes; an empty file is not an evaluation input.

The excerpts are track 0 of BWV 1001/4 (beats 0–16, 96 BPM), BWV 1006/1 (320–336, 120 BPM), BWV 1007/1 (36–52, 120 BPM), and BWV 1003/4 (276–292, 80 BPM). Each is rendered at 16, 22.05, 44.1, and 48 kHz with five harmonics, a 4 ms attack, an 8 ms release, and 200 ms of silence at each end. The fixed rendering tempi replace the corpus tempo maps. Ground-truth onsets and offsets use the rendered sample positions.

Notes are matched one-to-one by maximum bipartite matching, with onset error at most 50 ms and MIDI-note pitch error at most 50 cents. Since both note numbers are integers, the pitch criterion requires the same MIDI note; it does not measure the continuous F0 error. Each observation reports TP/FP/FN, precision, recall, F-measure, and transcription time. Missing corpus files fail an explicitly requested run; accuracy figures have no CI pass threshold. Keep the corpus revision and build alongside any before/after report.

This evaluation measures transcription of generated audio. Evaluating a recording against the same score requires a separate alignment: the [Bach Violin Dataset](https://hermandong.com/bach-violin-dataset/alignment.html) uses synthesized scores and CQT-based dynamic time warping. Such an estimated alignment needs checking before it can serve as timing ground truth.

Monophonic recordings with note annotations are measured through `tests/fixtures/music_eval/notes_manifest.tsv` instead, against the annotated notes rather than a generated score. Each row reports three figures under one-to-one matching: `f_onset` checks onset alone, so it reads segmentation (splits and merges) without any pitch term; `f_note` adds pitch within 50 cents and is the figure a row's `min_f_note` floor applies to; `f_note_offset` further requires the offset within the larger of 50 ms and 20% of the annotated note's duration. `note_count_ratio` is the predicted note count over the annotated one, so a value above 1 means over-splitting and a value below 1 means merged or missed notes.

The implicit analysis configurations used by monophonic transcription and auto-tune cap their hop at approximately 11.6 ms, while retaining the existing 512-sample hop at 44.1 kHz and above. Explicit pitch-analysis configurations keep their caller-supplied hop. [Tony's transcription method](https://www.justinsalamon.com/uploads/4/3/9/4/4394963/mauch_tony_tenor_2015.pdf) likewise uses a fixed analysis cadence after resampling; this implementation keeps the existing analysis-window duration and improves only the lower-rate hop.

Monophonic transcription also separates rearticulations inside an existing note using short valleys in its RMS envelope. The envelope window spans two periods at the note's measured pitch. A split requires a deep valley with a rapid fall and recovery, enough distance from the original boundaries, and child notes that satisfy the configured minimum duration. The fall and recovery are measured against their respective local peaks, so a quiet release cannot make a slow amplitude dip appear brief. Each child is measured again from its own pitch frames. This follows the use of amplitude changes for note segmentation in Tony §3.3; the valley-width and boundary checks are specific to this implementation. Regression controls include smooth amplitude modulation and vibrato. A short rearticulation at low pitch can remain merged because the two-period window smooths over its amplitude valley.

Run `./build/bin/sonare_tests "[reattack]"` for repeated-note regressions and controls. These generated signals require no external corpus.

Before extracting monophonic notes, transcription checks for stable pitch estimates at one-half, one-third, or one-quarter of a locally measured fundamental. A short fixed-window YIN analysis proposes a correction; a fractional-period waveform comparison must also show that the shorter period fits at least as well as its integer multiple. This second check protects low notes with a strong second harmonic. Corrections must persist for 30 ms. The correction applies to monophonic transcription only; the shared editing provider, public pYIN output, and persisted vocal analysis do not receive it. Run `./build/bin/sonare_tests "[pitch_alias]"` for independent leap melodies and low-fundamental controls.

## Corpora that can be measured against

Each is obtained separately, under its own terms. The annotations are what matters; several of these distribute annotations openly while the audio has to be sourced by the user.

| dimension | corpus | notes |
| --- | --- | --- |
| key | GiantSteps Key, GiantSteps MTG Key | EDM single-key excerpts; the key is the annotation |
| key | Isophonics (Beatles, Queen, Zweieck) | key and chord annotations for the same tracks |
| chord | Isophonics, Billboard (McGill) | Billboard is the larger and harder set |
| chord | JAAH | jazz; the corpus that exercises the extended vocabulary |
| beat, downbeat | Ballroom, Hainsworth, SMC | SMC is deliberately the hard one |
| beat, downbeat | GTZAN rhythm annotations | annotations are open; the audio is not |
| bpm | Ballroom, GTZAN | tempo annotations accompany the beat ones |
| meter | Ballroom | one time signature per genre class |
| structure | Harmonix Set, SALAMI | section boundaries and same-material labels; annotations are open, the audio is not |
| notes | URMP, Bach10, TONAS | note annotations on monophonic material: URMP's and Bach10's per-instrument tracks, TONAS's a cappella singing; frame-level F0 corpora carry no note boundaries and do not fit this manifest |

## Running a measurement

Rows that gate CI and rows that produce a measurement are not the same rows. A gating row asserts and prints nothing, so it contributes no observation; a measurement row is marked `report_only`, which turns the assertion into a printed figure. A manifest can hold both.

```sh
# 1. Point at the corpus and fill in the manifests (see the fixtures README for
#    the row format). Mark measurement rows report_only.
export SONARE_MUSIC_FIXTURE_ROOT=/path/to/corpus

# 2. Build the optional-fixture test binary and run the suite, capturing the
#    per-fixture observations.
make test-optional-fixtures
python3 tests/fixtures/run_optional_fixture_report.py \
    --suite music \
    --sonare-tests build-optional-fixtures/bin/sonare_tests \
    --output /tmp/accuracy-report.json

# 3. Roll it up.
python3 tools/eval/summarize_accuracy.py /tmp/accuracy-report.json --markdown
```

`make accuracy-report` runs steps 2 and 3 with the default paths.

## What the summary reports

- **key** — exact accuracy, plus the MIREX weighted score, which gives partial credit for a fifth (0.5), a relative (0.3) and a parallel (0.2) confusion. The category counts come with it, because *which* way key detection is wrong is more useful than how often.
- **bpm** — accuracy within 4% relative error (MIREX Acc1), plus the median relative error.
- **beat**, **downbeat** — mean F-measure at the manifest's tolerance, plus the share of tracks above 0.5.
- **meter** — numerator and denominator both correct.
- **structure** — mean boundary hit-rate F at ±0.5 s (HR.5F) and ±3 s (HR3F), and pairwise F on 0.1 s frames, per `features` value (`both` carries the unprefixed keys). HR is a maximum one-to-one matching of interior boundaries; pairwise F scores whether frame pairs share a group. Until sections carry a group of their own, the estimate's group is its section label.
- **chord** — weighted chord symbol recall over a major/minor vocabulary, the exact-quality WCSR beside it, and root, quality and bass accuracy separately. The gap between the maj/min and exact figures is the vocabulary's contribution.

Every figure carries its fixture count and is broken down by dataset. A mean over a mixed corpus says nothing without knowing what went into it.

## Two things the tool refuses to do

**It does not score an empty set.** A dimension with no observations reports `unmeasured`, not 0% and not 100%. A metric that skips unusable data points and averages the rest scores an empty set as perfect, and that failure mode has already been found once in this repo.

**It does not hide a partial corpus.** The fixture runner skips a row whose audio is missing rather than failing it, so a typo in a path looks exactly like a pass in a summary line. `--require <dimension>` makes the summarizer exit non-zero when a dimension it was told to expect produced nothing, which is what a publishing pipeline should use.

## Publishing

Publish the markdown table together with: the corpus name and version, how many tracks of it were used, the library version, and the configuration each dimension ran under. A number without those cannot be reproduced or compared, and an accuracy figure that cannot be compared is decoration.
