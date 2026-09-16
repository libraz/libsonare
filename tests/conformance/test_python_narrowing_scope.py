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
        self.assertGreaterEqual(
            scope._shared_reader_calls(scope.BINDING, ("_to_c_float",)),
            floor["shared_float_reader_calls"],
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

    def test_an_array_constructor_is_the_only_report(self) -> None:
        """The spelling whose callee is a BinOp rather than an attribute."""
        root = self.tree(
            "import ctypes\n\n\ndef f(values):\n"
            "    return (ctypes.c_uint8 * len(values))(*values)\n"
        )
        lines = self.only(self.evaluate(root), "neither performed by the shared reader")
        self.assertIn("(ctypes.c_uint8 * n)(...)", lines[0])
        self.assertIn("every element: values", lines[0])

    def test_an_array_constructor_is_keyed_on_its_element_expression(self) -> None:
        """A splat is unwrapped, so the record vocabulary reads the converted value."""
        root = self.tree(
            "import ctypes\n\n\ndef f(xs):\n"
            "    return (ctypes.c_uint8 * len(xs))(*[_narrow_int(x, 'x', 0, 255) for x in xs])\n"
        )
        records = {
            "shapes": [
                {
                    "name": "domain-validated-helper",
                    "argument_pattern": "_narrow_int\\(.+\\)",
                    "reason": "refuses anything outside the field's own domain first",
                }
            ],
            "narrowings": [],
        }
        self.assertEqual(self.evaluate(root, records=records), [])

    def test_an_empty_array_is_not_a_narrowing(self) -> None:
        """An out-buffer converts nothing, in either spelling."""
        root = self.tree("import ctypes\n\n\ndef f(n):\n    return (ctypes.c_int32 * n)()\n")
        self.assertEqual(self.evaluate(root), [])
        self.assertEqual(scope.Scan(root).token_counts["m.py"], 0)

    def test_a_masked_field_assignment_is_the_only_report(self) -> None:
        """The checked base is still installed; the mask is a layer in front of it."""
        root = self.tree(
            "import ctypes\n\nfrom ._cstruct import CStruct\n\n\n"
            "class S(CStruct):\n    _fields_ = [('kind', ctypes.c_uint8)]\n\n\n"
            "def f(raw, x):\n    raw.kind = int(x) & 0xFF\n"
        )
        lines = self.only(self.evaluate(root), "fold a value into range")
        self.assertIn("raw.kind = ... & 0xff", lines[0])

    def test_a_truncation_in_front_of_a_field_is_not_a_mask(self) -> None:
        """``int(...)`` is deliberate, so only a width mask is reportable."""
        root = self.tree(
            "import ctypes\n\nfrom ._cstruct import CStruct\n\n\n"
            "class S(CStruct):\n    _fields_ = [('kind', ctypes.c_uint8)]\n\n\n"
            "def f(raw, x):\n    raw.kind = int(x)\n    raw.count = x & 0x7\n"
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
        self.only(self.evaluate(root), "import a ctypes numeric type directly")

    def test_an_aliased_float_import_is_the_only_report(self) -> None:
        """The same common mode on the float half, which has its own type list."""
        root = self.tree("from ctypes import c_float\n\n\ndef f(x):\n    return c_float(x)\n")
        self.only(self.evaluate(root), "import a ctypes numeric type directly")

    def test_an_unrecorded_float_narrowing_is_the_only_report(self) -> None:
        """A float saturates rather than wraps, and is the same population."""
        root = self.tree("import ctypes\n\n\ndef f(x):\n    return ctypes.c_float(x)\n")
        lines = self.only(self.evaluate(root), "neither performed by the shared reader")
        self.assertIn("ctypes.c_float(x)", lines[0])

    def test_a_float_narrowing_through_the_shared_reader_reports_nothing(self) -> None:
        """The routed spelling is not a site, so the fix clears its own report."""
        root = self.tree("import ctypes\n\n\ndef f(x):\n    return _to_c_float(x, 'x')\n")
        self.assertEqual(self.evaluate(root), [])

    def test_a_c_double_conversion_is_not_a_narrowing(self) -> None:
        """The boundary of the float half: a Python float already IS a double."""
        root = self.tree("import ctypes\n\n\ndef f(x):\n    return ctypes.c_double(x)\n")
        scan = scope.Scan(root)
        self.assertEqual(len(scan.sites), 0)
        self.assertEqual(scan.token_counts["m.py"], 0)

    def test_a_shrunken_float_population_is_the_only_report(self) -> None:
        """Pinned apart from the total, so a growing integer count cannot hide it."""
        root = self.tree("import ctypes\n\n\ndef f(n):\n    return ctypes.c_int(n)\n")
        records = {
            "shapes": [{"name": "any", "argument_pattern": ".+", "reason": "not the subject"}],
            "narrowings": [],
        }
        failures = self.evaluate(root, records=records, floor={"float_narrowings": 1})
        lines = self.only(failures, "no longer finds the population")
        self.assertIn("float_narrowings: found 0", lines[0])

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

    def test_a_shrunken_array_population_is_the_only_report(self) -> None:
        """The array spelling is pinned apart, so a growing total cannot hide it."""
        root = self.tree("import ctypes\n\n\ndef f(n):\n    return ctypes.c_int(n)\n")
        records = {
            "shapes": [{"name": "any", "argument_pattern": ".+", "reason": "not the subject"}],
            "narrowings": [],
        }
        failures = self.evaluate(root, records=records, floor={"array_narrowings": 1})
        lines = self.only(failures, "no longer finds the population")
        self.assertIn("array_narrowings: found 0", lines[0])

    def test_an_unchecked_argtype_argument_is_the_only_report(self) -> None:
        """Scan C's anchor: no conversion is spelled anywhere near this call."""
        root = self.tree(
            "import ctypes\n\n\ndef configure(lib):\n"
            "    lib.sonare_set_track_gain.argtypes = [ctypes.c_void_p, ctypes.c_uint32]\n\n\n"
            "def f(lib, handle, track_id):\n"
            "    lib.sonare_set_track_gain(handle, int(track_id))\n"
        )
        lines = self.only(self.evaluate(root), "argtypes declares a narrowing C type")
        self.assertIn("sonare_set_track_gain(... argument 1: c_uint32) <- int(track_id)", lines[0])

    def test_an_argtype_argument_through_the_shared_reader_reports_nothing(self) -> None:
        root = self.tree(
            "import ctypes\n\n\ndef configure(lib):\n"
            "    lib.sonare_set_track_gain.argtypes = [ctypes.c_void_p, ctypes.c_uint32]\n\n\n"
            "def f(lib, handle, track_id):\n"
            "    lib.sonare_set_track_gain(handle, _to_c_uint32(track_id, 'track_id'))\n"
        )
        self.assertEqual(self.evaluate(root), [])

    def test_a_local_bound_by_a_shared_reader_reports_nothing(self) -> None:
        """The value was range-checked before the name was; reading the name cannot tell."""
        root = self.tree(
            "import ctypes\n\n\ndef configure(lib):\n"
            "    lib.sonare_set_track_gain.argtypes = [ctypes.c_void_p, ctypes.c_float]\n\n\n"
            "def f(lib, handle, gain):\n"
            "    g = _narrow_float(gain, 'gain')\n"
            "    lib.sonare_set_track_gain(handle, g)\n"
        )
        self.assertEqual(self.evaluate(root), [])

    def test_a_local_bound_twice_is_reported(self) -> None:
        """A second binding is a second contract, and this scan does not order them."""
        root = self.tree(
            "import ctypes\n\n\ndef configure(lib):\n"
            "    lib.sonare_set_track_gain.argtypes = [ctypes.c_void_p, ctypes.c_float]\n\n\n"
            "def f(lib, handle, gain, raw):\n"
            "    g = _narrow_float(gain, 'gain')\n"
            "    g = raw\n"
            "    lib.sonare_set_track_gain(handle, g)\n"
        )
        lines = self.only(self.evaluate(root), "argtypes declares a narrowing C type")
        self.assertIn("argument 1: c_float) <- g", lines[0])

    def test_a_non_narrowing_parameter_is_not_a_site(self) -> None:
        """A double converts nothing, and a pointer position carries no caller number."""
        root = self.tree(
            "import ctypes\n\n\ndef configure(lib):\n"
            "    lib.sonare_set_sample_rate.argtypes = [\n"
            "        ctypes.c_void_p,\n"
            "        ctypes.c_double,\n"
            "        ctypes.POINTER(ctypes.c_uint32),\n"
            "    ]\n\n\n"
            "def f(lib, handle, rate, out):\n"
            "    lib.sonare_set_sample_rate(handle, float(rate), out)\n"
        )
        scan = scope.Scan(root)
        self.assertEqual(scan.argument_positions, 0)
        self.assertEqual(self.evaluate(root), [])

    def test_an_inline_conversion_at_an_argtype_position_is_reported_once(self) -> None:
        """The populations are disjoint: the inline one owns this spelling."""
        root = self.tree(
            "import ctypes\n\n\ndef configure(lib):\n"
            "    lib.sonare_set_track_gain.argtypes = [ctypes.c_void_p, ctypes.c_uint32]\n\n\n"
            "def f(lib, handle, track_id):\n"
            "    lib.sonare_set_track_gain(handle, ctypes.c_uint32(track_id))\n"
        )
        lines = self.only(self.evaluate(root), "neither performed by the shared reader")
        self.assertIn("ctypes.c_uint32(track_id)", lines[0])

    def test_a_symbol_declared_two_ways_is_not_attributed(self) -> None:
        """Two declarations give no one list to attribute a position against."""
        root = self.tree(
            "import ctypes\n\n\ndef configure(lib, other):\n"
            "    lib.sonare_push.argtypes = [ctypes.c_uint32]\n"
            "    other.sonare_push.argtypes = [ctypes.c_float]\n\n\n"
            "def f(lib, value):\n"
            "    lib.sonare_push(int(value))\n"
        )
        scan = scope.Scan(root)
        self.assertEqual(scan.signatures, {})
        self.assertEqual(self.evaluate(root), [])

    def test_an_argument_past_a_splat_has_no_position(self) -> None:
        """Positional attribution ends where the positions stop being knowable."""
        root = self.tree(
            "import ctypes\n\n\ndef configure(lib):\n"
            "    lib.sonare_push.argtypes = [ctypes.c_void_p, ctypes.c_uint32]\n\n\n"
            "def f(lib, rest, value):\n"
            "    lib.sonare_push(*rest, int(value))\n"
        )
        self.assertEqual(scope.Scan(root).argument_positions, 0)
        self.assertEqual(self.evaluate(root), [])

    def test_a_shrunken_signature_table_is_the_only_report(self) -> None:
        """Scan C's anchor, pinned: resolving nothing agrees with everything."""
        root = self.tree("import ctypes\n")
        lines = self.only(
            self.evaluate(root, floor={"declared_signatures": 1}), "no longer finds the population"
        )
        self.assertIn("declared_signatures: found 0", lines[0])

    def test_a_shrunken_attribution_is_the_only_report(self) -> None:
        """The other half: declarations read, no call ever landing on them."""
        root = self.tree(
            "import ctypes\n\n\ndef configure(lib):\n"
            "    lib.sonare_push.argtypes = [ctypes.c_uint32]\n"
        )
        lines = self.only(
            self.evaluate(root, floor={"argument_positions": 1}), "no longer finds the population"
        )
        self.assertIn("argument_positions: found 0", lines[0])

    def test_a_stale_argument_record_is_the_only_report(self) -> None:
        root = self.tree("import ctypes\n")
        records = {
            "shapes": [],
            "narrowings": [],
            "argtype_arguments": [{"file": "gone.py", "argument": "n", "type": "c_int"}],
        }
        self.only(self.evaluate(root, records=records), "matched nothing")

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

    def test_an_array_narrowing_is_seen_by_both_scans(self) -> None:
        """Both routes carry the array spelling, so their agreement still asserts."""
        root = self.tree(
            "import ctypes\n\n\ndef f(n, xs):\n    return (ctypes.c_uint8 * (n + 1))(*xs)\n"
        )
        scan = scope.Scan(root)
        self.assertEqual(len(scan.sites), 1)
        self.assertEqual(scan.token_counts["m.py"], 1)

    def test_an_array_type_that_is_not_called_is_not_a_narrowing(self) -> None:
        """Naming the type converts nothing; the report is for the constructor."""
        root = self.tree(
            "import ctypes\n\n\ndef f(buf):\n"
            "    return (ctypes.c_uint8 * len(buf)).from_buffer_copy(buf)\n"
        )
        scan = scope.Scan(root)
        self.assertEqual(len(scan.sites), 0)
        self.assertEqual(scan.token_counts["m.py"], 0)

    def test_a_float_narrowing_is_seen_by_both_scans(self) -> None:
        """Both routes carry the float type list, so their agreement still asserts."""
        root = self.tree("import ctypes\n\n\ndef f(x):\n    return ctypes.c_float(x)\n")
        scan = scope.Scan(root)
        self.assertEqual(len(scan.sites), 1)
        self.assertEqual(scan.token_counts["m.py"], 1)

    def test_a_narrowing_split_across_lines_is_seen_by_both_scans(self) -> None:
        root = self.tree(
            "import ctypes\n\n\ndef f(n):\n    return ctypes.c_int(\n        n,\n    )\n"
        )
        scan = scope.Scan(root)
        self.assertEqual(len(scan.sites), 1)
        self.assertEqual(scan.token_counts["m.py"], 1)


if __name__ == "__main__":
    unittest.main()
