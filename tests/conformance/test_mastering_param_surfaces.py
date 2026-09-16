"""Self-tests for the mastering parameter surface check.

The check reports a clean repository, so its own power is the thing that has to
be established: three earlier drafts of this matcher returned clean while blind
to part of the population -- one matched a leaf anywhere in the file, so a
generic leaf like `mode` resolved against an unrelated block; one could not
follow a property declared as a named type; one could not follow a union, which
is how the repaired blocks are spelled. Each of those reported zero findings.

So the cases below drive the matcher with inputs whose answer is known, rather
than only asserting that the repository passes.
"""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKER_PATH = ROOT / "tests" / "conformance" / "check_mastering_param_surfaces.py"
SPEC = importlib.util.spec_from_file_location(
    "libsonare_mastering_param_checker", CHECKER_PATH
)
assert SPEC is not None and SPEC.loader is not None
check = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(check)


class MasteringParamSurfaces(unittest.TestCase):
    def test_the_repository_declares_every_parameter_key_on_both_surfaces(self):
        keys = check.parameter_keys(check.PARAM_SOURCE.read_text())
        surfaces = {side: path.read_text() for side, path in check.TS_SURFACES.items()}
        missing, unreached, comparisons = check.scan(keys, surfaces)
        self.assertEqual(
            unreached, [], "some key's block was never reached on a surface"
        )
        self.assertEqual(missing, [])
        # The population is the check's own subject: a key source that stopped
        # parsing would also report no findings.
        self.assertGreater(len(keys), 50)
        self.assertEqual(comparisons, len(keys) * len(surfaces))

    def test_a_missing_leaf_is_reported_rather_than_resolved_elsewhere(self):
        """A leaf absent from its own block, present in another, must fail."""
        surface = """
        export interface Config {
          repair?: {
            denoise?: { enabled?: boolean };
            dehum?: { mode?: number };
          };
        }
        """
        missing, unreached, comparisons = check.scan(
            ["repair.denoise.mode"], {"s": surface}
        )
        self.assertEqual(unreached, [])
        self.assertEqual(comparisons, 1)
        self.assertEqual(missing, [("repair.denoise.mode", "s")])

    def test_a_union_member_and_a_named_type_are_both_followed(self):
        """The two spellings the repository actually uses for these blocks."""
        surface = """
        export interface DenoiseOptions { mode?: number }
        export interface Config {
          repair?: {
            denoise?: boolean | DenoiseOptions;
            dehum?: boolean | { mode?: number };
          };
        }
        """
        missing, unreached, _ = check.scan(
            ["repair.denoise.mode", "repair.dehum.mode"], {"s": surface}
        )
        self.assertEqual(missing, [])
        self.assertEqual(unreached, [])

    def test_an_unreachable_block_fails_rather_than_passing_vacuously(self):
        """A path with no block at all is a comparison that did not run."""
        surface = "export interface Config { repair?: { denoise?: { mode?: number } } }"
        missing, unreached, comparisons = check.scan(
            ["saturation.tape.driveDb"], {"s": surface}
        )
        self.assertEqual(missing, [])
        self.assertEqual(comparisons, 0)
        self.assertEqual(unreached, [("saturation.tape.driveDb", "s")])

    def test_the_matcher_counts_rather_than_stopping_at_the_first_finding(self):
        """Two missing leaves on one surface must both be reported."""
        surface = (
            "export interface Config { repair?: { denoise?: { enabled?: boolean } } }"
        )
        missing, _, _ = check.scan(
            ["repair.denoise.mode", "repair.denoise.noiseEstimator"], {"s": surface}
        )
        self.assertEqual(len(missing), 2)

    def test_the_key_reader_finds_the_dotted_keys_and_nothing_else(self):
        source = (
            'if (key == "eq.tilt.tiltDb") { } else if (key == "repair.dehum.mode") { }'
        )
        self.assertEqual(
            check.parameter_keys(source), ["eq.tilt.tiltDb", "repair.dehum.mode"]
        )


if __name__ == "__main__":
    unittest.main()
