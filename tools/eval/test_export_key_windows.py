"""Synthetic integration checks for the optional one-second chroma export."""

from __future__ import annotations

import itertools
import json
import math
import os
import struct
import subprocess
import tempfile
import unittest
import wave
from pathlib import Path

EXPORTER = os.environ.get("SONARE_KEY_WINDOW_EXPORTER")


def write_tone(path: Path, sample_count: int) -> float:
    sample_rate = 22050
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        output.writeframes(
            b"".join(
                struct.pack("<h", round(12000 * math.sin(2 * math.pi * 440 * i / sample_rate)))
                for i in range(sample_count)
            )
        )
    return sample_count / sample_rate


def expected_support_frame_counts(
    cells: list[dict], duration: float, hop: int, sample_rate: int
) -> list[int]:
    counts = []
    frame_count = math.ceil(duration * sample_rate / hop)
    for cell in cells:
        count = sum(
            min(duration, cell["end"], (frame + 1) * hop / sample_rate)
            > max(cell["start"], frame * hop / sample_rate)
            for frame in range(frame_count)
        )
        counts.append(count)
    return counts


@unittest.skipUnless(EXPORTER, "set SONARE_KEY_WINDOW_EXPORTER to the built native tool")
class FineWindowExporterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.exporter = Path(EXPORTER).resolve()
        if not cls.exporter.is_file():
            raise RuntimeError(f"native exporter does not exist: {cls.exporter}")

    def export(self, audio: Path, fine: bool = False) -> dict:
        command = [str(self.exporter)]
        if fine:
            command.append("--fine-windows")
        command.append(str(audio))
        result = subprocess.run(command, check=True, capture_output=True, text=True, timeout=60)
        return json.loads(result.stdout)

    def test_fine_flag_preserves_full_track_and_eight_second_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            audio = Path(directory) / "tone.wav"
            duration = write_tone(audio, 22050 * 9 + 7350)
            baseline = self.export(audio)
            extended = self.export(audio, fine=True)

        fine_fields = {"fine_window_seconds", "fine_pooling", "fine_windows"}
        self.assertEqual(set(extended) - fine_fields, set(baseline))
        for key, value in baseline.items():
            self.assertEqual(extended[key], value, key)
        self.assertAlmostEqual(baseline["duration"], duration, places=12)
        self.assertEqual(extended["fine_window_seconds"], 1)
        self.assertEqual(extended["fine_pooling"], "physical_frame_support_overlap")

    def test_fine_cells_cover_duration_with_actual_frame_support(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            audio = Path(directory) / "non_integral.wav"
            duration = write_tone(audio, 2 * 22050 + 5432)
            result = self.export(audio, fine=True)

        cells = result["fine_windows"]
        self.assertEqual(len(cells), math.ceil(duration))
        self.assertAlmostEqual(cells[0]["start"], 0.0)
        self.assertAlmostEqual(cells[-1]["end"], duration, places=12)
        for previous, current in itertools.pairwise(cells):
            self.assertEqual(previous["end"], current["start"])
        self.assertAlmostEqual(
            sum(cell["supported_seconds"] for cell in cells), duration, places=10
        )
        self.assertEqual(
            [cell["frame_count"] for cell in cells],
            expected_support_frame_counts(
                cells, duration, result["hop_length"], result["sample_rate"]
            ),
        )
        for cell in cells:
            self.assertGreater(cell["frame_count"], 0)
            self.assertGreater(cell["supported_seconds"], 0.0)
            self.assertLessEqual(cell["supported_seconds"], cell["end"] - cell["start"] + 1e-12)
            self.assertEqual(len(cell["candidates"]), 24)
            self.assertEqual(cell["selected_profile"], result["selected_profile"])

    def test_sub_hop_tail_uses_overlapping_chroma_frame(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            audio = Path(directory) / "sub_hop_tail.wav"
            duration = write_tone(audio, 22067)
            result = self.export(audio, fine=True)

        tail = result["fine_windows"][-1]
        self.assertGreater(duration, 1.0)
        self.assertLess(duration - 1.0, result["hop_length"] / result["sample_rate"])
        self.assertAlmostEqual(tail["start"], 1.0)
        self.assertAlmostEqual(tail["end"], duration, places=12)
        self.assertGreater(tail["frame_count"], 0)
        self.assertAlmostEqual(tail["supported_seconds"], duration - 1.0, places=10)
        self.assertEqual(
            [cell["frame_count"] for cell in result["fine_windows"]],
            expected_support_frame_counts(
                result["fine_windows"], duration, result["hop_length"], result["sample_rate"]
            ),
        )
        correlations = [candidate["correlation"] for candidate in tail["candidates"]]
        self.assertTrue(any(abs(value) > 1e-6 for value in correlations))

    def test_padding_beyond_audio_has_no_fine_frame_support(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            audio = Path(directory) / "short.wav"
            duration = write_tone(audio, 22050 + 17)
            result = self.export(audio, fine=True)

        self.assertEqual(
            result["active_frame_count"], sum(cell["frame_count"] for cell in result["windows"])
        )
        for cell in result["fine_windows"]:
            self.assertLessEqual(cell["end"], duration)
            self.assertGreater(cell["frame_count"], 0)
        self.assertAlmostEqual(
            sum(cell["supported_seconds"] for cell in result["fine_windows"]), duration, places=10
        )


if __name__ == "__main__":
    unittest.main()
