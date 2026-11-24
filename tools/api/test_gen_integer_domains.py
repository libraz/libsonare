"""Unit tests for gen_integer_domains.rows and signatures, over hand-built domains."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gen_integer_domains as gen


def _row(entry: str, param: str, lo: int = 1, hi: int | None = 64) -> dict:
    return {"entry": entry, "param": param, "kind": "integer", "lo": lo, "hi": hi, "stricter": None}


_COUNT_IN_REACHED = {"Project.count_in_frames": frozenset({"project_count_in_frames"})}
_COUNT_IN_DOMAINS = [_row("project_count_in_frames", "count_in_bars", 0, 16)]


class ReachableRowsTest(unittest.TestCase):
    def test_a_direct_parameter_the_signature_does_not_take_gets_no_row(self) -> None:
        taken = {"Project.count_in_frames": frozenset({"self"})}
        table = gen.rows(_COUNT_IN_DOMAINS, {}, _COUNT_IN_REACHED, taken=taken)
        self.assertEqual(table, {})

    def test_without_signatures_every_row_is_kept(self) -> None:
        table = gen.rows(_COUNT_IN_DOMAINS, {}, _COUNT_IN_REACHED)
        self.assertEqual(table, {("Project.count_in_frames", "count_in_bars"): (0, 16, None)})

    def test_an_entry_absent_from_the_signatures_keeps_its_rows(self) -> None:
        table = gen.rows(_COUNT_IN_DOMAINS, {}, _COUNT_IN_REACHED, taken={})
        self.assertEqual(table, {("Project.count_in_frames", "count_in_bars"): (0, 16, None)})

    def test_a_python_unbound_struct_member_gets_no_row(self) -> None:
        domains = [
            _row("project_bounce", "options.instrument_count"),
            _row("project_bounce", "options.block_size"),
        ]
        reached = {"Project.bounce": frozenset({"project_bounce"})}
        taken = {"Project.bounce": frozenset({"self"})}
        table = gen.rows(
            domains, {}, reached, taken=taken, unbound={("project_bounce", "instrument_count")}
        )
        self.assertEqual(list(table), [("Project.bounce", "block_size")])

    def test_a_member_of_an_array_element_is_kept_whatever_its_name(self) -> None:
        domains = [_row("project_bounce", "options.articulations[].channel", 0, 15)]
        reached = {"Project.bounce": frozenset({"project_bounce"})}
        table = gen.rows(domains, {}, reached, taken={}, unbound={("project_bounce", "channel")})
        self.assertEqual(list(table), [("Project.bounce", "articulations[].channel")])

    def test_a_dropped_row_does_not_split_a_reachable_one(self) -> None:
        reached = {"downmix": frozenset({"downmix", "downmix_plan"})}
        domains = [
            _row("downmix", "num_channels", 1, 8),
            _row("downmix_plan", "num_channels", 1, 64),
        ]
        table = gen.rows(domains, {}, reached, unbound={("downmix", "num_channels")})
        self.assertEqual(table, {("downmix", "num_channels"): (1, 64, None)})


class SignaturesTest(unittest.TestCase):
    def test_reads_every_parameter_and_skips_an_entry_taking_kwargs(self) -> None:
        source = (
            '@_entry("a")\n'
            "def a(x, /, y, *, z=1):\n    pass\n\n"
            '@_entry("b")\n'
            "def b(x, **options):\n    pass\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "m.py").write_text(source)
            found = gen.signatures(Path(tmp))
        self.assertEqual(found, {"a": frozenset({"x", "y", "z"})})


if __name__ == "__main__":
    unittest.main()
