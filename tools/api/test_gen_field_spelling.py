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


class MemberPathTest(unittest.TestCase):
    def test_matrix_dimension_maps_to_the_caller_member_only_on_its_entry(self) -> None:
        fields = {
            "wasm": [
                {"entry": entry, "param": "request", "fields": ["values"]}
                for entry in ("pcen", "mfcc")
            ]
        }
        renames = {"wasm": {"members_of": {"pcen.n_bins": "values.rows"}}}
        facts = [Fact("mfcc", "n_bins", "param", "wasm", None)]
        rows, unbound, idle = gen.derive(
            facts, fields, renames, {"pcen": {"n_bins"}, "mfcc": {"n_bins"}}
        )
        self.assertEqual(rows["wasm"], [gen.Row("pcen", "n_bins", "values.rows")])
        self.assertEqual([(u.c_key, u.c_subject) for u in unbound], [("mfcc", "n_bins")])
        self.assertEqual(idle, [])


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

    def test_a_handle_class_request_answers_to_its_c_prefix(self) -> None:
        facts = [Fact("engine_render_offline", "num_channels", "param", "node", None)]
        request = {
            "node": [
                {"entry": "RealtimeEngine.renderOffline", "param": "r", "fields": ["channels"]}
            ]
        }
        rule = {"node": {"length_of": {"engine_render_offline.num_channels": "channels"}}}
        rows, unbound, idle = gen.derive(facts, request, rule)
        self.assertEqual(
            rows["node"], [gen.Row("engine_render_offline", "num_channels", "channels", True)]
        )
        self.assertEqual((unbound, idle), ([], []))

    def test_a_handle_method_answers_to_the_c_functions_parity_aliases_to_it(self) -> None:
        facts = [
            Fact("engine_set_track_strip_insert_param_by_name", "param_name", "param", "node", None)
        ]
        request = {
            "node": [
                {
                    "entry": "RealtimeEngine.setStripInsertParam",
                    "param": "param",
                    "fields": ["name", "value"],
                }
            ]
        }
        rows, unbound, idle = gen.derive(facts, request, {"node": {"param_name": "param.name"}})
        self.assertEqual(
            rows["node"],
            [gen.Row("engine_set_track_strip_insert_param_by_name", "param_name", "param.name")],
        )
        self.assertEqual((unbound, idle), ([], []))

    def test_a_declared_object_path_can_restore_a_paired_members_root(self) -> None:
        key = "engine_set_track_strip_insert_param_by_name"
        facts = [Fact(key, "value", "param", "node", "value")]
        rule = {"node": {"value": "param.value"}}
        request = {
            "node": [
                {
                    "entry": "RealtimeEngine.setStripInsertParam",
                    "param": "param",
                    "fields": ["name", "value"],
                }
            ]
        }
        rows, unbound, idle = gen.derive(facts, request, rule)
        self.assertEqual(rows["node"], [gen.Row(key, "value", "param.value")])
        self.assertEqual((unbound, idle), ([], []))
        rows, _, idle = gen.derive(facts, {}, rule)
        self.assertEqual(rows["node"], [])
        self.assertEqual(idle, [("node", "value", "param.value")])

    def test_a_free_function_answers_to_the_c_entries_parity_aliases_to_it(self) -> None:
        facts = [Fact("playback_render_interleaved", "in_channels", "param", "node", None)]
        request = {
            "node": [{"entry": "renderPlayback", "param": "request", "fields": ["numChannels"]}]
        }
        rows, unbound, idle = gen.derive(facts, request, {"node": {"in_channels": "numChannels"}})
        self.assertEqual(
            rows["node"], [gen.Row("playback_render_interleaved", "in_channels", "numChannels")]
        )
        self.assertEqual((unbound, idle), ([], []))

    def test_a_dotted_rename_binds_on_the_object_the_facade_takes(self) -> None:
        facts = [
            Fact("engine_set_bus_sidechain", "source_id", "param", "node", None),
            Fact("engine_set_bus_sidechain", None, "param", "node", "source"),
        ]
        rows, unbound, idle = gen.derive(facts, {}, {"node": {"source_id": "source.id"}})
        self.assertEqual(
            rows["node"], [gen.Row("engine_set_bus_sidechain", "source_id", "source.id")]
        )
        self.assertEqual((unbound, idle), ([], []))

    def test_a_dotted_rename_needs_its_root_on_that_entry(self) -> None:
        facts = [Fact("engine_set_bus_sidechain", "source_id", "param", "node", None)]
        rows, unbound, idle = gen.derive(facts, {}, {"node": {"source_id": "source.id"}})
        self.assertEqual(rows["node"], [])
        self.assertEqual([u.c_subject for u in unbound], ["source_id"])
        self.assertEqual(idle, [("node", "source_id", "source.id")])

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


class ParameterBagKeyTest(unittest.TestCase):
    def setUp(self) -> None:
        self.input = gen.tomllib.loads(gen.RENAMES.read_text(encoding="utf-8"))
        self.rows = gen.parse_parameter_bag_keys(self.input)

    def test_parameter_bag_spellings_are_entry_scoped_and_bidirectional(self) -> None:
        expected = {
            ("mixing_assistant_suggest", "n_fft"): "nFft",
            ("mixing_assistant_suggest", "hop_length"): "hopLength",
            ("mastering_analyze_stereo", "bands_per_octave"): "bandsPerOctave",
            ("mastering_analyze_stereo", "low_hz"): "lowHz",
            ("mastering_analyze_stereo", "high_hz"): "highHz",
        }
        self.assertEqual({(row.entry, row.subject): row.spelling for row in self.rows}, expected)
        for (entry, subject), spelling in expected.items():
            self.assertEqual(gen.parameter_bag_key(self.rows, entry, subject), spelling)
            self.assertEqual(gen.parameter_bag_subject(self.rows, entry, spelling), subject)
        self.assertEqual(gen.parameter_bag_key(self.rows, "other_entry", "n_fft"), "n_fft")
        self.assertEqual(
            gen.parameter_bag_subject(self.rows, "mixing_assistant_suggest", "n_fft"),
            "n_fft",
        )

    def test_cpp_and_python_outputs_share_the_exact_bag_rows(self) -> None:
        cpp = gen.render_parameter_bag_cpp(self.rows)
        py = gen.render_py([], self.rows)
        self.assertIn('{"mixing_assistant_suggest", "n_fft", "nFft"}', cpp)
        self.assertIn('{"mastering_analyze_stereo", "bands_per_octave", "bandsPerOctave"}', cpp)
        self.assertIn(
            '"mixing_assistant_suggest": {\n        "hop_length": "hopLength",\n        "n_fft": "nFft",',
            py,
        )
        self.assertIn(
            '"mastering_analyze_stereo": {\n        "bands_per_octave": "bandsPerOctave",',
            py,
        )
        self.assertIn("def parameter_bag_subject(entry: str, spelling: str)", py)


class ClassifyTest(unittest.TestCase):
    def _unbound(self, kind: str = "param") -> list:
        return [gen.Unbound("node", "downmix", "frames", kind, "h:1")]

    def test_an_unclassified_unbound_parameter_fails(self) -> None:
        problems = gen.classify(self._unbound(), {})
        self.assertTrue(any("downmix.frames" in p for p in problems), problems)
        self.assertTrue(any("records 0" in p for p in problems), problems)

    def test_a_recorded_count_of_unclassified_parameters_passes_quietly(self) -> None:
        self.assertEqual(gen.classify(self._unbound(), {}, recorded={"node": 1}), [])

    def test_a_recorded_count_off_in_either_direction_fails(self) -> None:
        for recorded in ({"node": 2}, {"node": 0}, {"node": 1, "wasm": 1}):
            with self.subTest(recorded=recorded):
                self.assertNotEqual(gen.classify(self._unbound(), {}, recorded=recorded), [])

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

    def test_a_constructor_rename_is_scoped_to_its_c_entry(self) -> None:
        facts = [
            Fact("engine_capture", "channels", "param", "node", None),
            Fact("engine_capture", None, "param", "node", "numChannels"),
        ]
        request = {
            "node": [
                {"entry": "PlaybackLoudnessMeter", "param": "options", "fields": ["numChannels"]}
            ]
        }
        rule = {
            "node": {
                "constructor_renames": {"playback_loudness_meter_create.channels": "numChannels"}
            }
        }
        c_params = {"playback_loudness_meter_create": {"channels"}}
        rows, unbound, idle = gen.derive(facts, request, rule, c_params)
        self.assertEqual(
            rows["node"], [gen.Row("playback_loudness_meter_create", "channels", "numChannels")]
        )
        self.assertEqual([row.c_key for row in unbound], ["engine_capture"])
        self.assertEqual(idle, [])

    def test_a_length_of_row_binds_a_count_to_the_array_the_entry_takes(self) -> None:
        facts = [
            Fact("apply", "rendered_mono", "param", "python", "rendered_mono"),
            Fact("apply", "rendered_count", "param", "python", None),
        ]
        rule = {"python": {"length_of": {"apply.rendered_count": "rendered_mono"}}}
        rows, unbound, idle = gen.derive(facts, {}, rule)
        self.assertEqual(
            rows["python"], [gen.Row("apply", "rendered_count", "rendered_mono", True)]
        )
        self.assertEqual((unbound, idle), ([], []))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "_field_spelling.py"
            path.write_text(gen.render_py(rows["python"]))
            spec = importlib.util.spec_from_file_location("_field_spelling", path)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
        self.assertEqual(module.surface_spelling("apply", "rendered_count"), "rendered_mono length")
        text = gen.render_cpp("node", [gen.Row("apply", "rendered_count", "renderedMono", True)])
        self.assertIn('kLengths = {{\n    {"apply", "rendered_count", "renderedMono"},', text)
        self.assertIn("std::array<Override, 0> kOverrides", text)
        self.assertIn('return std::string(length->spelling) + ".length";', text)

    def test_a_frame_count_over_an_interleaved_array_names_both_keywords(self) -> None:
        facts = [
            Fact("render", "in", "param", "python", None),
            Fact("render", "frames", "param", "python", None),
            Fact("render", None, "param", "python", "samples"),
            Fact("render", None, "param", "python", "num_input_channels"),
        ]
        rule = {"python": {"quotient_of": {"render.frames": ["samples", "num_input_channels"]}}}
        rows, _, idle = gen.derive(facts, {}, rule)
        row = gen.Row("render", "frames", "samples", True, "num_input_channels")
        self.assertEqual(rows["python"], [row])
        self.assertEqual(idle, [])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "_field_spelling.py"
            path.write_text(gen.render_py(rows["python"]))
            spec = importlib.util.spec_from_file_location("_field_spelling", path)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
        self.assertEqual(
            module.surface_spelling("render", "frames"), "samples length / num_input_channels"
        )
        self.assertEqual(module.LENGTHS, {})
        text = gen.render_cpp("node", [gen.Row("render", "frames", "samples", True, "inChannels")])
        self.assertIn('kQuotients = {{\n    {"render", "frames", "samples", "inChannels"},', text)
        self.assertIn("std::array<Override, 0> kLengths", text)
        self.assertIn('".length / " + std::string(quotient->divisor)', text)

    def test_a_document_root_reroots_the_slot_the_facade_wraps_the_caller_object_into(
        self,
    ) -> None:
        facts = [Fact("set_strip", None, "param", "python", "settings")]
        rule = {"python": {"document_roots": {"set_strip": ["strips[0].", "settings."]}}}
        rows, unbound, idle = gen.derive(facts, {}, rule)
        row = gen.Row("set_strip", "strips[0].", "settings.", document=True)
        self.assertEqual(rows["python"], [row])
        self.assertEqual((unbound, idle), ([], []))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "_field_spelling.py"
            path.write_text(gen.render_py(rows["python"]))
            spec = importlib.util.spec_from_file_location("_field_spelling", path)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
        self.assertEqual(
            module.surface_spelling("set_strip", "strips[0].faderDB"), "settings.faderDB"
        )
        self.assertEqual(module.surface_spelling("set_strip", "strips[0]"), "settings")
        self.assertEqual(module.surface_spelling("set_strip", "strips[1].pan"), "strips[1].pan")
        self.assertEqual(module.surface_spelling("other", "strips[0].pan"), "strips[0].pan")
        self.assertEqual(module.OVERRIDES, {})
        text = gen.render_cpp("node", [row])
        self.assertIn('kDocumentRoots = {{\n    {"set_strip", "strips[0].", "settings."},', text)
        self.assertIn("std::array<Override, 0> kOverrides", text)

    def test_a_document_root_whose_caller_keyword_the_entry_lacks_binds_nothing(self) -> None:
        facts = [Fact("set_strip", None, "param", "node", "options")]
        rule = {"node": {"document_roots": {"set_strip": ["strips[0].", "settings."]}}}
        rows, _, idle = gen.derive(facts, {}, rule)
        self.assertEqual(rows["node"], [])
        self.assertEqual(idle, [("node", "document_roots.set_strip", "strips[0]. -> settings.")])

    def test_a_resolver_subject_takes_the_argument_its_handle_class_binds(self) -> None:
        facts = [Fact("strip_set_fader_db", "strip", "param", "node", "strip")]
        rule = {"node": {"resolver_of": {"mixer_strip_at.index": "strip"}}}
        c_params = {"mixer_strip_at": {"mixer", "index"}}
        rows, unbound, idle = gen.derive(facts, {}, rule, c_params)
        self.assertEqual(rows["node"], [gen.Row("mixer_strip_at", "index", "strip")])
        self.assertEqual((unbound, idle), ([], []))

    def test_a_resolver_argument_no_entry_of_its_class_binds_stays_idle(self) -> None:
        facts = [Fact("engine_set_tempo", "bpm", "param", "node", "strip")]
        rule = {"node": {"resolver_of": {"mixer_strip_at.index": "strip"}}}
        c_params = {"mixer_strip_at": {"mixer", "index"}}
        rows, _, idle = gen.derive(facts, {}, rule, c_params)
        self.assertNotIn("mixer_strip_at", [row.c_key for row in rows["node"]])
        self.assertEqual(idle, [("node", "resolver_of.mixer_strip_at.index", "strip")])

    def test_a_quotient_operand_the_entry_does_not_take_binds_nothing(self) -> None:
        facts = [
            Fact("push", "frames", "param", "node", None),
            Fact("push", None, "param", "node", "samples"),
        ]
        rule = {"node": {"quotient_of": {"push.frames": ["samples", "channels"]}}}
        rows, unbound, idle = gen.derive(facts, {}, rule)
        self.assertEqual(rows["node"], [])
        self.assertEqual([u.c_subject for u in unbound], ["frames"])
        self.assertEqual(idle, [("node", "quotient_of.push.frames", "samples / channels")])

    def test_an_entrys_length_of_row_outranks_a_rename_for_the_subject(self) -> None:
        facts = [
            Fact("mix", "channels", "param", "node", "channels"),
            Fact("mix", "num_channels", "param", "node", None),
        ]
        rule = {"node": {"num_channels": "channels", "length_of": {"mix.num_channels": "channels"}}}
        rows, _, idle = gen.derive(facts, {}, rule)
        self.assertEqual(rows["node"], [gen.Row("mix", "num_channels", "channels", True)])
        self.assertEqual(idle, [("node", "num_channels", "channels")])

    def test_a_struct_parameter_declares_its_fields(self) -> None:
        params = gen.load_c_params(gen.ROOT)["project_set_warp_map"]
        self.assertTrue({"desc", "anchors", "anchor_count"} <= params)

    def test_a_length_of_row_binds_when_the_array_and_unbound_count_are_declared(self) -> None:
        facts = [Fact("set_events", "count", "param", "node", None)]
        rule = {"node": {"length_of": {"set_events.count": "events"}}}
        rows, unbound, idle = gen.derive(facts, {}, rule, {"set_events": {"count", "events"}})
        self.assertEqual(rows["node"], [gen.Row("set_events", "count", "events", True)])
        self.assertEqual((unbound, idle), ([], []))

    def test_a_length_of_row_needs_the_array_on_that_entry(self) -> None:
        facts = [Fact("apply", "rendered_count", "param", "python", None)]
        rule = {"python": {"length_of": {"apply.rendered_count": "rendered_mono"}}}
        rows, unbound, idle = gen.derive(facts, {}, rule)
        self.assertEqual(rows["python"], [])
        self.assertEqual(len(unbound), 1)
        self.assertEqual(idle, [("python", "length_of.apply.rendered_count", "rendered_mono")])

    def test_a_length_of_row_binds_a_count_the_extractors_fold_into_its_array(self) -> None:
        facts = [Fact("set_lanes", "lanes", "param", "python", "lanes")]
        rule = {"python": {"length_of": {"set_lanes.lane_count": "lanes"}}}
        rows, unbound, idle = gen.derive(facts, {}, rule, {"set_lanes": {"lanes", "lane_count"}})
        self.assertEqual(rows["python"], [gen.Row("set_lanes", "lane_count", "lanes", True)])
        self.assertEqual((unbound, idle), ([], []))

    def test_a_folded_count_binds_to_an_unpaired_array_the_c_declaration_takes(self) -> None:
        rule = {"node": {"length_of": {"split.note_count": "notes"}}}
        rows, _, idle = gen.derive([], {}, rule, {"split": {"notes", "note_count"}})
        self.assertEqual(rows["node"], [gen.Row("split", "note_count", "notes", True)])
        self.assertEqual(idle, [])

    def test_a_folded_count_needs_the_c_parameter_and_the_array(self) -> None:
        facts = [Fact("set_lanes", "lanes", "param", "python", "lanes")]
        for rule_array, params in (("lanes", {"lanes"}), ("tracks", {"lanes", "lane_count"})):
            rule = {"python": {"length_of": {"set_lanes.lane_count": rule_array}}}
            rows, _, idle = gen.derive(facts, {}, rule, {"set_lanes": params})
            self.assertEqual(rows["python"], [])
            self.assertEqual(idle, [("python", "length_of.set_lanes.lane_count", rule_array)])


if __name__ == "__main__":
    unittest.main()
