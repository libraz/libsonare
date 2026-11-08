# Measuring musical accuracy

`libsonare` matches librosa where the two overlap, and that agreement is checked in CI. It is not an accuracy claim. Matching librosa means the two compute the same thing; whether the thing they compute names the right key, or finds the right chord, is a separate question that only annotated recordings can answer.

This directory holds the tooling for asking that question, and for turning the answer into figures a docs page can carry. Corpora are obtained separately under their own licenses. Audio and annotations remain external; published measurements identify their source and evaluation conditions.

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

## Vocadito real-recording audit

[Vocadito v3](https://zenodo.org/records/5578807), by Bittner et al., provides solo singing and two independent note annotations under CC BY 4.0. Download its original `vocadito.zip` separately. The preparer verifies the published archive checksum and converts onset/pitch/duration CSV rows into onset/offset/pitch-Hz annotations without changing timings or audio.

```sh
rye run --pyproject bindings/python/pyproject.toml python tools/eval/prepare_vocadito.py \
  --archive /path/to/vocadito.zip --output /path/to/vocadito-eval
cmake -S . -B build-optional-fixtures -DSONARE_ENABLE_OPTIONAL_FIXTURE_TESTS=ON
cmake --build build-optional-fixtures --target sonare_tests -j2
SONARE_MUSIC_FIXTURE_ROOT=/path/to/vocadito-eval \
  rye run --pyproject bindings/python/pyproject.toml python tests/fixtures/run_optional_fixture_report.py \
  --suite music --sonare-tests build-optional-fixtures/bin/sonare_tests --output /path/to/report.json
rye run --pyproject bindings/python/pyproject.toml python tools/eval/summarize_accuracy.py \
  /path/to/report.json --require notes --markdown
```

The singer split is fixed before predictions: sort unique singer IDs by SHA256 of `libsonare-vocadito-v3-split-v1:` followed by the ID, and reserve the first ceiling of one third. In this archive that leaves 19 singers/28 recordings for audit and 10 singers/12 recordings reserved. The output contains only audit audio, both annotators' converted notes, a report-only manifest, and `split.json` with source and output hashes. The default evaluation command runs only these audit recordings. A nonempty output directory is rejected.

The [2026-11-08 audit snapshot](reports/vocadito-v3-audit.json) uses unchanged monophonic defaults at native 44.1 kHz. Its pooled note F is 0.559 against A1 and 0.511 against A2; including offsets gives 0.397 and 0.329. The detector emits 1,862 notes against 1,574/1,308 annotated notes. Annotator-to-annotator note F is 0.731; that describes annotation disagreement, not an accuracy ceiling. The snapshot also distinguishes pooled scores from mean recording scores and reports pitch errors only on onset-matched pairs with their coverage. Those note-median errors are not frame-level F0 accuracy.

Disabling the pitch-alias repair leaves all 28 exported note lists identical. Disabling RMS reattack splitting reduces 1,862 predictions to 1,860. With splitting enabled, recording 9 gains one false positive and recording 10 gains one true positive against both annotators. These additions do not explain most excess notes; upstream pitch tracking, voicing, and base segmentation remain to be diagnosed.

The earlier 0.994 Bach figure describes development on generated audio and does not estimate recorded-singing accuracy. The 28-recording audit is observed development evidence. The default preparation command excludes the reserved singers; the frozen experiment below subsequently used those 12 recordings for confirmation. They are now observed data too and cannot serve as an untouched test set for later tuning. Use a separate instrument corpus before claiming instrument coverage.

### Acoustic support for note boundaries

Monophonic transcription keeps a median-compatible boundary when a spectral onset falls within 50 ms. It joins unsupported, frame-contiguous regions only while the original median pitches of the whole group remain within the existing segmentation threshold. RMS reattack splitting follows. The onset cue uses maximum-filter vibrato suppression from [Böck and Widmer's SuperFlux method](https://www.dafx.de/paper-archive/details/0oee-99Z88WL7pSo749gcA), with the fixed geometry and peak rule recorded in the [measurement](reports/monophonic-acoustic-boundary-2026-11-08.json). Public pYIN and persisted vocal analysis retain their existing behavior. Run `./build/bin/sonare_tests "[acoustic_boundary]"` for generated vibrato, glide, and semitone controls.

The implementation and acceptance conditions were frozen before predictions on 18 separately selected recordings from [Annotated-VocalSet](https://zenodo.org/records/7061507), using the original [VocalSet audio](https://zenodo.org/records/1442513), both under CC BY 4.0. Six singers were selected by a fixed hash rule, each contributing straight scales, vibrato arpeggios, and breathy scales. Both boundary conventions were fixed in advance: extended1 is primary; extended4 is a sensitivity check. Neither convention was selected by its score.

| Reference | Note F, baseline → candidate | Offset F, baseline → candidate |
| --- | ---: | ---: |
| Vocadito, all 40 observed recordings, A1 | 0.552183 → 0.565997 | 0.384200 → 0.404528 |
| Vocadito, all 40 observed recordings, A2 | 0.511126 → 0.526267 | 0.328164 → 0.348993 |
| VocalSet, 18 recordings, extended1 primary | 0.136182 → 0.136300 | 0.072096 → 0.083449 |
| VocalSet, same recordings, extended4 sensitivity | 0.427236 → 0.445063 | 0.363151 → 0.397775 |

The primary VocalSet note result is nearly unchanged and its absolute accuracy remains low. Note true positives decrease from 51 to 49 while predictions decrease from 503 to 473. Extended4 retains 160 note true positives and gains seven offset matches. On the observed Vocadito recordings, predictions decrease from 2,573 to 2,445; A1 loses three note true positives and A2 retains its count. These are precision/recall tradeoffs, with all recording-level results in the measurement.

A separate development control multiplies float samples by 0.1 without normalization or re-quantization. Both annotators' pooled note and offset F improve against the same-gain baseline, but note true positives decrease by 16/10. The absolute spectral threshold remains gain-sensitive; passing this control does not establish gain invariance. Short repeated notes at 110 Hz, a held tone with wide vibrato, and fast 32 Hz amplitude modulation remain unresolved synthetic cases.

Three alternating serial runs on the original 28 development recordings increased median user CPU time from 10.74 to 11.51 seconds, about 7.2%. Median wall time was 12.46 → 14.38 seconds and peak RSS 137.4 → 147.3 MB, with unrelated background work causing variation. The short synthetic reattack matrix showed no measurable overhead; full-recording costs are the relevant comparison.

The measurement identifies all selected archive members, SHA256 hashes, the exact annotation conversion, source and executable hashes, and matching tolerances. Use `Sound` rows with `Start time`, `End time`, and `Median F0` as onset/offset/Hz fixture annotations, without alignment fitting. These human-reviewed annotations use pYIN-assisted pitch contours; they are not independent frame-level F0 labels. Only selected ZIP members were retrieved and checked against member CRC/size; whole archive checksums were not verified for this corpus. All 18 recordings are now observed. Broader singing and instrument coverage still needs separate data.

### Where the remaining error is

`note_oracle.py` separates tracker errors from segmentation errors. For every Vocadito recording it runs `sonare_eval_note_oracle`, which prints the tracker's contour and the default notes, then repeats the same segmentation stages along the annotated F0 contour through `transcribe_monophonic_track`. It also labels each annotated note span with the median of the tracker's voiced frames inside it, and scores frame voicing and pitch against the annotated contour. Matching uses the MIREX defaults given above.

```sh
cmake --build build --target sonare_eval_note_oracle
python3 tools/eval/note_oracle.py --vocadito /path/to/extracted/vocadito \
    --output /path/to/note-oracle.json
```

The [2026-11-08 measurement](reports/vocadito-v3-note-oracle-2026-11-08.json) covers all 40 recordings, all of them already observed. Pooled note F:

| Condition | A1 | A2 |
| --- | ---: | ---: |
| Default transcription | 0.566 | 0.526 |
| Annotated contour, same segmentation | 0.633 | 0.588 |
| Annotated note spans, tracker pitch | 0.899 | 0.956 |

Against the annotated contour the tracker reaches 0.920 raw pitch accuracy and 0.924 raw chroma accuracy, with 0.981 voicing recall and a 0.431 voicing false-alarm rate. Given correct boundaries, its pitch labels most notes correctly. Given a correct contour, segmentation still stays near 0.63. Most remaining note error therefore belongs to segmentation.

The boundary profile in the report shows the shape of that error. Against A1, the default output starts two or more notes inside 295 reference notes and starts inside two or more reference notes in 370 estimates; 214 estimates overlap no reference. With the annotated contour, spurious notes fall to 12, so most of them follow the tracker's voicing, while split references rise to 492. A pitch-movement threshold splits natural intonation movement once the contour is accurate, and it cannot separate same-pitch articulations. Each candidate below added one local cue to that threshold.

The annotated contour is itself an annotation, not ground truth. Annotated notes overlap in places, which is why the span condition stays under 1.

### Candidates that did not hold

Each candidate below was frozen before recorded predictions and judged against fixed acceptance conditions. The reports keep the patches, hashes and per-recording results.

| Candidate | Why it does not hold | Record |
| --- | --- | --- |
| Join frame-contiguous, median-compatible regions without onset support | Confirmation A2 offset F fell (0.325254 → 0.324633) | [report](reports/vocadito-v3-boundary-experiment.json) |
| One-period RMS pass with spectral support for short low repeats | Shallow RMS valleys and spectral recovery respond to the same vibrato fluctuation; VocalSet F fell on all four figures | [report](reports/monophonic-short-reattack-experiment-2026-11-08.json) |
| Filter RMS cuts by one-period width agreement | Width agreement does not separate amplitude modulation from articulation; failed 14 of 24 comparisons | [report](reports/monophonic-width-consistency-experiment-2026-11-08.json) |
| Require recurring broad valleys around a cut | Removed all 24 true boundaries where modulation continues across a reattack | [report](reports/monophonic-temporal-cue-experiment-2026-11-08.json) |

A published learned note-transcription model with its official checkpoint and unchanged decoder defaults was also compared on the same 32 synthetic recordings holding 96 reference notes. Source, checkpoint, runtime, scoring code and inputs were hashed before predictions, and every returned event was scored.

| Control family | Reference notes | Default transcription note TP / FP | Learned model note TP / FP |
| --- | ---: | ---: | ---: |
| Held AM24/32 | 8 | 8 / 115 | 8 / 12 |
| Ordinary repeated notes | 64 | 64 / 0 | 53 / 13 |
| Short 110 Hz repeats | 16 | 4 / 0 | 8 / 4 |
| Reattack during continuing AM | 8 | 8 / 114 | 4 / 4 |

The model reduces modulation oversegmentation and loses true notes in ordinary repeats and continuing modulation. Its nominal 127.70 ms minimum note length exceeds the 125 ms targets; at the 1.0 s reattack its onset activity peaks near 0.154, under the fixed 0.5 threshold, while note activity stays above 0.621. It was not integrated. These synthetic results establish no vocal accuracy, and training-data overlap with recorded evaluation corpora was not audited.

The 12 VocalSet confirmation recordings selected for the reattack and width experiments remain unpredicted.

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

## Timed-key and interval-overlap evaluation

`prepare_timed_keys.py` reads the performance-aligned local-key CSV files from the [Schubert Winterreise Dataset v2.1](https://zenodo.org/records/10839767). Point it at an extracted dataset root containing HU33 WAV files and the three `ann_audio_localkey-ann*` folders. It preserves annotation gaps and hashes each audio and annotation file. HU33 audio is marked public domain in the archive; annotations require CC BY 3.0 attribution. The first HU33 work includes an artificial repeat supplied by the dataset authors.

```sh
python3 tools/eval/prepare_timed_keys.py \
    --source /path/to/Schubert_Winterreise_Dataset_v2-1 \
    --output /tmp/timed-key-references.json
python3 tools/eval/evaluate_timed_keys.py \
    --references /tmp/timed-key-references.json \
    --predictions /tmp/timed-key-predictions.json \
    --output /tmp/timed-key-report.json
```

Predictions contain a `method_id` and a `records` array. Each record supplies the reference recording `id` and an `intervals` array of `{start, end, root, mode}` objects: seconds, pitch class 0–11, and `major` or `minor`. Every recording must appear exactly once. Missing predicted intervals count as incorrect; reference gaps are unscored. Overlaps and nonfinite times fail validation.

The report scores every annotation variant separately and also reports the common intervals where all three agree. These are related expert revisions, with earlier labels known to later annotators; they are not independent blind annotations ([dataset paper, section 4.3](https://doi.org/10.1145/3429743)). Exact-key accuracy and the MIREX 2017+ weighted score each carry their scored seconds. Duration-pooled scores and recording means are separate. These recordings share a performer and composer; they are external-domain controls and cannot establish artist-disjoint pop accuracy.

For the fixed temporal diagnostic, configure the default native build and build the optional exporter explicitly with `cmake --build build --target sonare_eval_key_windows`. It prints one recording's production chroma observations as JSON when called with an audio path. Wrap exports as `{"exports": [{"id": "recording-id", "export": {}}]}`, placing the printed object under `export`, then run:

```sh
python3 tools/eval/infer_temporal_keys.py \
    --exports /tmp/key-window-exports.json \
    --output-prefix /tmp/key-diagnostic
```

This creates whole-recording, independent-window, and fixed-HMM predictions from the same full-recording chroma and frozen profile. The [four-recording pilot](reports/timed-key-pilot-2026-11-08.json) includes every analyst comparison and the regressions. The diagnostic does not change the library's defaults. Freeze the recording list, references, parameters, tool sources, and binary hashes before generating predictions.

Chord observations marked `chord_overlap_v2` integrate matching interval durations. Historical `legacy_midpoint_v1` observations use a different measurement and cannot be mixed into the same aggregate. The summary retains recording means and reports pooled correct/scored seconds separately; a zero scored duration is unmeasured. Root, quality, and exact-quality diagnostics use known-chord scope, and bass uses explicit bass references. Exact-quality does not test bass identity.

[Billboard](https://ddmal.ca/research/The_McGill_Billboard_Project_%28Chord_Analysis_Dataset%29/) supplies tonic annotations without major/minor mode. Treat it as tonic/chord evidence, preserve recording duplicates as one evaluation group, and confirm audio edition and timing before scoring. Previously fitted recordings and connected artists belong to development data.

### Source conversion and finer temporal observations

`audit_key_transpositions.py` checks each selected work against the published score and performance global keys. Every ordered score/audio local-key label must use the same transposition and retain its mode. Missing inputs, unequal row counts, or a failure in any variant quarantine the whole work. The tool reads annotations only, writes a qualification report, and returns nonzero if any selected work fails. It does not repair labels or prove that the musical analysis or every time boundary is correct.

```sh
python3 tools/eval/audit_key_transpositions.py \
    --source /path/to/Schubert_Winterreise_Dataset_v2-1 \
    --performance HU33 \
    --works Schubert_D911-05 Schubert_D911-07 \
    --output /tmp/key-source-qualification.json
```

For the one-second diagnostic, call `build/bin/sonare_eval_key_windows --fine-windows AUDIO`. The existing eight-second observations remain in `windows`; `fine_windows` adds one-second observations from the same tuned chroma and fixed profile. Fine pooling integrates a piecewise-constant feature holding interval between frame timestamps. This convention describes feature pooling, not the temporal support of the CQT kernel.

```sh
python3 tools/eval/infer_continuous_keys.py \
    --exports /tmp/key-window-exports.json \
    --output-prefix /tmp/continuous-key
python3 tools/eval/score_key_regions.py \
    --references /tmp/timed-key-references.json \
    --predictions /tmp/continuous-key-hmm_ct_1s.json \
    --output /tmp/key-region-report.json
```

The continuous-time transition reproduces the eight-second prior at eight seconds; elapsed time between adjacent cell centers sets each transition. Emission evidence retains the original eight-second rate. Region scoring uses fixed ±4-second bands around touching, different-key reference intervals; annotation gaps and repeated same-key splits do not create changes. Boundary and interior numerators and denominators add back to the full score.

The [second pilot](reports/timed-key-round2-2026-11-08.json) checked eight new works, quarantined one work's inconsistent source conversion before inference, and scored the remaining seven. Its one-second candidate failed the predeclared improvement criteria: full-work means rose slightly, but boundary accuracy and one annotation variant's mean fell. It remains an evaluation-only experiment. Related annotation variants and multiple performances of the same work must not be counted as independent works.
