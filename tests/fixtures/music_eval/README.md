# Music evaluation fixtures

The six manifests in this directory gate the analyzers' accuracy against real recordings. The manifests are tracked; the audio and its annotations are not, because the corpora are licensed datasets that cannot be redistributed here. Every manifest therefore ships empty, and the tests that read them skip cleanly until you add rows and the audio they point at.

Until a manifest has rows, the dimension it covers is unmeasured — not merely ungated. A change to key detection, beat tracking or chord recognition currently has nothing to fail against.

## What each manifest gates

| manifest | analyzer | pass condition |
| --- | --- | --- |
| `key_manifest.tsv` | `KeyAnalyzer` | detected key matches one of the expected keys |
| `bpm_manifest.tsv` | tempo estimation | detected BPM within a relative tolerance |
| `beat_manifest.tsv` | beat tracking | F-measure against a beat annotation |
| `downbeat_manifest.tsv` | downbeat tracking | F-measure against a downbeat annotation |
| `chord_manifest.tsv` | chord recognition | weighted chord symbol recall against an annotation |
| `meter_manifest.tsv` | meter estimation | numerator, denominator and a confidence floor |

## Where the audio goes

Put audio and annotations under this directory. These paths are gitignored, so anything you add here stays local:

```
tests/fixtures/music_eval/audio/            # and any audio_* sibling
tests/fixtures/music_eval/annotation/       # and annotations/
tests/fixtures/music_eval/FSL10K/
tests/fixtures/music_eval/slakh/            # and Slakh*/, babyslakh*/
tests/fixtures/music_eval/guitarset_annotations/
```

Set `SONARE_MUSIC_FIXTURE_ROOT` to point somewhere else if you keep a shared corpus outside the checkout. The `file` and `annotation` columns are resolved relative to that root, or to this directory when it is unset.

## Row format

Tab-separated. Lines starting with `#` and blank lines are ignored, and a row whose audio file is missing is skipped rather than failed, so a partial corpus is fine. Each manifest's first comment line is its column header.

Key rows take `dataset`, `file`, `expected_key`, `expected_mode`, then any of these optional tokens in any order:

```
modes=major-minor|all|major,minor,dorian
profile=ks|temperley|shaath|edma
genre_hint=auto|edm|pop|classical|jazz
alt_key=C:maj|A:min
report_only
```

`alt_key` accepts further acceptable answers, separated by `|` or `,`, for material with a defensible relative or parallel reading. `report_only` turns the row into a warning instead of a failure — use it while adding material whose expected answer you are still confirming, so it reports a measurement without gating the build. Trailing columns that match none of these are ignored, so a free-text note at the end of a row is safe.

A minimal key row:

```
mydataset	audio/track01.wav	C	major
```

## Running it

The fixture tests are behind a build option, so a plain `make test` does not include them:

```
make test-optional-fixtures
```

## Confirming the rows were picked up

A skip and a pass look alike in a summary line, and a skip is what you get from a typo in a path. Check the reason:

```
ctest --test-dir build-optional-fixtures -R optional --output-on-failure -V | grep -i skip
```

- `No key fixture rows are configured` — the manifest has no non-comment rows.
- `No key audio fixtures are present` — rows exist, but none of their `file` paths resolve. This is the one that looks like success in a summary; it means the manifest and the audio disagree about where the files are.
- Neither message, and the case passes — the rows ran.
