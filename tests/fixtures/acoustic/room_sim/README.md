# Room-acoustics simulation fixtures

Golden fixtures for the geometric room-acoustics module (`src/acoustic/`,
`BUILD_ACOUSTIC_SIM`). Distinct from `../manifest.tsv`, which serves the
existing blind `AcousticAnalyzer`.

Contents:

- **Image-source oracle**: `ism_golden.tsv` — shoebox image-source arrival
  times (and per-image reflection) from pyroomacoustics' ISM, the independent
  cross-engine oracle (±1 sample). Generate with `gen_ism_golden.py` (requires
  `pip install pyroomacoustics numpy`); the comparison test
  (`tests/acoustic/ism_golden_test.cpp`, gated by
  `SONARE_ENABLE_OPTIONAL_FIXTURE_TESTS`) SKIPs when the TSV is absent. Normal
  CI relies on the analytic image-source tests.
- **RIR synthesis** (planned): full synthesized RIRs (image-source early +
  statistical late tail) with the designed RT60/C50 targets recorded alongside,
  for round-trip validation.
- **Room estimation** (planned): known-shoebox RIRs with ground-truth V / RT60
  for the blind estimator.

Determinism scope: goldens are baked per platform/build. Native goldens are not
imposed on WASM (float divergence); cross-platform comparison uses tolerance /
NCC rather than bit-exact equality.
