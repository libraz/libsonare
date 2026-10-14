"""Regression tests for the cross-language public-contract checker.

The C surface contains several adjacent anonymous structs.  The checker must
bind each field scan to the typedef name that follows that struct, otherwise a
field present in a preceding declaration can hide a missing field in the
target declaration.
"""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKER_PATH = ROOT / "tools" / "conformance" / "check_public_contracts.py"
SPEC = importlib.util.spec_from_file_location("check_public_contracts", CHECKER_PATH)
assert SPEC is not None and SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


class CTypedefBlockTest(unittest.TestCase):
    def _fields(self, source: str, target: str, fields: list[str]) -> list[str]:
        errors: list[str] = []
        body = CHECKER._c_typedef_block(source, target, f"C {target}", errors)
        CHECKER._require_fields(f"C {target}", body, fields, errors)
        return errors

    def test_target_fields_are_scoped_to_the_named_typedef(self) -> None:
        source = """
typedef struct {
  int n_frames;
  int n_mels;
} SonareStreamFramesU8;

typedef struct {
  int n_frames;
} SonareStreamFrames;
"""
        self.assertEqual(
            self._fields(source, "SonareStreamFrames", ["n_frames", "n_mels"]),
            ["C SonareStreamFrames: missing field n_mels"],
        )

    def test_a_complete_target_after_a_similar_struct_passes(self) -> None:
        source = """
typedef struct {
  int n_frames;
  int n_mels;
} SonareStreamFramesU8;

typedef struct {
  int n_frames;
  int n_mels;
} SonareStreamFrames;
"""
        self.assertEqual(self._fields(source, "SonareStreamFrames", ["n_frames", "n_mels"]), [])

    def test_tagged_struct_typedef_is_bound_to_its_alias(self) -> None:
        source = "typedef struct SonareFrameTag { int n_frames; } SonareStreamFrames;\n"
        self.assertEqual(self._fields(source, "SonareStreamFrames", ["n_frames"]), [])

    def test_an_incomplete_previous_typedef_cannot_supply_target_fields(self) -> None:
        source = """
typedef struct {
  int n_mels;
typedef struct {
  int n_frames;
} SonareStreamFrames;
"""
        self.assertEqual(
            self._fields(source, "SonareStreamFrames", ["n_frames", "n_mels"]),
            ["C SonareStreamFrames: missing field n_mels"],
        )

    def test_an_extra_closing_brace_is_not_a_target_boundary(self) -> None:
        source = "typedef struct { int n_mels; }} SonareStreamFrames;\n"
        self.assertEqual(
            self._fields(source, "SonareStreamFrames", ["n_frames"]),
            [
                "C SonareStreamFrames: declaration not found",
                "C SonareStreamFrames: missing field n_frames",
            ],
        )

    def test_a_nested_target_struct_fails_closed(self) -> None:
        source = "typedef struct { struct { int n_frames; } nested; } SonareStreamFrames;\n"
        self.assertEqual(
            self._fields(source, "SonareStreamFrames", ["n_frames"]),
            [
                "C SonareStreamFrames: declaration not found",
                "C SonareStreamFrames: missing field n_frames",
            ],
        )

    def test_missing_target_reports_a_declaration_error(self) -> None:
        errors: list[str] = []
        body = CHECKER._c_typedef_block(
            "typedef struct { int n_frames; } OtherFrames;\n",
            "SonareStreamFrames",
            "C SonareStreamFrames",
            errors,
        )
        self.assertEqual(body, "")
        self.assertEqual(errors, ["C SonareStreamFrames: declaration not found"])


class ShippingTreeTest(unittest.TestCase):
    def test_current_public_contracts_pass(self) -> None:
        self.assertEqual(CHECKER.main(), 0)


if __name__ == "__main__":
    unittest.main()
