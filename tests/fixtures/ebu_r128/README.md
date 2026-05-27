# EBU R128 optional fixtures

This directory defines the local fixture contract for EBU R128 / ITU-R
BS.1770 loudness compliance checks. Audio fixtures are not committed to this
repository; place them in this directory or point `SONARE_EBU_R128_FIXTURE_ROOT`
at a local/cache directory with the same manifest layout.

## Sources

- ITU-R BS.1770-5 (11/2023): Algorithms to measure audio programme loudness and
  true-peak audio level.
- EBU Tech 3341 v4.0 (2023): EBU Mode loudness metering requirements and test
  signals.
- EBU Tech 3342 v4.0 (2023): Loudness Range (LRA).
- EBU Loudness test set v5.0: 70 audio files for Tech 3341 / Tech 3342
  compliance and related loudness checks.

Keep source/licence notes in this README or in local cache documentation. Do
not add source URLs, licence text, rights labels, or other provenance metadata
as optional TSV columns; `tests/fixtures/audit_manifests.py` intentionally
rejects those tokens in optional manifest fields.

## Manifest schema

`manifest.tsv` is tab-separated. Comment lines start with `#`.

Columns:

1. `filename`: audio path relative to `SONARE_EBU_R128_FIXTURE_ROOT`
2. `integrated_lufs`: expected integrated loudness in LUFS
3. `lra`: expected Loudness Range in LU, or `-` when not applicable
4. `max_true_peak_db`: expected maximum true peak in dBTP, or `-` when not
   applicable
5. `tolerance_lu`: tolerance for LUFS/LRA checks
6. `tolerance_db`: tolerance for true-peak checks

Current minimum rows cover the Tech 3341 integrated loudness requirement
signals that this repository already names:

```text
seq-3341-1-16bit.wav    -23.0    -    -    0.1    0.1
```

Add LRA and true-peak vectors as separate manifest rows when the corresponding
local audio files are available. Use `-` for metrics that a row does not
validate.

## Local and CI modes

Local optional mode:

```sh
cmake -S . -B build-fixtures \
  -DSONARE_ENABLE_OPTIONAL_FIXTURE_TESTS=ON
cmake --build build-fixtures --target sonare_tests -j 8

SONARE_EBU_R128_FIXTURE_ROOT=tests/fixtures/ebu_r128 \
  ./build-fixtures/bin/sonare_tests "[mastering][ebu_r128]"
```

The fixture test source is compiled only when
`SONARE_ENABLE_OPTIONAL_FIXTURE_TESTS=ON`. The test is skipped when no
referenced audio files exist. This keeps clean source checkouts lightweight.

Report/audit mode:

```sh
python3 tests/fixtures/audit_manifests.py \
  --ebu-root tests/fixtures/ebu_r128

python3 tests/fixtures/run_optional_fixture_report.py \
  --suite ebu \
  --ebu-root tests/fixtures/ebu_r128 \
  --sonare-tests build-fixtures/bin/sonare_tests \
  --output build-fixtures/ebu_fixture_report.json
```

Mandatory CI mode adds `--require-ready --require-complete` to the report
command after the fixture cache/artifact has been restored.

Mandatory CI promotion should set `SONARE_EBU_R128_FIXTURE_ROOT` to a restored
cache/artifact directory, run `run_optional_fixture_report.py --suite ebu
--require-ready --require-complete`, and fail if any manifest row is missing
audio or if the test suite skips.

Promotion checklist for the mandatory CI gate:

- Fixture cache/artifact is restored before `sonare_tests` runs.
- `run_optional_fixture_report.py --suite ebu --require-ready
  --require-complete` reports `ready > 0`, `missing_fixtures == 0`, and
  `provenance_violations == 0`.
- Tech 3341 integrated loudness rows pass using the per-row `tolerance_lu`.
- Tech 3342 LRA rows are present before LRA is advertised as a compliance gate.
- True-peak rows are present before `max_true_peak_db()` is advertised as a
  compliance gate; use the per-row `tolerance_db`, normally no wider than
  0.2 dB unless the source document requires otherwise.
