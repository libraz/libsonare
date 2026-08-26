#!/usr/bin/env python3
"""The bank version registry's bump discipline, without building anything.

Everything here is the part that decides whether a voice's version has to move.
The reading side needs a `-DBUILD_TUNING=ON` library and is exercised by
`make bank-versions-check`; what a build cannot check is the rule itself — that
a changed unit bumps, that an unchanged one does not, and that the history is
appended to rather than rewritten.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))

import generate_bank_versions as gbv  # noqa: E402

PATCHES = {"concert_flute", "violin"}


def registry(units: dict, generation: int = 1) -> dict:
    """A registry as the generator would have written it."""
    return {
        "bank_generation": generation,
        "units": {
            name: {
                "kind": gbv.unit_kind(name, PATCHES),
                "version": entry.get("version", 1),
                "fingerprint": gbv.fingerprint(entry["values"]),
                "history": entry.get("history", [{"version": 1, "generation": 1,
                                                  "date": "2026-01-01", "note": "initial"}]),
                "values": entry["values"],
            }
            for name, entry in units.items()
        },
    }


class Fingerprint(unittest.TestCase):
    def test_order_does_not_change_it(self):
        a = gbv.fingerprint({"x": 1.0, "y": 2.0})
        b = gbv.fingerprint({"y": 2.0, "x": 1.0})
        self.assertEqual(a, b)

    def test_a_value_change_changes_it(self):
        self.assertNotEqual(gbv.fingerprint({"x": 1.0}), gbv.fingerprint({"x": 1.5}))

    def test_a_key_change_changes_it(self):
        self.assertNotEqual(gbv.fingerprint({"x": 1.0}), gbv.fingerprint({"y": 1.0}))

    def test_a_change_below_printed_precision_still_changes_it(self):
        """`repr` round-trips a float; a rounded form would let this pass as equal."""
        self.assertNotEqual(
            gbv.fingerprint({"x": 0.1}),
            gbv.fingerprint({"x": 0.1 + 1e-16}),
        )


class Kinds(unittest.TestCase):
    def test_a_drum_note_is_a_drum(self):
        """A drum note is not a GM program, so the program map cannot name it."""
        self.assertEqual(gbv.unit_kind("d038", PATCHES), "drum")
        self.assertEqual(gbv.unit_kind("d000", PATCHES), "drum")

    def test_a_program_map_name_is_a_patch(self):
        self.assertEqual(gbv.unit_kind("concert_flute", PATCHES), "patch")

    def test_a_family_patch_is_a_patch(self):
        self.assertEqual(gbv.unit_kind("fam10", PATCHES), "patch")

    def test_the_gs_and_fallback_tables_are_shared(self):
        for name in ("gs_effects", "gm_fallback_map", "gm_fallback_families"):
            self.assertEqual(gbv.unit_kind(name, PATCHES), "shared")

    def test_an_unlisted_engine_is_shared(self):
        """So a voice engine added to the tree is versioned without editing this."""
        self.assertEqual(gbv.unit_kind("some_new_voice", PATCHES), "shared")


class Bumps(unittest.TestCase):
    def rebuild(self, held, units, note="a note"):
        return gbv.rebuild(held, units, PATCHES, note, "2026-08-26")

    def test_an_empty_registry_starts_everything_at_one(self):
        out, changed, gone = self.rebuild(
            {"bank_generation": 0, "units": {}},
            {"gs_effects": {"kReverbDecayScale": 1.0}})
        self.assertEqual(changed, ["gs_effects"])
        self.assertEqual(gone, [])
        self.assertEqual(out["bank_generation"], 1)
        self.assertEqual(out["units"]["gs_effects"]["version"], 1)

    def test_an_unchanged_bank_bumps_nothing(self):
        values = {"kReverbDecayScale": 1.0}
        held = registry({"gs_effects": {"values": values}})
        out, changed, gone = self.rebuild(held, {"gs_effects": dict(values)})
        self.assertEqual((changed, gone), ([], []))
        self.assertEqual(out["bank_generation"], 1, "an idle run must not age the bank")
        self.assertEqual(out["units"]["gs_effects"]["version"], 1)
        self.assertEqual(len(out["units"]["gs_effects"]["history"]), 1)

    def test_only_the_changed_unit_bumps(self):
        held = registry({
            "gs_effects": {"values": {"kReverbDecayScale": 1.0}},
            "violin": {"values": {"bowed_string.bow_force": 0.4}},
        })
        out, changed, _ = self.rebuild(held, {
            "gs_effects": {"kReverbDecayScale": 0.93},
            "violin": {"bowed_string.bow_force": 0.4},
        })
        self.assertEqual(changed, ["gs_effects"])
        self.assertEqual(out["units"]["gs_effects"]["version"], 2)
        self.assertEqual(out["units"]["violin"]["version"], 1)

    def test_a_bump_appends_to_the_history_rather_than_replacing_it(self):
        """The history is the whole point; a rewrite loses what a version meant."""
        held = registry({"gs_effects": {"values": {"kReverbDecayScale": 1.0}}})
        out, _, _ = self.rebuild(
            held, {"gs_effects": {"kReverbDecayScale": 0.93}}, note="GS reverb decay")
        history = out["units"]["gs_effects"]["history"]
        self.assertEqual([h["version"] for h in history], [1, 2])
        self.assertEqual(history[0]["note"], "initial")
        self.assertEqual(history[1]["note"], "GS reverb decay")
        self.assertEqual(history[1]["generation"], 2)
        self.assertEqual(history[1]["date"], "2026-08-26")

    def test_a_bump_with_no_note_says_so(self):
        """The version says a voice moved and only the note can say why."""
        held = registry({"gs_effects": {"values": {"kReverbDecayScale": 1.0}}})
        out, _, _ = self.rebuild(held, {"gs_effects": {"kReverbDecayScale": 0.93}}, note="")
        self.assertEqual(out["units"]["gs_effects"]["history"][-1]["note"], "unrecorded")

    def test_the_generation_moves_once_however_many_units_moved(self):
        held = registry({
            "gs_effects": {"values": {"a": 1.0}},
            "violin": {"values": {"b": 1.0}},
        })
        out, changed, _ = self.rebuild(held, {"gs_effects": {"a": 2.0},
                                              "violin": {"b": 2.0}})
        self.assertEqual(changed, ["gs_effects", "violin"])
        self.assertEqual(out["bank_generation"], 2)
        for name in changed:
            self.assertEqual(out["units"][name]["history"][-1]["generation"], 2)

    def test_a_unit_that_left_the_bank_is_reported_and_dropped(self):
        held = registry({
            "gs_effects": {"values": {"a": 1.0}},
            "retired_voice": {"values": {"b": 1.0}},
        })
        out, changed, gone = self.rebuild(held, {"gs_effects": {"a": 1.0}})
        self.assertEqual(gone, ["retired_voice"])
        self.assertEqual(changed, [])
        self.assertNotIn("retired_voice", out["units"])
        self.assertEqual(out["bank_generation"], 2, "the bank changed shape")

    def test_the_values_are_recorded_so_the_diff_says_what_moved(self):
        out, _, _ = self.rebuild(
            {"bank_generation": 0, "units": {}},
            {"gs_effects": {"kReverbDecayScale": 0.93, "kReverbDampingScale": 1.0}})
        self.assertEqual(out["units"]["gs_effects"]["values"],
                         {"kReverbDampingScale": 1.0, "kReverbDecayScale": 0.93})


if __name__ == "__main__":
    unittest.main()
