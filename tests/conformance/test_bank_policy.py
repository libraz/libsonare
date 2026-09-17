"""Stdlib self-tests for the bank-policy check.

Every case drives :func:`check_bank_policy.evaluate` -- the function the
shipping entry point calls -- over a copy of the real files with one edit, so a
perturbation is demonstrated rather than asserted about. No file in the tree is
touched.

Two controls carry more weight than the rest.

The first is the vacuity one. An empty policy satisfies every membership rule
here: nothing is named, so nothing fails to resolve, and the check reports a
clean bank. :class:`FloorTest` puts exactly that tree through the check with
the floors switched off and asserts that neither membership class fires -- so
the floors are shown to be the thing doing the work rather than declared to be.

The second is the two number spaces. `kits: [3]` and `programs: [3]` name
different things, and a check reading them as one would pass a kit number no GS
map defines while quietly ranking the honky-tonk piano. :class:`KitNumberTest`
asserts the failure names the kit, and asserts in the same fixture that the
melodic program of that number still resolves on its own.

The render class has no policy-only positive control, and that is a property of
the field rules rather than of the tool: no input satisfying every rule above
currently kills `status.py`. It is guarded by substituting the renderer, which
demonstrates the one thing that could go wrong silently -- a raise being
swallowed and read as a run that found nothing.
"""

from __future__ import annotations

import copy
import importlib.util
import json
import shutil
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

_SPEC = importlib.util.spec_from_file_location(
    "check_bank_policy", Path(__file__).resolve().parent / "check_bank_policy.py"
)
assert _SPEC and _SPEC.loader
check = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(check)

POLICY = check.POLICY
BANK = check.BANK
GS_LAYER = check.GS_LAYER
FILES = (POLICY, BANK, GS_LAYER)

NO_FLOOR: dict[str, int] = {}
NO_CEILING: dict[str, int] = {}


class _CopiedTree(unittest.TestCase):
    """A throwaway tree holding the real files, with the policy rewritten."""

    def tree(self, policy=None, bank=None, omit: tuple[str, ...] = (),
             captures: dict[str, dict] | None = None) -> Path:
        """@p policy and @p bank are callables taking the loaded object.

        @p captures writes a synthetic capture directory rather than copying the
        129 real ones, so a case about the source classification carries only
        the definitions it is about. Given none, the directory is absent — which
        the floors report and every case here therefore switches off.
        """
        root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, root)
        for name, definition in (captures or {}).items():
            path = root / check.CAPTURE_DIR / f"{name}.json"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(definition, indent=2) + "\n", encoding="utf-8")
        edits = {POLICY: policy, BANK: bank}
        for relative in FILES:
            if relative in omit:
                continue
            source = check.ROOT / relative
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            edit = edits.get(relative)
            if edit is None:
                shutil.copyfile(source, destination)
                continue
            loaded = json.loads(source.read_text(encoding="utf-8"))
            edit(loaded)
            destination.write_text(json.dumps(loaded, indent=2, ensure_ascii=False) + "\n",
                                   encoding="utf-8")
        return root

    def classes(self, failures) -> list[str]:
        return [heading for heading, _ in failures]

    def lines(self, failures, fragment: str) -> list[str]:
        matched = [lines for heading, lines in failures if fragment in heading]
        self.assertEqual(len(matched), 1,
                         f"expected one `{fragment}` class, got {self.classes(failures)}")
        return matched[0]

    def absent(self, failures, fragment: str) -> None:
        self.assertNotIn(fragment, " | ".join(self.classes(failures)))


class ShippingTreeTest(unittest.TestCase):
    def test_the_tree_as_it_stands_passes(self) -> None:
        self.assertEqual(check.evaluate(), [])

    def test_every_population_is_reached_and_clears_its_floor(self) -> None:
        scan = check.Scan()
        check._renders(scan, check._import_status())
        measured = check._measured(scan)
        self.assertEqual(sorted(measured), sorted(check.FLOOR))
        for name, minimum in check.FLOOR.items():
            with self.subTest(population=name):
                self.assertGreaterEqual(measured[name], minimum)

    def test_the_kit_table_is_read_at_its_own_declared_extent(self) -> None:
        """26 rhythm sets, against a bank that carries one kit row."""
        kits = check.gs_kit_programs(check.ROOT)
        self.assertEqual(len(kits), 26)
        self.assertEqual(kits[0], "Standard")
        self.assertEqual(len(check.Scan().kits), 1)

    def test_the_declared_goals_are_put_through_the_shipping_tool(self) -> None:
        """A goal nothing was run against is not a goal this check covered."""
        scan = check.Scan()
        self.assertEqual(check._renders(scan, check._import_status()), [])
        self.assertEqual(scan.goals_run, sorted(scan.policy["goals"]))


class ProgramNumberTest(_CopiedTree):
    def test_a_program_past_127_is_reported(self) -> None:
        def edit(pol):
            pol["tiers"][0]["programs"].append(128)

        lines = self.lines(check.evaluate(self.tree(edit), NO_FLOOR, NO_CEILING), "outside the GM range")
        self.assertEqual(len(lines), 1, lines)
        self.assertIn("program 128", lines[0])

    def test_a_negative_program_is_reported(self) -> None:
        def edit(pol):
            pol["goals"]["1.8.0"]["programs"].append(-1)

        lines = self.lines(check.evaluate(self.tree(edit), NO_FLOOR, NO_CEILING), "outside the GM range")
        self.assertIn("goal `1.8.0`", lines[0])

    def test_an_out_of_range_number_is_not_also_reported_as_unresolved(self) -> None:
        """It resolves against nothing by construction; two lines would read as two faults."""
        failures = check.evaluate(self.tree(lambda p: p["tiers"][0]["programs"].append(200)),
                                  NO_FLOOR, NO_CEILING)
        self.absent(failures, "name a slot the bank has no row for")


class TierTest(_CopiedTree):
    def test_two_tiers_at_one_rank_are_reported(self) -> None:
        lines = self.lines(
            check.evaluate(self.tree(lambda p: p["tiers"][1].__setitem__("rank", 1)), NO_FLOOR, NO_CEILING),
            "cannot express an order",
        )
        self.assertTrue(any("unique and contiguous from 1" in line for line in lines), lines)

    def test_ranks_that_do_not_start_at_one_are_reported(self) -> None:
        def edit(pol):
            for tier in pol["tiers"]:
                tier["rank"] += 1

        lines = self.lines(check.evaluate(self.tree(edit), NO_FLOOR, NO_CEILING), "cannot express an order")
        self.assertTrue(any("[2, 3, 4]" in line for line in lines), lines)

    def test_no_default_tier_is_reported(self) -> None:
        def edit(pol):
            for tier in pol["tiers"]:
                tier.pop("default", None)

        lines = self.lines(check.evaluate(self.tree(edit), NO_FLOOR, NO_CEILING), "cannot express an order")
        self.assertTrue(any("0 tier(s) carry `default: true`" in line for line in lines), lines)

    def test_two_default_tiers_are_reported(self) -> None:
        lines = self.lines(
            check.evaluate(self.tree(lambda p: p["tiers"][0].__setitem__("default", True)),
                           NO_FLOOR, NO_CEILING),
            "cannot express an order",
        )
        self.assertTrue(any("2 tier(s) carry" in line for line in lines), lines)

    def test_a_nameless_tier_is_reported(self) -> None:
        lines = self.lines(
            check.evaluate(self.tree(lambda p: p["tiers"][0].pop("name")), NO_FLOOR, NO_CEILING),
            "cannot express an order",
        )
        self.assertTrue(any("carries no `name`" in line for line in lines), lines)

    def test_an_invented_tier_key_is_reported(self) -> None:
        """The policy is read through `.get()`, so a misspelling is silent."""
        lines = self.lines(
            check.evaluate(self.tree(lambda p: p["tiers"][0].__setitem__("program", [7])),
                           NO_FLOOR, NO_CEILING),
            "cannot express an order",
        )
        self.assertTrue(any("which nothing reads" in line for line in lines), lines)

    def test_a_tier_that_is_not_an_object_is_reported_rather_than_raised(self) -> None:
        lines = self.lines(
            check.evaluate(self.tree(lambda p: p["tiers"].append("core")), NO_FLOOR, NO_CEILING),
            "cannot express an order",
        )
        self.assertTrue(any("not an object" in line for line in lines), lines)


class KitNumberTest(_CopiedTree):
    """A kit is a second number space over the same integers."""

    def test_a_kit_number_no_gs_map_defines_is_reported(self) -> None:
        lines = self.lines(
            check.evaluate(self.tree(lambda p: p["tiers"][0].__setitem__("kits", [3])), NO_FLOOR, NO_CEILING),
            "name a slot the bank has no row for",
        )
        self.assertEqual(len(lines), 1, lines)
        self.assertIn("kit 3", lines[0])
        self.assertIn("not a rhythm-part program", lines[0])

    def test_the_melodic_program_of_that_number_still_resolves_on_its_own(self) -> None:
        """The control: read as one space, `kits: [3]` would rank the honky-tonk piano.

        The core tier already names program 3, so the number sits in both lists
        at once -- which is the case a single space cannot represent, and the
        reason the two are tagged where they are collected rather than resolved.
        """
        scan = check.Scan(self.tree(lambda p: p["tiers"][0].__setitem__("kits", [3])))
        self.assertIn(3, scan.capitals)
        self.assertNotIn(3, scan.gs_kits)
        self.assertEqual(sorted(s.kind for s in scan.tier_slots if s.number == 3),
                         ["kit", "program"])

    def test_a_real_kit_with_no_row_in_the_bank_is_reported(self) -> None:
        """Kit 8 is the Room set. The bank's kit rows come from the captures."""
        lines = self.lines(
            check.evaluate(self.tree(lambda p: p["goals"]["1.8.0"].__setitem__("kits", [0, 8])),
                           NO_FLOOR, NO_CEILING),
            "name a slot the bank has no row for",
        )
        self.assertEqual(len(lines), 1, lines)
        self.assertIn("kit 8", lines[0])
        self.assertIn("Room", lines[0])

    def test_the_kit_the_policy_does_name_resolves_both_ways(self) -> None:
        scan = check.Scan()
        self.assertIn(0, scan.gs_kits)
        self.assertIn(0, scan.kits)


class GoalTest(_CopiedTree):
    def test_a_stage_between_two_rungs_is_reported(self) -> None:
        lines = self.lines(
            check.evaluate(self.tree(lambda p: p["goals"]["1.8.0"].__setitem__("stage", 0.7)),
                           NO_FLOOR, NO_CEILING),
            "do not say what they ask for",
        )
        self.assertEqual(len(lines), 1, lines)
        self.assertIn("not a rung of the ladder", lines[0])

    def test_a_goal_key_that_would_widen_it_to_variations_is_reported(self) -> None:
        """A goal names capital tones and kits. `banks` is silently ignored today."""
        lines = self.lines(
            check.evaluate(self.tree(lambda p: p["goals"]["1.8.0"].__setitem__("banks", [8])),
                           NO_FLOOR, NO_CEILING),
            "do not say what they ask for",
        )
        self.assertTrue(any("banks" in line and "nothing reads" in line for line in lines), lines)

    def test_a_goal_naming_a_program_the_bank_carries_only_as_a_variation_is_reported(self) -> None:
        def drop_capital(bank):
            bank["voices"] = [r for r in bank["voices"]
                              if not (r["program"] == 0 and not r["kit"] and not r["bank"])]

        failures = check.evaluate(self.tree(bank=drop_capital), NO_FLOOR, NO_CEILING)
        lines = self.lines(failures, "do not say what they ask for")
        self.assertTrue(any("only as variation(s)" in line for line in lines), lines)
        self.lines(failures, "name a slot the bank has no row for")

    def test_a_documentation_key_inside_goals_is_not_a_goal(self) -> None:
        """`_` documents a block everywhere else in this tree, so it must here.

        It reached `goal_progress` as a goal with no `stage` and killed the only
        tool that reads the policy, on a file that parses and satisfies every
        field rule above.
        """
        failures = check.evaluate(
            self.tree(lambda p: p["goals"].__setitem__("_", "which release asks for what")),
            NO_FLOOR,
        )
        self.assertEqual(failures, [], self.classes(failures))

    def test_a_goal_that_is_not_an_object_is_reported_rather_than_raised(self) -> None:
        lines = self.lines(
            check.evaluate(self.tree(lambda p: p["goals"].__setitem__("2.0.0", "later")),
                           NO_FLOOR, NO_CEILING),
            "do not say what they ask for",
        )
        self.assertTrue(any("not an object" in line for line in lines), lines)


class ReferenceLayerTest(_CopiedTree):
    def test_an_axis_with_neither_answer_is_reported(self) -> None:
        lines = self.lines(
            check.evaluate(
                self.tree(lambda p: p["reference_layer"]["kits"].__setitem__("timbre", "either")),
                NO_FLOOR,
            ),
            "which reference a slot is aimed at",
        )
        self.assertEqual(len(lines), 1, lines)
        self.assertIn("kits.timbre", lines[0])

    def test_a_branch_with_no_reason_is_reported(self) -> None:
        lines = self.lines(
            check.evaluate(
                self.tree(lambda p: p["reference_layer"]["machine_defined"].pop("reason")),
                NO_FLOOR,
            ),
            "which reference a slot is aimed at",
        )
        self.assertTrue(any("carries no reason" in line for line in lines), lines)

    def test_a_missing_default_branch_is_reported(self) -> None:
        lines = self.lines(
            check.evaluate(self.tree(lambda p: p["reference_layer"].pop("default")), NO_FLOOR, NO_CEILING),
            "which reference a slot is aimed at",
        )
        self.assertTrue(any("no `default` branch" in line for line in lines), lines)


class ApproximatedTest(_CopiedTree):
    """The block is empty today, and an entry is what it has to survive."""

    ENTRY: ClassVar[dict[str, str]] = {
        "answered_by": "p105-banjo",
        "reason": "no plucked-membrane mechanism; the banjo's bridge loads a head rather "
                  "than a soundboard",
    }

    def test_a_well_formed_entry_passes(self) -> None:
        failures = check.evaluate(
            self.tree(lambda p: p["approximated"].__setitem__("p105-banjo", self.ENTRY)),
            NO_FLOOR,
        )
        self.assertEqual(failures, [], self.classes(failures))

    def test_an_entry_naming_no_voice_is_reported(self) -> None:
        lines = self.lines(
            check.evaluate(
                self.tree(lambda p: p["approximated"].__setitem__("p105-banjoo", self.ENTRY)),
                NO_FLOOR,
            ),
            "do not say what they are",
        )
        self.assertTrue(any("matches no voice" in line for line in lines), lines)

    def test_an_entry_with_no_reason_is_reported(self) -> None:
        entry = dict(self.ENTRY, reason="  ")
        lines = self.lines(
            check.evaluate(self.tree(lambda p: p["approximated"].__setitem__("p105-banjo", entry)),
                           NO_FLOOR, NO_CEILING),
            "do not say what they are",
        )
        self.assertTrue(any("carries no reason" in line for line in lines), lines)

    def test_an_entry_answered_by_nothing_is_reported(self) -> None:
        entry = {"reason": "no mechanism"}
        lines = self.lines(
            check.evaluate(self.tree(lambda p: p["approximated"].__setitem__("p105-banjo", entry)),
                           NO_FLOOR, NO_CEILING),
            "do not say what they are",
        )
        self.assertTrue(any("names no `answered_by`" in line for line in lines), lines)

    def test_an_absent_block_is_not_the_same_as_an_empty_one(self) -> None:
        """Empty is the claim that every slot has a patch written for it."""
        lines = self.lines(
            check.evaluate(self.tree(lambda p: p.pop("approximated")), NO_FLOOR, NO_CEILING),
            "do not say what they are",
        )
        self.assertTrue(any("declares no `approximated` block" in line for line in lines), lines)


class SourceClassTest(_CopiedTree):
    """What answered a capture is a classification in the tracked file.

    The product and its presets live in the untracked `<id>.local.json`, so the
    class is the only fact a check can reach — and the one the policy needs,
    because a slot whose timbre axis says `machine` has nothing behind it for a
    recording to be made of.
    """

    def _defs(self, **classes) -> dict[str, dict]:
        return {name: {"id": name, "program": program, **extra}
                for name, (program, extra) in classes.items()}

    def test_a_capture_with_no_source_class_is_unclassified_and_not_a_default(self) -> None:
        captures = {"pad_warm": {"id": "pad_warm", "program": 89}}
        lines = self.lines(check.evaluate(self.tree(captures=captures), NO_FLOOR, NO_CEILING),
                           "what kind of source answered them")
        self.assertEqual(len(lines), 1, lines)
        self.assertIn("declares no `source_class`", lines[0])

    def test_a_class_outside_the_three_is_reported(self) -> None:
        captures = {"pad_warm": {"id": "pad_warm", "program": 89, "source_class": "sampler"}}
        lines = self.lines(check.evaluate(self.tree(captures=captures), NO_FLOOR, NO_CEILING),
                           "what kind of source answered them")
        self.assertIn("'sampler'", lines[0])

    def test_a_classified_capture_passes(self) -> None:
        captures = {"pad_warm": {"id": "pad_warm", "program": 89, "source_class": "library"}}
        failures = check.evaluate(self.tree(captures=captures), NO_FLOOR, NO_CEILING)
        self.absent(failures, "what kind of source answered them")

    def test_a_definition_that_does_not_parse_is_reported_rather_than_skipped(self) -> None:
        root = self.tree(captures={"pad_warm": {"id": "pad_warm", "program": 89,
                                                "source_class": "library"}})
        (root / check.CAPTURE_DIR / "broken.json").write_text("{", encoding="utf-8")
        lines = self.lines(check.evaluate(root, NO_FLOOR, NO_CEILING), "what kind of source answered them")
        self.assertTrue(any("broken.json" in line for line in lines), lines)

    def test_the_untracked_overlay_is_never_opened(self) -> None:
        """Nothing mechanical may depend on a file that is not in the repository."""
        root = self.tree(captures={"pad_warm": {"id": "pad_warm", "program": 89,
                                                "source_class": "library"}})
        (root / check.CAPTURE_DIR / "pad_warm.local.json").write_text(
            json.dumps({"id": "pad_warm", "plugin": "aumu:xxxx:yyyy"}), encoding="utf-8")
        scan = check.Scan(root)
        self.assertEqual(sorted(scan.captures), ["pad_warm"])
        self.assertNotIn("plugin", scan.captures["pad_warm"])

    def test_the_census_separates_the_four_answers_a_slot_can_have(self) -> None:
        captures = {
            "pad_warm": {"id": "pad_warm", "program": 89, "source_class": "library"},
            "lead_square": {"id": "lead_square", "program": 80, "source_class": "module"},
            "synth_bass_1": {"id": "synth_bass_1", "program": 38},
        }

        def one_slot_each(pol):
            pol["reference_layer"]["machine_defined"]["programs"] = [38, 80, 89, 90]

        got = check.machine_answers(check.Scan(self.tree(one_slot_each, captures=captures)))
        self.assertEqual(got["module"], 1)
        self.assertEqual(got["library"], 1)
        self.assertEqual(got["unclassified"], 1)
        self.assertEqual(got["uncaptured"], 1)

    def test_a_slot_answered_by_the_wrong_class_is_counted_and_never_failed_on(self) -> None:
        """Acquiring a module reference is open-ended work against hardware.

        A red build here could not be cleared by anyone reading it, which is the
        shape this file refuses everywhere else. It is counted instead, in the
        output of every `make conformance` run.
        """
        captures = {"pad_warm": {"id": "pad_warm", "program": 89, "source_class": "library"}}

        def one_slot(pol):
            pol["reference_layer"]["machine_defined"]["programs"] = [89]

        root = self.tree(one_slot, captures=captures)
        self.assertEqual(check.machine_answers(check.Scan(root))["library"], 1)
        self.assertEqual(check.evaluate(root, NO_FLOOR, NO_CEILING), [])

    def test_a_variation_capture_does_not_answer_a_capital_slot(self) -> None:
        """A machine-defined program is a capital tone; a variation is not it."""
        captures = {"pad_warm_b8": {"id": "pad_warm_b8", "program": 89, "bank": 8,
                                    "source_class": "module"}}

        def one_slot(pol):
            pol["reference_layer"]["machine_defined"]["programs"] = [89]

        got = check.machine_answers(check.Scan(self.tree(one_slot, captures=captures)))
        self.assertEqual(got["uncaptured"], 1)
        self.assertEqual(got["module"], 0)

    def test_a_module_capture_declaring_a_room_is_a_contradiction(self) -> None:
        """The module is captured with every effect off, so one of the two is wrong."""
        captures = {"pad_warm": {"id": "pad_warm", "program": 89,
                                 "source_class": "module", "room": "present"}}
        lines = self.lines(check.evaluate(self.tree(captures=captures), NO_FLOOR, NO_CEILING),
                           "what kind of source answered them")
        self.assertEqual(len(lines), 1, lines)
        self.assertIn("declaring a room", lines[0])

    def test_a_module_capture_with_no_room_declared_is_not_a_contradiction(self) -> None:
        """Unclassified is nobody having answered, which is a gap and not a clash."""
        for definition in ({"source_class": "module"},
                           {"source_class": "module", "room": "none"},
                           {"source_class": "library", "room": "present"}):
            with self.subTest(definition=definition):
                captures = {"pad_warm": {"id": "pad_warm", "program": 89, **definition}}
                self.absent(check.evaluate(self.tree(captures=captures), NO_FLOOR, NO_CEILING),
                            "what kind of source answered them")

    def test_the_shipping_captures_are_all_classified(self) -> None:
        scan = check.Scan()
        self.assertEqual(check._captures(scan), [])
        self.assertEqual(len(scan.captures), check._measured(scan)["captures_classified"])

    def test_the_shipping_machine_defined_slots_are_counted(self) -> None:
        """Every one is captured, and the census says what from."""
        got = check.machine_answers(check.Scan())
        self.assertEqual(got["uncaptured"], 0)
        self.assertEqual(got["unclassified"], 0)
        self.assertEqual(sum(got.values()), len(check.Scan().layer_slots))


class MachineRatchetTest(_CopiedTree):
    """The count that may fall and may not rise.

    A machine-defined slot answered by something other than the module is not
    clearable today — forty module references is open-ended work against
    hardware — so the forty are carried rather than failed on. What a new one
    is is clearable, by whoever added the capture, and that is what this holds.
    """

    def _tree(self, programs, captures):
        def edit(pol):
            pol["reference_layer"]["machine_defined"]["programs"] = programs

        return self.tree(edit, captures=captures)

    def test_the_recorded_count_is_what_the_shipping_tree_reaches(self) -> None:
        self.assertEqual(check._measured_ceilings(check.Scan()), dict(check.CEILING))

    def test_a_rise_is_reported_and_names_the_direction(self) -> None:
        root = self._tree([80, 89], {
            "lead_square": {"id": "lead_square", "program": 80, "source_class": "library"},
            "pad_warm": {"id": "pad_warm", "program": 89, "source_class": "library"},
        })
        lines = self.lines(check.evaluate(root, NO_FLOOR, {"machine_slots_without_a_module_reference": 1}),
                           "recorded count has moved")
        self.assertEqual(len(lines), 1, lines)
        self.assertIn("2, above the 1 recorded", lines[0])

    def test_an_uncaptured_slot_counts_against_the_ratchet_too(self) -> None:
        """Otherwise deleting a capture would clear the slot instead of the work."""
        root = self._tree([80, 89], {
            "lead_square": {"id": "lead_square", "program": 80, "source_class": "module"},
        })
        self.assertEqual(check._measured_ceilings(check.Scan(root)),
                         {"machine_slots_without_a_module_reference": 1})

    def test_a_module_capture_lowers_it_and_the_stale_ceiling_is_reported(self) -> None:
        """A ratchet left above the tree is where the next wrong answer hides."""
        root = self._tree([80, 89], {
            "lead_square": {"id": "lead_square", "program": 80, "source_class": "module"},
            "pad_warm": {"id": "pad_warm", "program": 89, "source_class": "module"},
        })
        lines = self.lines(check.evaluate(root, NO_FLOOR, {"machine_slots_without_a_module_reference": 1}),
                           "recorded count has moved")
        self.assertEqual(len(lines), 1, lines)
        self.assertIn("0, below the 1 recorded", lines[0])
        self.assertIn("re-record it at 0", lines[0])

    def test_matching_the_ceiling_passes(self) -> None:
        root = self._tree([80, 89], {
            "lead_square": {"id": "lead_square", "program": 80, "source_class": "module"},
            "pad_warm": {"id": "pad_warm", "program": 89, "source_class": "library"},
        })
        self.assertEqual(
            check.evaluate(root, NO_FLOOR, {"machine_slots_without_a_module_reference": 1}), [])


class DecisionLeakTest(_CopiedTree):
    """`tools/voice-status.json` holds facts. A tier or a goal is a decision."""

    def test_a_row_carrying_a_tier_is_reported(self) -> None:
        def edit(bank):
            bank["voices"][0]["tier"] = "core"

        lines = self.lines(check.evaluate(self.tree(bank=edit), NO_FLOOR, NO_CEILING),
                           "generated rows carry a decision")
        self.assertEqual(len(lines), 1, lines)
        self.assertIn("tier", lines[0])

    def test_a_decision_hidden_inside_the_axes_is_reported(self) -> None:
        def edit(bank):
            bank["voices"][2]["axes"]["priority"] = 1

        lines = self.lines(check.evaluate(self.tree(bank=edit), NO_FLOOR, NO_CEILING),
                           "generated rows carry a decision")
        self.assertIn("priority", lines[0])

    def test_the_shipping_rows_carry_none_of_them(self) -> None:
        self.assertEqual(check._decisions(check.Scan()), [])


class RenderTest(_CopiedTree):
    """The tool is asked to run, and a raise from it may not read as a clean run."""

    class _Raises:
        @staticmethod
        def render_table(*_args, **_kwargs):
            raise KeyError("name")

    def test_a_renderer_that_raises_is_reported_rather_than_swallowed(self) -> None:
        scan = check.Scan()
        lines = check._renders(scan, self._Raises)
        self.assertEqual(len(lines), 1 + len(scan.policy["goals"]), lines)
        self.assertTrue(all("KeyError" in line for line in lines), lines)
        self.assertEqual(scan.goals_run, [])

    def test_a_tool_that_cannot_be_imported_is_reported(self) -> None:
        scan = check.Scan()
        lines = check._renders(scan, None)
        self.assertEqual(len(lines), 1, lines)
        self.assertIn("could not be imported", lines[0])

    def test_a_tool_that_never_ran_fails_the_floor_rather_than_reading_clean(self) -> None:
        """"the tool found nothing" and "the tool was never asked" are one output."""
        scan = check.Scan()
        check._renders(scan, None)
        self.assertEqual(check._measured(scan)["goals_run"], 0)
        self.assertTrue(check._floors(scan, check.FLOOR))

    def test_a_goal_the_tool_does_not_resolve_is_reported(self) -> None:
        class Silent:
            @staticmethod
            def render_table(*_args, **_kwargs):
                print("  nothing to say")

        scan = check.Scan()
        lines = check._renders(scan, Silent)
        self.assertEqual(len(lines), len(scan.policy["goals"]), lines)
        self.assertIn("printed no line for the goal", lines[0])


class FloorTest(_CopiedTree):
    def test_an_empty_policy_satisfies_every_membership_rule(self) -> None:
        """The control the floors exist for: nothing named is nothing unresolved."""
        def empty(pol):
            pol["tiers"] = []
            pol["goals"] = {}
            pol["reference_layer"]["machine_defined"]["programs"] = []

        failures = check.evaluate(self.tree(empty), NO_FLOOR, NO_CEILING)
        self.absent(failures, "name a slot the bank has no row for")
        self.absent(failures, "outside the GM range")

    def test_the_same_tree_fails_every_floor_it_emptied(self) -> None:
        def empty(pol):
            pol["tiers"] = []
            pol["goals"] = {}
            pol["reference_layer"]["machine_defined"]["programs"] = []

        lines = self.lines(check.evaluate(self.tree(empty)), "no longer reaches the population")
        named = {line.split(":")[0].strip() for line in lines}
        # The tree carries no capture directory either, which is the same shape:
        # an absent population agrees with every rule that ranges over it.
        self.assertEqual(named, {"tier_members", "goal_members", "machine_defined", "goals_run",
                                 "captures", "captures_classified"})

    def test_a_bank_with_no_rows_is_reported_before_every_rule_it_would_empty(self) -> None:
        failures = check.evaluate(self.tree(bank=lambda b: b.__setitem__("voices", [])))
        anchors = self.lines(failures, "could not be read")
        self.assertTrue(any("no voices" in line for line in anchors), anchors)
        self.lines(failures, "no longer reaches the population")

    def test_an_unparsable_kit_table_holds_a_kit_number_to_nothing(self) -> None:
        """The extent is read off the declaration, so an appended kit is a miss."""
        root = self.tree()
        path = root / GS_LAYER
        text = path.read_text(encoding="utf-8")
        self.assertIn("std::array<GsDrumKit, 26> kGsDrumKits", text)
        path.write_text(text.replace("std::array<GsDrumKit, 26> kGsDrumKits",
                                     "std::array<GsDrumKit, 27> kGsDrumKits", 1),
                        encoding="utf-8")
        failures = check.evaluate(root)
        anchors = self.lines(failures, "could not be read")
        self.assertTrue(any("kGsDrumKits" in line for line in anchors), anchors)
        floors = self.lines(failures, "no longer reaches the population")
        self.assertTrue(any("gs_kits: found 0" in line for line in floors), floors)

    def test_the_floors_are_what_the_shipping_tree_reaches(self) -> None:
        """A floor above the measurement would fail on the tree it was sized on."""
        scan = check.Scan()
        check._renders(scan, check._import_status())
        measured = check._measured(scan)
        self.assertTrue(all(check.FLOOR[name] <= value for name, value in measured.items()),
                        (check.FLOOR, measured))


class SlotTest(unittest.TestCase):
    """The number spaces are tagged at the point they are collected."""

    def test_a_tier_names_both_spaces_and_they_stay_apart(self) -> None:
        policy = {"tiers": [{"name": "core", "rank": 1, "programs": [0, 1], "kits": [0]}]}
        slots = check.tier_slots(policy)
        self.assertEqual([(s.kind, s.number) for s in slots],
                         [("program", 0), ("program", 1), ("kit", 0)])

    def test_a_non_integer_member_is_not_collected_as_a_number(self) -> None:
        """It resolves to no row, which is what the membership rule reports."""
        policy = {"tiers": [{"name": "core", "rank": 1, "programs": [0, "1"]}]}
        self.assertEqual([s.number for s in check.tier_slots(policy)], [0])

    def test_the_slot_collectors_survive_a_block_of_the_wrong_shape(self) -> None:
        for policy in ({"tiers": "core"}, {"goals": []}, {"reference_layer": []}):
            with self.subTest(policy=policy):
                self.assertEqual(check.tier_slots(copy.deepcopy(policy)), [])
                self.assertEqual(check.goal_slots(copy.deepcopy(policy)), [])
                self.assertEqual(check.layer_slots(copy.deepcopy(policy)), [])


if __name__ == "__main__":
    unittest.main()
