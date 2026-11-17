"""Every feature build option has a key in the capability descriptor.

The rows of src/c_api/capability_features.h are compared with the option
lists in the top-level CMakeLists.txt: each BUILD_<X> option is either a
feature gate or a build-shape option, and each feature gate reaches at least
one `features` key.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CMAKE = ROOT / "CMakeLists.txt"
ROWS = ROOT / "src" / "c_api" / "capability_features.h"

# Gates that are not a BUILD_<X> option.
_NON_OPTION_GATES = {"FFMPEG"}


def _cmake_list(text: str, name: str) -> set[str]:
    match = re.search(rf"set\(\s*{name}\s+([^)]*)\)", text)
    if match is None:
        raise AssertionError(f"{name} is not set in CMakeLists.txt")
    return set(match.group(1).split())


def build_options(text: str) -> set[str]:
    return set(re.findall(r"^\s*option\(\s*BUILD_(\w+)", text, re.MULTILINE))


def capability_rows(text: str) -> list[tuple[str, str]]:
    return re.findall(r'X\("(\w+)",\s*\w+,\s*(\w+)\)', text)


def problems(cmake_text: str, rows_text: str) -> list[str]:
    options = build_options(cmake_text)
    gates = _cmake_list(cmake_text, "SONARE_FEATURE_GATES")
    shape = _cmake_list(cmake_text, "SONARE_BUILD_SHAPE_OPTIONS")
    rows = capability_rows(rows_text)
    found: list[str] = []
    if not options or not rows:
        found.append("parsed no options or no capability rows")
    found += [f"BUILD_{o} is in neither option list" for o in sorted(options - gates - shape)]
    found += [f"{o} is listed as both a gate and a build-shape option" for o in sorted(gates & shape)]
    found += [f"{o} is listed but is not a BUILD_ option" for o in sorted((gates | shape) - options)]
    row_gates = {gate for _, gate in rows}
    found += [f"gate {g} has no features key" for g in sorted(gates - row_gates)]
    found += [
        f"features key {key} names unknown gate {gate}"
        for key, gate in rows
        if gate not in gates | _NON_OPTION_GATES
    ]
    keys = [key for key, _ in rows]
    found += [f"features key {k} appears twice" for k in sorted({k for k in keys if keys.count(k) > 1})]
    return found


class CapabilityFeatureGatesTest(unittest.TestCase):
    def test_every_feature_option_has_a_key(self) -> None:
        self.assertEqual(problems(CMAKE.read_text(), ROWS.read_text()), [])

    def test_a_new_option_without_a_key_is_reported(self) -> None:
        cmake = CMAKE.read_text() + '\noption(BUILD_NEW_THING "x" ON)\n'
        self.assertIn(
            "BUILD_NEW_THING is in neither option list", problems(cmake, ROWS.read_text())
        )

    def test_a_gate_without_a_row_is_reported(self) -> None:
        rows = re.sub(r'X\("assist",[^)]*\)', "", ROWS.read_text())
        self.assertIn("gate ASSIST has no features key", problems(CMAKE.read_text(), rows))


if __name__ == "__main__":
    unittest.main()
