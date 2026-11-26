"""Unit tests for refusal_parity.compare over hand-built capture rows."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import refusal_parity as rp


def row(entry: str, message: str, args: list, cls: str = "RangeError") -> dict:
    return {"entry": entry, "errorClass": cls, "message": message, "args": args}


class SubjectTest(unittest.TestCase):
    def test_reads_the_path_after_the_entry_prefix(self) -> None:
        self.assertEqual(rp.subject_of("f: chords[1].root must be an integer"), "chords[1].root")
        self.assertEqual(rp.subject_of("Project.addTrack: kind must be one of a, b"), "kind")
        self.assertIsNone(rp.subject_of("f: [object] is wrong"))


class CompareTest(unittest.TestCase):
    def test_joins_on_the_refused_value_and_flags_a_different_text(self) -> None:
        args = [{"chords": [{"root": 0}, {"root": 12}]}]
        node = [row("f", "f: chords[1].root must be an integer in [0, 11]", args)]
        wasm = [row("f", "f: chords[1].root is out of range", args)]
        result = rp.compare(node, wasm, [])
        self.assertEqual(result["compared"], 1)
        self.assertEqual(len(result["differing"]), 1)
        self.assertEqual(result["differing"][0]["input"], {"at": "chords[1].root", "value": 12})

    def test_flags_a_different_class_with_the_same_text(self) -> None:
        args = [{"x": "a"}]
        node = [row("f", "f: x must be a number", args, "TypeError")]
        wasm = [row("f", "f: x must be a number", args, "RangeError")]
        self.assertEqual(len(rp.compare(node, wasm, [])["differing"]), 1)

    def test_does_not_join_two_different_refused_values(self) -> None:
        node = [row("f", "f: x must be finite", [{"x": "$NaN"}])]
        wasm = [row("f", "f: x must be an integer", [{"x": 0.5}])]
        result = rp.compare(node, wasm, [])
        self.assertEqual((result["compared"], result["node_only"], result["wasm_only"]), (0, 1, 1))

    def test_falls_back_to_the_whole_call_when_the_subject_is_positional(self) -> None:
        node = [row("f", "f: sampleRate must be positive", ["$Float32Array:4", 0])]
        wasm = [row("f", "f: sampleRate must be positive", ["$Float32Array:4", 0])]
        result = rp.compare(node, wasm, [])
        self.assertEqual((result["compared"], len(result["differing"])), (1, 0))

    def test_a_parked_entry_excuses_its_pair_and_expires_without_one(self) -> None:
        args = [{"x": 1}]
        node = [row("f", "f: x is wrong", args)]
        wasm = [row("f", "f: x is bad", args)]
        parked = [{"entry": "f", "subject": "x"}, {"entry": "g", "subject": "y"}]
        result = rp.compare(node, wasm, parked)
        self.assertEqual(result["differing"], [])
        self.assertEqual(len(result["excused"]), 1)
        self.assertEqual(result["stale_parked"], [{"entry": "g", "subject": "y"}])

    def test_a_value_that_does_not_decide_the_refusal_is_keyed_by_the_whole_call(self) -> None:
        small = [{"room": [5], "x": 6}]
        large = [{"room": [7], "x": 6}]
        node = [row("f", "f: x must be in [0, 5]", small)]
        wasm = [
            row("f", "f: x must be in [0, 5]", small),
            row("f", "f: x must be in [0, 7]", large),
        ]
        result = rp.compare(node, wasm, [])
        self.assertEqual((result["compared"], result["differing"], result["refined"]), (1, [], 1))

    def test_a_call_that_does_not_decide_the_refusal_is_not_compared(self) -> None:
        args = [0]
        node = [row("f", "f: n must be an integer in [1, 8]", args)]
        wasm = [
            row("f", "f: n must be an integer in [1, 128]", args),
            row("f", "f: n must be an integer in [1, 512]", args),
        ]
        result = rp.compare(node, wasm, [])
        self.assertEqual((result["compared"], result["undecided"], result["differing"]), (0, 1, []))
        result = rp.compare(wasm, wasm, [])
        self.assertEqual((result["compared"], result["undecided"]), (1, 0))

    def test_a_row_without_arguments_is_not_keyed(self) -> None:
        node = [{"entry": "f", "errorClass": "TypeError", "message": "f: x must be a number"}]
        result = rp.compare(node, [], [])
        self.assertEqual(result["unkeyed"]["node"], 1)


if __name__ == "__main__":
    unittest.main()
