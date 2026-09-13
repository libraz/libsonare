#!/usr/bin/env python3
"""Stdlib self-tests for the Python narrowing-scope checker.

Every case drives :func:`check_python_narrowing_scope.evaluate` -- the function
the shipping entry point calls -- rather than restating its rules, because a
class asserted through a reimplementation would only ever agree with itself.

The non-vacuity requirement is per class, not per run: each class is reverted on
its own and must produce exactly one report, so a green run means every class
still fires for its own reason instead of one loud class covering for the rest.
A floor on the real tree is pinned too, because every check passes vacuously on
an empty population.
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "check_python_narrowing_scope",
    Path(__file__).resolve().parent / "check_python_narrowing_scope.py",
)
assert _SPEC and _SPEC.loader
scope = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(scope)

NO_FLOOR: dict[str, int] = {}
NO_RECORDS: dict[str, list] = {"shapes": [], "narrowings": []}


class _SyntheticTree(unittest.TestCase):
    """Cases that need a tree of their own, one module deep."""

    def tree(self, body: str, name: str = "m.py") -> Path:
        root = Path(tempfile.mkdtemp())
        (root / name).write_text(body, encoding="utf-8")
        self.addCleanup(lambda: None)
        return root

    def evaluate(self, root: Path, *, records=None, floor=None, **kwargs):
        scan = scope.Scan(root, **kwargs)
        return scope.evaluate(
            scan,
            scope.Records(records if records is not None else NO_RECORDS),
            floor if floor is not None else NO_FLOOR,
        )

    def only(self, failures, fragment: str):
        """One class, and the one intended -- not merely at least the one."""
        self.assertEqual(
            len(failures),
            1,
            f"expected one failure class, got: {[heading for heading, _ in failures]}",
        )
        self.assertIn(fragment, failures[0][0])
        return failures[0][1]


class ShippingTreeTest(unittest.TestCase):
    def test_the_binding_reports_nothing(self) -> None:
        data = scope.load_records()
        scan = scope.Scan()
        self.assertEqual(scope.evaluate(scan, scope.Records(data), data["floor"]), [])

    def test_the_binding_clears_the_floor_it_is_sized_for(self) -> None:
        floor = scope.load_records()["floor"]
        self.assertGreaterEqual(len(scope.Scan().sites), floor["narrowings"])
        self.assertGreaterEqual(
            scope._shared_reader_calls(scope.BINDING), floor["shared_reader_calls"]
        )


class FailureClassTest(_SyntheticTree):
    def test_an_unrecorded_narrowing_is_the_only_report(self) -> None:
        root = self.tree("import ctypes\n\n\ndef f(n):\n    return ctypes.c_int(n)\n")
        lines = self.only(self.evaluate(root), "neither performed by the shared reader")
        self.assertIn("ctypes.c_int(n)", lines[0])

    def test_a_recorded_narrowing_reports_nothing(self) -> None:
        root = self.tree("import ctypes\n\n\ndef f(n):\n    return ctypes.c_int(1 if n else 0)\n")
        records = {
            "shapes": [
                {
                    "name": "flag",
                    "argument_pattern": "(?:0|1) if .+ else (?:0|1)",
                    "reason": "rendered to 0/1 at the call site",
                }
            ],
            "narrowings": [],
        }
        self.assertEqual(self.evaluate(root, records=records), [])

    def test_an_out_parameter_is_not_a_narrowing(self) -> None:
        root = self.tree("import ctypes\n\n\ndef f():\n    return ctypes.c_int()\n")
        self.assertEqual(self.evaluate(root), [])

    def test_a_plain_structure_base_is_the_only_report(self) -> None:
        root = self.tree(
            "import ctypes\n\n\nclass S(ctypes.Structure):\n    _fields_ = [('a', ctypes.c_int32)]\n"
        )
        lines = self.only(self.evaluate(root), scope.STRUCT_BASE)
        self.assertIn("class S(ctypes.Structure)", lines[0])

    def test_a_structure_on_the_checked_base_reports_nothing(self) -> None:
        root = self.tree(
            "import ctypes\n\nfrom ._cstruct import CStruct\n\n\n"
            "class S(CStruct):\n    _fields_ = [('a', ctypes.c_int32)]\n"
        )
        self.assertEqual(self.evaluate(root), [])

    def test_a_file_local_reader_is_the_only_report(self) -> None:
        root = self.tree(
            "import ctypes\n\n\ndef _checked_size_t(v: int, name: str) -> ctypes.c_size_t:\n"
            "    return _to_c_size_t(v, name)\n"
        )
        lines = self.only(self.evaluate(root), "file-local readers")
        self.assertIn("_checked_size_t", lines[0])

    def test_an_aliased_ctypes_import_is_the_only_report(self) -> None:
        """The known common mode: a conversion reached without the qualification."""
        root = self.tree("from ctypes import c_int\n\n\ndef f(n):\n    return c_int(n)\n")
        self.only(self.evaluate(root), "import a ctypes integer type directly")

    def test_a_stale_record_is_the_only_report(self) -> None:
        root = self.tree("import ctypes\n")
        records = {
            "shapes": [],
            "narrowings": [{"file": "gone.py", "argument": "n", "type": "c_int"}],
        }
        self.only(self.evaluate(root, records=records), "matched nothing")

    def test_a_shrunken_population_is_the_only_report(self) -> None:
        """Two empty sets agree perfectly, so the floor is asserted first."""
        root = self.tree("import ctypes\n")
        lines = self.only(
            self.evaluate(root, floor={"narrowings": 1}), "no longer finds the population"
        )
        self.assertIn("stopped matching", lines[0])

    def test_the_two_scans_disagreeing_is_the_only_report(self) -> None:
        """Narrow the tree scan past a call shape the token scan still sees."""
        root = self.tree("import ctypes\n\n\ndef f(n):\n    return ctypes.c_int(len(n))\n")
        lines = self.only(self.evaluate(root, simple_arguments_only=True), "disagree")
        self.assertIn("the token scan saw 1, the tree scan saw 0", lines[0])


class ScanFidelityTest(_SyntheticTree):
    def test_a_mention_in_a_comment_or_a_literal_is_not_a_narrowing(self) -> None:
        for body in (
            "import ctypes\nX = '''ctypes.c_int(n)'''\n",
            "import ctypes\n# ctypes.c_int(n)\n",
            'import ctypes\nX = "ctypes.c_int(n)"\n',
        ):
            with self.subTest(body=body):
                root = self.tree(body)
                self.assertEqual(self.evaluate(root), [])
                self.assertEqual(scope.Scan(root).token_counts["m.py"], 0)

    def test_a_narrowing_split_across_lines_is_seen_by_both_scans(self) -> None:
        root = self.tree(
            "import ctypes\n\n\ndef f(n):\n    return ctypes.c_int(\n        n,\n    )\n"
        )
        scan = scope.Scan(root)
        self.assertEqual(len(scan.sites), 1)
        self.assertEqual(scan.token_counts["m.py"], 1)


if __name__ == "__main__":
    unittest.main()
