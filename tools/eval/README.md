# Measuring musical accuracy

`libsonare` matches librosa where the two overlap, and that agreement is checked in CI. It is not an accuracy claim. Matching librosa means the two compute the same thing; whether the thing they compute names the right key, or finds the right chord, is a separate question that only annotated recordings can answer.

This directory holds the tooling for asking that question, and for turning the answer into figures a docs page can carry. Nothing here ships a corpus — the datasets below are licensed for research use and cannot be redistributed — so the numbers are produced locally against a corpus you hold, and only the numbers are published.

## The three pieces

| piece | what it is |
| --- | --- |
| `tests/fixtures/music_eval/*.tsv` | manifests naming the audio, the annotation and the expected answer |
| `tests/fixtures/run_optional_fixture_report.py` | runs the fixture tests and writes one observation per fixture per metric |
| `tools/eval/summarize_accuracy.py` | rolls those observations into per-dataset aggregates and a markdown table |

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
- **chord** — weighted chord symbol recall over a major/minor vocabulary, the exact-quality WCSR beside it, and root, quality and bass accuracy separately. The gap between the maj/min and exact figures is the vocabulary's contribution.

Every figure carries its fixture count and is broken down by dataset. A mean over a mixed corpus says nothing without knowing what went into it.

## Two things the tool refuses to do

**It does not score an empty set.** A dimension with no observations reports `unmeasured`, not 0% and not 100%. A metric that skips unusable data points and averages the rest scores an empty set as perfect, and that failure mode has already been found once in this repo.

**It does not hide a partial corpus.** The fixture runner skips a row whose audio is missing rather than failing it, so a typo in a path looks exactly like a pass in a summary line. `--require <dimension>` makes the summarizer exit non-zero when a dimension it was told to expect produced nothing, which is what a publishing pipeline should use.

## Publishing

Publish the markdown table together with: the corpus name and version, how many tracks of it were used, the library version, and the configuration each dimension ran under. A number without those cannot be reproduced or compared, and an accuracy figure that cannot be compared is decoration.
