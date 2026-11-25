"""Unit tests for gen_integer_domains.rows and signatures, over hand-built domains."""

from __future__ import annotations

import ast
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gen_integer_domains as gen


def _row(entry: str, param: str, lo: int = 1, hi: int | None = 64) -> dict:
    return {"entry": entry, "param": param, "kind": "integer", "lo": lo, "hi": hi, "stricter": None}


def _real_row(
    entry: str,
    param: str,
    lo: float | None,
    hi: float | None,
    lo_included: bool = True,
    hi_included: bool = True,
    sentinel: float | None = None,
) -> dict:
    return {
        "entry": entry,
        "param": param,
        "kind": "real",
        "lo": lo,
        "hi": hi,
        "lo_included": lo_included,
        "hi_included": hi_included,
        "stricter": None,
        "sentinel": sentinel,
    }


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

    def test_an_array_element_keeps_its_brackets(self) -> None:
        domains = [_row("waveform_peak_pyramid", "samples_per_bucket_levels[]", 1, None)]
        reached = {"waveform_peak_pyramid": frozenset({"waveform_peak_pyramid"})}
        table = gen.rows(domains, {}, reached, taken={}, unbound=set())
        self.assertEqual(list(table), [("waveform_peak_pyramid", "samples_per_bucket_levels[]")])

    def test_a_dropped_row_does_not_split_a_reachable_one(self) -> None:
        reached = {"downmix": frozenset({"downmix", "downmix_plan"})}
        domains = [
            _row("downmix", "num_channels", 1, 8),
            _row("downmix_plan", "num_channels", 1, 64),
        ]
        table = gen.rows(domains, {}, reached, unbound={("downmix", "num_channels")})
        self.assertEqual(table, {("downmix", "num_channels"): (1, 64, None)})


class RealRowsTest(unittest.TestCase):
    def test_keeps_reachable_real_domains_and_drops_unbound_values(self) -> None:
        domains = [
            _real_row("eq_set_gain_scale", "scale", 0.0, 2.0),
            _real_row("eq_set_output_pan", "pan", -1.0, 1.0),
            _real_row("eq_set_band", "band.gainDb", -3.4e38, 770.0),
        ]
        reached = {
            "StreamingEqualizer.set_gain_scale": frozenset({"eq_set_gain_scale"}),
            "StreamingEqualizer.set_output_pan": frozenset({"eq_set_output_pan"}),
            "StreamingEqualizer.set_band": frozenset({"eq_set_band"}),
        }
        table = gen.real_rows(domains, {}, reached, taken={})
        self.assertEqual(
            table,
            {
                ("StreamingEqualizer.set_gain_scale", "scale"): (0.0, 2.0, True, True, None),
                ("StreamingEqualizer.set_output_pan", "pan"): (-1.0, 1.0, True, True, None),
                ("StreamingEqualizer.set_band", "gainDb"): (-3.4e38, 770.0, True, True, None),
            },
        )

    def test_real_sentinel_at_an_endpoint_is_excluded_from_given_values(self) -> None:
        row = _real_row("entry", "value", 0.0, 2.0, sentinel=0.0)
        table = gen.real_rows([row], {}, {"entry": frozenset({"entry"})})
        self.assertEqual(table, {("entry", "value"): (0.0, 2.0, False, True, 0.0)})

    def test_real_sentinel_outside_the_domain_does_not_change_its_bounds(self) -> None:
        row = _real_row("entry", "value", 0.0, 2.0, sentinel=-1.0)
        table = gen.real_rows([row], {}, {"entry": frozenset({"entry"})})
        self.assertEqual(table, {("entry", "value"): (0.0, 2.0, True, True, -1.0)})

    def test_real_numeric_annotations_do_not_look_like_enumerations(self) -> None:
        source = '@_entry("real")\ndef real(scale: float, count: int, mode: str):\n    pass\n'
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "module.py").write_text(source)
            found = gen.named_real_keywords(Path(tmp))
        self.assertEqual(found, {("real", "mode")})

    def test_rendered_real_domain_text_values_remain_strings(self) -> None:
        table = {
            ("StreamingEqualizer.set_output_gain_db", "gain_db"): (
                None,
                770.0,
                True,
                True,
                None,
            ),
            ("example_parameter_reader_with_a_long_entry_name", "gain_db"): (
                -3.4028234663852886e38,
                770.0,
                True,
                True,
                None,
            ),
        }
        rendered = gen.render({}, table)
        module = ast.parse(rendered)
        declaration = next(
            node
            for node in module.body
            if isinstance(node, ast.AnnAssign)
            and getattr(node.target, "id", None) == "REAL_DOMAIN_TEXT"
        )
        self.assertIsInstance(declaration.value, ast.Dict)
        values = [ast.literal_eval(value) for value in declaration.value.values]
        self.assertEqual(values[0], "must be finite and at most 770")
        self.assertEqual(values[1], "must be finite and in [-3.4028235e38, 770]")
        self.assertTrue(rendered.endswith("\n"))


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
