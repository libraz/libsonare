"""Stdlib self-tests for the interface-convention check.

Each rule is driven with hand-built inventory records: a clean set passes, then one
planted violation must be named. The framework cases cover the two ways a rule can
go blind -- reading nothing, and a table entry that excuses nothing.
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "check_api_conventions", Path(__file__).resolve().parent / "check_api_conventions.py"
)
assert _SPEC and _SPEC.loader
check = importlib.util.module_from_spec(_SPEC)
# dataclasses resolve their module through sys.modules.
sys.modules[_SPEC.name] = check
_SPEC.loader.exec_module(check)


def rule(name: str):
    return next(r for r in check.RULES if r.name == name)


def member(surface: str, owner: str, name: str, *, ts_kind: str, params=(), static=False) -> dict:
    sigs = [] if ts_kind == "accessor" else [{"params": [{"name": p} for p in params]}]
    return {
        "surface": surface,
        "kind": "method",
        "owner": owner,
        "member": name,
        "name": f"{owner}.{name}",
        "ts_kind": ts_kind,
        "sigs": sigs,
        "static": static,
    }


class GettersRule(unittest.TestCase):
    CLEAN = (
        member("node", "Project", "trackCount", ts_kind="accessor"),
        member("wasm", "Mixer", "stripNonFiniteDiscardCount", ts_kind="method", params=["strip"]),
        member("python", "Project", "track_count", ts_kind="method"),
        member("node", "Project", "toJson", ts_kind="method"),
    )

    def test_clean_surface_passes(self) -> None:
        verdict = check.judge(rule("getters"), self.CLEAN)
        self.assertEqual(verdict.read, 2)
        self.assertFalse(verdict.failed)

    def test_argument_less_count_method_is_named(self) -> None:
        planted = member("wasm", "Project", "soundFontPresetCount", ts_kind="method")
        verdict = check.judge(rule("getters"), [*self.CLEAN, planted])
        self.assertTrue(verdict.failed)
        self.assertEqual([f.key for f in verdict.violations], ["wasm:Project.soundFontPresetCount"])

    def test_static_count_method_is_outside_the_rule(self) -> None:
        factory = member("node", "Pool", "defaultCount", ts_kind="method", static=True)
        self.assertFalse(check.judge(rule("getters"), [*self.CLEAN, factory]).failed)


class Framework(unittest.TestCase):
    def test_reading_nothing_fails(self) -> None:
        verdict = check.judge(rule("getters"), [])
        self.assertEqual(verdict.read, 0)
        self.assertTrue(verdict.failed)

    def test_table_entry_that_excuses_nothing_is_stale(self) -> None:
        base = rule("getters")
        stale = check.Rule(
            base.name,
            base.summary,
            base.population,
            base.violation,
            exempt={"node:Project.trackCount": "reason"},
        )
        verdict = check.judge(stale, GettersRule.CLEAN)
        self.assertEqual(verdict.stale, ["node:Project.trackCount"])
        self.assertTrue(verdict.failed)

    def test_exempt_and_pending_entries_excuse_their_violation(self) -> None:
        base = rule("getters")
        planted = [
            member("node", "A", "xCount", ts_kind="method"),
            member("node", "B", "yCount", ts_kind="method"),
        ]
        tabled = check.Rule(
            base.name,
            base.summary,
            base.population,
            base.violation,
            exempt={"node:A.xCount": "reason"},
            pending={"node:B.yCount": "owner"},
        )
        verdict = check.judge(tabled, planted)
        self.assertFalse(verdict.failed)
        self.assertEqual(
            (verdict.excused, verdict.deferred), (["node:A.xCount"], ["node:B.yCount"])
        )


if __name__ == "__main__":
    unittest.main()
