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

    def test_a_plain_rename_claims_its_members_moved(self):
        self.base["records"].append(_rec("wasm", "option", "getBpm(window)", owner="getBpm"))
        code, lines = self.run_base()
        self.assertEqual(code, 1)
        self.assertIn("  wasm option getBpm(window)", lines)

    def test_a_positional_parameter_rename_needs_no_row_while_the_count_survives(self):
        self.base["records"] = [
            r for r in self.base["records"] if (r["surface"], r["name"]) != ("wasm", "getBpm")
        ]
        self.base["records"] += [
            _sig_rec("wasm", "function", "getBpm", "input"),
            _rec("wasm", "option", "getBpm(input)", owner="getBpm"),
        ]
        code, lines = self.run_base()
        self.assertEqual(code, 0, "\n".join(lines))
        self.assertIn(
            "  not compared: wasm option, 1 names (a TypeScript caller passes them by position"
            " and the parameter count survived)",
            lines,
        )
        self.base["records"][-2] = _sig_rec("wasm", "function", "getBpm", "input", "rate")
        code, lines = self.run_base()
        self.assertEqual(code, 1)
        self.assertIn("  wasm option getBpm(input)", lines)


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
        self.assertIn("domain vote: 13 names, 0 by tie-break", lines)
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
        self.assertIn("Regenerated domains.json: 13 names", lines)


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
    def test_render_is_deterministic(self):
        text = upgrade_notes.render(TD / "ledgers", TD / "rules.toml")
        self.assertEqual(text, upgrade_notes.render(TD / "ledgers", TD / "rules.toml"))
        self.assertIn("## Title of getters", text)
        self.assertIn(
            "### WASM (`.`)\n\n| 1.x | 2.0.0 |\n|---|---|\n| `getBpm` | `detectBpm` |", text
        )
        self.assertIn(
            "| `bpm --verbose` (native CLI only) | removed: drop the flag; the output is the same |",
            text,
        )
        self.assertIn("| `detect_bpm` | sample_rate is a required argument. |", text)
        self.assertLess(
            text.index("## Title of sample-rate-required"), text.index("## Title of individual")
        )
        self.assertNotIn("## Title of teardown", text)

    def test_removed_row_says_what_replaces_it(self):
        self.assertEqual(upgrade_notes._target({"removed": "call x"}), "removed: call x")
        self.assertEqual(
            upgrade_notes._target({"removed": True, "no_replacement": "gone"}),
            "removed, no replacement: gone",
        )

    def test_changed_row_leads_with_its_target(self):
        e = {"old": "a", "changed": "takes an object", "to": "b"}
        self.assertEqual(upgrade_notes._target(e), "`b`: takes an object")

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
            first = upgrade_notes.render(d, TD / "rules.toml")
            (d / "wasm.toml").write_text(extra + "\n" + base)
            self.assertEqual(first, upgrade_notes.render(d, TD / "rules.toml"))


if __name__ == "__main__":
    unittest.main()
