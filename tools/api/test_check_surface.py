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

    def run_check(self, *extra, ledger_dir=None, mirrors=None, allowlist=None, domains_file=None):
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
            "--root",
            str(ROOT),
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


class Conventions(CheckerCase):
    ZERO_FAIL = "    FAIL options-struct:struct-size: no candidates on the selected surfaces; its extractor found nothing"

    def test_zero_candidates_fails(self):
        self.drop("c", "type", "SonareBpmOptions")
        for extra in ((), ("--surface", "c")):
            code, lines = self.run_check(*extra)
            self.assertEqual(code, 1)
            self.assertIn("  check options-struct:struct-size: checked 0, violations 0", lines)
            self.assertIn(self.ZERO_FAIL, lines)
            self.assertFalse(any(x.lstrip().startswith("not compared:") for x in lines))

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
        self.assertIn("domain vote: 11 names, 0 by tie-break", lines)
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
        self.assertIn("Regenerated domains.json: 11 names", lines)


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
        self.assertIn("| `bpm --verbose` (native CLI only) | removed |", text)
        self.assertIn("| `detect_bpm` | sample_rate is a required argument. |", text)
        self.assertLess(
            text.index("## Title of sample-rate-required"), text.index("## Title of individual")
        )
        self.assertNotIn("## Title of teardown", text)

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
