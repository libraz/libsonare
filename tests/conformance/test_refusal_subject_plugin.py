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
        compared, missing, failures = plugin.emitted_pairs("wasm", rows, ({}, set(), {}, {}, set()))
        self.assertEqual(compared, set())
        self.assertEqual(missing, set())
        self.assertEqual(len(failures), 1)


if __name__ == "__main__":
    unittest.main()
