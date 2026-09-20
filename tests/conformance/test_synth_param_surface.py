"""The patch/automation comparison, checked against the drift it was written for.

A checker that reads two files and compares them passes for two reasons: the
files agree, or it stopped reading one of them. These cases reconstruct each
half of the drift that was actually in the tree and require the comparison to
name it, so a parse that quietly matches nothing cannot be mistaken for a
clean result.
"""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

# By path rather than by name: this target runs from the repository root, where
# a bare import of a sibling resolves only if the caller happened to be standing
# in this directory.
_SPEC = importlib.util.spec_from_file_location(
    "check_synth_param_surface",
    Path(__file__).resolve().parent / "check_synth_param_surface.py",
)
assert _SPEC and _SPEC.loader
checker = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(checker)

HEADER = Path(checker.HEADER).read_text()
TABLE = Path(checker.TABLE).read_text()


class SynthParamSurfaceTest(unittest.TestCase):
    def test_the_parse_reaches_both_files(self) -> None:
        fields = checker.patch_float_fields(HEADER)
        names = checker.table_names(TABLE)
        self.assertGreaterEqual(len(fields), checker.MIN_PATCH_FLOATS)
        self.assertGreaterEqual(len(names), checker.MIN_TABLE_ENTRIES)
        # Named anchors, so a parse that collects the wrong lines and still
        # clears the floor is caught.
        self.assertIn("cutoff_hz", fields)
        self.assertIn("pitch_offset_cents", fields)
        self.assertIn("cutoffHz", names)
        self.assertIn("pitchOffsetCents", names)

    def test_the_engine_block_is_recognised_by_its_section(self) -> None:
        fields = checker.patch_float_fields(HEADER)
        self.assertTrue(fields["sample_level"].startswith("sample engine"))
        self.assertTrue(fields["sample_start_offset"].startswith("sample engine"))
        # And the wrapper tail blocks are NOT engine sections, which is the
        # distinction the whole comparison rests on.
        for field in ("hp_cutoff_hz", "sample_hold_hz", "bit_depth", "pitch_offset_cents"):
            self.assertFalse(
                any(fields[field].startswith(s) for s in checker.ENGINE_SECTIONS),
                f"{field} was classified as an engine block",
            )

    def test_a_wrapper_field_missing_from_the_table_is_reported(self) -> None:
        # The real drift: the highpass reached the patch at struct_version 4 and
        # never reached the table.
        without = TABLE.replace('{"hpCutoffHz", NativeSynthParamId::kHpCutoffHz},\n', "")
        self.assertNotEqual(without, TABLE)
        names = checker.table_names(without)
        self.assertNotIn("hpCutoffHz", names)
        self.assertIn("sampleHoldHz", names)

    def test_a_table_name_that_is_no_patch_field_is_reported(self) -> None:
        # The other half: a name automatable everywhere and a field nowhere.
        without = HEADER.replace("  float pitch_offset_cents;\n", "")
        self.assertNotEqual(without, HEADER)
        fields = checker.patch_float_fields(without)
        self.assertNotIn("pitch_offset_cents", fields)
        self.assertIn("pitchOffsetCents", checker.table_names(TABLE))

    def test_the_struct_body_is_bounded_at_its_own_typedef(self) -> None:
        # A regex over the whole header would sweep in every other struct's
        # floats and make the comparison meaningless in the permissive
        # direction, which no assertion above would catch.
        fields = checker.patch_float_fields(HEADER)
        self.assertNotIn("rt60", fields)
        self.assertNotIn("integrated_lufs", fields)

    def test_the_names_are_spelled_the_way_the_bindings_spell_them(self) -> None:
        self.assertEqual(checker.camel("pitch_offset_cents"), "pitchOffsetCents")
        self.assertEqual(checker.camel("hp_cutoff_hz"), "hpCutoffHz")
        self.assertEqual(checker.camel("gain"), "gain")


if __name__ == "__main__":
    unittest.main()
