"""Stdlib self-tests for the C-ABI header self-containment checker.

The checker can fail in two directions and each is pinned separately.

It can miss: a header that needs a sibling's type compiles anyway because the
probe pulled the sibling in some other way, and the scan certifies a header that
is not self-contained.  The synthetic cases below fix the positive direction on
hand-written headers, including one that breaks only in C++, so the second
language mode is shown to carry its own weight.

It can over-report: a scan that reacts to include lines rather than to
compilation would flag every redundant include in the tree and turn the guard
into a style checker.  So the negative direction is pinned too -- removing an
include nothing needs must leave the report unchanged -- on a synthetic header
and again on a copy of the real tree.

Both tree breaks are made on a ``shutil.copytree`` of the headers, never on the
shared tree: a deliberately broken public header sitting in the working tree,
however briefly, is something a parallel session can compile.
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
CHECKER_PATH = ROOT / "tests" / "conformance" / "check_c_api_header_self_contained.py"
SPEC = importlib.util.spec_from_file_location(
    "libsonare_c_api_header_self_contained_checker", CHECKER_PATH
)
assert SPEC is not None and SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CHECKER
SPEC.loader.exec_module(CHECKER)

HEADER_DIR = ROOT / "include" / "sonare"

# A load-bearing include: stripping it must name exactly its own header. Pinned
# by measurement, not by eye -- removing it has to be the thing that breaks the
# header, so the pair is re-derived whenever the include graph moves.
BREAK_HEADER = "sonare_c_voice_changer.h"
BREAK_INCLUDE = '#include "sonare_c_types.h"\n'

# A redundant include: the same header compiles without it, because a sibling
# already supplies the same declarations.  Stripping it must name nothing.
REDUNDANT_HEADER = "sonare_c_engine.h"
REDUNDANT_INCLUDE = '#include "sonare_c_types.h"\n'

# A floor, not a target.  Raise only alongside a real widening of the scan.
MIN_HEADERS = 20


def _audit(headers: dict[str, str], languages: tuple | None = None):
    """Run the checker over a hand-written set of headers."""
    with tempfile.TemporaryDirectory() as tmp:
        header_dir = Path(tmp) / "sonare"
        header_dir.mkdir(parents=True)
        for name, text in headers.items():
            (header_dir / name).write_text(text, encoding="utf-8")
        return CHECKER.audit(header_dir, languages or CHECKER.LANGUAGES)


C_ONLY = (CHECKER.LANGUAGES[0],)
CXX_ONLY = (CHECKER.LANGUAGES[1],)


class SyntheticTest(unittest.TestCase):
    def test_a_header_using_a_sibling_type_without_including_it_is_reported(self) -> None:
        report = _audit(
            {
                "base.h": "#pragma once\ntypedef struct { int x; } SonareThing;\n",
                "user.h": "#pragma once\nvoid sonare_use(SonareThing* thing);\n",
            },
            C_ONLY,
        )
        self.assertEqual([f.header for f in report.findings], ["user.h"])
        self.assertIn("SonareThing", report.findings[0].detail)

    def test_including_the_sibling_makes_it_self_contained(self) -> None:
        report = _audit(
            {
                "base.h": "#pragma once\ntypedef struct { int x; } SonareThing;\n",
                "user.h": '#pragma once\n#include "base.h"\nvoid sonare_use(SonareThing* thing);\n',
            },
            C_ONLY,
        )
        self.assertEqual(report.findings, [])
        self.assertEqual(report.headers, 2)

    def test_an_include_nothing_needs_is_not_a_finding(self) -> None:
        # The guard must not degrade into an include-line detector: a header
        # that includes more than it uses still compiles, so it stays clean.
        report = _audit(
            {
                "base.h": "#pragma once\ntypedef struct { int x; } SonareThing;\n",
                "user.h": '#pragma once\n#include "base.h"\nvoid sonare_use(int n);\n',
            },
            C_ONLY,
        )
        self.assertEqual(report.findings, [])

    def test_removing_an_include_nothing_needs_leaves_the_report_unchanged(self) -> None:
        without = _audit(
            {
                "base.h": "#pragma once\ntypedef struct { int x; } SonareThing;\n",
                "user.h": "#pragma once\nvoid sonare_use(int n);\n",
            },
            C_ONLY,
        )
        self.assertEqual(without.findings, [])

    def test_a_break_only_the_cxx_mode_sees_is_reported(self) -> None:
        # The layout static_asserts live behind `#ifdef __cplusplus`, so a
        # C-only scan would pass over them; this pins that the second mode is
        # not along for the ride.
        source = (
            "#pragma once\n"
            "typedef struct { int x; } SonareThing;\n"
            "#ifdef __cplusplus\n"
            'static_assert(sizeof(SonareThing) == 99u, "layout drift");\n'
            "#endif\n"
        )
        self.assertEqual(_audit({"thing.h": source}, C_ONLY).findings, [])
        cxx = _audit({"thing.h": source}, CXX_ONLY)
        self.assertEqual([f.header for f in cxx.findings], ["thing.h"])
        self.assertEqual(cxx.findings[0].language, "c++")

    def test_both_languages_are_compiled_for_every_header(self) -> None:
        report = _audit({"a.h": "#pragma once\n", "b.h": "#pragma once\n"})
        self.assertEqual(report.headers, 2)
        self.assertEqual(report.compiles, 4)


class TreeTest(unittest.TestCase):
    """The scan against the shipped public headers."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.report = CHECKER.audit(HEADER_DIR)

    def test_every_public_header_compiles_alone(self) -> None:
        self.assertEqual([f.as_line() for f in self.report.findings], [])

    def test_the_scan_compiles_a_nonzero_population(self) -> None:
        self.assertGreaterEqual(self.report.headers, MIN_HEADERS)
        self.assertEqual(self.report.compiles, self.report.headers * len(CHECKER.LANGUAGES))

    def test_stripping_the_load_bearing_include_names_exactly_that_header(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "sonare"
            shutil.copytree(HEADER_DIR, copy)
            path = copy / BREAK_HEADER
            text = path.read_text(encoding="utf-8")
            self.assertIn(BREAK_INCLUDE, text)
            path.write_text(text.replace(BREAK_INCLUDE, "", 1), encoding="utf-8")
            broken = CHECKER.audit(copy)
        self.assertEqual(sorted({f.header for f in broken.findings}), [BREAK_HEADER])

    def test_stripping_a_redundant_include_names_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "sonare"
            shutil.copytree(HEADER_DIR, copy)
            path = copy / REDUNDANT_HEADER
            text = path.read_text(encoding="utf-8")
            self.assertIn(REDUNDANT_INCLUDE, text)
            path.write_text(text.replace(REDUNDANT_INCLUDE, "", 1), encoding="utf-8")
            thinned = CHECKER.audit(copy)
        self.assertEqual([f.as_line() for f in thinned.findings], [])

    def test_an_absent_compiler_is_a_dead_scan_not_23_findings(self) -> None:
        absent = ("c", "sonare-no-such-compiler", "-std=c11", "probe.c", "int main(void){return 0;}")
        with self.assertRaises(CHECKER.CompilerMissing):
            CHECKER.audit(HEADER_DIR, (absent,))

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
                    str(MIN_HEADERS),
                ],
                capture_output=True,
                check=False,
                text=True,
            )
        self.assertEqual(result.returncode, 2)
        self.assertIn("below the floor", result.stderr)


if __name__ == "__main__":
    unittest.main()
