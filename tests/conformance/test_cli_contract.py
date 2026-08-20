#!/usr/bin/env python3
"""Stdlib self-tests for the CLI contract manifest/checker."""

from __future__ import annotations

import copy
from contextlib import redirect_stderr, redirect_stdout
import importlib.util
from io import StringIO
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
CHECKER_PATH = ROOT / "tools" / "conformance" / "check_cli_contract.py"
SPEC = importlib.util.spec_from_file_location(
    "libsonare_cli_contract_checker", CHECKER_PATH
)
assert SPEC is not None and SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


class CliContractSelfTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = CHECKER.load_manifest(CHECKER.DEFAULT_MANIFEST)

    def test_manifest_active_paths_are_the_source_of_truth(self) -> None:
        active = {
            path
            for path, record in self.manifest["commands"].items()
            if record["status"] == "active"
        }
        self.assertEqual(
            {contract["path"] for contract in self.manifest["active_paths"]},
            active,
        )

        # A future batch can promote a different path without changing this
        # checker.  Renaming the existing version contract keeps this fixture
        # small while proving validation follows the manifest declarations.
        candidate = copy.deepcopy(self.manifest)
        version = next(
            item for item in candidate["active_paths"] if item["path"] == "version"
        )
        version["path"] = "synthetic"
        candidate["commands"]["synthetic"] = candidate["commands"].pop("version")
        candidate["inventory"]["expected_options"]["synthetic"] = candidate[
            "inventory"
        ]["expected_options"].pop("version")
        self.assertEqual(CHECKER.validate_manifest(candidate), [])

    def test_manifest_top_level_is_closed(self) -> None:
        candidate = copy.deepcopy(self.manifest)
        candidate["unexpected"] = True
        errors = CHECKER.validate_manifest(candidate)
        self.assertTrue(
            any("unknown keys unexpected" in error for error in errors), errors
        )

    def test_payload_schema_is_closed_and_typed(self) -> None:
        version = next(
            item for item in self.manifest["active_paths"] if item["path"] == "version"
        )
        schema = version["payloads"]["success"]
        self.assertEqual(
            CHECKER.validate_payload(
                {"cli": "native", "cli_version": "1.0.0", "lib_version": "1.0.0"},
                schema,
                "version",
            ),
            [],
        )
        errors = CHECKER.validate_payload(
            {
                "cli": "native",
                "cli_version": "1.0.0",
                "lib_version": "1.0.0",
                "extra": 1,
            },
            schema,
            "version",
        )
        self.assertTrue(any("unknown keys extra" in error for error in errors), errors)
        invalid = copy.deepcopy(self.manifest)
        invalid_version = next(
            item for item in invalid["active_paths"] if item["path"] == "version"
        )
        invalid_version["payloads"]["success"] = {"keys": {}}
        self.assertTrue(
            any(
                "expected a non-empty object" in error
                for error in CHECKER.validate_manifest(invalid)
            ),
        )

    def test_recursive_payload_schema_supports_nested_arrays_optional_and_variants(
        self,
    ) -> None:
        schema = {
            "type": "object",
            "properties": {
                "id": "string",
                "meta": {
                    "type": "object",
                    "required": {"count": "integer"},
                    "optional": {"label": {"type": "string|null"}},
                },
                "items": {
                    "type": "array",
                    "items": {
                        "one_of": [
                            {"type": "object", "keys": {"name": "string"}},
                            {
                                "type": "object",
                                "required": {"error": "string"},
                                "optional": {"code": "integer"},
                            },
                        ],
                    },
                },
            },
            "required": ["id", "meta", "items"],
        }
        payload = {
            "id": "contract",
            "meta": {"count": 2, "label": None},
            "items": [{"name": "ok"}, {"error": "bad", "code": 3}],
        }
        self.assertEqual(CHECKER.validate_payload(payload, schema, "nested"), [])
        errors = CHECKER.validate_payload(
            {
                "id": "contract",
                "meta": {"count": "two", "extra": True},
                "items": [{"unknown": False}],
            },
            schema,
            "nested",
        )
        self.assertTrue(any("nested.meta.count" in error for error in errors), errors)
        self.assertTrue(any("unknown keys extra" in error for error in errors), errors)
        self.assertTrue(any("nested.items[0]" in error for error in errors), errors)

    def test_analyze_contract_is_active_and_canonical(self) -> None:
        analyze = self.manifest["commands"]["analyze"]
        self.assertEqual(analyze["status"], "active")
        self.assertEqual(analyze["classification"], "shared")
        contract = next(
            item for item in self.manifest["active_paths"] if item["path"] == "analyze"
        )
        self.assertEqual(
            [option["name"] for option in contract["options"]],
            ["chroma-highpass", "json", "no-hpss", "with-seventh"],
        )
        self.assertEqual(
            next(
                option
                for option in contract["options"]
                if option["name"] == "chroma-highpass"
            )["default"],
            80,
        )
        payload_keys = set(contract["payloads"]["success"]["keys"])
        self.assertEqual(
            payload_keys,
            {
                "bpm",
                "bpm_confidence",
                "key",
                "time_signature",
                "beats",
                "downbeat_indices",
                "downbeat_phase",
                "chords",
                "sections",
                "timbre",
                "dynamics",
                "rhythm",
                "form",
            },
        )

        missing = copy.deepcopy(self.manifest)
        missing_contract = next(
            item for item in missing["active_paths"] if item["path"] == "analyze"
        )
        del missing_contract["payloads"]["success"]["keys"]["sections"]
        errors = CHECKER.validate_manifest(missing)
        self.assertTrue(
            any("canonical keys differ" in error for error in errors), errors
        )

        replaced = copy.deepcopy(self.manifest)
        replaced_contract = next(
            item for item in replaced["active_paths"] if item["path"] == "analyze"
        )
        replaced_contract["payloads"]["success"]["keys"]["sections_typo"] = (
            replaced_contract["payloads"]["success"]["keys"].pop("sections")
        )
        errors = CHECKER.validate_manifest(replaced)
        self.assertTrue(
            any("canonical keys differ" in error for error in errors), errors
        )

    def test_analyze_section_type_must_be_lowercase_kebab(self) -> None:
        contract = next(
            item for item in self.manifest["active_paths"] if item["path"] == "analyze"
        )
        case = next(item for item in contract["cases"] if item["id"] == "default")
        payload = {
            "bpm": 120.0,
            "bpm_confidence": 0.5,
            "key": {"root": 0, "mode": 0, "confidence": 0.5, "name": "C major"},
            "time_signature": {"numerator": 4, "denominator": 4, "confidence": 0.5},
            "beats": [],
            "chords": [],
            "sections": [{"type": "Verse", "start": 0.0, "end": 1.0}],
            "timbre": {
                "brightness": 0.0,
                "warmth": 0.0,
                "density": 0.0,
                "roughness": 0.0,
                "complexity": 0.0,
            },
            "dynamics": {
                "dynamic_range_db": 0.0,
                "loudness_range_db": 0.0,
                "crest_factor": 0.0,
                "is_compressed": False,
            },
            "rhythm": {
                "syncopation": 0.0,
                "groove_type": "straight",
                "pattern_regularity": 0.0,
            },
            "form": "A",
        }
        report: list[tuple[str, str]] = []
        CHECKER._validate_case_payload(
            "analyze",
            case,
            payload,
            contract,
            "native",
            "native.analyze.default",
            report,
        )
        self.assertTrue(
            any("lowercase-kebab section type" in message for _, message in report),
            report,
        )

    def test_spectral_contract_is_closed_for_every_feature_statistic(self) -> None:
        spectral = self.manifest["commands"]["spectral"]
        self.assertEqual(spectral["status"], "active")
        contract = next(
            item for item in self.manifest["active_paths"] if item["path"] == "spectral"
        )
        payload = contract["payloads"]["success"]["keys"]
        self.assertEqual(set(payload), {"n_frames", "features"})
        self.assertEqual(
            set(payload["features"]["keys"]),
            {"centroid", "bandwidth", "rolloff", "flatness", "zcr", "rms"},
        )
        for stats in payload["features"]["keys"].values():
            self.assertEqual(set(stats["keys"]), {"mean", "std", "min", "max"})

        missing = copy.deepcopy(self.manifest)
        missing_contract = next(
            item for item in missing["active_paths"] if item["path"] == "spectral"
        )
        del missing_contract["payloads"]["success"]["keys"]["features"]["keys"]["rms"][
            "keys"
        ]["max"]
        errors = CHECKER.validate_manifest(missing)
        self.assertTrue(
            any("canonical keys differ" in error for error in errors), errors
        )

    def test_spectral_payload_rejects_missing_canonical_statistic(self) -> None:
        contract = next(
            item for item in self.manifest["active_paths"] if item["path"] == "spectral"
        )
        case = contract["cases"][0]
        payload = {
            "n_frames": 2,
            "features": {
                name: {"mean": 1.0, "std": 0.1, "min": 0.0, "max": 2.0}
                for name in (
                    "centroid",
                    "bandwidth",
                    "rolloff",
                    "flatness",
                    "zcr",
                    "rms",
                )
            },
        }
        del payload["features"]["rms"]["max"]
        report: list[tuple[str, str]] = []
        CHECKER._validate_case_payload(
            "spectral",
            case,
            payload,
            contract,
            "native",
            "native.spectral.success",
            report,
        )
        self.assertTrue(any("missing max" in message for _, message in report), report)

    def test_promoted_payload_schemas_reject_missing_extra_and_type_drift(self) -> None:
        """Each promoted path remains a closed, typed schema contract."""
        for path in ("rhythm", "pitch", "eq", "mastering-processor"):
            for mutation in ("missing", "extra", "type"):
                candidate = copy.deepcopy(self.manifest)
                contract = next(
                    item for item in candidate["active_paths"] if item["path"] == path
                )
                fields = contract["payloads"]["success"]["keys"]
                field = next(iter(fields))
                if mutation == "missing":
                    del fields[field]
                elif mutation == "extra":
                    fields["unexpected"] = "string"
                else:
                    fields[field] = "number" if fields[field] == "string" else "string"

                errors = CHECKER.validate_manifest(candidate)
                self.assertTrue(
                    any(
                        "canonical keys differ" in error
                        or "canonical type differs" in error
                        for error in errors
                    ),
                    (path, mutation, errors),
                )

    def test_analysis_output_rejection_cases_cover_both_spellings(self) -> None:
        cases = {
            case["id"]: case
            for case in self.manifest["parser_cases"]
            if "reject_output" in case["id"]
        }
        self.assertEqual(
            set(cases),
            {
                "analyze_reject_output",
                "analyze_reject_output_alias",
                "spectral_reject_output",
                "spectral_reject_output_alias",
            },
        )
        for case in cases.values():
            self.assertEqual(case["exit"], 2)
            self.assertEqual(case["legacy_exit"], 1)

    def test_parser_cases_do_not_allow_surface_specific_exits(self) -> None:
        candidate = copy.deepcopy(self.manifest)
        candidate["parser_cases"][0]["surface_exit"] = {"native": 3, "python": 2}
        errors = CHECKER.validate_manifest(candidate)
        self.assertTrue(
            any("unknown keys surface_exit" in error for error in errors), errors
        )

    def test_single_json_document_and_numeric_tolerance(self) -> None:
        self.assertEqual(CHECKER.parse_single_json('{"ok":true}'), {"ok": True})
        with self.assertRaises(ValueError):
            CHECKER.parse_single_json('{"ok":true}\n{"extra":false}')
        self.assertIsNone(CHECKER._compare_values(1.0, 1.0 + 5e-7, "value", 1e-6, 1e-5))
        self.assertIsNotNone(CHECKER._compare_values(1.0, 1.1, "value", 1e-6, 1e-5))

    def test_legacy_exit_folds_every_nonzero_case(self) -> None:
        cases = list(self.manifest["parser_cases"])
        for contract in self.manifest["active_paths"]:
            cases.extend(contract["cases"])
        for case in cases:
            if case["exit"] == 0:
                self.assertEqual(case["legacy_exit"], 0, case)
            else:
                self.assertEqual(case["legacy_exit"], 1, case)

    def test_fixture_documents_have_the_declared_shape(self) -> None:
        projects = self.manifest["fixtures"]["projects"]
        self.assertIsInstance(json.loads(projects["clean"]), dict)
        self.assertIsInstance(json.loads(projects["warning"]), dict)
        with self.assertRaises(json.JSONDecodeError):
            json.loads(projects["malformed"])

    def test_legacy_payload_mismatch_is_reported(self) -> None:
        """Legacy mode may fold exits, but it must not forge a different JSON API."""
        version = next(
            contract
            for contract in self.manifest["active_paths"]
            if contract["path"] == "version"
        )
        manifest = {
            "comparison": self.manifest["comparison"],
            "active_paths": [version],
        }

        def fake_run(_surface, _executable, _argv, legacy, _timeout):
            payload = {
                "cli": "native",
                "cli_version": "1.0.0",
                "lib_version": "1.0.0",
            }
            if legacy:
                payload.update({"cli": "forged-legacy", "cli_version": "9.9.9"})
            return {
                "returncode": 0,
                "stdout": json.dumps(payload),
                "stderr": "",
                "error": "",
            }

        report: list[tuple[str, str]] = []
        with mock.patch.object(CHECKER, "_run", fake_run):
            CHECKER._run_active_cases("native", "unused", manifest, {}, 1.0, report)
        self.assertTrue(
            any(
                "legacy payload must match normal mode" in message
                for _, message in report
            ),
            report,
        )

    def test_artifact_digest_rejects_arbitrary_json(self) -> None:
        """A byte count plus parseable JSON is insufficient for a canonical artifact."""
        contract = next(
            contract
            for contract in self.manifest["active_paths"]
            if contract["path"] == "project.validate"
        )
        with tempfile.TemporaryDirectory() as directory:
            artifact = Path(directory) / "canonical.json"
            artifact.write_text("{}", encoding="utf-8")
            report: list[tuple[str, str]] = []
            CHECKER._check_artifact(
                "canonical_project",
                contract,
                {"bytes": artifact.stat().st_size},
                {"project_warning_output": str(artifact)},
                "test.artifact",
                report,
            )
        self.assertTrue(any("SHA-256" in message for _, message in report), report)

    def test_path_specific_payload_invariants_are_not_shape_only(self) -> None:
        """Warning and normalized-preset cases reject plausible but empty JSON values."""
        project = next(
            contract
            for contract in self.manifest["active_paths"]
            if contract["path"] == "project.validate"
        )
        warning = next(case for case in project["cases"] if case["id"] == "warning")
        report: list[tuple[str, str]] = []
        CHECKER._validate_case_payload(
            "project.validate",
            warning,
            {"valid": True, "bytes": 1, "diagnostic_count": 0, "diagnostics": []},
            project,
            "native",
            "test.warning",
            report,
        )
        self.assertTrue(
            any("at least one diagnostic" in message for _, message in report), report
        )

        preset = next(
            contract
            for contract in self.manifest["active_paths"]
            if contract["path"] == "voice-preset-validate"
        )
        success = next(case for case in preset["cases"] if case["id"] == "success")
        report.clear()
        CHECKER._validate_case_payload(
            "voice-preset-validate",
            success,
            {"ok": True, "normalized_json": "null"},
            preset,
            "native",
            "test.preset",
            report,
        )
        self.assertTrue(
            any("expected an object" in message for _, message in report), report
        )

    def test_executable_path_resolution_preserves_path_commands(self) -> None:
        """A repo-relative runtime path survives the Python surface's cwd change."""
        relative = "bindings/python/.venv/bin/python"
        self.assertEqual(
            CHECKER._resolve_executable(relative),
            str(ROOT / relative),
        )
        self.assertEqual(CHECKER._resolve_executable("python3"), "python3")

    def test_pending_payload_with_active_options_still_checks_inventory(self) -> None:
        """Payload promotion and option promotion are independent gates."""
        manifest = copy.deepcopy(self.manifest)
        manifest["commands"]["version"]["status"] = "pending"
        manifest["active_paths"] = [
            contract
            for contract in manifest["active_paths"]
            if contract["path"] != "version"
        ]
        self.assertEqual(CHECKER.validate_manifest(manifest), [])
        inventory = self._fake_inventory(manifest, "native")
        version = next(
            command for command in inventory["commands"] if command["path"] == "version"
        )
        version["options"][0]["default"] = "drifted"
        report: list[tuple[str, str]] = []

        def fake_run(_surface, _executable, _argv, _legacy, _timeout):
            return {
                "returncode": 0,
                "stdout": json.dumps(inventory),
                "stderr": "",
                "error": "",
            }

        with mock.patch.object(CHECKER, "_run", fake_run):
            CHECKER._inventory_checks("native", "unused", manifest, 1.0, report)
        self.assertTrue(
            any(
                "version: active option schema differs" in message
                for _, message in report
            ),
            report,
        )

    def test_semantically_pending_shared_paths_still_require_option_parity(self) -> None:
        """Semantic promotion remains independent from the shared option gate."""
        manifest = copy.deepcopy(self.manifest)
        native = self._fake_inventory(manifest, "native")
        python = self._fake_inventory(manifest, "python")
        self.assertEqual(manifest["commands"]["bpm"]["status"], "pending")
        self.assertEqual(manifest["commands"]["bpm"]["option_status"], "active")
        native_bpm = next(
            command for command in native["commands"] if command["path"] == "bpm"
        )
        native_bpm["options"][0]["default"] = True

        report: list[tuple[str, str]] = []
        CHECKER._compare_active_inventory_options(
            {command["path"]: command for command in native["commands"]},
            {command["path"]: command for command in python["commands"]},
            manifest,
            report,
        )
        self.assertTrue(any("inventory.shared.bpm" in message for _, message in report), report)

    def _fake_inventory(self, manifest, surface):
        expected = manifest["inventory"]["expected_options"]
        commands = {
            path: {
                "path": path,
                "aliases": [],
                "options": copy.deepcopy(expected.get(path, [])),
            }
            for path in CHECKER._expected_paths(manifest["commands"], surface)
        }
        return {
            "schema_version": 2,
            "surface": surface,
            "commands": list(commands.values()),
        }

    def test_all_shared_paths_have_active_canonical_option_contracts(self) -> None:
        shared = {
            path
            for path, record in self.manifest["commands"].items()
            if record["classification"] == "shared"
        }
        self.assertEqual(len(shared), 57)
        self.assertEqual(set(self.manifest["inventory"]["expected_options"]), shared)
        for path in shared:
            self.assertEqual(
                self.manifest["commands"][path]["option_status"], "active", path
            )
            expected = self.manifest["inventory"]["expected_options"][path]
            self.assertEqual(
                [option["name"] for option in expected],
                sorted(option["name"] for option in expected),
                path,
            )
        for path, record in self.manifest["commands"].items():
            if record["classification"] != "shared":
                self.assertEqual(record["option_status"], "pending", path)

        for contract in self.manifest["active_paths"]:
            self.assertEqual(
                contract["options"],
                self.manifest["inventory"]["expected_options"][contract["path"]],
                contract["path"],
            )

    def test_shared_option_snapshot_is_deterministic_and_normalized(self) -> None:
        native = self._fake_inventory(self.manifest, "native")
        python = self._fake_inventory(self.manifest, "python")
        first = CHECKER._build_shared_option_snapshot(native, python, self.manifest)

        # Registration order and alias order are surface details; the
        # committed snapshot must not depend on either one.
        for inventory in (native, python):
            command = next(
                command
                for command in inventory["commands"]
                if command["path"] == "estimate-room"
            )
            command["options"].reverse()
            for option in command["options"]:
                option["aliases"].reverse()
        second = CHECKER._build_shared_option_snapshot(native, python, self.manifest)
        self.assertEqual(first, second)
        self.assertEqual(
            json.dumps(first, indent=2, ensure_ascii=False),
            json.dumps(second, indent=2, ensure_ascii=False),
        )
        self.assertEqual(first, self.manifest["inventory"]["expected_options"])

    def test_shared_option_snapshot_rejects_missing_malformed_and_mismatched_input(
        self,
    ) -> None:
        native = self._fake_inventory(self.manifest, "native")
        python = self._fake_inventory(self.manifest, "python")

        # Snapshot generation is intentionally usable before option
        # promotion: a semantically pending path must still reject drift.
        pre_activation = copy.deepcopy(self.manifest)
        pre_activation["commands"]["bpm"]["option_status"] = "pending"
        mismatch = copy.deepcopy(native)
        next(
            command for command in mismatch["commands"] if command["path"] == "bpm"
        )["options"][0]["default"] = True
        with self.assertRaisesRegex(ValueError, "inventory.shared.bpm"):
            CHECKER._build_shared_option_snapshot(mismatch, python, pre_activation)

        missing = copy.deepcopy(native)
        missing["commands"] = [
            command for command in missing["commands"] if command["path"] != "bpm"
        ]
        with self.assertRaisesRegex(ValueError, "missing classified path bpm"):
            CHECKER._build_shared_option_snapshot(missing, python, self.manifest)

        malformed = copy.deepcopy(native)
        del next(
            command for command in malformed["commands"] if command["path"] == "bpm"
        )["options"][0]["required"]
        with self.assertRaisesRegex(ValueError, "malformed option metadata"):
            CHECKER._build_shared_option_snapshot(malformed, python, self.manifest)

    def test_emit_shared_option_snapshot_mode_only_runs_inventory_dumps(self) -> None:
        native = self._fake_inventory(self.manifest, "native")
        python = self._fake_inventory(self.manifest, "python")

        def fake_run(surface, _executable, argv, legacy, _timeout):
            self.assertEqual(argv, ["--dump-cli-contract"])
            self.assertFalse(legacy)
            return {
                "returncode": 0,
                "stdout": json.dumps(native if surface == "native" else python),
                "stderr": "",
                "error": "",
            }

        stdout = StringIO()
        stderr = StringIO()
        with mock.patch.object(CHECKER, "_run", fake_run):
            with redirect_stdout(stdout), redirect_stderr(stderr):
                result = CHECKER.main(
                    [
                        "--manifest",
                        str(CHECKER.DEFAULT_MANIFEST),
                        "--native",
                        "native",
                        "--python",
                        "python",
                        "--emit-shared-option-snapshot",
                    ]
                )
        self.assertEqual(result, 0)
        self.assertEqual(stderr.getvalue(), "")
        snapshot = json.loads(stdout.getvalue())
        self.assertEqual(snapshot, self.manifest["inventory"]["expected_options"])
        self.assertEqual(
            stdout.getvalue(),
            json.dumps(snapshot, indent=2, ensure_ascii=False) + "\n",
        )

    def test_active_option_inventory_rejects_shared_and_one_sided_drift(self) -> None:
        """Canonical metadata catches equal drift; cross-surface catches asymmetric drift."""
        manifest = copy.deepcopy(self.manifest)
        native = self._fake_inventory(manifest, "native")
        python = self._fake_inventory(manifest, "python")

        def run_inventory(inventory, surface):
            report: list[tuple[str, str]] = []

            def fake_run(_surface, _executable, _argv, _legacy, _timeout):
                return {
                    "returncode": 0,
                    "stdout": json.dumps(inventory),
                    "stderr": "",
                    "error": "",
                }

            with mock.patch.object(CHECKER, "_run", fake_run):
                CHECKER._inventory_checks(surface, "unused", manifest, 1.0, report)
            return [message for _, message in report]

        # The same wrong default on both surfaces must fail against the
        # manifest's canonical expected_options map.
        for inventory in (native, python):
            version = next(
                command
                for command in inventory["commands"]
                if command["path"] == "version"
            )
            version["options"][0]["default"] = True
        self.assertTrue(
            any(
                "version: active option schema differs" in message
                for message in run_inventory(native, "native")
            )
        )
        self.assertTrue(
            any(
                "version: active option schema differs" in message
                for message in run_inventory(python, "python")
            )
        )

        # A one-sided metadata drift must be reported by the
        # explicit native/Python active-inventory comparison.
        for field, value in (
            ("default", True),
            ("type", "path"),
            ("aliases", ["v"]),
            ("repeatable", True),
            ("required", True),
        ):
            left = self._fake_inventory(manifest, "native")
            right = self._fake_inventory(manifest, "python")
            left_version = next(
                command for command in left["commands"] if command["path"] == "version"
            )
            left_version["options"][0][field] = value
            report: list[tuple[str, str]] = []
            CHECKER._compare_active_inventory_options(
                {command["path"]: command for command in left["commands"]},
                {command["path"]: command for command in right["commands"]},
                manifest,
                report,
            )
            self.assertTrue(
                any("inventory.shared.version" in message for _, message in report),
                (field, report),
            )

    def test_malformed_expected_active_option_is_a_schema_error(self) -> None:
        candidate = copy.deepcopy(self.manifest)
        del candidate["inventory"]["expected_options"]["version"][0]["type"]
        errors = CHECKER.validate_manifest(candidate)
        self.assertTrue(
            any("expected_options.version[0]" in error for error in errors), errors
        )

        candidate = copy.deepcopy(self.manifest)
        candidate["inventory"]["expected_options"]["version"][0] = {"name": "json"}
        report: list[tuple[str, str]] = []
        inventory = self._fake_inventory(self.manifest, "native")
        with mock.patch.object(
            CHECKER,
            "_run",
            return_value={
                "returncode": 0,
                "stdout": json.dumps(inventory),
                "stderr": "",
                "error": "",
            },
        ):
            CHECKER._inventory_checks("native", "unused", candidate, 1.0, report)
        self.assertTrue(
            any("malformed option metadata" in message for _, message in report),
            report,
        )

        inventory = self._fake_inventory(self.manifest, "native")
        version = next(
            command for command in inventory["commands"] if command["path"] == "version"
        )
        del version["options"][0]["required"]
        report.clear()
        with mock.patch.object(
            CHECKER,
            "_run",
            return_value={
                "returncode": 0,
                "stdout": json.dumps(inventory),
                "stderr": "",
                "error": "",
            },
        ):
            CHECKER._inventory_checks("native", "unused", self.manifest, 1.0, report)
        self.assertTrue(
            any("missing keys required" in message for _, message in report),
            report,
        )
        self.assertTrue(
            any("malformed option metadata" in message for _, message in report),
            report,
        )

        candidate = copy.deepcopy(self.manifest)
        candidate["inventory"]["expected_options"]["version"][0]["type"] = []
        errors = CHECKER.validate_manifest(candidate)
        self.assertTrue(any("unknown type token" in error for error in errors), errors)

    def test_required_and_repeatable_defaults_are_schema_contracts(self) -> None:
        # A required path is allowed to have no default; this is exercised by
        # project.validate.in in the canonical manifest as well.
        required = {
            "name": "input",
            "type": "path",
            "default": None,
            "aliases": [],
            "repeatable": False,
            "required": True,
        }
        errors: list[str] = []
        CHECKER._validate_option(required, "required", errors)
        self.assertEqual(errors, [])

        required["default"] = "input.wav"
        errors.clear()
        CHECKER._validate_option(required, "required", errors)
        self.assertTrue(
            any("required option needs a null default" in error for error in errors)
        )

        repeatable = {
            "name": "set",
            "type": "string",
            "default": [],
            "aliases": [],
            "repeatable": True,
            "required": False,
        }
        errors.clear()
        CHECKER._validate_option(repeatable, "repeatable", errors)
        self.assertEqual(errors, [])

        repeatable["default"] = "value"
        errors.clear()
        CHECKER._validate_option(repeatable, "repeatable", errors)
        self.assertTrue(
            any(
                "repeatable option needs an empty array default" in error
                for error in errors
            )
        )

        optional_number = {
            "name": "semitones",
            "type": "number",
            "default": None,
            "aliases": [],
            "repeatable": False,
            "required": False,
        }
        errors.clear()
        CHECKER._validate_option(optional_number, "optional_number", errors)
        self.assertEqual(errors, [])

    def test_missing_required_metadata_is_reported_without_key_error(self) -> None:
        candidate = copy.deepcopy(self.manifest)
        del candidate["active_paths"][0]["options"][0]["required"]
        errors = CHECKER.validate_manifest(candidate)
        self.assertTrue(
            any(
                "active_paths[0].options[0]: missing keys required" in error
                for error in errors
            ),
            errors,
        )

    def test_active_option_inventory_normalizes_only_option_and_alias_order(
        self,
    ) -> None:
        options = [
            {
                "name": "zeta",
                "type": "string",
                "default": "",
                "aliases": ["z", "zz"],
                "repeatable": False,
                "required": False,
            },
            {
                "name": "alpha",
                "type": "number",
                "default": [],
                "aliases": ["a"],
                "repeatable": True,
                "required": False,
            },
        ]
        reordered = [copy.deepcopy(options[0]), copy.deepcopy(options[1])]
        reordered[0]["aliases"].reverse()
        reordered.reverse()
        self.assertEqual(
            CHECKER._normalized_option_inventory(options),
            CHECKER._normalized_option_inventory(reordered),
        )
        for field, value in (
            ("type", "path"),
            ("default", "drifted"),
            ("aliases", ["different"]),
            ("repeatable", False),
        ):
            drifted = copy.deepcopy(reordered)
            drifted[0][field] = value
            self.assertNotEqual(
                CHECKER._normalized_option_inventory(options),
                CHECKER._normalized_option_inventory(drifted),
                field,
            )


if __name__ == "__main__":
    unittest.main()
