"""Unit tests for gen_field_spelling.derive and its renderers, over hand-built facts."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gen_field_spelling as gen


@dataclass(frozen=True)
class Fact:
    c_key: str
    c_subject: str | None
    kind: str
    surface: str
    surface_spelling: str | None
    file: str = "f.ts"
    line: int = 1


class MechanicalTest(unittest.TestCase):
    def test_node_and_wasm_camel_and_python_identity(self) -> None:
        self.assertEqual(gen.mechanical("smoothing_window_sec", "node"), "smoothingWindowSec")
        self.assertEqual(gen.mechanical("f0_min_hz", "wasm"), "f0MinHz")
        self.assertEqual(gen.mechanical("rt60_bands", "node"), "rt60Bands")
        self.assertEqual(gen.mechanical("n_fft", "python"), "n_fft")


class DeriveTest(unittest.TestCase):
    def test_a_mechanical_binding_is_not_tabled(self) -> None:
        rows, unbound, _ = gen.derive([Fact("analyze", "hop_length", "param", "node", "hopLength")])
        self.assertEqual(rows["node"], [])
        self.assertEqual(unbound, [])

    def test_a_paired_binding_off_the_rule_is_tabled(self) -> None:
        rows, _, _ = gen.derive([Fact("room", "rt60_bands", "param", "wasm", "RT60Bands")])
        self.assertEqual(rows["wasm"], [gen.Row("room", "rt60_bands", "RT60Bands")])

    def test_a_facade_name_with_no_c_subject_is_not_taken_for_a_rename(self) -> None:
        rows, unbound, _ = gen.derive(
            [
                Fact("resample", "target_sr", "param", "node", None),
                Fact("resample", None, "param", "node", "targetSampleRate"),
            ]
        )
        self.assertEqual(rows["node"], [])
        self.assertEqual([u.c_subject for u in unbound], ["target_sr"])

    def test_a_subject_with_no_binding_is_unbound_per_surface(self) -> None:
        _, unbound, _ = gen.derive([Fact("chain", "params_json", "param", "python", None)])
        self.assertEqual([(u.surface, u.c_subject) for u in unbound], [("python", "params_json")])


class RequestFieldTest(unittest.TestCase):
    def test_a_subject_folded_into_a_request_binds_to_its_field(self) -> None:
        facts = [Fact("detect_boundaries", "reference_window_sec", "param", "node", None)]
        request = {
            "node": [
                {"entry": "detectBoundaries", "param": "request", "fields": ["referenceWindowSec"]}
            ]
        }
        rows, unbound, _ = gen.derive(facts, request)
        self.assertEqual(rows["node"], [])
        self.assertEqual(unbound, [])

    def test_a_method_request_answers_to_the_handle_key(self) -> None:
        facts = [Fact("project_add_clip", "track_id", "param", "wasm", None)]
        request = {"wasm": [{"entry": "Project.addClip", "param": "desc", "fields": ["trackId"]}]}
        _, unbound, _ = gen.derive(facts, request)
        self.assertEqual(unbound, [])

    def test_a_ruled_rename_is_tabled(self) -> None:
        facts = [Fact("resample", "target_sr", "param", "node", None)]
        request = {
            "node": [{"entry": "resample", "param": "request", "fields": ["targetSampleRate"]}]
        }
        rows, _, idle = gen.derive(facts, request, {"node": {"target_sr": "targetSampleRate"}})
        self.assertEqual(rows["node"], [gen.Row("resample", "target_sr", "targetSampleRate")])
        self.assertEqual(idle, [])

    def test_a_rename_needs_the_spelling_on_that_entry(self) -> None:
        facts = [Fact("resample", "target_sr", "param", "node", None)]
        rows, unbound, idle = gen.derive(facts, {}, {"node": {"target_sr": "targetSampleRate"}})
        self.assertEqual(rows["node"], [])
        self.assertEqual([u.c_subject for u in unbound], ["target_sr"])
        self.assertEqual(idle, [("node", "target_sr", "targetSampleRate")])

    def test_a_rename_binds_an_unpaired_positional_name(self) -> None:
        facts = [
            Fact("cqt", "fmin_hz", "param", "python", None),
            Fact("cqt", None, "param", "python", "fmin"),
        ]
        rows, unbound, _ = gen.derive(facts, {}, {"python": {"fmin_hz": "fmin"}})
        self.assertEqual(rows["python"], [gen.Row("cqt", "fmin_hz", "fmin")])
        self.assertEqual(unbound, [])

    def test_a_rename_binds_a_name_another_subject_pairs_with(self) -> None:
        facts = [
            Fact("align", "sample_rate", "param", "python", "sample_rate"),
            Fact("align", "reference_sample_rate", "param", "python", None),
        ]
        rows, unbound, idle = gen.derive(
            facts, {}, {"python": {"reference_sample_rate": "sample_rate"}}
        )
        self.assertEqual(rows["python"], [gen.Row("align", "reference_sample_rate", "sample_rate")])
        self.assertEqual((unbound, idle), ([], []))

    def test_a_rename_is_per_surface(self) -> None:
        facts = [
            Fact("cqt", "fmin_hz", "param", "node", None),
            Fact("cqt", None, "param", "node", "fmin"),
        ]
        rows, unbound, _ = gen.derive(facts, {}, {"python": {"fmin_hz": "fmin"}})
        self.assertEqual(rows["node"], [])
        self.assertEqual([u.c_subject for u in unbound], ["fmin_hz"])

    def test_a_rename_list_binds_the_spelling_each_entry_carries(self) -> None:
        facts = [
            Fact("tone", "frequency_hz", "param", "python", None),
            Fact("tone", None, "param", "python", "frequency"),
            Fact("clicks", "frequency_hz", "param", "python", None),
            Fact("clicks", None, "param", "python", "click_freq"),
        ]
        rule = {"python": {"frequency_hz": ["click_freq", "frequency"]}}
        rows, _, idle = gen.derive(facts, {}, rule)
        self.assertEqual(
            rows["python"],
            [
                gen.Row("clicks", "frequency_hz", "click_freq"),
                gen.Row("tone", "frequency_hz", "frequency"),
            ],
        )
        self.assertEqual(idle, [])


class ClassifyTest(unittest.TestCase):
    def _unbound(self, kind: str = "param") -> list:
        return [gen.Unbound("node", "downmix", "frames", kind, "h:1")]

    def test_an_unclassified_unbound_parameter_fails(self) -> None:
        self.assertEqual(len(gen.classify(self._unbound(), {})), 1)

    def test_a_classified_parameter_passes(self) -> None:
        self.assertEqual(gen.classify(self._unbound(), {"downmix.frames": "derived length: x"}), [])

    def test_an_entry_every_surface_binds_is_stale(self) -> None:
        problems = gen.classify([], {"downmix.frames": "output"})
        self.assertIn("every surface binds it", problems[0])

    def test_a_reason_of_no_known_kind_fails(self) -> None:
        problems = gen.classify(self._unbound(), {"downmix.frames": "harmless"})
        self.assertIn("starts with none of", problems[0])

    def test_a_pending_parameter_passes_until_it_binds(self) -> None:
        pending = {"downmix.frames": "unit split"}
        self.assertEqual(gen.classify(self._unbound(), {}, pending), [])
        self.assertIn("is pending but every surface binds it", gen.classify([], {}, pending)[0])

    def test_an_unbound_record_field_needs_no_entry(self) -> None:
        self.assertEqual(gen.classify(self._unbound("record_field"), {}), [])


class RenderTest(unittest.TestCase):
    def test_cpp_rows_are_sorted_for_the_binary_search(self) -> None:
        facts = [Fact("b", "x_y", "param", "node", "X"), Fact("a", "z_w", "param", "node", "Z")]
        rows, _, _ = gen.derive(facts)
        text = gen.render_cpp("node", rows["node"])
        self.assertLess(text.index('{"a", "z_w", "Z"}'), text.index('{"b", "x_y", "X"}'))

    def test_python_lookup_falls_back_to_the_c_spelling(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "_field_spelling.py"
            path.write_text(gen.render_py([gen.Row("resample", "sr", "sample_rate")]))
            spec = importlib.util.spec_from_file_location("_field_spelling", path)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
        self.assertEqual(module.surface_spelling("resample", "sr"), "sample_rate")
        self.assertEqual(module.surface_spelling("resample", "n_fft"), "n_fft")


if __name__ == "__main__":
    unittest.main()
