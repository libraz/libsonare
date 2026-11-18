"""``project bounce --synth`` takes the kebab-case preset name the CLI lists.

The library names its presets in lowerCamel; the command line keeps kebab-case,
reads it in any letter case, maps it to the library name before the project is
loaded, and refuses any other spelling, the library's own included.
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

_CLEAN_PROJECT = {"version": 7, "sample_rate": 48000, "tracks": [], "clips": []}


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
        "--in",
        str(project),
        "-o",
        str(output),
        "--frames",
        "4800",
        "--synth",
        synth,
    )
    return result, output


def test_the_preset_listing_is_kebab_case() -> None:
    result = _run_console("project", "synth-presets")
    assert result.returncode == 0, result.stderr
    assert "saw-lead" in result.stdout
    assert "sawLead" not in result.stdout


@pytest.mark.parametrize("spelling", ["saw-lead", "Saw-Lead", "SAW-LEAD"])
def test_a_kebab_preset_name_is_accepted_in_any_case(tmp_path: Path, spelling: str) -> None:
    result, output = _bounce(tmp_path, spelling)
    assert result.returncode == 0, result.stderr
    assert output.is_file()


@pytest.mark.parametrize("spelling", ["sawLead", "saw_lead", "SawLead"])
def test_another_spelling_is_refused_before_rendering(tmp_path: Path, spelling: str) -> None:
    result, output = _bounce(tmp_path, spelling)
    assert result.returncode == EXIT_INVALID_PARAMETER, result.stderr
    assert "saw-lead" in result.stderr
    assert not output.exists()
