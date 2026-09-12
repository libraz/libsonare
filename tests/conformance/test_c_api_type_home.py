#!/usr/bin/env python3
"""Stdlib self-tests for the C-ABI type-placement checker.

The dangerous direction here is over-reporting.  Most public types are defined
and used in the same header, so a check that merely listed "types a header
defines" would look identical to a working one on a clean tree and would light
up the moment anyone read its output.  The negative population is therefore
pinned first and on the real headers: 126 of the 146 typedefs in surface headers
are defined and used in the same file, and none of them may be reported.

The positive direction is pinned on a copy of the real headers by relocating the
one type this check was calibrated against back where it used to live, and on
hand-written headers for each rule in isolation -- including the two exclusions
(a doc-comment mention, a ``static_assert`` mention) that each on their own would
have hidden the calibration site.

Every tree break is made on a ``shutil.copytree``.  Nothing is written into
``include/sonare`` at any point.
"""

from __future__ import annotations

import importlib.util
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKER_PATH = ROOT / "tests" / "conformance" / "check_c_api_type_home.py"
SPEC = importlib.util.spec_from_file_location("libsonare_c_api_type_home_checker", CHECKER_PATH)
assert SPEC is not None and SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CHECKER
SPEC.loader.exec_module(CHECKER)

HEADER_DIR = ROOT / "include" / "sonare"

# The calibration site: the block that used to sit in the effects header.
CALIBRATION_TYPE = "SonareRealtimeVoiceChangerConfig"
CALIBRATION_HOME = "sonare_c_voice_changer.h"
CALIBRATION_FORMER_HOME = "sonare_c_effects.h"
BLOCK_FIRST_LINE = "/// @brief Flat POD mirror of"
BLOCK_LAST_LINE = "uint32_t sonare_voice_changer_abi_version(void);"
FORMER_HOME_ANCHOR = '#include "sonare_c_voice_changer.h"'

# Floors, not targets.  Raise only alongside a real widening of the scan.
MIN_TYPEDEFS = 150
MIN_SAME_HEADER_TYPES = 100


def _audit(headers: dict[str, str]):
    """Run the checker over a hand-written set of headers."""
    with tempfile.TemporaryDirectory() as tmp:
        header_dir = Path(tmp) / "sonare"
        header_dir.mkdir(parents=True)
        for name, text in headers.items():
            (header_dir / name).write_text(text, encoding="utf-8")
        return CHECKER.audit(header_dir)


SURFACE = "SonareError sonare_take(const SonareThing* thing);\n"


class ClassificationTest(unittest.TestCase):
    def test_a_type_defined_and_used_in_the_same_header_is_never_reported(self) -> None:
        report = _audit(
            {
                "home.h": "typedef struct { int x; } SonareThing;\n" + SURFACE,
                "other.h": "SonareError sonare_also(const SonareThing* thing);\n",
            }
        )
        self.assertEqual(report.findings, [])

    def test_a_type_only_a_sibling_takes_is_reported(self) -> None:
        report = _audit(
            {
                "home.h": "typedef struct { int x; } SonareThing;\n"
                "SonareError sonare_unrelated(int n);\n",
                "other.h": SURFACE,
            }
        )
        self.assertEqual([f.type_name for f in report.findings], ["SonareThing"])
        self.assertEqual(report.findings[0].header, "home.h")
        self.assertEqual(report.findings[0].used_by, ["other.h"])

    def test_a_doc_comment_mention_is_not_a_use(self) -> None:
        report = _audit(
            {
                "home.h": "typedef struct { int x; } SonareThing;\n"
                "/// @brief Unrelated. See @ref SonareThing for the layout.\n"
                "SonareError sonare_unrelated(int n);\n",
                "other.h": SURFACE,
            }
        )
        self.assertEqual([f.type_name for f in report.findings], ["SonareThing"])

    def test_a_static_assert_mention_is_not_a_use(self) -> None:
        report = _audit(
            {
                "home.h": "typedef struct { int x; } SonareThing;\n"
                "#ifdef __cplusplus\n"
                'static_assert(sizeof(SonareThing) == 4u, "layout drift");\n'
                "#endif\n"
                "SonareError sonare_unrelated(int n);\n",
                "other.h": SURFACE,
            }
        )
        self.assertEqual([f.type_name for f in report.findings], ["SonareThing"])

    def test_a_header_that_declares_no_functions_is_exempt(self) -> None:
        # Defining types for siblings to take is what a types header is for, so
        # it cannot be evidence against one.
        report = _audit(
            {
                "types.h": "typedef struct { int x; } SonareThing;\n",
                "other.h": SURFACE,
            }
        )
        self.assertEqual(report.findings, [])
        self.assertEqual(report.types_headers, 1)
        self.assertEqual(report.surface_headers, 1)

    def test_a_type_no_sibling_takes_is_not_a_placement_question(self) -> None:
        report = _audit({"home.h": "typedef struct { int x; } SonareThing;\n" + SURFACE.replace("SonareThing", "int")})
        self.assertEqual(report.findings, [])

    def test_a_nested_struct_body_does_not_produce_a_member_name_as_a_type(self) -> None:
        # A pattern that models one level of nesting stops early inside a deeper
        # struct and reads `k` (a member) as the type's name.
        report = _audit(
            {
                "home.h": "typedef struct {\n"
                "  struct { struct { int k; } inner; } outer;\n"
                "} SonareThing;\n" + SURFACE,
            }
        )
        self.assertEqual(report.findings, [])
        names = {
            d.name
            for d in CHECKER.parse_definitions(
                "home.h",
                "typedef struct {\n  struct { struct { int k; } inner; } outer;\n} SonareThing;\n",
            )
        }
        self.assertEqual(names, {"SonareThing"})


class TreeTest(unittest.TestCase):
    """The scan against the shipped public headers."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.report = CHECKER.audit(HEADER_DIR)
        cls.code_of = {
            path.name: CHECKER.strip_assertions(
                CHECKER.strip_comments_and_literals(path.read_text(encoding="utf-8"))
            )
            for path in sorted(HEADER_DIR.glob("*.h"))
        }

    def test_every_type_is_defined_where_its_declarations_are(self) -> None:
        self.assertEqual([f.as_line() for f in self.report.findings], [])

    def test_the_scan_resolves_a_nonzero_population(self) -> None:
        self.assertGreaterEqual(self.report.typedefs, MIN_TYPEDEFS)
        self.assertGreater(self.report.surface_headers, 0)
        self.assertGreater(self.report.types_headers, 0)

    def test_the_population_it_must_stay_off_is_large(self) -> None:
        # The check would look the same on a clean tree whether or not it
        # excluded same-header types, so the size of that excluded set is
        # asserted rather than assumed.
        surface = {
            name for name, code in self.code_of.items() if CHECKER.declares_functions(code) > 0
        }
        same_header = 0
        for name in surface:
            code = self.code_of[name]
            definitions = CHECKER.parse_definitions(name, code)
            for definition in definitions:
                spans = [(d.start, d.end) for d in definitions if d.name == definition.name]
                if CHECKER._mentioned_outside(code, definition.name, spans):
                    same_header += 1
        self.assertGreaterEqual(same_header, MIN_SAME_HEADER_TYPES)

    def test_the_calibration_type_lives_with_its_declarations(self) -> None:
        home = self.code_of[CALIBRATION_HOME]
        self.assertIn(f"}} {CALIBRATION_TYPE};", home)
        self.assertNotIn(CALIBRATION_TYPE, self.code_of[CALIBRATION_FORMER_HOME])

    def test_moving_the_calibration_type_back_names_exactly_its_former_home(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "sonare"
            shutil.copytree(HEADER_DIR, copy)
            home = copy / CALIBRATION_HOME
            lines = home.read_text(encoding="utf-8").split("\n")
            start = next(i for i, line in enumerate(lines) if line.startswith(BLOCK_FIRST_LINE))
            end = next(i for i in range(start, len(lines)) if lines[i] == BLOCK_LAST_LINE)
            block = lines[start : end + 1]
            home.write_text("\n".join(lines[:start] + lines[end + 1 :]), encoding="utf-8")

            former = copy / CALIBRATION_FORMER_HOME
            former_lines = former.read_text(encoding="utf-8").split("\n")
            anchor = former_lines.index(FORMER_HOME_ANCHOR)
            former.write_text(
                "\n".join(former_lines[:anchor] + block + [""] + former_lines[anchor:]),
                encoding="utf-8",
            )

            moved_back = CHECKER.audit(copy)

        self.assertEqual(
            [(f.header, f.type_name) for f in moved_back.findings],
            [(CALIBRATION_FORMER_HOME, CALIBRATION_TYPE)],
        )

    def test_the_floor_rejects_an_empty_header_directory(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            empty = Path(tmp) / "sonare"
            empty.mkdir(parents=True)
            result = subprocess.run(
                [
                    sys.executable,
                    str(CHECKER_PATH),
                    "--header-dir",
                    str(empty),
                    "--floor",
                    str(MIN_TYPEDEFS),
                ],
                capture_output=True,
                text=True,
            )
        self.assertEqual(result.returncode, 2)
        self.assertIn("below the floor", result.stderr)


if __name__ == "__main__":
    unittest.main()
