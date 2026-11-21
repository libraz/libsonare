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

    def test_the_tree_declarations_are_read(self) -> None:
        params = plugin.header_parameters()
        self.assertGreater(len(params), 500)
        self.assertIn("sample_rate", params["chirp"])
        self.assertIn("overrides", params["master_audio"])


if __name__ == "__main__":
    unittest.main()
