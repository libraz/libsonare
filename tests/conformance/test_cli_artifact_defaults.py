"""Regression cases for checked CLI artifact and voice-latency contracts."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKER_PATH = ROOT / "tools" / "conformance" / "check_cli_contract.py"
SPEC = importlib.util.spec_from_file_location("libsonare_cli_artifact_checker", CHECKER_PATH)
assert SPEC is not None and SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CHECKER
SPEC.loader.exec_module(CHECKER)


class VoiceLatencyContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = CHECKER.load_manifest(CHECKER.DEFAULT_MANIFEST)

    @staticmethod
    def payload(length: int, sample_rate: int, latency: int, preset: str | None = None) -> dict:
        result = {"length": length, "sample_rate": sample_rate, "latency_samples": latency}
        if preset is not None:
            result["preset"] = preset
        return result

    def payloads(self) -> dict[tuple[str, str], dict]:
        return {
            ("voice-change", "simple"): {
                **self.payload(13230, 22050, 333),
                "pitch_semitones": 5.0,
                "formant_factor": 1.1,
            },
            ("voice-change", "preset"): self.payload(13230, 22050, 333, "bright-idol"),
            ("voice-change", "custom"): self.payload(13230, 22050, 82),
            ("voice-change", "preset_grain"): self.payload(13230, 22050, 1042, "bright-idol"),
        }

    def validate(self, payloads: dict[tuple[str, str], dict]) -> list[tuple[str, str]]:
        report: list[tuple[str, str]] = []
        CHECKER._validate_voice_case_relationships(payloads, self.manifest, "native", report)
        return report

    def test_default_explicit_custom_and_grain_preset_latencies_are_pinned(self) -> None:
        self.assertEqual(self.validate(self.payloads()), [])

    def test_each_engine_latency_has_a_named_negative_control(self) -> None:
        expected = {"preset": 333, "custom": 82, "preset_grain": 1042}
        for case_id, wrong_latency in (("preset", 1042), ("custom", 333), ("preset_grain", 333)):
            with self.subTest(case_id=case_id):
                payloads = self.payloads()
                payloads[("voice-change", case_id)]["latency_samples"] = wrong_latency
                report = self.validate(payloads)
                matches = [
                    message
                    for level, message in report
                    if level == "fail" and f"voice-change.{case_id}.latency_samples" in message
                ]
                self.assertEqual(len(matches), 1, report)
                self.assertIn(str(expected[case_id]), matches[0])

    def test_custom_latency_fixture_selects_grain_explicitly(self) -> None:
        custom = self.manifest["fixtures"]["presets"]["custom"]["dsp"]
        self.assertEqual(custom.get("voiceEngine"), {"engine": "grain", "f0MinHz": 70})

    def test_preset_grain_case_selects_grain_explicitly(self) -> None:
        voice_change = next(
            item for item in self.manifest["active_paths"] if item["path"] == "voice-change"
        )
        preset_grain = next(case for case in voice_change["cases"] if case["id"] == "preset_grain")
        self.assertIn("--set", preset_grain["argv"])
        self.assertIn("dsp.voiceEngine.engine=grain", preset_grain["argv"])
