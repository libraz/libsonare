"""Stdlib self-tests for the refusal-subject pytest plugin's pure parts."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import refusal_subject_plugin as plugin


class EntryKeyTest(unittest.TestCase):
    def test_the_prefix_and_one_variant_suffix_are_dropped(self) -> None:
        self.assertEqual(plugin.entry_key("sonare_cqt_with_progress"), "cqt")
        self.assertEqual(plugin.entry_key("sonare_mix_ex"), "mix")
        self.assertEqual(plugin.entry_key("sonare_ex"), "ex")


_PARAMS = {"render": {"engine", "options", "ranges"}}


class MisnamedTest(unittest.TestCase):
    def test_a_member_path_and_an_indexed_path_resolve_to_their_root(self) -> None:
        seen = {("render", "options.total_frames"): 1, ("render", "ranges[2].start"): 1}
        self.assertEqual(plugin.misnamed(seen, _PARAMS), [])

    def test_a_core_spelling_and_an_unknown_entry_are_reported(self) -> None:
        seen = {("render", "total_frames"): 1, ("gone", "engine"): 1}
        self.assertEqual(
            plugin.misnamed(seen, _PARAMS), [("gone", "engine"), ("render", "total_frames")]
        )

    def test_a_json_document_parameter_roots_its_paths_at_its_stem(self) -> None:
        self.assertEqual(
            plugin.document_roots({"config_json", "out_json", "samples"}), {"config", "out"}
        )

    def test_a_document_content_refusal_roots_at_the_document_keys(self) -> None:
        params = {"load": {"document", "json_out"}, "apply": {"params"}}
        seen = {("load", "params.ratio"): 1, ("apply", "version"): 1}
        content = {("load", "params.ratio"), ("apply", "version")}
        self.assertEqual(plugin.misnamed(seen, params, content), [("apply", "version")])
        self.assertEqual(
            plugin.misnamed(seen, params), [("apply", "version"), ("load", "params.ratio")]
        )

    def test_every_json_input_spelling_reads_a_document(self) -> None:
        for names in ({"json", "sample_rate"}, {"scene_json"}, {"document", "json_out"}):
            self.assertTrue(plugin.takes_document(names), names)
        for names in ({"out_json"}, {"json_out", "params"}, {"samples"}):
            self.assertFalse(plugin.takes_document(names), names)

    def test_a_content_refusal_under_a_json_parameter_is_not_checked(self) -> None:
        params = {"from_scene_json": {"json", "sample_rate"}}
        seen = {("from_scene_json", "strips[0].gainDb"): 1}
        self.assertEqual(plugin.misnamed(seen, params, set(seen)), [])

    def test_the_tree_declarations_are_read(self) -> None:
        params = plugin.header_parameters()
        self.assertGreater(len(params), 500)
        self.assertIn("sample_rate", params["chirp"])
        self.assertIn("overrides", params["master_audio"])


class EmittedPairsTest(unittest.TestCase):
    def test_an_unbound_reason_cannot_excuse_an_observed_alias(self):
        key = "engine_set_track_strip_insert_param_by_name"
        facts = (
            {key: {"track_id", "value", "param_name"}},
            {(key, "param_name")},
            {(key, "track_id"): "ref", (key, "value"): "param.value"},
            {},
            set(),
            {},
        )
        rows = [
            {
                "entry": "RealtimeEngine.setStripInsertParam",
                "refusal": {
                    "cEntry": key,
                    "code": 4,
                    "subjects": ["trackId", "value"],
                    "raw": {
                        "subjects": ["track_id", "value"],
                        "code": 4,
                        "callerSpelled": False,
                        "fromC": True,
                    },
                },
            }
        ]
        compared, missing, failures = plugin.emitted_pairs("node", rows, facts)
        self.assertEqual(compared, {(key, "track_id"), (key, "value")})
        self.assertEqual(missing, {(key, "param_name")})
        self.assertEqual(len(failures), 2)

    def test_a_required_record_missing_from_a_bridge_refusal_is_a_failure(self):
        rows = [{"entry": "RealtimeEngine.resolveStripAutomationId", "requireNative": True}]
        compared, missing, failures = plugin.emitted_pairs("wasm", rows, ({}, set(), {}, {}, set(), {}))
        self.assertEqual(compared, set())
        self.assertEqual(missing, set())
        self.assertEqual(len(failures), 1)


    def test_converted_bound_uses_the_selected_argument(self):
        key = "engine_set_capture_timing"
        native = "punch_end_sample"
        facts = (
            {key: {native}},
            {(key, native)},
            {(key, native): "punchEndSample"},
            {},
            {(key, "punch")},
            {},
        )
        raw = {"subjects": [native], "code": 4, "callerSpelled": False, "fromC": True}
        final = {
            "cEntry": key, "code": 4, "raw": raw,
            "subjects": ["punch.endSec"],
            "convertedSubjects": ["punch.endSec"],
        }
        row = {
            "entry": "RealtimeEngine.configureCapture",
            "args": [{"punch": {"startSec": 2, "endSec": 1}}],
            "refusal": final,
        }
        self.assertEqual(
            plugin.emitted_pairs("node", [row], facts), ({(key, native)}, set(), set())
        )
        final["subjects"] = ["punch.startSec"]
        _, _, failures = plugin.emitted_pairs("node", [row], facts)
        self.assertTrue(any("expected 'punch.endSec'" in f for f in failures))
        final["convertedSubjects"] = ["punch.startSec"]
        _, _, failures = plugin.emitted_pairs("node", [row], facts)
        self.assertTrue(any("expected 'punch.endSec'" in f for f in failures))
        raw["subjects"] = ["undeclared"]
        final["convertedSubjects"] = ["punch.endSec"]
        final["subjects"] = ["punch.endSec"]
        _, _, failures = plugin.emitted_pairs("node", [row], facts)
        self.assertTrue(any("'undeclared' names no C argument" in f for f in failures))
        raw["subjects"] = [native]
        final["subjects"] = final["convertedSubjects"] = ["punch.absent"]
        _, _, failures = plugin.emitted_pairs("node", [row], facts)
        self.assertTrue(any("expected 'punchEndSample'" in f for f in failures))

    def test_positional_record_prefix_comes_from_public_signature(self):
        key = "project_set_destination_instrument"
        entry = "Project.setDestinationInstrument"
        facts = (
            {key: {"instrument"}},
            {(key, "instrument")},
            {},
            {},
            {(key, "instrument")},
            {entry: {(1, "instrument")}},
        )
        row = {
            "entry": entry,
            "args": [1, {"kind": "builtin", "config": {"gain": 5}}],
            "refusal": {
                "cEntry": key, "code": 4, "subjects": ["instrument.config.gain"],
                "convertedSubjects": ["instrument.config.gain"],
                "raw": {
                    "subjects": ["instrument.builtin.gain"],
                    "code": 4, "callerSpelled": False, "fromC": True,
                },
            },
        }
        self.assertEqual(
            plugin.emitted_pairs("node", [row], facts), ({(key, "instrument")}, set(), set())
        )

    def test_direct_bridge_can_name_the_declared_request_field(self):
        key = "midi_param_to_cc"
        facts = (
            {key: {"param_id"}},
            {(key, "param_id")},
            {(key, "param_id"): "paramId"},
            {},
            {(key, "paramId")},
            {},
        )
        row = {
            "entry": "midiParamToCc",
            "args": [{"paramId": 0}],
            "refusal": {
                "code": 4,
                "subjects": ["paramId"],
                "raw": {"subjects": ["paramId"], "code": 4, "callerSpelled": True, "fromC": False},
            },
        }
        self.assertEqual(
            plugin.emitted_pairs("node", [row], facts), ({(key, "param_id")}, set(), set())
        )

    def test_direct_core_bag_keeps_its_literal_key_under_the_supplied_root(self):
        key = "streaming_loudness_gain"
        facts = (
            {key: {"config_json"}},
            {(key, "config_json")},
            {(key, "config_json"): "config"},
            {},
            {(key, "config")},
            {},
        )
        row = {
            "entry": "streamingLoudnessGain",
            "args": [{"config": {"no.such.key": 1}}],
            "refusal": {
                "code": 4,
                "form": "unknownKey",
                "subjects": ["config.no.such.key"],
                "raw": {
                    "subjects": ["config.no.such.key"],
                    "code": 4, "callerSpelled": False, "fromC": False,
                },
            },
        }
        self.assertEqual(
            plugin.emitted_pairs("node", [row], facts), ({(key, "config_json")}, set(), set())
        )


if __name__ == "__main__":
    unittest.main()
