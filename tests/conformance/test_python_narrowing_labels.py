"""Stdlib self-tests for the Python narrowing-label checker.

Every case drives :func:`check_python_narrowing_labels.scan` and ``evaluate`` -- the
functions the shipping entry point calls -- on a synthetic module, so each acceptance
rule and each failure class is shown to fire on its own. A floor on the real tree is
pinned too, because every check passes vacuously on an empty population.
"""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "check_python_narrowing_labels",
    Path(__file__).resolve().parent / "check_python_narrowing_labels.py",
)
assert _SPEC and _SPEC.loader
labels = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = labels  # dataclass resolves its module by name
_SPEC.loader.exec_module(labels)


def _record(label: str, state: str = "open", function: str = "f") -> dict:
    return {"file": "m.py", "function": function, "label": label, "state": state, "reason": "r"}


class LabelChecks(unittest.TestCase):
    def scan(self, body: str):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        root = Path(scratch.name)
        (root / "m.py").write_text(body, encoding="utf-8")
        return labels.scan(root)

    def failures(self, body: str, records: list[dict]) -> list[str]:
        sites, read, counted = self.scan(body)
        return [h for h, _ in labels.evaluate(sites, read, counted, {"records": records})]

    def test_a_parameter_label_is_accepted(self) -> None:
        body = "def f(hop_length):\n    _to_c_int(hop_length, 'hop_length')\n"
        self.assertEqual(self.failures(body, []), [])

    def test_a_field_of_a_parameter_is_accepted(self) -> None:
        body = "def f(clips):\n    _narrow_double(clips[0].gain, 'clips[0].gain')\n"
        self.assertEqual(self.failures(body, []), [])

    def test_an_enclosing_function_parameter_is_accepted(self) -> None:
        body = "def f(ref):\n    def g():\n        _to_c_uint32(ref, 'ref')\n    g()\n"
        self.assertEqual(self.failures(body, []), [])

    def test_a_wide_array_count_is_accepted_and_counted(self) -> None:
        body = "def f(x):\n    arr, n = _to_c_float_array(x)\n    _to_c_size_t(n // 2, 'length')\n"
        sites, read, counted = self.scan(body)
        self.assertEqual((sites, read, counted), ([], 1, 1))

    def test_a_narrow_array_count_needs_a_record(self) -> None:
        body = "def f(x):\n    arr, n = _to_c_float_array(x)\n    _to_c_int(n, 'length')\n"
        self.assertEqual(len(self.failures(body, [])), 1)
        self.assertEqual(self.failures(body, [_record("length")]), [])

    def test_a_constant_value_is_accepted(self) -> None:
        body = "def f():\n    _to_c_size_t(_CAPACITY, 'capacity')\n"
        self.assertEqual(self.failures(body, []), [])

    def test_a_label_built_at_run_time_is_not_read(self) -> None:
        body = "def f(x):\n    _to_c_int(x, surface_spelling('f', 'n_fft'))\n"
        self.assertEqual(self.scan(body), ([], 0, 0))

    def test_a_foreign_label_fails_without_a_record(self) -> None:
        body = "def f(ref):\n    _to_c_uint32(ref, 'strip')\n"
        failures = self.failures(body, [])
        self.assertEqual(len(failures), 1)
        self.assertIn("not a parameter", failures[0])

    def test_a_record_matching_no_call_fails(self) -> None:
        body = "def f(ref):\n    _to_c_uint32(ref, 'ref')\n"
        failures = self.failures(body, [_record("strip")])
        self.assertEqual(len(failures), 1)
        self.assertIn("match no call", failures[0])

    def test_a_record_without_a_known_state_or_reason_fails(self) -> None:
        body = "def f(ref):\n    _to_c_uint32(ref, 'strip')\n"
        bad = _record("strip", state="fine")
        failures = self.failures(body, [bad])
        self.assertIn("Malformed records (file, function, label, state, reason)", failures)

    def test_the_floor_fails_a_short_population(self) -> None:
        body = "def f(x):\n    _to_c_int(x, 'x')\n"
        sites, read, counted = self.scan(body)
        floor = {"floor": {"labels_read": 2}, "records": []}
        self.assertEqual(len(labels.evaluate(sites, read, counted, floor)), 1)


class RealTree(unittest.TestCase):
    def test_the_shipped_records_hold_and_the_population_clears_its_floor(self) -> None:
        data = json.loads(labels.RECORDS.read_text())
        sites, read, counted = labels.scan(labels.PACKAGE)
        self.assertEqual(labels.evaluate(sites, read, counted, data), [])
        self.assertGreaterEqual(read, data["floor"]["labels_read"])
        self.assertGreaterEqual(counted, data["floor"]["wide_counts"])


if __name__ == "__main__":
    unittest.main()
