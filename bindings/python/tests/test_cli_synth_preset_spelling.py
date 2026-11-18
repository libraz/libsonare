"""``project bounce --synth`` takes the library preset id the CLI lists.

The command line names a preset by its lowerCamel library id in any letter case
and leaves any other spelling, kebab-case included, for the library to refuse.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

EXIT_INVALID_PARAMETER = 3

_CLEAN_PROJECT = {"version": 7, "sampleRate": 48000, "tracks": [], "clips": []}


def _run_console(*args: str) -> subprocess.CompletedProcess[str]:
    script = Path(sys.executable).parent / "sonare"
    assert script.is_file(), f"installed console script is missing: {script}"
    return subprocess.run([str(script), *args], capture_output=True, text=True)


def _bounce(tmp_path: Path, synth: str) -> tuple[subprocess.CompletedProcess[str], Path]:
    project = tmp_path / "project.json"
    project.write_text(json.dumps(_CLEAN_PROJECT), encoding="utf-8")
    output = tmp_path / "bounce.wav"
    result = _run_console(
        "project",
        "bounce",
        str(project),
        "-o",
        str(output),
        "--frames",
        "4800",
        "--synth",
        synth,
    )
    return result, output


def test_the_preset_listing_is_the_library_ids() -> None:
    result = _run_console("project", "synth-presets")
    assert result.returncode == 0, result.stderr
    assert "sawLead" in result.stdout
    assert "saw-lead" not in result.stdout


@pytest.mark.parametrize("spelling", ["sawLead", "SawLead", "SAWLEAD"])
def test_a_library_preset_id_is_accepted_in_any_case(tmp_path: Path, spelling: str) -> None:
    result, output = _bounce(tmp_path, spelling)
    assert result.returncode == 0, result.stderr
    assert output.is_file()


@pytest.mark.parametrize("spelling", ["saw-lead", "saw_lead", "SAW-LEAD"])
def test_another_spelling_is_refused_before_rendering(tmp_path: Path, spelling: str) -> None:
    result, output = _bounce(tmp_path, spelling)
    assert result.returncode == EXIT_INVALID_PARAMETER, result.stderr
    assert "synth preset" in result.stderr
    assert not output.exists()
