"""Self-test for the public-interface checker, over fixtures only.

Run from the repository root: ``python3 -m unittest tools/api/test_check_surface.py``.
No node, no live extraction: every case runs check_surface against
tools/api/testdata/inventory.json, mutated per case, and asserts the exact line
the failure prints.
"""

from __future__ import annotations

import copy
import json
import sys
import tempfile
import types
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import check_surface
import domains
import upgrade_notes

TD = HERE / "testdata"
ROOT = HERE.parents[1]
BASE = json.loads((TD / "inventory.json").read_text(encoding="utf-8"))
LEDGER_FILES = ("c", "python", "cli", "wasm", "node")


def _rec(surface, kind, name, *, owner="", member="", **kw):
    r = {
        "surface": surface,
        "kind": kind,
        "name": name,
        "file": "bindings/x.ts",
        "line": 1,
        "has_doc": True,
        "owner": owner,
        "member": member,
        "domain": "analysis",
        "domain_path": "inherited" if owner else "declaring-file",
        "entries": ["."] if surface == "wasm" else [],
    }
    r.update(kw)
    return r


class CheckerCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.inv = copy.deepcopy(BASE)

    def tearDown(self):
        self.tmp.cleanup()

    def add(self, *records):
        self.inv["records"].extend(records)

    def drop(self, surface, kind, name):
        self.inv["records"] = [
            r
            for r in self.inv["records"]
            if (r["surface"], r["kind"], r["name"]) != (surface, kind, name)
        ]

    def ledgers(self, **files):
        """A ledger dir holding the given surface files; the rest are empty."""
        d = self.dir / "ledgers"
        d.mkdir(exist_ok=True)
        for s in LEDGER_FILES:
            (d / f"{s}.toml").write_text(files.get(s, ""), encoding="utf-8")
        return d

    def run_check(
        self,
        *extra,
        ledger_dir=None,
        mirrors=None,
        allowlist=None,
        domains_file=None,
        dimensionless=None,
    ):
        inv_path = self.dir / "inventory.json"
        inv_path.write_text(json.dumps(self.inv), encoding="utf-8")
        argv = [
            "--inventory-file",
            str(inv_path),
            "--ledger-dir",
            str(ledger_dir or TD / "ledgers"),
            "--rules-file",
            str(TD / "rules.toml"),
            "--mirrors-file",
            str(mirrors or TD / "mirrors.toml"),
            "--allowlist-file",
            str(allowlist or TD / "allowlist.toml"),
            "--domains-file",
            str(domains_file or TD / "domains.json"),
            "--dimensionless-file",
            str(dimensionless or TD / "dimensionless.toml"),
            "--root",
            str(ROOT),
            "--catalog-file",
            str(TD / "catalog.json"),
            "--document-types-file",
            str(TD / "document_types.toml"),
            *extra,
        ]
        code, lines, self.report = check_surface.run(check_surface.parse_args(argv))
        return code, lines

    def write(self, name, text):
        p = self.dir / name
        p.write_text(text, encoding="utf-8")
        return p


class Baseline(CheckerCase):
    def test_fixture_is_green(self):
        code, lines = self.run_check("--audit-allowlist")
        self.assertEqual(code, 0, "\n".join(lines))
        self.assertIn("Validation problems: 0", lines)
        self.assertIn("Ledger failures: 0", lines)
        self.assertIn("  python function detect_bpm: sample_rate is a required argument.", lines)
        self.assertIn("  | total | 0 | 0 | 0 | 0 | 0 | 0 | 0 |", lines)

    def test_every_check_has_candidates(self):
        _, lines = self.run_check()
        for chk in check_surface.CHECKS:
            line = next(x for x in lines if x.startswith(f"  check {chk.id}: "))
            self.assertNotIn("checked 0,", line)

    def test_output_is_deterministic(self):
        self.inv["records"].reverse()
        a = self.run_check()
        self.inv["records"].reverse()
        self.assertEqual(a, self.run_check())


class DomainSplits(CheckerCase):
    def test_baseline_has_no_splits(self):
        _, lines = self.run_check()
        self.assertIn("domain splits: 0", lines)

    def test_split_is_listed_and_informational(self):
        self.add(
            _rec("wasm", "function", "peakLevel", sigs=[], domain="mastering"),
            _rec("python", "function", "peak_level", sigs=[], domain="analysis"),
            _rec(
                "c",
                "function",
                "sonare_peak_level",
                sigs=[{"params": [], "returns": "float"}],
                domain="analysis",
            ),
        )
        code, lines = self.run_check()
        self.assertEqual(code, 0, "\n".join(lines))
        self.assertIn("domain splits: 1", lines)
        self.assertIn("  peak_level: wasm=mastering, python=analysis, c=analysis", lines)


class LedgerCheck(CheckerCase):
    def test_restored_old_name_fails(self):
        self.add(_rec("wasm", "function", "getBpm", sigs=[]))
        code, lines = self.run_check()
        self.assertEqual(code, 1)
        self.assertIn("  wasm function getBpm: old name still present", lines)

    def test_removed_new_name_fails(self):
        self.drop("node", "function", "detectBpm")
        code, lines = self.run_check()
        self.assertEqual(code, 1)
        self.assertIn("  node function getBpm: new name detectBpm not present", lines)

    def test_removed_name_present_fails(self):
        self.add(_rec("python", "function", "bpm_legacy", sigs=[]))
        code, lines = self.run_check("--check", "doc-comments")
        self.assertEqual(code, 1)
        self.assertIn("  python function bpm_legacy: removed name still present", lines)

    def test_frontend_limits_cli_entry(self):
        self.add(_rec("cli-python", "flag", "bpm --verbose", owner="bpm", member="verbose"))
        _, lines = self.run_check("--surface", "cli")
        self.assertIn("Ledger failures: 0", lines)
        self.add(_rec("cli-native", "flag", "bpm --verbose", owner="bpm", member="verbose"))
        code, lines = self.run_check("--surface", "cli", "--check", "cli-positional-input")
        self.assertEqual(code, 1)
        self.assertIn("  cli-native flag bpm --verbose: removed name still present", lines)

    def test_wasm_entry_is_part_of_the_key(self):
        led = self.ledgers(
            wasm='[[entry]]\nkind = "function"\nold = "getBpm"\nnew = "detectBpm"\n'
            'entry = "./analysis"\ndomain = "analysis"\nrule = "getters"\n'
        )
        code, lines = self.run_check("--surface", "wasm", ledger_dir=led)
        self.assertEqual(code, 1)
        self.assertIn("  wasm function getBpm [./analysis]: new name detectBpm not present", lines)


class LedgerValidation(CheckerCase):
    def problems(self, entry: str):
        led = self.ledgers(python="[[entry]]\n" + entry)
        code, lines = self.run_check(ledger_dir=led)
        self.assertEqual(code, 1)
        return lines

    def entry(self, **kw):
        base = {
            "kind": "function",
            "old": "detect_bpm",
            "domain": "analysis",
            "rule": "getters",
            "new": "find_bpm",
        }
        base.update(kw)
        return "".join(f"{k} = {json.dumps(v)}\n" for k, v in base.items() if v is not None)

    def assertProblem(self, line, **kw):
        self.assertIn(
            f"  python.toml entry 1 (detect_bpm): {line}", self.problems(self.entry(**kw))
        )

    def test_zero_forms(self):
        self.assertProblem("needs exactly one of new / removed / changed, has 0", new=None)

    def test_two_forms(self):
        self.assertProblem("needs exactly one of new / removed / changed, has 2", removed=True)

    def test_removed_needs_the_callers_replacement(self):
        self.assertProblem(
            "removed needs the caller's replacement as its text, or true with"
            " no_replacement = <reason>",
            new=None,
            removed=True,
        )
        self.assertProblem(
            "removed must be the caller's replacement, or true", new=None, removed=" "
        )

    def test_no_replacement_goes_only_with_removed_true(self):
        self.assertProblem(
            "no_replacement is only for removed = true",
            new=None,
            removed="call x",
            no_replacement="gone",
        )
        self.assertProblem("no_replacement is only for removed = true", no_replacement="gone")
        lines = self.problems(self.entry(new=None, removed=True, no_replacement="gone") + "x = 1\n")
        self.assertFalse(any("no_replacement" in x for x in lines), lines)

    def test_a_name_takes_one_row(self):
        lines = self.problems(self.entry() + "\n[[entry]]\n" + self.entry(new="other"))
        self.assertIn(
            "  python.toml entry 2 (detect_bpm): a second row for detect_bpm"
            " (first at python.toml entry 1 (detect_bpm))",
            lines,
        )

    def test_a_member_of_a_removed_name_takes_no_row(self):
        owner = self.entry(kind="class", old="Bpm", new=None, removed="use detect_bpm")
        member = self.entry(kind="method", old="Bpm.value", new=None, removed="read it")
        lines = self.problems(owner + "\n[[entry]]\n" + member)
        self.assertIn(
            "  python.toml entry 2 (Bpm.value): Bpm is removed, which already covers removing"
            " Bpm.value",
            lines,
        )

    def test_unknown_kind(self):
        self.assertProblem("unknown kind 'func'", kind="func")

    def test_unknown_domain(self):
        self.assertProblem("unknown domain 'misc'", domain="misc")

    def test_unknown_slug(self):
        self.assertProblem("rule 'no-such-rule' is not defined in rules.toml", rule="no-such-rule")

    def test_changed_without_test(self):
        self.assertProblem('changed requires test = "<path>::<substring>"', new=None, changed="x")

    def test_changed_with_missing_substring(self):
        self.assertProblem(
            "test file tools/api/testdata/changed_case.txt does not contain 'absent case'",
            new=None,
            changed="x",
            test="tools/api/testdata/changed_case.txt::absent case",
        )

    def test_changed_with_present_substring_is_valid(self):
        led = self.ledgers(
            python="[[entry]]\n"
            + self.entry(
                new=None,
                changed="x",
                test="tools/api/testdata/changed_case.txt::detect_bpm requires",
            )
        )
        code, lines = self.run_check(ledger_dir=led)
        self.assertEqual(code, 0, "\n".join(lines))


class LedgerTo(LedgerValidation):
    def test_to_without_changed(self):
        self.assertProblem("to is only for changed entries; a plain rename is new", to="find_bpm")

    def test_to_naming_old(self):
        self.assertProblem(
            "to must name the member the changed one became",
            new=None,
            changed="x",
            test="tools/api/testdata/changed_case.txt::detect_bpm requires",
            to="detect_bpm",
        )


def _sig_rec(surface, kind, name, *params, **kw):
    sig = {"params": [{"name": p, "type": "number", "optional": False} for p in params]}
    return _rec(surface, kind, name, sigs=[sig], **kw)


class ConstructorKeys(unittest.TestCase):
    """A public constructor is a method key a ledger row can name; a private one is not."""

    def test_public_constructor_is_keyed_and_private_is_not(self):
        lookup = check_surface.build_lookup(
            [
                _rec("node", "class", "Engine", ctor_private=False),
                _rec("node", "class", "Timeline", ctor_private=True),
            ]
        )
        self.assertIn(("node", "method", "Engine.constructor", None), lookup)
        self.assertNotIn(("node", "method", "Timeline.constructor", None), lookup)


class LedgerAgainstBaseline(CheckerCase):
    """Row targets at tip, row old names at the base release, and the completeness direction."""

    CHANGED = 'test = "tools/api/testdata/changed_case.txt::detect_bpm requires"\n'

    def setUp(self):
        super().setUp()
        self.base = copy.deepcopy(BASE)
        self.base["records"] += [
            _rec("wasm", "function", "getBpm", sigs=[]),
            _rec("node", "function", "getBpm", sigs=[]),
            _rec("python", "function", "bpm_legacy", sigs=[]),
            _rec("cli-native", "flag", "bpm --verbose", owner="bpm", member="verbose"),
        ]

    def run_base(self, *extra, ledger_dir=None):
        path = self.dir / "base.json"
        path.write_text(json.dumps(self.base), encoding="utf-8")
        return self.run_check("--baseline-inventory-file", str(path), *extra, ledger_dir=ledger_dir)

    def wasm_row(self, body, domain="analysis"):
        fixture = (TD / "ledgers" / "wasm.toml").read_text(encoding="utf-8")
        return self.ledgers(
            wasm=f'{fixture}\n[[entry]]\ndomain = "{domain}"\nrule = "getters"\n' + body,
            node=(TD / "ledgers" / "node.toml").read_text(encoding="utf-8"),
            python=(TD / "ledgers" / "python.toml").read_text(encoding="utf-8"),
            cli=(TD / "ledgers" / "cli.toml").read_text(encoding="utf-8"),
        )

    def test_fixture_is_green_against_its_baseline(self):
        code, lines = self.run_base()
        self.assertEqual(code, 0, "\n".join(lines))
        self.assertIn("v1.10.0 names gone with no ledger row: 0", lines)

    def test_without_a_baseline_both_directions_are_not_compared(self):
        _, lines = self.run_check()
        self.assertTrue(any("not compared: ledger old names against v1.10.0" in x for x in lines))
        self.assertTrue(any("not compared: v1.10.0 names gone with no row" in x for x in lines))

    def test_a_default_run_without_the_baseline_file_fails_naming_it(self):
        missing = self.dir / "baseline-missing.tsv"
        argv = ["--ledger-dir", str(TD / "ledgers"), "--baseline-file", str(missing)]
        code, lines, report = check_surface.run(check_surface.parse_args(argv))
        self.assertEqual(code, 1)
        expected = f"baseline {missing} is missing; regenerate it with --write-baseline"
        self.assertEqual(lines, [expected])
        self.assertEqual(report, {"error": lines[0]})

    def test_the_baseline_file_round_trips_its_records(self):
        records = [
            _rec("python", "field", "bpm_legacy.window", owner="bpm_legacy", member="window"),
            _rec("node", "class", "Timeline", ctor_private=True),
            _rec("cli-native", "flag", "bpm --verbose", owner="bpm", member="verbose"),
        ]
        path = self.dir / "baseline.tsv"
        check_surface.write_baseline(path, records + records[:1])
        back = check_surface.read_baseline(path)
        self.assertEqual(len(back), 3)
        key = {(r["surface"], r["kind"], r["name"]): r for r in back}
        self.assertTrue(key[("node", "class", "Timeline")]["ctor_private"])
        self.assertEqual(key[("python", "field", "bpm_legacy.window")]["owner"], "bpm_legacy")

    def test_old_name_absent_from_the_base_release_fails(self):
        self.base["records"] = [r for r in self.base["records"] if r["name"] != "bpm_legacy"]
        code, lines = self.run_base()
        self.assertEqual(code, 1)
        self.assertIn("  python function bpm_legacy: old name not in v1.10.0", lines)

    def test_changed_name_absent_without_to_fails(self):
        self.base["records"].append(_rec("wasm", "function", "tempo", sigs=[]))
        led = self.wasm_row('kind = "function"\nold = "tempo"\nchanged = "x"\n' + self.CHANGED)
        code, lines = self.run_base(ledger_dir=led)
        self.assertEqual(code, 1)
        self.assertIn(
            "  wasm function tempo: changed name not present;"
            " a renamed member names its target in to",
            lines,
        )

    def test_changed_with_to_names_the_target(self):
        self.base["records"].append(_rec("wasm", "function", "tempo", sigs=[]))
        row = 'kind = "function"\nold = "tempo"\nchanged = "x"\n' + self.CHANGED
        code, lines = self.run_base(ledger_dir=self.wasm_row(row + 'to = "detectBpm"\n'))
        self.assertEqual(code, 0, "\n".join(lines))
        code, lines = self.run_base(ledger_dir=self.wasm_row(row + 'to = "findBpm"\n'))
        self.assertEqual(code, 1)
        self.assertIn("  wasm function tempo: to name findBpm not present", lines)

    def test_getter_claim_needs_an_accessor(self):
        row = 'kind = "method"\nold = "Session.destroy"\nchanged = "readonly getter now"\n'
        led = self.wasm_row(row + self.CHANGED, domain="engine")
        code, lines = self.run_base(ledger_dir=led)
        self.assertEqual(code, 1)
        self.assertIn(
            "  wasm method Session.destroy: text claims a getter but Session.destroy is not one",
            lines,
        )
        for r in self.inv["records"]:
            if r["surface"] == "wasm" and r["name"] == "Session.destroy":
                r["ts_kind"] = "accessor"
        code, lines = self.run_base(ledger_dir=led)
        self.assertEqual(code, 0, "\n".join(lines))

    def test_a_vanished_name_with_no_row_is_an_orphan(self):
        self.base["records"].append(_rec("node", "field", "BpmRequest.window"))
        code, lines = self.run_base()
        self.assertEqual(code, 1)
        self.assertIn("v1.10.0 names gone with no ledger row: 1", lines)
        self.assertIn("  node field BpmRequest.window", lines)

    def test_a_removed_owner_speaks_for_its_members(self):
        self.base["records"].append(_rec("python", "field", "bpm_legacy.window"))
        code, lines = self.run_base()
        self.assertEqual(code, 0, "\n".join(lines))

    def test_a_changed_command_does_not_speak_for_a_gone_flag(self):
        self.base["records"].append(
            _rec("cli-native", "flag", "bpm --tempo", owner="bpm", member="tempo")
        )
        fixture = (TD / "ledgers" / "cli.toml").read_text(encoding="utf-8")
        row = (
            '[[entry]]\nkind = "command"\nold = "bpm"\nchanged = "x"\n'
            'domain = "analysis"\nrule = "individual"\n' + self.CHANGED
        )
        led = self.ledgers(
            wasm=(TD / "ledgers" / "wasm.toml").read_text(encoding="utf-8"),
            node=(TD / "ledgers" / "node.toml").read_text(encoding="utf-8"),
            python=(TD / "ledgers" / "python.toml").read_text(encoding="utf-8"),
            cli=f"{fixture}\n{row}",
        )
        code, lines = self.run_base(ledger_dir=led)
        self.assertEqual(code, 1)
        self.assertIn("  cli-native flag bpm --tempo", lines)

    def test_a_rename_covers_a_member_only_where_it_survived_under_the_new_name(self):
        self.base["records"].append(
            _rec("wasm", "field", "getBpm.window", owner="getBpm", member="window")
        )
        code, lines = self.run_base()
        self.assertEqual(code, 1)
        self.assertIn("  wasm field getBpm.window", lines)
        self.inv["records"].append(
            _rec("wasm", "field", "detectBpm.window", owner="detectBpm", member="window")
        )
        code, lines = self.run_base()
        self.assertEqual(code, 0, "\n".join(lines))
        self.assertTrue(
            any(x.startswith("  completeness wasm:") and "owner-verified=1" in x for x in lines)
        )

    def test_a_changed_owner_does_not_speak_for_a_vanished_keyword(self):
        self.base["records"].append(
            _rec("python", "option", "detect_bpm(hop_length)", owner="detect_bpm")
        )
        code, lines = self.run_base()
        self.assertEqual(code, 1)
        self.assertIn("  python option detect_bpm(hop_length)", lines)

    def test_a_removed_owner_speaks_for_its_flags(self):
        self.base["records"].append(
            _rec("cli-native", "command", "bpm_old"),
        )
        self.base["records"].append(
            _rec("cli-native", "flag", "bpm_old --fast", owner="bpm_old", member="fast")
        )
        fixture = (TD / "ledgers" / "cli.toml").read_text(encoding="utf-8")
        row = (
            '[[entry]]\nkind = "command"\nold = "bpm_old"\nremoved = "use bpm"\nfrontend = "native"\n'
            'domain = "analysis"\nrule = "individual"\n'
        )
        led = self.ledgers(
            wasm=(TD / "ledgers" / "wasm.toml").read_text(encoding="utf-8"),
            node=(TD / "ledgers" / "node.toml").read_text(encoding="utf-8"),
            python=(TD / "ledgers" / "python.toml").read_text(encoding="utf-8"),
            cli=f"{fixture}\n{row}",
        )
        code, lines = self.run_base(ledger_dir=led)
        self.assertEqual(code, 0, "\n".join(lines))

    def test_positional_parameter_names_are_exempt_by_class_and_keywords_are_not(self):
        self.base["records"] += [
            _rec("wasm", "option", "getBpm(input)", owner="getBpm"),
            _rec("c", "option", "sonare_old_call(rate)", owner="sonare_old_call"),
        ]
        code, lines = self.run_base()
        self.assertEqual(code, 0, "\n".join(lines))
        wasm = next(x for x in lines if x.startswith("  completeness wasm:"))
        self.assertIn("exempt=1 (TypeScript parameter names)", wasm)
        c = next(x for x in lines if x.startswith("  completeness c:"))
        self.assertIn("exempt=1 (C parameter names)", c)
        self.base["records"].append(_rec("python", "option", "bpm_new(window)", owner="bpm_new"))
        code, lines = self.run_base()
        self.assertEqual(code, 1)
        self.assertIn("  python option bpm_new(window)", lines)

    def test_a_surface_with_nothing_compared_fails(self):
        self.base["records"] = [r for r in self.base["records"] if r["surface"] != "cli-python"]
        code, lines = self.run_base()
        self.assertEqual(code, 1)
        self.assertTrue(any("completeness cli-python: compared=0" in x for x in lines))
        self.assertTrue(any("no v1.10.0 name of this surface was compared" in x for x in lines))


class Conventions(CheckerCase):
    ZERO_FAIL = "    FAIL options-struct:struct-size: no candidates on the selected surfaces; its extractor found nothing"

    def test_zero_candidates_fails(self):
        self.drop("c", "type", "SonareBpmOptions")
        for extra in ((), ("--surface", "c")):
            code, lines = self.run_check(*extra)
            self.assertEqual(code, 1)
            self.assertIn("  check options-struct:struct-size: checked 0, violations 0", lines)
            self.assertIn(self.ZERO_FAIL, lines)
            self.assertFalse(
                any(
                    x.lstrip().startswith("not compared:")
                    and "v1.10.0" not in x
                    and "native CLI help" not in x
                    and "enum-coded" not in x
                    and "the catalog advertises" not in x
                    for x in lines
                )
            )

    def test_c_free_accepts_the_plain_buffer_set_and_names_the_rest(self):
        def free_fn(name, param_type):
            return _rec(
                "c",
                "function",
                name,
                sigs=[{"params": [{"name": "p", "type": param_type}], "returns": "void"}],
            )

        self.add(
            free_fn("sonare_free_floats", "float*"),
            free_fn("sonare_free_ints", "int*"),
            free_fn("sonare_free_doubles", "double*"),
            free_fn("sonare_free_result", "SonareBpmOptions*"),
        )
        _, lines = self.run_check("--surface", "c", "--check", "c-free")
        bad = [x.strip() for x in lines if x.startswith("    c function sonare_")]
        self.assertEqual(
            [x.split(":")[0] for x in bad],
            ["c function sonare_free_doubles", "c function sonare_free_result"],
        )

    def test_zero_candidates_in_a_narrowed_run_is_not_a_failure(self):
        self.drop("c", "type", "SonareBpmOptions")
        for extra in (
            ("--domain", "core"),
            ("--match", "Bpm"),
            ("--check", "options-struct:struct-size"),
        ):
            code, lines = self.run_check("--surface", "c", *extra)
            self.assertEqual(code, 0, "\n".join(lines))
            self.assertIn("  check options-struct:struct-size: checked 0, violations 0", lines)
            self.assertNotIn(self.ZERO_FAIL, lines)
            self.assertIn(
                "    not compared: options-struct:struct-size (no candidates under this filter)",
                lines,
            )

    def test_zero_candidates_from_an_excluded_surface_is_not_applicable(self):
        # record-input-pair judges Python but takes its candidates from Node and WASM.
        zero = "    FAIL record-input-pair: no candidates on the selected surfaces; its extractor found nothing"
        _, lines = self.run_check("--surface", "python")
        self.assertIn("  check record-input-pair: checked 0, violations 0", lines)
        self.assertNotIn(zero, lines)
        self.assertIn(
            "    not compared: record-input-pair (not applicable to the selected surfaces:"
            " its candidates come from node, wasm)",
            lines,
        )
        # A full run reaching no candidates still fails.
        for surface in ("node", "wasm"):
            self.drop(surface, "type", "BpmMarkerInput")
        code, lines = self.run_check()
        self.assertEqual(code, 1)
        self.assertIn(zero, lines)

    def test_unused_allowlist_entry_fails_audit(self):
        allow = self.write(
            "allow.toml",
            '[[allow]]\ncheck = "getters"\nsurface = "wasm"\n'
            'name = "getNothing*"\ncategory = "platform"\nreason = "r"\n',
        )
        code, lines = self.run_check("--audit-allowlist", allowlist=allow)
        self.assertEqual(code, 1)
        self.assertIn("Allowlist audit: 1 entry excused nothing", lines)
        self.assertIn("  allow 1: getters wasm getNothing*", lines)

    def test_used_allowlist_entry_passes_audit(self):
        self.add(_rec("wasm", "function", "getLevel", sigs=[]))
        allow = self.write(
            "allow.toml",
            '[[allow]]\ncheck = "getters"\nsurface = "wasm"\n'
            'name = "getLevel"\ncategory = "platform"\nreason = "r"\n',
        )
        code, lines = self.run_check("--audit-allowlist", allowlist=allow)
        self.assertEqual(code, 0, "\n".join(lines))
        self.assertIn("  check getters: checked 12, violations 0, excused 1", lines)

    def test_a_platform_mirror_member_is_exempt_from_naming_and_listed(self):
        self.add(
            _rec("wasm", "type", "AudioClock", platform_mirror="AudioContext"),
            _rec(
                "wasm",
                "method",
                "AudioClock.getOutputTimestamp",
                owner="AudioClock",
                member="getOutputTimestamp",
                sigs=[],
                platform_mirror="AudioContext",
            ),
            _rec(
                "wasm",
                "method",
                "AudioClock.getDriftEstimate",
                owner="AudioClock",
                member="getDriftEstimate",
                sigs=[],
            ),
        )
        code, lines = self.run_check("--check", "getters")
        self.assertEqual(code, 1)
        self.assertIn("  wasm AudioClock mirrors AudioContext: 1 members", lines)
        self.assertIn("  check getters: checked 13, violations 1, platform-mirrored 1", lines)
        self.assertIn(
            "    platform mirror: wasm AudioClock.getOutputTimestamp (AudioContext)", lines
        )
        self.assertIn("    wasm method AudioClock.getDriftEstimate: drop the get prefix", lines)

    def test_librosa_mirror_exempts_python_only(self):
        self.add(
            _rec("wasm", "function", "stft", sigs=[]),
            _rec("wasm", "option", "stft(sr)", owner="stft", member="sr", type="number"),
        )
        code, lines = self.run_check("--check", "sample-rate-name")
        self.assertEqual(code, 1)
        self.assertIn(
            "    wasm option stft(sr): spell the sample rate sampleRate / sample_rate, not sr",
            lines,
        )
        self.assertNotIn(
            "    python option stft(sr): spell the sample rate sampleRate / sample_rate, not sr",
            lines,
        )

    def test_without_mirror_python_sr_is_reported(self):
        mirrors = self.write("mirrors.toml", "")
        code, lines = self.run_check("--check", "sample-rate-name:family", mirrors=mirrors)
        self.assertEqual(code, 1)
        self.assertIn(
            "    python function stft: sr is reserved for librosa mirrors; use a required sample_rate",
            lines,
        )


class NameTables(CheckerCase):
    """The fixture's ErrorCodeName is ErrorCode's member names on python and node."""

    def retoken(self, surface, old, new):
        for r in self.inv["records"]:
            if r["surface"] != surface:
                continue
            if r["kind"] == "type" and r["name"] == "ErrorCodeName":
                r["literals"] = sorted(new if t == old else t for t in r["literals"])
            elif r.get("owner") == "ErrorCodeName" and r.get("token") == old:
                r.update(name=f"ErrorCodeName='{new}'", member=new, token=new)

    def test_member_name_table_is_not_a_token_set(self):
        code, lines = self.run_check("--check", "token-case,token-case:name-table")
        self.assertEqual(code, 0, "\n".join(lines))
        self.assertIn("  check token-case:name-table: checked 2, violations 0", lines)

    def test_a_lower_camel_member_makes_it_a_token_set_again(self):
        self.retoken("python", "InvalidParameter", "invalidParameter")
        code, lines = self.run_check("--check", "token-case,token-case:name-table")
        self.assertEqual(code, 1)
        self.assertIn("    python value ErrorCodeName='Ok': token 'Ok' is not lowerCamel", lines)
        self.assertIn(
            "    node type ErrorCodeName: name table differs from python: InvalidParameter, invalidParameter",
            lines,
        )

    def test_an_extra_non_member_value_makes_it_a_token_set_again(self):
        self.retoken("node", "Ok", "Retry")
        code, lines = self.run_check("--check", "token-case")
        self.assertEqual(code, 1)
        self.assertIn(
            "    node value ErrorCodeName='Retry': token 'Retry' is not lowerCamel", lines
        )

    def test_a_python_member_missing_from_the_table_is_reported(self):
        self.add(
            _rec(
                "python",
                "value",
                "ErrorCode.STALE_RESULT",
                owner="ErrorCode",
                member="STALE_RESULT",
            )
        )
        code, lines = self.run_check("--check", "token-case,token-case:name-table")
        self.assertEqual(code, 1)
        self.assertIn("    python value ErrorCodeName='Ok': token 'Ok' is not lowerCamel", lines)


def _py_fn(name, *params):
    """A public Python function; each param is (name, default-or-None)."""
    ps = [
        {
            "name": n,
            "type": "int",
            "optional": d is not None,
            "default": d,
            "rest": False,
            "boolean": False,
            "literals": [],
        }
        for n, d in params
    ]
    return _rec(
        "python",
        "function",
        name,
        in_all=True,
        modules=["__init__", "analyzer"],
        sigs=[{"params": ps, "returns": "float"}],
    )


class RateFamily(CheckerCase):
    def family(self, *extra, mirrors=None):
        code, lines = self.run_check(
            "--check", "sample-rate-name:family,sample-rate-required", *extra, mirrors=mirrors
        )
        self.assertEqual(code, 1)
        return lines

    def test_listed_mirror_with_sample_rate_fails(self):
        self.drop("python", "function", "stft")
        self.add(_py_fn("stft", ("samples", None), ("sample_rate", "22050")))
        self.assertIn(
            "    python function stft: listed librosa mirror spells its rate sample_rate; use sr=22050",
            self.family(),
        )

    def test_listed_mirror_with_other_sr_default_fails(self):
        self.drop("python", "function", "stft")
        self.add(_py_fn("stft", ("samples", None), ("sr", "44100")))
        self.assertIn(
            "    python function stft: listed librosa mirror has sr=44100, not sr=22050",
            self.family(),
        )

    def test_unlisted_function_with_sr_fails(self):
        self.add(_py_fn("onset_detect", ("samples", None), ("sr", "22050")))
        self.assertIn(
            "    python function onset_detect: sr is reserved for librosa mirrors; "
            "use a required sample_rate",
            self.family(),
        )

    def test_both_spellings_fail(self):
        self.add(_py_fn("onset_detect", ("samples", None), ("sr", "22050"), ("sample_rate", None)))
        self.assertIn(
            "    python function onset_detect: carries both sr and sample_rate", self.family()
        )

    def test_sample_rate_with_default_fails_sample_rate_required(self):
        self.drop("python", "function", "detect_bpm")
        self.add(_py_fn("detect_bpm", ("samples", None), ("sample_rate", "22050")))
        self.assertIn(
            "    python function detect_bpm: sample_rate has a default; make it required",
            self.family(),
        )

    def test_missing_mirror_target_is_a_finding_not_a_validation_problem(self):
        mirrors = self.write(
            "mirrors.toml",
            '[[mirror]]\nname = "stft"\nevidence = "in-tree"\n\n'
            '[[mirror]]\nname = "onset_strength"\nevidence = "recalled"\n',
        )
        code, lines = self.run_check("--check", "librosa-mirror:present", mirrors=mirrors)
        self.assertEqual(code, 1)
        self.assertIn("Validation problems: 0", lines)
        self.assertIn("  check librosa-mirror:present: checked 2, violations 1", lines)
        self.assertIn(
            "    python function onset_strength: listed in librosa_mirrors.toml "
            "but not a public Python function",
            lines,
        )


def _param(name, *, type="number", boolean=False, optional=False):
    return {
        "name": name,
        "type": type,
        "optional": optional,
        "boolean": boolean,
        "literals": [],
        "rest": False,
    }


def _js_method(owner, member, *params, static=False):
    return _rec(
        "node",
        "method",
        f"{owner}.{member}",
        owner=owner,
        member=member,
        static=static,
        sigs=[{"params": list(params), "returns": "void"}],
    )


class PositionalBools(CheckerCase):
    def bools(self, *records):
        self.add(*records)
        _, lines = self.run_check("--surface", "node", "--check", "no-positional-bools")
        return [x.strip() for x in lines if x.startswith("    node method Strip.")]

    def test_a_trailing_bool_the_setter_names_is_labelled(self):
        found = self.bools(
            _js_method("Strip", "setMuted", _param("strip"), _param("muted", boolean=True)),
            _js_method("Strip", "setClockEnabled", _param("enabled", type="boolean", boolean=True)),
        )
        self.assertEqual(found, [])

    def test_a_bool_the_name_does_not_label_is_positional(self):
        found = self.bools(
            _js_method("Strip", "setAutoGain", _param("enabled", boolean=True)),
            _js_method(
                "Strip", "setPolarity", _param("left", boolean=True), _param("right", boolean=True)
            ),
            _js_method("Strip", "muteIf", _param("muted", boolean=True)),
            _js_method("Strip", "setMuted", _param("muted", boolean=True), _param("strip")),
        )
        self.assertEqual(
            found,
            [
                "node method Strip.muteIf: positional boolean muted",
                "node method Strip.setAutoGain: positional boolean enabled",
                "node method Strip.setMuted: positional boolean muted",
                "node method Strip.setPolarity: positional boolean left, right",
            ],
        )


class RateScope(CheckerCase):
    def request(self, name, consumer):
        fields = [
            _rec("node", "field", f"{name}.samples", owner=name, member="samples", optional=False),
        ]
        self.add(_rec("node", "type", name), *fields, consumer)

    def rate_lines(self):
        _, lines = self.run_check("--check", "sample-rate-required")
        return [x.strip() for x in lines if x.startswith(("    node type", "    python method"))]

    def test_a_request_only_a_handle_method_takes_runs_at_the_handle_rate(self):
        self.request(
            "RenderRequest", _js_method("Engine", "render", _param("request", type="RenderRequest"))
        )
        self.assertEqual(self.rate_lines(), [])

    def test_a_request_a_free_function_takes_needs_its_rate(self):
        self.request(
            "TrimRequest",
            _rec(
                "node",
                "function",
                "trim",
                sigs=[{"params": [_param("request", type="TrimRequest")], "returns": "void"}],
            ),
        )
        self.assertIn(
            "node type TrimRequest: takes samples but sampleRate is not a required field",
            self.rate_lines(),
        )

    def test_a_python_instance_method_runs_at_its_handle_rate_and_a_factory_does_not(self):
        def method(member, decorators):
            fn = _py_fn(member, ("samples", None))
            fn.update(kind="method", name=f"Chain.{member}", owner="Chain", member=member)
            fn["decorators"] = decorators
            return fn

        self.add(method("process", []), method("from_samples", ["classmethod"]))
        self.assertEqual(
            self.rate_lines(),
            ["python method Chain.from_samples: takes audio but has no sample_rate parameter"],
        )


class NumericTokens(CheckerCase):
    def test_a_channel_layout_number_is_a_token(self):
        self.add(
            _rec("node", "type", "Layout", literals=["stereo", "5.1", "7.1.4", "5_1", "5."]),
            _rec(
                "node",
                "option",
                "Bus(layout)",
                owner="Bus",
                member="layout",
                inline_literals=True,
                literals=["stereo", "5.1", "7.1.4", "5_1", "5."],
            ),
        )
        _, lines = self.run_check("--surface", "node", "--check", "token-case")
        bad = sorted(x.strip() for x in lines if x.startswith("    node option Bus(layout)"))
        self.assertEqual(
            bad,
            [
                "node option Bus(layout): token '5.' is not lowerCamel",
                "node option Bus(layout): token '5_1' is not lowerCamel",
            ],
        )


class CliFrontEnds(CheckerCase):
    def test_one_front_end_run_compares_against_the_other(self):
        self.add(_rec("cli-native", "flag", "bpm --window-sec", owner="bpm", member="window-sec"))
        rows = {}
        for surface in ("cli", "cli-python", "cli-native"):
            _, lines = self.run_check("--surface", surface, "--check", "cli-front-end-parity")
            rows[surface] = sorted(x.strip() for x in lines if "missing on" in x)
        self.assertEqual(rows["cli"], ["cli-native flag bpm --window-sec: missing on cli-python"])
        self.assertEqual(rows["cli-native"], rows["cli"])
        self.assertEqual(rows["cli-python"], [])

    def test_a_switch_names_no_quantity(self):
        for boolean, expected in ((True, []), (False, ["cli-native flag bpm --auto-threshold"])):
            with self.subTest(boolean=boolean):
                self.setUp()
                self.add(
                    _rec(
                        "cli-native",
                        "flag",
                        "bpm --auto-threshold",
                        owner="bpm",
                        member="auto-threshold",
                        boolean=boolean,
                    )
                )
                _, lines = self.run_check("--surface", "cli-native", "--check", "unit-in-name")
                bad = [x.strip().split(":")[0] for x in lines if "names a physical quantity" in x]
                self.assertEqual(bad, expected)


class CliExamples(unittest.TestCase):
    """A rules.toml CLI example names a command that exists and only flags it takes."""

    CURRENT = (
        _rec("cli-native", "command", "analyze"),
        _rec(
            "cli-native",
            "flag",
            "analyze --chroma-high-pass-hz",
            owner="analyze",
            member="chroma-high-pass-hz",
        ),
        _rec("cli-python", "command", "chroma"),
        _rec("cli-python", "flag", "chroma --n-fft", owner="chroma", member="n-fft"),
        _rec("cli-native", "command", "project.import-smf"),
        _rec(
            "cli-native",
            "flag",
            "project.import-smf --output",
            owner="project.import-smf",
            member="output",
        ),
    )
    BASE = (
        _rec("cli-python", "command", "analyze"),
        _rec(
            "cli-python",
            "flag",
            "analyze --chroma-highpass",
            owner="analyze",
            member="chroma-highpass",
        ),
    )
    CONTRACT = types.MappingProxyType(
        {
            "inventory": {
                "expected_options": {"project.import-smf": [{"name": "output", "aliases": ["o"]}]}
            }
        }
    )

    def run_examples(self, before, after, base=BASE):
        rules = [
            {"slug": "high-pass", "example": [{"surface": "cli", "before": before, "after": after}]}
        ]
        return check_surface.cli_example_problems(
            rules, list(self.CURRENT), list(base) if base is not None else None, dict(self.CONTRACT)
        )

    def test_a_flag_on_the_command_that_takes_it_passes(self):
        problems, skipped = self.run_examples(
            "sonare analyze song.wav --chroma-highpass 100\n",
            "sonare analyze song.wav --chroma-high-pass-hz 100  # comment\n",
        )
        self.assertEqual((problems, skipped), ([], []))

    def test_a_flag_on_a_sibling_command_fails_on_both_sides(self):
        problems, _ = self.run_examples(
            "sonare chroma song.wav --chroma-highpass 100\n",
            "sonare chroma song.wav --chroma-high-pass-hz 100\n",
        )
        self.assertEqual(
            problems,
            [
                (
                    "rules.toml rule 1 (high-pass): after example `sonare chroma song.wav"
                    " --chroma-high-pass-hz 100`: the current CLI chroma has no"
                    " --chroma-high-pass-hz"
                ),
                (
                    "rules.toml rule 1 (high-pass): before example `sonare chroma song.wav"
                    " --chroma-highpass 100`: v1.10.0 has no command 'chroma'"
                ),
            ],
        )

    def test_a_group_command_short_alias_and_negative_value(self):
        problems, skipped = self.run_examples(
            "", "sonare project import-smf song.mid -o p.json\nsonare project import-smf -x -14\n"
        )
        self.assertEqual(
            problems,
            [
                (
                    "rules.toml rule 1 (high-pass): after example `sonare project import-smf"
                    " -x -14`: the current CLI project.import-smf has no -x"
                )
            ],
        )
        self.assertEqual(skipped, [])

    def test_without_a_baseline_the_before_side_is_reported_not_compared(self):
        problems, skipped = self.run_examples("sonare nothing\n", "sonare analyze\n", base=None)
        self.assertEqual(problems, [])
        self.assertEqual(
            skipped, ["rules.toml CLI example before lines against v1.10.0 (no baseline)"]
        )


class CodeExamples(unittest.TestCase):
    CURRENT = (
        _rec("wasm", "function", "analyze"),
        _rec("wasm", "field", "AnalyzeResult.bpm", owner="AnalyzeResult", member="bpm"),
        _rec(
            "wasm",
            "function",
            "init",
            sigs=[{"params": [{"name": "options", "type": "{ wasmBinary?: ArrayBuffer; }"}]}],
        ),
        _rec("python", "function", "analyze"),
        _rec("python", "option", "analyze(hop_length)", owner="analyze", member="hop_length"),
        _rec("c", "function", "sonare_analyze"),
    )
    BASE = (_rec("wasm", "function", "detectBpm"), _rec("python", "function", "detect_bpm"))

    def run_examples(self, surface, before, after, base=BASE):
        rules = [
            {
                "slug": "typed-results",
                "example": [{"surface": surface, "before": before, "after": after}],
            }
        ]
        return check_surface.code_example_problems(
            rules, list(self.CURRENT), list(base) if base is not None else None
        )

    def test_public_names_bound_names_and_platform_words_pass(self):
        problems, skipped = self.run_examples(
            "wasm",
            "const bpm = detectBpm(samples);\n",
            "const result = analyze(samples); // unknownWord(\n"
            "const label = 'fake(';\n"
            "for (const x of [result.bpm]) console.log(x);\n",
        )
        self.assertEqual((problems, skipped), ([], []))

    def test_an_unpublished_name_fails_on_its_side(self):
        problems, _ = self.run_examples(
            "python",
            "detect_tempo(samples)\n",
            "analyze(samples, hop_length=512, n_fft=2048)\nskip_strip()\n",
        )
        self.assertEqual(
            problems,
            [
                (
                    "rules.toml rule 1 (typed-results): after python example: the current python"
                    " surface has no 'n_fft'"
                ),
                (
                    "rules.toml rule 1 (typed-results): after python example: the current python"
                    " surface has no 'skip_strip'"
                ),
                (
                    "rules.toml rule 1 (typed-results): before python example: v1.10.0 python"
                    " surface has no 'detect_tempo'"
                ),
            ],
        )

    def test_a_c_name_is_read_against_the_c_abi(self):
        problems, _ = self.run_examples(
            "c", "", "sonare_analyze(a); sonare_analyse(a);\n", base=None
        )
        self.assertEqual(
            problems,
            [
                (
                    "rules.toml rule 1 (typed-results): after c example: the current C ABI has no"
                    " 'sonare_analyse'"
                ),
            ],
        )

    def test_an_inline_object_key_and_a_failing_before_line_are_not_compared(self):
        problems, skipped = self.run_examples(
            "wasm",
            "missingName(); // TypeError: not a function\n",
            "await init({ wasmBinary: bytes, notAKey: 1 });\n",
        )
        self.assertEqual(
            problems,
            [
                (
                    "rules.toml rule 1 (typed-results): after wasm example: the current wasm surface"
                    " has no 'notAKey'"
                ),
            ],
        )
        self.assertEqual(
            skipped,
            [
                (
                    "rules.toml rule 1 (typed-results): after wasm example: key 'wasmBinary' of an"
                    " inline object parameter of init"
                ),
                (
                    "rules.toml rule 1 (typed-results): before wasm example: `missingName(); //"
                    " TypeError: not a function` shows a failure"
                ),
            ],
        )


class CliHelpSentinel(CheckerCase):
    def test_python_cli_help_text_is_read(self):
        self.add(
            _rec(
                "cli-python",
                "flag",
                "bpm --bands",
                owner="bpm",
                member="bands",
                doc_text="Octave-band count (0 = library default)",
            )
        )
        _, lines = self.run_check("--surface", "cli-python", "--check", "absent-not-sentinel:doc")
        self.assertTrue(
            any("cli-python flag bpm --bands: doc offers a sentinel" in x for x in lines)
        )

    def test_native_help_is_reported_unread(self):
        unread = "    not compared: native CLI help (the registry carries no flag descriptions)"
        _, lines = self.run_check("--surface", "cli", "--check", "absent-not-sentinel:doc")
        self.assertIn(unread, lines)
        _, lines = self.run_check("--surface", "cli-python", "--check", "absent-not-sentinel:doc")
        self.assertNotIn(unread, lines)


class Dimensionless(CheckerCase):
    ROW = (
        '[[name]]\nnames = ["{name}"]\nclass = "{cls}"\n'
        'range = [0, 1]\nevidence = "a correlation"\n'
    )

    def run_dim(self, body, with_record=True, name="Probe.threshold"):
        if with_record:
            self.add(_rec("node", "field", "Probe.threshold", owner="Probe", member="threshold"))
        dim = self.write("d.toml", body.replace("{name}", name))
        _, lines = self.run_check("--surface", "node", "--check", "unit-in-name", dimensionless=dim)
        flagged = [x.strip() for x in lines if x.strip().startswith("node field Probe.threshold:")]
        problems = [x.strip() for x in lines if x.startswith("  dimensionless_names.toml")]
        return flagged, problems

    def test_a_listed_name_takes_no_unit(self):
        flagged, problems = self.run_dim("")
        self.assertEqual(len(flagged), 1, flagged)
        self.setUp()
        flagged, problems = self.run_dim(self.ROW.format(name="{name}", cls="correlation"))
        self.assertEqual((flagged, problems), ([], []))

    def test_one_row_covers_every_spelling_of_the_name(self):
        for spelling in ("SonareProbe.threshold", "probe.threshold", "Probe.threshold"):
            with self.subTest(spelling=spelling):
                self.setUp()
                self.add(
                    _rec(
                        "c",
                        "field",
                        "SonareProbe.threshold",
                        owner="SonareProbe",
                        member="threshold",
                    )
                )
                flagged, problems = self.run_dim(
                    self.ROW.format(name="{name}", cls="ratio"), name=spelling
                )
                _, lines = self.run_check(
                    "--surface", "c", "--check", "unit-in-name", dimensionless=self.dir / "d.toml"
                )
                self.assertEqual((flagged, problems), ([], []))
                self.assertFalse(any("SonareProbe.threshold:" in x for x in lines), lines)

    def test_a_field_projected_from_a_listed_declaration_shares_its_row(self):
        decl = {"file": "bindings/node/src/repair.ts", "line": 30}
        self.add(
            _rec("node", "field", "Union.threshold", owner="Union", member="threshold", **decl),
            _rec(
                "node",
                "field",
                "Elsewhere.threshold",
                owner="Elsewhere",
                member="threshold",
                file="bindings/node/src/other.ts",
                line=7,
            ),
        )
        self.add(
            _rec("node", "field", "Probe.threshold", owner="Probe", member="threshold", **decl)
        )
        dim = self.write("d.toml", self.ROW.format(name="Probe.threshold", cls="magnitude"))
        _, lines = self.run_check("--surface", "node", "--check", "unit-in-name", dimensionless=dim)
        flagged = {x.strip().split(":")[0] for x in lines if x.strip().startswith("node field ")}
        self.assertNotIn("node field Union.threshold", flagged)
        self.assertIn("node field Elsewhere.threshold", flagged)

    def test_a_row_must_be_well_formed_and_name_a_candidate(self):
        _, problems = self.run_dim(self.ROW.format(name="{name}", cls="percent"))
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("class must be one of", problems[0])
        self.setUp()
        _, problems = self.run_dim(self.ROW.format(name="{name}", cls="ratio"), with_record=False)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("names nothing unit-in-name would flag", problems[0])

    def run_level(self, member, body=""):
        self.add(_rec("node", "field", f"Probe.{member}", owner="Probe", member=member))
        dim = self.write("d.toml", body.replace("{name}", f"Probe.{member}"))
        _, lines = self.run_check("--surface", "node", "--check", "unit-in-name", dimensionless=dim)
        return [x.strip() for x in lines if x.strip().startswith(f"node field Probe.{member}:")]

    def test_a_bare_peak_rms_or_level_needs_a_row(self):
        for member in ("monoPeak", "sideRms", "harmonyLevel"):
            with self.subTest(member=member):
                self.setUp()
                self.assertEqual(len(self.run_level(member)), 1)

    def test_a_bare_peak_or_rms_row_is_class_magnitude(self):
        for member in ("monoPeak", "sideRms"):
            with self.subTest(member=member):
                self.setUp()
                flagged = self.run_level(member, self.ROW.format(name="{name}", cls="ratio"))
                self.assertEqual(len(flagged), 1, flagged)
                self.assertIn("its row's class is magnitude", flagged[0])
                self.setUp()
                self.assertEqual(
                    self.run_level(member, self.ROW.format(name="{name}", cls="magnitude")), []
                )
        self.setUp()
        self.assertEqual(
            self.run_level("harmonyLevel", self.ROW.format(name="{name}", cls="ratio")), []
        )

    def test_a_switch_verb_or_a_unit_takes_a_level_out(self):
        for member in ("measureTruePeak", "peakDb", "rmsDbfs"):
            with self.subTest(member=member):
                self.setUp()
                self.assertEqual(self.run_level(member), [])

    def test_a_name_in_two_rows_is_refused(self):
        row = self.ROW.format(name="{name}", cls="ratio")
        _, problems = self.run_dim(row + "\n" + row.replace("Probe.threshold", "probe.threshold"))
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("listed twice", problems[0])


class UnitInName(CheckerCase):
    def flagged(self, *records):
        self.add(*records)
        _, lines = self.run_check("--check", "unit-in-name")
        return sorted(x.strip().split(":")[0] for x in lines if "names a physical quantity" in x)

    def test_a_value_that_is_not_a_number_names_no_quantity(self):
        rows = [
            ("node", "A.autoThreshold", "boolean | undefined", True),
            ("node", "A.midiRelease", "MidiRelease", False),
            ("wasm", "A.latency", "{ samples: number; stages: Record<S, L>; }", False),
            ("node", "A.timbreOverTime", "TimbreFrame[]", False),
            ("python", "A.midi_release", "str", False),
            ("python", "A.timbre_over_time", "list[TimbreFrame]", False),
            ("c", "A.timbre_over_time", "SonareTimbreFrame*", False),
            ("node", "B.threshold", "number | undefined", False),
            ("python", "B.ceiling", "float | None", False),
            ("c", "B.delay", "float", False),
            ("c", "B.release", "float*", False),
        ]
        recs = [
            _rec(s, "field", n, owner=n.split(".")[0], member=n.split(".")[1], type=ty, boolean=b)
            for s, n, ty, b in rows
        ]
        recs.append(_rec("cli-native", "flag", "x --duration", owner="x", member="duration"))
        self.assertEqual(
            self.flagged(*recs),
            [
                "c field B.delay",
                "c field B.release",
                "cli-native flag x --duration",
                "node field B.threshold",
                "python field B.ceiling",
            ],
        )

    def test_a_unit_word_anywhere_in_the_name_names_the_unit(self):
        names = (
            "frame_offset",
            "sample_offset",
            "base_sample_offset",
            "sample_start_offset",
            "envelope_offset",
            "index_offset",
            "attack_ms",
            "offset",
        )
        recs = [_rec("c", "option", f"f({n})", owner="f", member=n, type="int64_t") for n in names]
        recs.append(
            _rec(
                "node",
                "field",
                "A.sampleStartOffset",
                owner="A",
                member="sampleStartOffset",
                type="number",
            )
        )
        self.assertEqual(self.flagged(*recs), ["c option f(envelope_offset)", "c option f(offset)"])


class WrongUnit(CheckerCase):
    def test_a_loudness_quantity_takes_its_own_unit(self):
        cases = {
            ("node", "field", "A.truePeakDbtp", "number"): False,
            ("node", "field", "A.truePeakDbtpL", "number"): False,
            ("node", "field", "A.truePeakDbL", "number"): True,
            ("python", "field", "A.true_peak_ceiling_db", "float | None"): True,
            ("python", "field", "A.true_peak_oversample", "int"): False,
            ("node", "field", "A.truePeak", "boolean | undefined"): False,
            ("c", "option", "f(measure_true_peak)", "int"): False,
            ("node", "method", "Audio.truePeakDb", None): True,
            ("wasm", "function", "meteringTruePeakDb", ""): True,
            ("python", "function", "ebur128_loudness_range", None): True,
            ("c", "option", "f(out_loudness_range_lu)", "float*"): False,
            ("python", "field", "A.lra_lu", "float"): True,
            ("node", "field", "A.integratedLufs", "number"): False,
            ("node", "field", "A.shortTermLufsStd", "number"): False,
            ("python", "field", "A.momentary_db", "float"): True,
            ("c", "option", "f(out_short_term)", "float**"): True,
            ("c", "option", "f(out_short_term_length)", "size_t*"): False,
        }
        recs = []
        for surface, kind, name, ty in cases:
            owner, _, member = name.rpartition(".") if "." in name else name.partition("(")
            member = member.rstrip(")")
            kw = {} if ty is None else {"type": ty}
            if ty and ty.startswith("boolean"):
                kw["boolean"] = True
            if kind in ("function", "method"):
                kw["sigs"] = []
            recs.append(
                _rec(
                    surface,
                    kind,
                    name,
                    owner=owner,
                    member=member if kind != "function" else "",
                    **kw,
                )
            )
        self.add(*recs)
        _, lines = self.run_check("--check", "unit-in-name:wrong-unit")
        bad = sorted(x.strip().split(":")[0] for x in lines if "measured in" in x)
        want = sorted(f"{s} {k} {n}" for (s, k, n, _), flag in cases.items() if flag)
        self.assertEqual(bad, want)

    def test_a_peak_or_ceiling_documented_as_true_peak_takes_dbtp(self):
        cases = {
            ("node", "A.loudnessStaticGainPeakDb", "True peak (dBFS) of the stage input."): True,
            ("node", "A.ceilingDb", "Limiter ceiling in dBTP."): True,
            ("python", "A.ceiling_db", "Ceiling of the true-peak limiter, in dB."): True,
            ("node", "A.peakDb", "Sample peak in dBFS."): False,
            ("node", "A.ceilingDbtp", "Limiter ceiling in dBTP."): False,
            ("node", "A.makeupGainDb", "Gain applied before the true-peak limiter, in dB."): False,
            ("node", "A.peakHoldMs", "Hold time of the true-peak readout."): False,
        }
        recs = [
            _rec(s, "field", n, owner="A", member=n.split(".")[1], type="number", doc_text=doc)
            for s, n, doc in cases
        ]
        self.add(*recs)
        _, lines = self.run_check("--check", "unit-in-name:wrong-unit")
        bad = sorted(x.strip().split(":")[0] for x in lines if "measured in" in x)
        want = sorted(f"{s} field {n}" for (s, n, _), flag in cases.items() if flag)
        self.assertEqual(bad, want)


class PythonKeywordSurface(CheckerCase):
    """A constructor's parameters and a forwarded `**config` are named options in the inventory."""

    TIP = """
from dataclasses import dataclass
from typing import TypedDict, Unpack
from .other import external


@dataclass
class Opts:
    depth: int = 1
    width: float = 2.0


class Keys(TypedDict, total=False):
    alpha: float
    beta: int


class A:
    def __init__(self, rate: int, *, gain: float = 1.0, _token: object = None) -> None:
        pass

    @classmethod
    def make(cls, rate: int, **config: object) -> A:
        return cls(rate, **config)

    def tune(self, **opts: object) -> None:
        Opts(**opts)

    def keyed(self, **keys: Unpack[Keys]) -> None:
        pass

    def lost(self, **rest: object) -> None:
        external(**rest)
"""

    def records(self, source: str) -> list[dict]:

        import inventory

        pkg_dir = self.dir / "pkg" / inventory.PY_PACKAGE
        pkg_dir.mkdir(parents=True, exist_ok=True)
        (pkg_dir / "mod.py").write_text(source, encoding="utf-8")
        pkg = inventory._PyPackage(self.dir / "pkg")
        node = pkg.py["mod"].defs["A"]
        recs = inventory._py_class(
            pkg, "A", node, None, "mod", "mod.py", lambda *_: None, {}, {"in_all": True}
        )
        for r in recs:
            r["domain"], r["domain_path"] = "analysis", "declaring-file"
        return recs

    def test_constructor_and_forwarded_keywords_are_options(self):
        recs = self.records(self.TIP)
        opts = {r["name"] for r in recs if r["kind"] == "option"}
        self.assertLessEqual(
            {
                "A.constructor(rate)",
                "A.constructor(gain)",
                "A.make(gain)",
                "A.tune(depth)",
                "A.tune(width)",
                "A.keyed(alpha)",
                "A.keyed(beta)",
            },
            opts,
        )
        self.assertNotIn("A.constructor(_token)", opts)
        self.assertNotIn("A.make(rate)", opts)
        lost = [r["name"] for r in recs if r.get("kwargs_unresolved")]
        self.assertEqual(lost, ["A.lost"])

    def test_a_constructor_keyword_gone_since_the_base_release_is_reported(self):
        base = copy.deepcopy(BASE)
        base["records"] += self.records(self.TIP)
        self.add(*self.records(self.TIP.replace(", *, gain: float = 1.0", "")))
        path = self.dir / "base.json"
        path.write_text(json.dumps(base), encoding="utf-8")
        _, lines = self.run_check("--baseline-inventory-file", str(path))
        gone = [x.strip() for x in lines if x.startswith("  python option A.")]
        self.assertEqual(gone, ["python option A.constructor(gain)", "python option A.make(gain)"])


class CatalogUnitSuffix(CheckerCase):
    def test_a_catalog_parameter_name_states_the_unit_the_catalog_gives(self):
        params = {
            "ceilingDb": ("dBFS", False),
            "ceilingDbtp": ("dBFS", True),
            "truePeakCeilingDbtp": ("dBTP", False),
            "loudCeilingDb": ("dBTP", True),
            "attackMs": ("ms", False),
            "release": ("ms", True),
            "slopeDbOct": ("dBPerOct", False),
            "slopeDbPerOct": ("dBPerOct", True),
            "fadeSec": ("s", False),
            "ratio": ("ratio", False),
            "mixDb": ("none", True),
            "enabled": (None, False),
        }
        catalog = {
            "chain": {"params": []},
            "processors": [
                {
                    "id": "p.x",
                    "params": [{"name": n, "unit": u} for n, (u, _) in params.items()],
                }
            ],
        }
        path = self.dir / "catalog.json"
        path.write_text(json.dumps(catalog), encoding="utf-8")
        _, lines = self.run_check("--check", "catalog-unit-suffix", "--catalog-file", str(path))
        bad = sorted(x.strip().split(":")[0] for x in lines if "catalog unit" in x)
        want = sorted(f"catalog param p.x.{n}" for n, (_, flag) in params.items() if flag)
        self.assertEqual(bad, want)

    def test_an_axis_component_passes_only_under_a_group_doc_stating_the_length_unit(self):
        catalog = {
            "chain": {"params": []},
            "processors": [
                {
                    "id": "p.room",
                    "params": [
                        {"name": "sourceX", "unit": "m"},
                        {"name": "z", "unit": "m"},
                        {"name": "lengthM", "unit": "m"},
                    ],
                }
            ],
        }
        path = self.dir / "catalog.json"
        path.write_text(json.dumps(catalog), encoding="utf-8")

        def flagged():
            _, lines = self.run_check("--check", "catalog-unit-suffix", "--catalog-file", str(path))
            return sorted(x.strip() for x in lines if x.strip().startswith("catalog param"))

        undocumented = flagged()
        self.assertEqual(len(undocumented), 2, undocumented)
        for line in undocumented:
            self.assertIn("an axis component in m", line)
            self.assertIn("the group's doc does not state the length unit", line)
        self.add(
            _rec(
                "node",
                "field",
                "MasteringProcessorParamMap.p.room",
                owner="MasteringProcessorParamMap",
                member="p.room",
                doc_text="Source and listener positions in metres from the room's origin.",
            )
        )
        self.assertEqual(flagged(), [])


class DocumentKeys(CheckerCase):
    """A Python TypedDict declared as a core document keeps the core's camelCase keys."""

    def typed(self, name, *fields, bases=("TypedDict",)):
        self.add(_rec("python", "type", name, py_kind="typed", bases=list(bases)))
        for member, typ in fields:
            self.add(
                _rec("python", "field", f"{name}.{member}", owner=name, member=member, type=typ)
            )

    def run_docs(self, body):
        docs = self.write("docs.toml", body)
        _, lines = self.run_check(
            "--surface", "python", "--check", "snake-case-only", "--document-types-file", str(docs)
        )
        flagged = sorted(
            x.strip().split(":")[0] for x in lines if x.strip().endswith("camelCase name")
        )
        problems = [x.strip() for x in lines if x.startswith("  document_types.toml")]
        return flagged, problems

    ROW = '[[document]]\ntype = "{t}"\nevidence = "bindings/python/src/libsonare/_mastering_pair.py:1: json.loads"\n'

    def setUp(self):
        super().setUp()
        self.typed("Profile", ("durationSec", "float"), ("loudness", "ProfileLoudness | None"))
        self.typed("ProfileLoudness", ("truePeakDbtp", "float"))
        self.typed("Plain", ("sample_rate", "int"))
        self.typed("Record", ("hopLength", "int"), bases=())

    def test_keys_of_a_declared_document_and_the_types_it_reaches_are_exempt(self):
        flagged, problems = self.run_docs("")
        self.assertEqual(
            flagged,
            [
                "python field Profile.durationSec",
                "python field ProfileLoudness.truePeakDbtp",
                "python field Record.hopLength",
            ],
        )
        self.assertEqual(problems, [])
        flagged, problems = self.run_docs(self.ROW.format(t="Profile"))
        self.assertEqual(flagged, ["python field Record.hopLength"])
        self.assertEqual(problems, [])

    def test_a_declaration_must_name_a_typeddict_with_a_camelcase_key(self):
        for t, why in (
            ("Record", "is not a Python TypedDict"),
            ("Missing", "is not a Python TypedDict"),
            ("Plain", "has no camelCase key"),
        ):
            with self.subTest(t=t):
                self.setUp()
                _, problems = self.run_docs(self.ROW.format(t=t))
                self.assertEqual(len(problems), 1, problems)
                self.assertIn(why, problems[0])

    def test_a_declaration_needs_in_tree_evidence_and_is_listed_once(self):
        _, problems = self.run_docs('[[document]]\ntype = "Profile"\n')
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("evidence is required", problems[0])
        self.setUp()
        _, problems = self.run_docs(self.ROW.format(t="Profile") * 2)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("listed twice", problems[0])


class MetadataOnlyDocs(CheckerCase):
    """A doc built only from catalog metadata is counted apart from a doc that describes."""

    def test_a_metadata_only_doc_does_not_satisfy_doc_comments(self):
        meta = "Unit: dB.\nRange: -60 to 0.\nDefault: -1."
        self.add(
            _rec(
                "node",
                "field",
                "Gen.described",
                owner="Gen",
                member="described",
                doc_text="Output ceiling of the limiter.\n\n" + meta,
            ),
            _rec("node", "field", "Gen.metaOnly", owner="Gen", member="metaOnly", doc_text=meta),
            _rec("node", "field", "Gen.bare", owner="Gen", member="bare", has_doc=False),
        )
        _, lines = self.run_check("--surface", "node", "--check", "doc-comments")
        flagged = {
            x.strip().split(":")[0]: x.strip()
            for x in lines
            if x.strip().startswith("node field Gen.")
        }
        self.assertEqual(sorted(flagged), ["node field Gen.bare", "node field Gen.metaOnly"])
        self.assertIn("no doc comment", flagged["node field Gen.bare"])
        self.assertIn("states only the catalog metadata", flagged["node field Gen.metaOnly"])
        self.assertIn("    metadata-only doc: 1", lines)

    def test_a_member_inherited_from_a_dependency_is_not_compared(self):
        lib = "bindings/node/node_modules/typescript/lib/lib.es5.d.ts"
        self.add(
            _rec(
                "node", "field", "Err.stack", owner="Err", member="stack", has_doc=False, file=lib
            ),
            _rec("node", "field", "Err.code", owner="Err", member="code", has_doc=False),
        )
        _, lines = self.run_check("--surface", "node", "--check", "doc-comments")
        flagged = [x.strip() for x in lines if x.strip().startswith("node field Err.")]
        self.assertEqual([x.split(":")[0] for x in flagged], ["node field Err.code"])
        self.assertTrue(any("declared in a dependency (node_modules)" in x for x in lines))


class GeneratedCatalogDocs(CheckerCase):
    """A field generated from the catalog may be documented by catalog metadata alone."""

    def test_a_generated_catalog_field_and_its_projection_take_a_metadata_doc(self):
        meta = "Unit: dB. Range: [-60, 0]. Default: -18."
        decl = {"file": "bindings/node/src/types_mastering.ts", "line": 335}
        self.add(
            _field(
                "node",
                "MasteringProcessorParamMap.dynamics.compressor",
                "thresholdDb",
                doc_text=meta,
                **decl,
            ),
            _field("node", "MixSceneInsert.params", "thresholdDb", doc_text=meta, **decl),
            _field(
                "node",
                "MasteringProcessorParamMap.dynamics.gate",
                "holdMs",
                has_doc=False,
                line=340,
            ),
            _field("node", "LimiterOptions", "ceilingDb", doc_text=meta, line=12),
        )
        _, lines = self.run_check("--surface", "node", "--check", "doc-comments")
        flagged = sorted(
            x.strip()
            for x in lines
            if x.strip().startswith(
                (
                    "node field MasteringProcessorParamMap.",
                    "node field MixSceneInsert.",
                    "node field LimiterOptions.",
                )
            )
        )
        self.assertEqual(
            flagged,
            [
                "node field LimiterOptions.ceilingDb: " + check_surface._META_ONLY,
                "node field MasteringProcessorParamMap.dynamics.gate.holdMs: no doc comment",
            ],
        )


class CEnumFields(CheckerCase):
    def enum(self, name, *values):
        recs = [_rec("c", "type", name, c_kind="enum")]
        recs += [_rec("c", "value", f"{name}.{v}", owner=name, member=v) for v in values]
        return recs

    def field(self, name, doc, ty="int32_t"):
        owner, member = name.split(".")
        return _rec("c", "field", name, owner=owner, member=member, type=ty, doc_text=doc)

    def test_an_enum_coded_field_names_no_quantity_and_is_counted(self):
        self.add(
            *self.enum("SonareMidiRelease", "SONARE_MIDI_RELEASE_HOLD"),
            self.field(
                "SonareCfg.midi_release", "@brief A @ref SonareMidiRelease: what voices do."
            ),
            self.field("SonareCfg.fade_release", "Seconds the fade takes.", ty="float"),
        )
        _, lines = self.run_check("--surface", "c", "--check", "unit-in-name")
        bad = [x.strip().split(":")[0] for x in lines if "names a physical quantity" in x]
        self.assertEqual(bad, ["c field SonareCfg.fade_release"])
        self.assertIn(
            "    not compared: 2 C int fields read as enum-coded (doc opens 'A @ref Sonare<Enum>')",
            lines,
        )

    def test_an_enum_field_whose_doc_does_not_open_with_the_reference_is_reported(self):
        self.add(
            *self.enum("SonareChannelLayout", "SONARE_CHANNEL_LAYOUT_MONO"),
            *self.enum("SonarePitchTargetMode", "SONARE_PITCH_TARGET_MODE_SCALE"),
            self.field("SonareBus.channel_layout", ""),
            self.field("SonareLane.source_channel_layout", "Layout of the source."),
            self.field("SonareCfg.target_mode", "@ref SonarePitchTargetMode."),
            self.field("SonareGood.channel_layout", "@brief A @ref SonareChannelLayout."),
            self.field("SonareCtl.index", "CC number for SONARE_PITCH_TARGET_MODE_SCALE."),
            self.field("SonareX.count", "Plain count."),
        )
        _, lines = self.run_check("--surface", "c", "--check", "c-enum-field-doc")
        bad = sorted(x.strip().split(":")[0] for x in lines if "open its doc with" in x)
        self.assertEqual(
            bad,
            [
                "c field SonareBus.channel_layout",
                "c field SonareCfg.target_mode",
                "c field SonareLane.source_channel_layout",
            ],
        )


class DocSentinel(CheckerCase):
    def run_doc(self, surface, text):
        self.add(
            _rec(surface, "field", "Probe.value", owner="Probe", member="value", doc_text=text)
        )
        _, lines = self.run_check("--surface", surface, "--check", "absent-not-sentinel:doc")
        return [x.strip() for x in lines if x.startswith(f"    {surface} field Probe.value")]

    def test_a_documented_sentinel_fails(self):
        for surface, text in (
            ("node", "Slash-bass pitch class 0..11 or 255 for none. Default 255."),
            ("wasm", "Root pitch class 0..11 (C=0) or `255` for unknown."),
            ("node", "MIDI channel 0..15. Omit for the any-channel sentinel `255`."),
            ("python", "``filter_channel`` (``-1`` = any), ``remap_channel`` (``-1`` = no remap)"),
            ("node", "Destination id, or the transport sentinel `0xFFFFFFFF` for every port."),
            (
                "wasm",
                (
                    "Originating track destination, or `0xFFFFFFFF` for clock and transport bytes"
                    " meant for every port."
                ),
            ),
            ("python", "``destination_id`` of ``0xFFFFFFFF`` (4294967295) tags a clock byte."),
            ("python", "Analyzer octave-band count (0 = library default)."),
            ("wasm", "Gate threshold; ``0`` keeps the library default."),
            ("node", "Block size in frames; 0 selects the engine's own."),
            (
                "wasm",
                "`0` is the master mix, `33..40` the buses, and `0xFFFF` the input-monitor tap.",
            ),
        ):
            with self.subTest(text=text):
                self.setUp()
                bad = self.run_doc(surface, text)
                self.assertEqual(len(bad), 1, bad)
                self.assertIn("doc offers a sentinel", bad[0])

    def c_macro(self, name, value):
        self.add(_rec("c", "export", name, c_kind="macro", type=value))

    def test_a_marker_the_c_abi_names_as_an_identifier_passes(self):
        self.c_macro("SONARE_TARGET_INPUT_MONITOR", "0xFFFFu")
        tap = "`0` is the master mix, `33..40` the buses, and `0xFFFF` the input-monitor tap."
        self.assertEqual(self.run_doc("wasm", tap), [])

    def test_a_c_constant_does_not_excuse_an_absence_meaning(self):
        for name, text in (
            ("SONARE_TARGET_NONE", "`0` is the master mix and `0xFFFF` the unrouted case."),
            ("SONARE_TARGET_INPUT_MONITOR", "Target id, or `0xFFFF` for none."),
        ):
            with self.subTest(name=name, text=text):
                self.setUp()
                self.c_macro(name, "0xFFFFu")
                self.assertEqual(len(self.run_doc("wasm", text)), 1)

    def test_absence_negation_and_real_quantities_pass(self):
        for text in (
            "Root pitch class 0..11 (C=0). Omit when the root is unknown.",
            "Throws RangeError for an unknown name; no surface returns a `-1` sentinel.",
            "True-peak ceiling in dBTP, for example -1 for streaming delivery.",
            "Velocity 1..255 for the note on.",
            "Full 32-bit value, `0`..`4294967295`.",
            "Chroma high-pass corner in Hz; 0 = no high-pass.",
            "Fade-in length in PPQ, 0 for no fade.",
        ):
            with self.subTest(text=text):
                self.setUp()
                self.assertEqual(self.run_doc("node", text), [])


class MirrorSchema(CheckerCase):
    def problems(self, body):
        code, lines = self.run_check("--check", "doc-comments", mirrors=self.write("m.toml", body))
        self.assertEqual(code, 1)
        return lines

    def test_bad_evidence(self):
        self.assertIn(
            "  librosa_mirrors.toml mirror 1 (stft): evidence must be in-tree or recalled, not 'guess'",
            self.problems('[[mirror]]\nname = "stft"\nevidence = "guess"\n'),
        )

    def test_leftover_reason_key(self):
        lines = self.problems('[[mirror]]\nname = "stft"\nevidence = "in-tree"\nreason = "x"\n')
        self.assertIn("  librosa_mirrors.toml mirror 1 (stft): unknown key 'reason'", lines)

    def test_missing_evidence(self):
        self.assertIn(
            "  librosa_mirrors.toml mirror 1 (stft): evidence must be in-tree or recalled, not None",
            self.problems('[[mirror]]\nname = "stft"\n'),
        )


class NullMember(CheckerCase):
    """A JS record field is optional, never `| null`; a declared document follows its schema."""

    def run_null(self, docs=""):
        path = self.write("docs.toml", docs)
        _, lines = self.run_check(
            "--surface",
            "node",
            "--check",
            "absent-not-sentinel:null",
            "--document-types-file",
            str(path),
        )
        flagged = sorted(
            x.strip().split(":")[0] for x in lines if x.strip().endswith("leave it out")
        )
        problems = [x.strip() for x in lines if x.startswith("  document_types.toml")]
        return flagged, problems

    def setUp(self):
        super().setUp()
        for name in ("Report", "Config", "Speaker"):
            self.add(_rec("node", "type", name, ts_kind="interface"))
        for owner, member, typ in (
            ("Report", "lufs", "number | null"),
            ("Report", "peak", "number | undefined"),
            ("Report", "frames", "Array<number | null>"),
            ("Config", "map", "string[] | null | undefined"),
            ("Config", "speaker", "Partial<Record<string, Speaker>>"),
            ("Speaker", "distance", "number | null | undefined"),
        ):
            self.add(
                _rec("node", "field", f"{owner}.{member}", owner=owner, member=member, type=typ)
            )

    def test_a_top_level_null_member_fails(self):
        flagged, _ = self.run_null()
        self.assertEqual(
            flagged,
            [
                "node field Config.map",
                "node field Report.lufs",
                "node field Speaker.distance",
            ],
        )

    def test_a_declared_document_and_the_types_it_reaches_are_exempt(self):
        row = '[[document]]\ntype = "Config"\nevidence = "schemas/playback-renderer-config.schema.json:5: schema"\n'
        flagged, problems = self.run_null(row)
        self.assertEqual(flagged, ["node field Report.lufs"])
        self.assertEqual(problems, [])


class DomainRule(unittest.TestCase):
    def test_override_wins_over_file(self):
        self.assertEqual(
            domains.classify("wasm", "Mixer", "bindings/wasm/src/mixer.ts"), ("engine", "override")
        )
        self.assertEqual(
            domains.classify("wasm", "Limiter", "bindings/wasm/src/mixer.ts"),
            ("mastering", "declaring-file"),
        )

    def test_aggregate_file_falls_back_to_key(self):
        self.assertEqual(
            domains.classify("wasm", "detectBpm", "bindings/wasm/src/index.ts"),
            ("analysis", "aggregate-key"),
        )

    def test_unknown_is_core(self):
        self.assertEqual(
            domains.classify("wasm", "Zzz", "bindings/wasm/src/qqq.ts"), ("core", "default")
        )
        self.assertEqual(
            domains.classify("wasm", "Zzz", "bindings/wasm/src/index.ts"), ("core", "default")
        )

    def test_key_pattern_when_basename_names_no_domain(self):
        f = "bindings/wasm/src/worklet/protocol.ts"
        self.assertEqual(
            domains.classify("wasm", "ENGINE_MIXER_PARAM_PAN", f), ("engine", "key-pattern")
        )
        self.assertEqual(
            domains.classify("wasm", "engineMixerMasterTarget", f), ("engine", "key-pattern")
        )
        self.assertEqual(
            domains.classify("wasm", "SonareWorkletScopeSnapshot", f), ("engine", "key-pattern")
        )
        self.assertEqual(domains.classify("wasm", "decodeHeader", f), ("core", "default"))
        self.assertEqual(
            domains.classify("wasm", "init", "bindings/wasm/src/analysis.ts"), ("core", "override")
        )

    def test_version_is_core_everywhere(self):
        for surface, name, f in (
            ("python", "version", "bindings/python/src/libsonare/_analysis_music.py"),
            ("wasm", "version", "bindings/wasm/src/index.ts"),
            ("c", "sonare_version", "include/sonare/sonare_c.h"),
        ):
            self.assertEqual(domains.classify(surface, name, f), ("core", "override"))

    def test_catalog_types_by_key_prefix(self):
        f = "bindings/node/src/types_capabilities.ts"
        self.assertEqual(
            domains.classify("node", "MasteringInsertSlot", f), ("mastering", "key-pattern")
        )
        self.assertEqual(domains.classify("node", "SynthParamInfo", f), ("engine", "name-pattern"))
        self.assertEqual(domains.classify("node", "CapabilityCatalog", f), ("core", "default"))

    def test_key_patterns_apply_to_free_names_in_aggregate_files(self):
        f = "bindings/wasm/src/index.ts"
        self.assertEqual(domains.classify("wasm", "noteMove", f), ("effects", "key-pattern"))
        self.assertEqual(domains.classify("wasm", "truePeakDb", f), ("mastering", "key-pattern"))
        self.assertEqual(domains.classify("wasm", "rmsEnergy", f), ("analysis", "key-pattern"))

    def test_mastering_repair_prefix_wins_over_the_c_header(self):
        h = "include/sonare/sonare_c_mastering.h"
        self.assertEqual(
            domains.classify("c", "sonare_mastering_repair_declick", h), ("effects", "override")
        )
        self.assertEqual(
            domains.classify("c", "SonareMasteringRepairStage", h), ("effects", "override")
        )
        self.assertEqual(
            domains.classify("c", "sonare_mastering_process", h), ("mastering", "declaring-file")
        )

    def test_scale_name_override_beats_the_scale_pattern(self):
        f = "bindings/wasm/src/public_types_music.ts"
        self.assertEqual(domains.classify("wasm", "ScaleName", f), ("analysis", "override"))
        self.assertEqual(
            domains.classify("node", "ScaleName", "bindings/node/src/types_analysis.ts"),
            ("analysis", "override"),
        )
        self.assertEqual(
            domains.classify("wasm", "scaleMaskForMode", f), ("effects", "name-pattern")
        )
        self.assertEqual(
            domains.classify("wasm", "scalePitchClassEnabled", f), ("effects", "name-pattern")
        )

    def test_feature_probe_is_core(self):
        h = "include/sonare/sonare_c_types_functions.h"
        self.assertEqual(domains.classify("c", "sonare_has_feature", h), ("core", "override"))

    def test_aggregate_with_a_domain_basename_falls_back_to_it(self):
        f = "bindings/python/src/libsonare/_types_analysis.py"
        self.assertEqual(
            domains.classify("python", "NormalizeStereoResult", f), ("mastering", "key-pattern")
        )
        self.assertEqual(domains.classify("python", "Zzz", f), ("analysis", "declaring-file"))

    def test_members_of_a_member_keyed_override_class(self):
        import inventory

        def r(name, owner="", member="", kind="method"):
            return {
                "surface": "wasm",
                "kind": kind,
                "name": name,
                "file": "bindings/wasm/src/audio.ts",
                "line": 1,
                "owner": owner,
                "member": member,
            }

        recs = [
            r("Audio", kind="class"),
            r("Audio.detectChords", "Audio", "detectChords"),
            r("Audio.pitchShift", "Audio", "pitchShift"),
            r("Audio.masterAudio", "Audio", "masterAudio"),
            r("Audio.pitchCorrectToMidi", "Audio", "pitchCorrectToMidi"),
            r("Audio.harmonic", "Audio", "harmonic"),
            r("Audio.peakDb", "Audio", "peakDb"),
            r("Audio.zeroCrossingRate", "Audio", "zeroCrossingRate"),
            r("Audio.destroy", "Audio", "destroy"),
            r("Audio.detectChords(options)", "Audio.detectChords", "options", "option"),
            r("Mixer", kind="class"),
            r("Mixer.processStereo", "Mixer", "processStereo"),
        ]
        _, errors = inventory.assign_domains(recs)
        self.assertEqual(errors, [])
        got = {x["name"]: (x["domain"], x["domain_path"]) for x in recs}
        self.assertEqual(got["Audio"], ("core", "override"))
        self.assertEqual(got["Audio.detectChords"], ("analysis", "member-key"))
        self.assertEqual(got["Audio.pitchShift"], ("effects", "member-key"))
        self.assertEqual(got["Audio.masterAudio"], ("mastering", "member-key"))
        self.assertEqual(got["Audio.pitchCorrectToMidi"], ("effects", "member-key"))
        self.assertEqual(got["Audio.destroy"], ("core", "member-default"))
        self.assertEqual(got["Audio.harmonic"], ("effects", "member-key"))
        self.assertEqual(got["Audio.peakDb"], ("mastering", "member-key"))
        self.assertEqual(got["Audio.zeroCrossingRate"], ("analysis", "member-key"))
        self.assertEqual(got["Audio.detectChords(options)"], ("analysis", "inherited"))
        self.assertEqual(got["Mixer.processStereo"], ("engine", "inherited"))

    def test_unknown_cli_command_fails(self):
        with self.assertRaises(domains.UnknownCommandError):
            domains.classify("cli-native", "no-such-command", "tools/cli/x.cpp")
        import inventory

        recs = [
            {
                "surface": "cli-native",
                "kind": "command",
                "name": "no-such-command",
                "file": "tools/cli/x.cpp",
                "owner": "",
            }
        ]
        _, errors = inventory.assign_domains(recs)
        self.assertEqual(
            errors, ["cli-native command no-such-command: not in the CLI command domain table"]
        )

    def test_a_package_extra_is_core_whatever_its_manifest_is_called(self):
        """The basename `pyproject.toml` would otherwise classify every extra as project."""
        import inventory

        with tempfile.TemporaryDirectory() as d:
            manifest = Path(d) / "bindings/python/pyproject.toml"
            manifest.parent.mkdir(parents=True)
            manifest.write_text(
                '[project]\nname = "x"\n[project.optional-dependencies]\ndev = ["a"]\n'
            )
            recs = inventory._python_extras(Path(d))
        _, errors = inventory.assign_domains(recs)
        self.assertEqual(errors, [])
        self.assertEqual(
            [(r["kind"], r["name"], r["line"], r["domain"]) for r in recs],
            [("extra", "dev", 4, "core")],
        )


class NamePatterns(unittest.TestCase):
    def test_name_patterns_outrank_the_declaring_file(self):
        cases = [
            ("wasm", "synthPresetNames", "bindings/wasm/src/project_synth.ts", "engine"),
            ("c", "SONARE_SYNTH_FIELD_GAIN", "include/sonare/sonare_c_types_analysis.h", "engine"),
            ("wasm", "MeterEstimate", "bindings/wasm/src/public_types_mastering.ts", "analysis"),
            ("node", "RirSynthOptions", "bindings/node/src/types_analysis.ts", "effects"),
            ("node", "DynamicsResult", "bindings/node/src/types_mastering.ts", "analysis"),
            (
                "python",
                "analyze_dynamics",
                "bindings/python/src/libsonare/_mastering.py",
                "analysis",
            ),
            ("c", "sonare_scale_quantize_midi", "include/sonare/sonare_c_mastering.h", "effects"),
            (
                "c",
                "sonare_streaming_retune_create",
                "include/sonare/sonare_c_analysis.h",
                "effects",
            ),
        ]
        for surface, name, file, want in cases:
            with self.subTest(name=name):
                self.assertEqual(domains.classify(surface, name, file), (want, "name-pattern"))

    def test_mastering_dynamics_processors_stay_mastering(self):
        for name in ("MasteringDynamicsCompressor", "DynamicsProcessorResult"):
            with self.subTest(name=name):
                got = domains.classify("wasm", name, "bindings/wasm/src/mastering.ts")
                self.assertEqual(got, ("mastering", "declaring-file"))


class NameVote(unittest.TestCase):
    @staticmethod
    def rec(surface, name, domain, path="declaring-file", file="f", line=1, kind="type"):
        return {
            "surface": surface,
            "kind": kind,
            "name": name,
            "owner": "",
            "file": file,
            "line": line,
            "domain": domain,
            "domain_path": path,
        }

    def test_majority_wins(self):
        recs = [
            self.rec("c", "SonareTrimReport", "mastering"),
            self.rec("wasm", "TrimReport", "effects"),
            self.rec("node", "TrimReport", "effects"),
        ]
        self.assertEqual(
            domains.vote(recs),
            {
                "trim_report": {
                    "domain": "effects",
                    "surfaces": {"c": "mastering", "wasm": "effects", "node": "effects"},
                    "tie": False,
                }
            },
        )

    def test_tie_goes_to_c_then_wasm(self):
        recs = [self.rec("python", "lufs", "mastering"), self.rec("c", "sonare_lufs", "analysis")]
        self.assertEqual(domains.vote(recs)["lufs"]["domain"], "analysis")
        self.assertTrue(domains.vote(recs)["lufs"]["tie"])
        recs = [
            self.rec("python", "lufs", "mastering"),
            self.rec("node", "lufs", "analysis"),
            self.rec("wasm", "lufs", "mastering"),
        ]
        self.assertEqual(domains.vote(recs)["lufs"]["domain"], "mastering")
        self.assertFalse(domains.vote(recs)["lufs"]["tie"])

    def test_two_domains_on_one_surface_vote_with_the_first_record(self):
        recs = [
            self.rec("python", "Clip", "engine", file="b.py"),
            self.rec("python", "Clip", "project", file="a.py"),
            self.rec("node", "Clip", "project"),
        ]
        self.assertEqual(domains.vote(recs)["clip"]["domain"], "project")
        recs = [
            self.rec("python", "Clip", "project", file="a.py"),
            self.rec("node", "Clip", "engine"),
            self.rec("wasm", "Clip", "project"),
        ]
        self.assertEqual(domains.vote(recs)["clip"]["domain"], "project")

    def test_override_pins_the_name(self):
        recs = [
            self.rec("c", "SonareMixer", "mastering"),
            self.rec("node", "Mixer", "mastering"),
            self.rec("wasm", "Mixer", "engine", path="override"),
        ]
        self.assertEqual(domains.vote(recs)["mixer"]["domain"], "engine")

    def test_assign_applies_the_frozen_domain_then_the_ledger(self):
        recs = [
            self.rec("c", "SonareMixer", "mastering"),
            self.rec("wasm", "Mixer", "engine"),
            self.rec("wasm", "NewThing", "core"),
            self.rec("wasm", "Loose", "core"),
        ]
        frozen = {"mixer": {"domain": "engine", "surfaces": ["c", "wasm"]}}
        self.assertEqual(domains.assign(recs, frozen, {"new_thing": "effects"}), ["loose"])
        self.assertEqual(
            [(r["domain"], r["domain_path"]) for r in recs],
            [
                ("engine", "unified"),
                ("engine", "declaring-file"),
                ("effects", "ledger"),
                ("core", "declaring-file"),
            ],
        )

    def test_fixture_domains_file_matches_a_regeneration(self):
        want = domains.regenerate(copy.deepcopy(BASE)["records"])
        self.assertEqual((TD / "domains.json").read_text(encoding="utf-8"), want)


class FrozenDomains(CheckerCase):
    def test_baseline_counts(self):
        _, lines = self.run_check()
        self.assertIn("domain vote: 16 names, 0 by tie-break", lines)
        self.assertIn("unassigned: 0", lines)

    def test_file_wins_over_the_per_surface_domain(self):
        for r in self.inv["records"]:
            if (r["surface"], r["name"]) == ("wasm", "Session"):
                r["domain"] = "mastering"
        self.run_check()
        self.assertEqual(self.report["domain_paths"]["unified"], 1)

    def test_unknown_name_is_unassigned(self):
        self.add(_rec("wasm", "function", "peakLevel", sigs=[], domain="mastering"))
        _, lines = self.run_check()
        self.assertIn("unassigned: 1", lines)
        self.assertIn("  peak_level", lines)

    def test_new_name_inherits_the_old_entry_domain(self):
        for r in self.inv["records"]:
            if (r["surface"], r["name"]) == ("wasm", "detectBpm"):
                r["name"], r["domain"] = "estimateTempo", "engine"
        led = self.ledgers(
            wasm='[[entry]]\nkind = "function"\nold = "detectBpm"\nnew = "estimateTempo"\n'
            'domain = "analysis"\nrule = "getters"\n'
        )
        _, lines = self.run_check(ledger_dir=led)
        self.assertIn("unassigned: 0", lines)
        self.assertEqual(self.report["domain_paths"]["ledger"], 1)

    def test_ledger_domain_disagreeing_with_the_file(self):
        led = self.ledgers(
            wasm='[[entry]]\nkind = "function"\nold = "getBpm"\nnew = "detectBpm"\n'
            'domain = "engine"\nrule = "getters"\n'
        )
        code, lines = self.run_check(ledger_dir=led)
        self.assertEqual(code, 1)
        self.assertIn(
            "  wasm.toml entry 1 (getBpm): domain 'engine' disagrees with domains.json;"
            " expected 'analysis'",
            lines,
        )

    def test_missing_domains_file_is_a_validation_problem(self):
        code, lines = self.run_check(domains_file=self.dir / "absent.json")
        self.assertEqual(code, 1)
        self.assertIn("  absent.json: missing; run with --regenerate-domains", lines)

    def test_regenerate_writes_the_file(self):
        out = self.dir / "domains.json"
        _, lines = self.run_check("--regenerate-domains", domains_file=out)
        self.assertEqual(out.read_text(encoding="utf-8"), (TD / "domains.json").read_text())
        self.assertIn("Regenerated domains.json: 16 names", lines)


class SkipCheck(CheckerCase):
    def test_skip_is_narrowing_not_failure(self):
        code, lines = self.run_check("--skip-check", "doc-comments")
        self.assertEqual(code, 0, "\n".join(lines))
        self.assertIn("  check doc-comments: skipped", lines)
        self.assertIn("    not compared: doc-comments (excluded by --skip-check)", lines)

    def test_unknown_skip_id_is_refused(self):
        with self.assertRaises(SystemExit):
            self.run_check("--skip-check", "no-such-check")


class UpgradeNotes(unittest.TestCase):
    GUIDE = TD / "guide.toml"

    def render(self, ledgers=TD / "ledgers", rules=TD / "rules.toml"):
        return upgrade_notes.render(ledgers, rules, self.GUIDE)

    def test_rows_sit_under_their_surface_domain_and_rule(self):
        text = self.render()
        self.assertEqual(text, self.render())
        self.assertIn(
            "### WASM: Analysis\n\n#### Title of getters\n\n| 1.x | 2.0.0 |\n|---|---|\n"
            "| `getBpm` | `detectBpm` |",
            text,
        )
        self.assertIn(
            "| `bpm --verbose` (native CLI only) | removed: drop the flag; the output is the same |",
            text,
        )
        self.assertIn("| `detect_bpm` | sample_rate is a required argument. |", text)
        self.assertIn("## Python\n\nLead of Python.", text)
        python = text[text.index("## Python") : text.index("## Node")]
        self.assertLess(
            python.index("#### Title of sample-rate-required"),
            python.index("#### Title of individual"),
        )
        # The C fixture ledger has no entries, so the surface has no section.
        self.assertNotIn("## C\n", text)
        self.assertLess(text.index("## Python"), text.index("## WASM"))
        self.assertLess(text.index("## WASM"), text.index("## Command line"))

    def test_a_wasm_row_names_a_subpath_entry(self):
        e = {"old": "getBpm", "entry": "./analysis"}
        self.assertEqual(upgrade_notes._old("wasm", e), "`getBpm` (`./analysis`)")
        self.assertEqual(upgrade_notes._old("wasm", {"old": "getBpm", "entry": "."}), "`getBpm`")

    def test_an_extra_row_names_the_install_spelling(self):
        e = {"kind": "extra", "old": "dev"}
        self.assertEqual(upgrade_notes._old("python", e), "`libsonare[dev]`")

    def test_removed_row_says_what_replaces_it(self):
        self.assertEqual(upgrade_notes._target({"removed": "call x"}), "removed: call x")
        self.assertEqual(
            upgrade_notes._target({"removed": True, "no_replacement": "gone"}),
            "removed, no replacement: gone",
        )

    def test_changed_row_leads_with_its_target(self):
        e = {"old": "a", "changed": "takes an object", "to": "b"}
        self.assertEqual(upgrade_notes._target(e), "`b`: takes an object")

    def test_conventions_list_cited_rules_and_rules_with_an_example(self):
        base = (TD / "rules.toml").read_text()
        anchor = 'summary = "Summary of teardown."\n'
        self.assertIn(anchor, base)
        example = '[[rule.example]]\nsurface = "wasm"\nbefore = "old()"\nafter = "new()"\n'
        with tempfile.TemporaryDirectory() as tmp:
            rules = Path(tmp) / "rules.toml"
            rules.write_text(base.replace(anchor, anchor + example), encoding="utf-8")
            text = self.render(rules=rules)
        conventions = text[text.index("## The conventions behind the changes") :]
        self.assertIn("### Title of teardown\n\nSummary of teardown.", conventions)
        self.assertIn("WASM, 1.x:\n\n```ts\nold()\n```", conventions)
        self.assertIn("### Title of getters", conventions)
        self.assertNotIn("### Title of async-teardown", conventions)

    def test_a_row_with_no_section_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            for s in LEDGER_FILES:
                (d / f"{s}.toml").write_text(
                    (TD / "ledgers" / f"{s}.toml").read_text(), encoding="utf-8"
                )
            for field, value in (("domain", "nowhere"), ("rule", "no-such-rule")):
                row = {"domain": "analysis", "rule": "getters", field: value}
                (d / "node.toml").write_text(
                    '[[entry]]\nkind = "function"\nold = "lost"\nnew = "found"\n'
                    f'domain = "{row["domain"]}"\nrule = "{row["rule"]}"\n',
                    encoding="utf-8",
                )
                with self.assertRaisesRegex(ValueError, "'lost'"):
                    self.render(ledgers=d)

    def test_entry_order_does_not_change_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            for s in LEDGER_FILES:
                (d / f"{s}.toml").write_text(
                    (TD / "ledgers" / f"{s}.toml").read_text(), encoding="utf-8"
                )
            extra = (
                '[[entry]]\nkind = "function"\nold = "aaa"\nremoved = true\n'
                'domain = "analysis"\nrule = "getters"\n'
            )
            base = (d / "wasm.toml").read_text()
            (d / "wasm.toml").write_text(base + "\n" + extra)
            first = self.render(ledgers=d)
            (d / "wasm.toml").write_text(extra + "\n" + base)
            self.assertEqual(first, self.render(ledgers=d))

    def test_check_fails_on_a_stale_file_and_passes_on_a_fresh_one(self):
        args = [
            "--ledger-dir",
            str(TD / "ledgers"),
            "--rules-file",
            str(TD / "rules.toml"),
            "--guide-file",
            str(self.GUIDE),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "UPGRADING.md"
            self.assertEqual(upgrade_notes.main([*args, "--check", "-o", str(out)]), 1)
            self.assertEqual(upgrade_notes.main([*args, "-o", str(out)]), 0)
            self.assertEqual(upgrade_notes.main([*args, "--check", "-o", str(out)]), 0)
            out.write_text(out.read_text() + "edited\n", encoding="utf-8")
            self.assertEqual(upgrade_notes.main([*args, "--check", "-o", str(out)]), 1)

    def test_the_guide_has_a_section_for_every_ledger_and_domain(self):
        guide = upgrade_notes._load(upgrade_notes.DEFAULT_GUIDE)
        self.assertEqual({s["key"] for s in guide["surface"]}, set(LEDGER_FILES))
        self.assertEqual(tuple(d["key"] for d in guide["domain"]), domains.DOMAINS)
        fixture = upgrade_notes._load(self.GUIDE)
        self.assertEqual(tuple(d["key"] for d in fixture["domain"]), domains.DOMAINS)


class RateFromCEntry(CheckerCase):
    """A sample rate is required where the C entry the operation maps to takes one."""

    def c_fn(self, name, *params):
        self.add(_fn("c", name, "SonareError", *(_c_param(n, t) for n, t in params)))

    def rate(self):
        _, lines = self.run_check("--check", "sample-rate-required")
        return sorted(x.strip() for x in lines if x.startswith("    ") and " trim" in x.lower())

    def test_an_operation_whose_c_entry_takes_no_rate_is_not_compared(self):
        self.c_fn("sonare_trim", ("samples", "const float*"), ("length", "size_t"))
        self.add(_py_fn("trim", ("samples", None)))
        self.add(_rec("node", "type", "TrimRequest"))
        self.add(_field("node", "TrimRequest", "samples", "Float32Array"))
        self.assertEqual(
            self.rate(),
            [
                "not compared: node type TrimRequest (sonare_trim takes no sample rate)",
                "not compared: python function trim (sonare_trim takes no sample rate)",
            ],
        )

    def test_an_operation_whose_c_entry_takes_a_rate_must_require_one(self):
        self.c_fn("sonare_trim", ("samples", "const float*"), ("sample_rate", "int"))
        self.add(_py_fn("trim", ("samples", None), ("sample_rate", "22050")))
        self.assertEqual(
            self.rate(), ["python function trim: sample_rate has a default; make it required"]
        )

    def test_an_audio_free_query_may_leave_its_rate_absent(self):
        self.c_fn("sonare_trim", ("name", "const char*"), ("sample_rate", "int"))
        self.add(_py_fn("trim", ("name", None), ("sample_rate", "None")))
        self.assertEqual(self.rate(), [])

    def test_an_audio_free_query_may_not_default_its_rate_to_a_number(self):
        self.c_fn("sonare_trim", ("name", "const char*"), ("sample_rate", "int"))
        self.add(_py_fn("trim", ("name", None), ("sample_rate", "48000")))
        self.assertEqual(
            self.rate(), ["python function trim: sample_rate has a default; make it required"]
        )

    def test_audio_in_may_not_leave_its_rate_absent(self):
        self.c_fn("sonare_trim", ("samples", "const float*"), ("sample_rate", "int"))
        self.add(_py_fn("trim", ("samples", None), ("sample_rate", "None")))
        self.assertEqual(
            self.rate(), ["python function trim: sample_rate has a default; make it required"]
        )

    def test_a_rate_in_the_c_options_struct_counts(self):
        self.c_fn(
            "sonare_trim", ("samples", "const float*"), ("options", "const SonareBpmOptions*")
        )
        self.add(_py_fn("trim", ("samples", None)))
        self.assertEqual(
            self.rate(), ["python function trim: takes audio but has no sample_rate parameter"]
        )

    def test_a_rate_on_the_ex_variant_counts(self):
        self.c_fn("sonare_trim", ("samples", "const float*"))
        self.c_fn("sonare_trim_ex", ("samples", "const float*"), ("sample_rate", "int"))
        self.add(_py_fn("trim", ("samples", None)))
        self.assertEqual(
            self.rate(), ["python function trim: takes audio but has no sample_rate parameter"]
        )


def _field(surface, owner, member, type="number", **kw):
    return _rec(surface, "field", f"{owner}.{member}", owner=owner, member=member, type=type, **kw)


def _fn(surface, name, returns="void", *params, **kw):
    return _rec(
        surface, "function", name, sigs=[{"params": list(params), "returns": returns}], **kw
    )


def _member_method(surface, owner, member):
    return _rec(
        surface,
        "method",
        f"{owner}.{member}",
        owner=owner,
        member=member,
        static=False,
        sigs=[{"params": [], "returns": "void"}],
    )


def _c_param(name, type):
    return dict(_param(name, type=type), default=None)


# One reintroduced violation per rule check: (check id, records, the exact finding lines).
REFUSALS = (
    (
        "seconds-suffix",
        [_field("node", "FadeOptions", "fadeSeconds")],
        ["node field FadeOptions.fadeSeconds: name ends in seconds; use the Sec suffix"],
    ),
    (
        "teardown:js",
        [_rec("node", "class", "Player", sigs=[]), _member_method("node", "Player", "close")],
        [
            "node class Player: has a teardown member but no destroy()",
            "node class Player: has a teardown member but no [Symbol.dispose] / [Symbol.asyncDispose]",
            "node class Player: teardown is spelled destroy(), not close()",
        ],
    ),
    (
        "teardown:python",
        [
            _rec("python", "class", "Player"),
            _member_method("python", "Player", "destroy"),
            _member_method("python", "Player", "__enter__"),
        ],
        [
            "python class Player: has a teardown member but no close",
            "python class Player: has a teardown member but no __exit__",
            "python class Player: teardown is spelled close(), not destroy()",
        ],
    ),
    (
        "no-internal-types",
        [_fn("wasm", "renderStems", "WasmStemSet")],
        ["wasm function renderStems: exposes internal type WasmStemSet"],
    ),
    (
        "worklet-scope",
        [_fn("node", "packEvents", file="bindings/node/src/internal/events.ts")],
        [
            "node function packEvents: declared in internal module bindings/node/src/internal/events.ts"
        ],
    ),
    (
        "record-input-pair",
        [
            _rec("node", "type", "TempoMark"),
            _rec("node", "type", "TempoMarkInput"),
            _rec("python", "type", "TempoMark"),
        ],
        [
            "python type TempoMark: Node/WASM split it into TempoMark and TempoMarkInput; Python must carry both"
        ],
    ),
    (
        "progress-callback",
        [_fn("node", "separateWithProgress")],
        [
            "node function separateWithProgress: progress is an option of the operation, not a WithProgress twin"
        ],
    ),
    (
        "options-struct:suffix",
        [_fn("c", "sonare_detect_bpm_ex", "SonareError")],
        ["c function sonare_detect_bpm_ex: suffix _ex marks a second form of one operation"],
    ),
    (
        "options-struct:suffix",
        [_fn("c", "sonare_tempogram", "SonareError"), _fn("c", "sonare_tempogram_with_mode")],
        [
            (
                "c function sonare_tempogram_with_mode: is sonare_tempogram plus _with_mode,"
                " a second form of sonare_tempogram"
            )
        ],
    ),
    (
        "options-struct:struct-size",
        [
            _rec("c", "type", "SonareFadeOptions", c_kind="struct"),
            _field("c", "SonareFadeOptions", "struct_version", "uint32_t", index=0),
            _field("c", "SonareFadeOptions", "length", "size_t", index=1),
        ],
        [
            "c type SonareFadeOptions: first field must be uint32_t struct_size",
            "c type SonareFadeOptions: no sonare_fade_options_init()",
            "c type SonareFadeOptions: field struct_version duplicates struct_size",
        ],
    ),
    (
        "c-create",
        [_fn("c", "sonare_player_create", "SonarePlayer*", _c_param("sample_rate", "int"))],
        [
            "c function sonare_player_create: returns SonarePlayer*, not SonareError",
            "c function sonare_player_create: last parameter is not the out-pointer to the handle",
        ],
    ),
    (
        "c-matrix-shape",
        [
            _fn(
                "c",
                "sonare_grid",
                "SonareError",
                _c_param("out", "float**"),
                _c_param("out_rows", "int*"),
                _c_param("out_n_frames", "size_t*"),
            ),
            _fn(
                "c",
                "sonare_vector",
                "SonareError",
                _c_param("out", "float**"),
                _c_param("out_count", "size_t*"),
            ),
            _fn(
                "c",
                "sonare_shaped",
                "SonareError",
                _c_param("out", "float**"),
                _c_param("out_length", "size_t*"),
                _c_param("out_n_bins", "int*"),
                _c_param("out_n_frames", "int*"),
            ),
            _fn(
                "c",
                "sonare_render",
                "SonareError",
                _c_param("out", "float**"),
                _c_param("out_frames", "size_t*"),
                _c_param("out_channels", "int*"),
            ),
            _fn(
                "c",
                "sonare_wide",
                "SonareError",
                _c_param("out", "float**"),
                _c_param("out_frames", "size_t*"),
                _c_param("out_channels", "size_t*"),
            ),
        ],
        [
            "c function sonare_grid: dimension out_n_frames is size_t, not int",
            "c function sonare_grid: dimension out_rows does not name what it counts",
            "c function sonare_grid: no size_t* out_length beside the matrix",
            "c function sonare_wide: dimension out_channels is size_t, not int",
            "c function sonare_wide: dimension out_frames is size_t, not int",
            "c function sonare_wide: no size_t* out_length beside the matrix",
        ],
    ),
    (
        "c-count-types",
        [
            _field("c", "SonareRegion", "frame_count", "int"),
            _field("c", "SonareRegion", "start_sample", "size_t"),
            _field("c", "SonareRegion", "sample_rate", "float"),
            _field("c", "SonareMidiEventView", "word_count", "uint8_t"),
        ],
        [
            "c field SonareMidiEventView.word_count: word_count is size_t, not uint8_t",
            "c field SonareRegion.frame_count: frame_count is size_t, not int",
            "c field SonareRegion.start_sample: start_sample is int64_t, not size_t",
            "c field SonareRegion.sample_rate: sample_rate is int, not float",
        ],
    ),
    (
        "getters",
        [_member_method("node", "Player", "getVolume"), _fn("python", "get_tempo")],
        [
            "node method Player.getVolume: drop the get prefix",
            "python function get_tempo: drop the get prefix",
        ],
    ),
    (
        "typed-results",
        [_fn("node", "suggestMastering", "TypedJson<MasteringAssistantResult>")],
        [
            "node function suggestMastering: returns or takes a JSON string; return the parsed object"
        ],
    ),
    (
        "array-results",
        [
            _fn("python", "beat_times", "list[float]"),
            _fn("python", "key_pair", "tuple[str, str]"),
            _fn("python", "notes", "tuple[tuple[Note, ...], tuple[Transition, ...]] | None"),
        ],
        [
            "python function beat_times: returns a list of numbers; return an ndarray",
            "python function key_pair: returns a bare tuple; return a typed result",
            "python function notes: returns a bare tuple; return a typed result",
        ],
    ),
    (
        "frozen-results",
        [
            _rec("python", "type", "TempoResult", dataclass=True, frozen=False),
            _fn("python", "tempo", "TempoResult"),
        ],
        ["python type TempoResult: result dataclass is not frozen=True"],
    ),
    (
        "one-positional-form",
        [_fn("node", "trim", "void", *(_param(n) for n in "abcd"))],
        ["node function trim: 4 positional parameters; take a request object"],
    ),
    (
        "digit-tokens",
        [
            _field(
                "node",
                "EqOptions",
                "slope",
                "string",
                inline_literals=True,
                literals=["band-3", "12dB"],
            )
        ],
        [
            "node field EqOptions.slope: token 'band-3' separates digits with - or _",
            "node field EqOptions.slope: token '12dB' spells the unit dB; use Db as in identifiers",
        ],
    ),
    (
        "high-pass",
        [
            _field("node", "EqOptions", "hpfCutoffHz"),
            _field("python", "EqOptions", "highpass"),
            _field("wasm", "DeesserOptions", "bandpassQ"),
            _field(
                "node",
                "WahOptions",
                "filterType",
                "'bandPass' | 'lowpass'",
                inline_literals=True,
                literals=["bandPass", "lowpass"],
            ),
        ],
        [
            "node field EqOptions.hpfCutoffHz: spell high-pass as highPass / high_pass",
            "python field EqOptions.highpass: spell high-pass as highPass / high_pass",
            "wasm field DeesserOptions.bandpassQ: spell band-pass as bandPass / band_pass",
            (
                "node field WahOptions.filterType: token 'lowpass' spells low-pass as one word;"
                " use lowPass"
            ),
        ],
    ),
    (
        "num-channels",
        [_field("node", "Layout", "channelCount"), _field("python", "Layout", "channels", "int")],
        [
            "node field Layout.channelCount: channelCount: a channel count is numChannels / num_channels",
            "python field Layout.channels: field channels holds a count; channels is the planar array",
        ],
    ),
    (
        "pitch-semitones",
        [_field("node", "ShiftOptions", "semitones")],
        ["node field ShiftOptions.semitones: semitones does not say which pitch quantity it moves"],
    ),
    (
        "frequency-bounds",
        [_field("node", "PitchOptions", "fmin"), _field("python", "PitchOptions", "f0_max")],
        [
            "node field PitchOptions.fmin: fmin is a frequency bound; add Hz",
            "python field PitchOptions.f0_max: f0_max is a frequency bound; add Hz",
        ],
    ),
    (
        "frequency-bounds:centre",
        [_field("node", "BandOptions", "centreHz")],
        ["node field BandOptions.centreHz: spell centre as center"],
    ),
    (
        "python-public-names",
        [
            _fn("python", "separate", "StemBuffers"),
            _fn("python", "decode", "_RawFrames"),
            _rec("python", "class", "Mixer", in_all=False, modules=["__init__", "mixing"]),
        ],
        [
            "python function separate: annotation names StemBuffers, which libsonare does not export",
            "python function decode: annotation names _RawFrames, which libsonare does not export",
            "python class Mixer: public in libsonare.mixing but not in __all__",
        ],
    ),
    (
        "cli-positional-input",
        [
            _rec("cli-python", "flag", "bpm --in", owner="bpm", member="in"),
            _rec("cli-native", "flag", "bpm --use-hpss", owner="bpm", member="use-hpss"),
        ],
        [
            "cli-native flag bpm --use-hpss: --use-hpss: name the feature, not the act of using it",
            "cli-python flag bpm --in: --in names an input; take it positionally",
        ],
    ),
)


# Shapes a rule allows that a check once refused: (check id, records).
PERMITS = (
    (
        "token-case",
        [
            _field(
                "node",
                "LibError",
                "name",
                "'LibError'",
                inline_literals=True,
                literals=["LibError"],
            ),
            _field(
                "wasm", "Handle", "[brand]", "'Handle'", inline_literals=True, literals=["Handle"]
            ),
        ],
    ),
    (
        "pitch-semitones",
        [
            _rec(
                "node",
                "value",
                "ParamUnit='semitones'",
                owner="ParamUnit",
                member="semitones",
                token="semitones",
            )
        ],
    ),
    ("frequency-bounds:centre", [_field("node", "PitchOptions", "centRefHz")]),
    (
        "array-results",
        [
            _fn("python", "edit_dependencies", "tuple[Dependency, ...]"),
            _fn("python", "enum_tables", "dict[str, tuple[str, ...]]"),
        ],
    ),
)


class Refusals(CheckerCase):
    """Each rule check refuses the shape it exists for; the fixture alone is green."""

    def test_every_rule_check_refuses_a_reintroduced_violation(self):
        for check, records, want in REFUSALS:
            with self.subTest(check=check):
                self.inv = copy.deepcopy(BASE)
                _, before = self.run_check("--check", check)
                self.add(*copy.deepcopy(records))
                _, lines = self.run_check("--check", check)
                found = sorted(x.strip() for x in set(lines) - set(before) if x.startswith("    "))
                self.assertEqual(found, sorted(want))

    def test_a_shape_the_rule_allows_is_not_refused(self):
        for check, records in PERMITS:
            with self.subTest(check=check):
                self.inv = copy.deepcopy(BASE)
                _, before = self.run_check("--check", check)
                self.add(*copy.deepcopy(records))
                _, lines = self.run_check("--check", check)
                self.assertEqual(
                    sorted(x.strip() for x in set(lines) - set(before) if x.startswith("    ")), []
                )


class FixedLayoutRecords(CheckerCase):
    """A count inside a named fixed-layout realtime record keeps its width and is counted."""

    def test_a_fixed_layout_record_count_is_counted_not_refused(self):
        _, before = self.run_check("--check", "c-count-types")
        self.add(
            _field("c", "SonareEngineMidiEvent", "word_count", "uint8_t"),
            _field("c", "SonareScopeTelemetryRecord", "point_count", "uint32_t"),
        )
        _, lines = self.run_check("--check", "c-count-types")
        added = [x.strip() for x in set(lines) - set(before) if x.startswith("    ")]
        self.assertEqual(len(added), 1, added)
        self.assertTrue(added[0].startswith("not compared: 2 count member(s) of fixed-layout"))


class SecondFormExtensions(CheckerCase):
    """A C function named as another plus an extension is a second form unless it names
    what it acts on or is allowlisted as a distinct operation."""

    def allow(self, name, category="distinct-operation"):
        return self.write(
            "allow.toml",
            f'[[allow]]\ncheck = "options-struct:suffix"\nsurface = "c"\nname = "{name}"\n'
            f'category = "{category}"\nreason = "r"\n',
        )

    def flagged(self, lines):
        return [x.strip() for x in lines if x.startswith("    c function")]

    def test_an_initializer_and_a_release_are_counted_not_refused(self):
        _, before = self.run_check("--check", "options-struct:suffix")
        self.add(
            _fn("c", "sonare_render"),
            _fn("c", "sonare_render_options_init"),
            _fn("c", "sonare_render_event_config_init"),
            _fn("c", "sonare_free_frames"),
            _fn("c", "sonare_free_frames_i16"),
        )
        _, lines = self.run_check("--check", "options-struct:suffix")
        added = [x.strip() for x in set(lines) - set(before) if x.startswith("    ")]
        self.assertEqual(
            added,
            [
                (
                    "not compared: functions named as another plus an extension that names"
                    " what they act on: 1 releases of a distinct result type; 2 struct"
                    " initializers"
                )
            ],
        )

    def test_a_distinct_operation_is_excused(self):
        self.add(_fn("c", "sonare_cqt"), _fn("c", "sonare_cqt_to_audio"))
        allow = self.allow("sonare_cqt_to_audio")
        _, lines = self.run_check("--check", "options-struct:suffix", allowlist=allow)
        self.assertEqual(self.flagged(lines), [])
        self.assertIn("excused 1", "\n".join(lines))

    def test_a_distinct_operation_does_not_excuse_a_fixed_suffix(self):
        self.add(_fn("c", "sonare_cqt"), _fn("c", "sonare_cqt_ex"))
        allow = self.allow("sonare_cqt_ex")
        code, lines = self.run_check("--check", "options-struct:suffix", allowlist=allow)
        self.assertEqual(code, 1)
        self.assertEqual(
            self.flagged(lines),
            [
                (
                    "c function sonare_cqt_ex: suffix _ex marks a second form of one operation;"
                    " allowlisted as distinct-operation, but a fixed second-form suffix, a"
                    " channel form or a sample format names no other operation"
                )
            ],
        )

    def mono(self, name, *tail, returns="SonareError"):
        audio = (_c_param("samples", "const float*"), _c_param("length", "size_t"))
        return _fn("c", name, returns, *audio, _c_param("sample_rate", "int"), *tail)

    def stereo(self, name, *tail, returns="SonareError"):
        audio = (
            _c_param("left", "const float*"),
            _c_param("right", "const float*"),
            _c_param("length", "size_t"),
        )
        return _fn("c", name, returns, *audio, _c_param("sample_rate", "int"), *tail)

    def test_a_channel_form_matching_its_base_is_counted_not_refused(self):
        _, before = self.run_check("--check", "options-struct:suffix")
        self.add(
            self.mono("sonare_dehum", _c_param("out", "SonareDehumResult*")),
            self.stereo("sonare_dehum_stereo", _c_param("out", "SonareDehumStereoResult*")),
        )
        _, lines = self.run_check("--check", "options-struct:suffix")
        added = [x.strip() for x in set(lines) - set(before) if x.startswith("    ")]
        self.assertEqual(
            added,
            [
                (
                    "not compared: 1 channel-form twins (_stereo, _linked, _interleaved)"
                    " match their base after the audio"
                )
            ],
        )

    def test_a_stereo_buffer_pair_matches_a_base_buffer(self):
        _, before = self.run_check("--check", "options-struct:suffix")
        self.add(
            self.mono(
                "sonare_morph", _c_param("out", "float**"), _c_param("out_length", "size_t*")
            ),
            self.stereo(
                "sonare_morph_stereo",
                _c_param("out_left", "float**"),
                _c_param("out_right", "float**"),
                _c_param("out_length", "size_t*"),
            ),
        )
        _, lines = self.run_check("--check", "options-struct:suffix")
        added = [x.strip() for x in set(lines) - set(before) if x.startswith("    ")]
        self.assertEqual(len(added), 1, added)
        self.assertIn("1 channel-form twins", added[0])

    def test_a_channel_form_with_another_result_is_refused_and_not_excusable(self):
        self.add(
            self.mono(
                "sonare_morph", _c_param("out", "float**"), _c_param("out_length", "size_t*")
            ),
            self.stereo("sonare_morph_stereo", _c_param("out", "SonareMorphStereoResult*")),
        )
        allow = self.allow("sonare_morph_stereo")
        code, lines = self.run_check("--check", "options-struct:suffix", allowlist=allow)
        self.assertEqual(code, 1)
        self.assertEqual(
            self.flagged(lines),
            [
                (
                    "c function sonare_morph_stereo: is the _stereo channel form of sonare_morph"
                    " but differs from it after the audio: base has float** out, size_t*"
                    " out_length; twin has SonareMorphStereoResult* out; allowlisted as"
                    " distinct-operation, but a fixed second-form suffix, a channel form or a"
                    " sample format names no other operation"
                )
            ],
        )

    def frames(self, struct, elem):
        return (
            _rec("c", "type", struct, c_kind="struct"),
            _field("c", struct, "n_frames", "int", index=0),
            _field("c", struct, "mel", f"{elem}*", index=1),
        )

    def reader(self, name, struct, *extra):
        return _fn(
            "c",
            name,
            "SonareError",
            _c_param("analyzer", "SonareStreamAnalyzer*"),
            *extra,
            _c_param("out", f"{struct}*"),
        )

    def test_a_sample_format_differing_in_element_type_alone_is_counted(self):
        _, before = self.run_check("--check", "options-struct:suffix")
        self.add(
            *self.frames("SonareFrames", "float"),
            *self.frames("SonareFramesU8", "uint8_t"),
            self.reader("sonare_read_frames", "SonareFrames"),
            self.reader("sonare_read_frames_u8", "SonareFramesU8"),
        )
        _, lines = self.run_check("--check", "options-struct:suffix")
        added = [x.strip() for x in set(lines) - set(before) if x.startswith("    ")]
        self.assertEqual(len(added), 1, added)
        self.assertIn("1 sample-format twins (_u8, _i16)", added[0])

    def test_a_sample_format_with_another_input_or_member_is_refused(self):
        self.add(
            *self.frames("SonareFrames", "float"),
            _field("c", "SonareFrames", "chord_root", "int*", index=2),
            *self.frames("SonareFramesI16", "int16_t"),
            self.reader("sonare_read_frames", "SonareFrames"),
            self.reader(
                "sonare_read_frames_i16",
                "SonareFramesI16",
                _c_param("config", "const SonareQuantizeConfig*"),
            ),
        )
        allow = self.allow("sonare_read_frames_i16")
        code, lines = self.run_check("--check", "options-struct:suffix", allowlist=allow)
        self.assertEqual(code, 1)
        self.assertEqual(
            self.flagged(lines),
            [
                (
                    "c function sonare_read_frames_i16: is the _i16 sample format of"
                    " sonare_read_frames but differs from it: parameters: sonare_read_frames"
                    " has nothing else; twin has const SonareQuantizeConfig* config; SonareFramesI16"
                    " lacks chord_root; allowlisted as distinct-operation, but a fixed"
                    " second-form suffix, a channel form or a sample format names no other"
                    " operation"
                )
            ],
        )

    def test_two_sample_formats_with_no_base_are_compared_with_each_other(self):
        self.add(
            *self.frames("SonareQuantU8", "uint8_t"),
            *self.frames("SonareQuantI16", "int16_t"),
            self.reader("sonare_read_quantized_u8", "SonareQuantU8"),
            self.reader("sonare_read_quantized_i16", "SonareQuantI16"),
        )
        code, lines = self.run_check("--check", "options-struct:suffix")
        self.assertEqual(code, 0)
        self.assertEqual(self.flagged(lines), [])
        self.assertIn("sample-format twins", "\n".join(lines))

    def test_sibling_sample_formats_that_differ_are_refused_and_not_excusable(self):
        self.add(
            *self.frames("SonareQuantU8", "uint8_t"),
            _field("c", "SonareQuantU8", "chord_root", "int*", index=2),
            *self.frames("SonareQuantI16", "int16_t"),
            self.reader("sonare_read_quantized_u8", "SonareQuantU8"),
            self.reader("sonare_read_quantized_i16", "SonareQuantI16"),
        )
        allow = self.allow("sonare_read_quantized_i16")
        code, lines = self.run_check("--check", "options-struct:suffix", allowlist=allow)
        self.assertEqual(code, 1)
        self.assertEqual(
            self.flagged(lines),
            [
                (
                    "c function sonare_read_quantized_i16: is the _i16 sample format beside"
                    " sonare_read_quantized_u8 but differs from it: SonareQuantI16 lacks"
                    " chord_root; allowlisted as distinct-operation, but a fixed second-form"
                    " suffix, a channel form or a sample format names no other operation"
                ),
                (
                    "c function sonare_read_quantized_u8: is the _u8 sample format beside"
                    " sonare_read_quantized_i16 but differs from it: SonareQuantU8 adds"
                    " chord_root"
                ),
            ],
        )

    def test_the_category_covers_no_other_check(self):
        allow = self.write(
            "allow.toml",
            '[[allow]]\ncheck = "c-free"\nsurface = "c"\nname = "x"\n'
            'category = "distinct-operation"\nreason = "r"\n',
        )
        self.run_check("--check", "c-free", allowlist=allow)
        self.assertIn(
            "surface_allowlist.toml allow 1: category distinct-operation does not apply to c-free",
            self.report["problems"],
        )


class AllowCategories(CheckerCase):
    """A category with a definition excuses only the findings that meet it."""

    def allow(self, check, name, category):
        return self.write(
            "allow.toml",
            f'[[allow]]\ncheck = "{check}"\nsurface = "node"\nname = "{name}"\n'
            f'category = "{category}"\nreason = "r"\n',
        )

    def tube(self, *tokens):
        literal = " | ".join(f"'{t}'" for t in tokens)
        self.add(
            _field("node", "Amp", "tube", literal, inline_literals=True, literals=list(tokens))
        )

    def test_a_standard_symbol_excuses_a_token_with_no_lower_camel_form(self):
        self.tube("6l6", "6v6")
        allow = self.allow("token-case", "Amp.tube", "standard-symbol")
        code, lines = self.run_check("--check", "token-case", allowlist=allow)
        self.assertEqual(code, 0, "\n".join(lines))
        self.assertEqual([x for x in lines if x.startswith("    node field Amp.tube")], [])

    def test_a_standard_symbol_does_not_excuse_a_separated_token(self):
        self.tube("6l6", "el_34")
        allow = self.allow("token-case", "Amp.tube", "standard-symbol")
        code, lines = self.run_check("--check", "token-case", allowlist=allow)
        self.assertEqual(code, 1)
        self.assertEqual(
            [x.strip() for x in lines if x.startswith("    node field Amp.tube")],
            [
                (
                    "node field Amp.tube: token 'el_34' is not lowerCamel; allowlisted as"
                    " standard-symbol, but a token with a word separator has a lowerCamel form"
                )
            ],
        )

    def test_a_scalar_function_is_excused(self):
        self.add(
            _fn("node", "snap", "number", *(_param(n) for n in "abc"), _param("d", optional=True))
        )
        allow = self.allow("one-positional-form", "snap", "scalar")
        code, lines = self.run_check("--check", "one-positional-form", allowlist=allow)
        self.assertEqual(code, 0, "\n".join(lines))

    def test_a_function_taking_options_is_not_scalar(self):
        self.add(
            _fn(
                "node",
                "snap",
                "number",
                *(_param(n) for n in "abc"),
                _param("o", type="SnapOptions"),
            )
        )
        allow = self.allow("one-positional-form", "snap", "scalar")
        code, lines = self.run_check("--check", "one-positional-form", allowlist=allow)
        self.assertEqual(code, 1)
        self.assertIn(
            "    node function snap: 4 positional parameters; take a request object; allowlisted"
            " as scalar, but a scalar function takes at most four number or boolean parameters"
            " and no options",
            lines,
        )

    def test_a_category_on_a_check_it_does_not_cover_is_a_problem(self):
        allow = self.allow("token-case", "snap", "scalar")
        self.run_check("--check", "token-case", allowlist=allow)
        self.assertIn(
            "surface_allowlist.toml allow 1: category scalar does not apply to token-case",
            self.report["problems"],
        )


class CliAliases(unittest.TestCase):
    """Every long spelling of a CLI option is inventoried, later ones naming the first."""

    def test_native_alias_lists_are_read_by_primary_name(self):
        import inventory

        call = (
            '"resample", {required_int("target-rate", {"target-sr"}),'
            ' flag("quiet", true, false, {"q"}), string_value("mode")}'
        )
        self.assertEqual(inventory._native_aliases(call), {"target-rate": ["target-sr"]})

    def test_python_option_strings_after_the_first_are_aliases(self):
        import inventory

        with tempfile.TemporaryDirectory() as d:
            pkg = Path(d) / inventory.PY_PACKAGE
            pkg.mkdir(parents=True)
            (pkg / "cli.py").write_text(
                "import argparse\n"
                "p = argparse.ArgumentParser()\n"
                "sub = p.add_subparsers()\n"
                'r = sub.add_parser("resample")\n'
                'r.add_argument("--target-rate", "--target-sr", type=int)\n'
                'r.add_argument("--quiet", "-q", action="store_true")\n',
                encoding="utf-8",
            )
            scan = inventory._PyCliScan(Path(d))
            scan.scan()
        self.assertEqual(
            sorted(scan.commands["resample"]["flags"]), ["quiet", "target-rate", "target-sr"]
        )
        self.assertEqual(scan.alias_of, {"target-sr": "target-rate"})


class CliFlagLiteralTest(unittest.TestCase):
    """A CLI flag name is a string literal at its registration, on both front-ends."""

    def _python(self, body: str):
        import inventory

        with tempfile.TemporaryDirectory() as d:
            pkg = Path(d) / inventory.PY_PACKAGE
            pkg.mkdir(parents=True)
            (pkg / "cli.py").write_text(
                "import argparse\n"
                "class P(argparse.ArgumentParser):\n"
                "    def add_argument(self, *args, **kwargs):\n"
                "        return super().add_argument(*args, **kwargs)\n"
                "p = P()\n"
                "sub = p.add_subparsers()\n"
                'r = sub.add_parser("resample")\n' + body,
                encoding="utf-8",
            )
            scan = inventory._PyCliScan(Path(d))
            scan.scan()
        return scan

    def test_python_literal_flag_is_read(self):
        scan = self._python('r.add_argument("--target-rate", type=int)\n')
        self.assertEqual(scan.problems, [])
        self.assertEqual(scan.registrations, 1)

    def test_python_non_literal_flag_fails(self):
        scan = self._python('NAME = "--target-rate"\nr.add_argument(NAME, type=int)\n')
        self.assertEqual(scan.registrations, 1)
        self.assertEqual(len(scan.problems), 1)
        self.assertRegex(scan.problems[0], r"cli\.py:9: flag name is not a literal$")

    def _native(self, body: str):
        import inventory

        with tempfile.TemporaryDirectory() as d:
            cli = Path(d) / "tools" / "cli"
            cli.mkdir(parents=True)
            (cli / "registry.cpp").write_text(
                "void build(Commands& commands) {\n"
                "  const auto add_leaf = [&commands](const char* path, Options options) {\n"
                "    add_command(commands, path, false, std::move(options));\n"
                "  };\n" + body + "}\n",
                encoding="utf-8",
            )
            return inventory._native_cli(Path(d))

    def test_native_literal_flag_is_read(self):
        commands, _, registrations, problems = self._native(
            '  add_command(commands, "resample", true, {int_value("target-rate")});\n'
            '  for (const char* path : {"bpm", "beats"})\n'
            "    add_command(commands, path, true, {});\n"
        )
        self.assertEqual(problems, [])
        self.assertEqual(registrations, 1)
        self.assertEqual(sorted(commands), ["beats", "bpm", "resample"])

    def test_native_non_literal_flag_fails(self):
        _, _, registrations, problems = self._native(
            '  add_command(commands, "resample", true, {int_value(kTargetRate)});\n'
        )
        self.assertEqual(registrations, 1)
        self.assertEqual(problems, ["tools/cli/registry.cpp:5: flag name is not a literal"])

    def test_native_unknown_command_shape_fails(self):
        _, _, _, problems = self._native("  add_command(commands, kNames[0], true, {});\n")
        self.assertEqual(len(problems), 1)
        self.assertRegex(problems[0], r"registry\.cpp:5: unknown registration shape")

    def test_zero_registrations_fail(self):
        import inventory

        self.assertEqual(
            inventory.zero_registrations({"cli-python": 0, "cli-native": 3}),
            ["cli-python: 0 flag registrations"],
        )


class PythonNodeIndexTest(unittest.TestCase):
    """A function node is looked up in the tree being read, never in one read before it."""

    def _package(self, d: str, source: str):
        import inventory

        pkg = Path(d) / inventory.PY_PACKAGE
        pkg.mkdir(parents=True)
        (pkg / "_pages.py").write_text(source, encoding="utf-8")
        return inventory._PyPackage(Path(d))

    def test_a_second_tree_does_not_read_the_first_trees_nodes(self):
        import inventory

        rel = f"{inventory.PY_PACKAGE}/_pages.py"
        with tempfile.TemporaryDirectory() as old, tempfile.TemporaryDirectory() as new:
            tagged = self._package(old, "def supply():\n    pass\n")
            tip = self._package(new, 'X = 1\n\ndef supply():\n    """Make a page resident."""\n')
            self.assertEqual(inventory._node_at(tagged, rel, 1).name, "supply")
            self.assertIsNone(inventory._node_at(tip, rel, 1))
            node = inventory._node_at(tip, rel, 3)
            self.assertEqual(inventory._docstring_text(node), "Make a page resident.")


if __name__ == "__main__":
    unittest.main()


class QuotedSignature(unittest.TestCase):
    """A ledger text quoting its member's own signature must match a declaration of it."""

    def by_key(self, surface, name, *sigs, kind="function"):
        rec = {
            "sigs": [
                {"params": [{"name": n, "optional": o} for n, o in sig], "returns": ""}
                for sig in sigs
            ]
        }
        return {(surface, kind, name, None): rec}

    def check(self, text, surface="node", by_key=None, options=None):
        return check_surface.quoted_signature_mismatches(
            text, surface, "Engine.arm", None, by_key, options or {}
        )

    def test_a_matching_quote_is_compared_and_passes(self):
        keys = self.by_key("node", "Engine.arm", [("id", False), ("state", True)], kind="method")
        self.assertEqual(self.check("arm(id, state?) arms the track", by_key=keys), (1, []))

    def test_a_name_or_optionality_mismatch_fails(self):
        keys = self.by_key("node", "Engine.arm", [("id", False), ("state", True)], kind="method")
        for text in ("arm(id, state) arms it", "arm(armed = true) arms it"):
            with self.subTest(text=text):
                compared, bad = self.check(text, by_key=keys)
                self.assertEqual(compared, 1)
                self.assertEqual(len(bad), 1, bad)
                self.assertIn("Engine.arm declares (id, state?)", bad[0])

    def test_any_overload_may_match(self):
        keys = self.by_key(
            "node", "Engine.arm", [("request", False)], [("id", False)], kind="method"
        )
        self.assertEqual(self.check("arm(id) or arm(request)", by_key=keys), (2, []))

    def test_old_shapes_object_lists_and_other_names_are_not_read(self):
        keys = self.by_key("node", "Engine.arm", [("id", False)], kind="method")
        text = "arm(id) replaces arm(trackId, on); arm({ id }) or disarm(x)"
        self.assertEqual(self.check(text, by_key=keys), (1, []))

    def test_python_compares_names_as_a_set_with_keyword_only_options(self):
        keys = self.by_key("python", "Engine.arm", [("id", False)], kind="method")
        options = {"Engine.arm": {("state", True)}}
        ok = self.check("arm(id, state=None)", "python", keys, options)
        self.assertEqual(ok, (1, []))
        compared, bad = self.check("arm(id)", "python", keys, options)
        self.assertEqual((compared, len(bad)), (1, 1))
