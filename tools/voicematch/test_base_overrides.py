"""A fit run on top of fixed engine constants: the merge, its refusals, its reach and its record."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import fits
import render_rows
from _repo import REPO_ROOT
from build_lib import dylib_path
from catalogue import Catalogue
from knobs import (
    Knob,
    check_base_overrides,
    load_base_overrides,
    merge_overrides,
)

HERE = Path(__file__).resolve().parent
TUNING_BUILD = REPO_ROOT / "build-tuning"
#: A patch field that moves a normalized short render without being a level.
KEY = "fam0.piano.strike_position"


def _catalogue(*keys: str) -> Catalogue:
    return Catalogue(defaults=dict.fromkeys(keys, 1.0), programs={}, modes={}, bounds={})


def _knob(label: str) -> Knob:
    return Knob(label=label, lo=0.0, hi=1.0, log=False, start_value=0.5, tunable=label)


def test_candidate_key_wins_and_base_keys_survive():
    merged = merge_overrides("a.x=1,a.y=2", "a.y=5,a.z=7")
    assert merged == "a.x=1,a.y=5,a.z=7"
    assert merge_overrides("", "a.y=5") == "a.y=5"
    assert merge_overrides("a.x=1", "") == "a.x=1"


def test_inline_and_file_bases_read_the_same(tmp_path):
    f = tmp_path / "base.txt"
    f.write_text("a.x=1  # note\na.y=2\n")
    assert load_base_overrides(f"@{f}") == load_base_overrides("a.x=1,a.y=2") == "a.x=1,a.y=2"
    with pytest.raises(ValueError, match="not a number"):
        load_base_overrides("a.x=big")


def test_a_base_key_that_is_a_search_knob_is_refused():
    cat = _catalogue("fam0.gain", "fam0.body")
    with pytest.raises(ValueError, match="also searched as a knob: fam0.gain"):
        check_base_overrides("fam0.gain=0.1", [_knob("fam0.gain")], cat, tunables={})


def test_a_base_key_that_resolves_to_nothing_is_refused():
    cat = _catalogue("fam0.gain")
    with pytest.raises(ValueError, match="does not know: fam0.nope"):
        check_base_overrides("fam0.gain=0.1,fam0.nope=1", [], cat, tunables={})
    assert check_base_overrides("fam0.gain=0.1", [], cat, tunables={}) == "fam0.gain=0.1"


def test_every_render_goes_through_the_merge(monkeypatch, tmp_path):
    seen = {}

    def fake_run(cmd, **kw):
        seen["env"] = kw["env"].get("SONARE_TUNING_OVERRIDES")
        return subprocess.CompletedProcess(cmd, 0, stdout="[]", stderr="")

    monkeypatch.setattr(render_rows, "dylib_path", lambda b: Path("lib"))
    monkeypatch.setattr(render_rows.subprocess, "run", fake_run)
    render_rows.render_model_rows_subprocess(
        tmp_path, 0, "sustain", "60", overrides="a.y=5", base_overrides="a.x=1,a.y=2"
    )
    assert seen["env"] == "a.x=1,a.y=5"


def _render(lib: Path, overrides: str) -> np.ndarray:
    code = (
        f"import sys; sys.path.insert(0, {str(HERE)!r})\n"
        "import numpy as np\n"
        "from render_model import render_model\n"
        "from smf import Note, write_smf\n"
        "smf = write_smf([Note(60, 100, 0.0, 0.2)], program=0, end_pad=0.1)\n"
        "np.save(sys.argv[1], render_model(smf, 0.3, 48000, rig=False))\n"
    )
    env = {**os.environ, "SONARE_LIB_PATH": str(lib)}
    env.pop("SONARE_TUNING_OVERRIDES", None)
    if overrides:
        env["SONARE_TUNING_OVERRIDES"] = overrides
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "a.npy"
        subprocess.run([sys.executable, "-c", code, str(out)], env=env, check=True)
        return np.load(out)


@pytest.fixture(scope="module")
def tuning_lib():
    lib = dylib_path(TUNING_BUILD)
    if lib is None:
        pytest.skip("build-tuning/lib/libsonare.dylib is unavailable")
    newest = max(p.stat().st_mtime for p in (REPO_ROOT / "src").rglob("*") if p.is_file())
    if Path(lib).resolve().stat().st_mtime < newest:
        pytest.skip("build-tuning library is older than its sources")
    return Path(lib)


def test_the_base_reaches_the_library(tuning_lib):
    plain = _render(tuning_lib, "")
    based = _render(tuning_lib, merge_overrides(f"{KEY}=0.4", ""))
    assert not np.allclose(plain, based, atol=1e-6)
    alone = _render(tuning_lib, f"{KEY}=0.2")
    both = _render(tuning_lib, merge_overrides(f"{KEY}=0.4", f"{KEY}=0.2"))
    assert np.array_equal(alone, both)


def _report(base: str = "") -> dict:
    return {
        "objective": "d",
        "knobs": [{"key": "brass.x"}],
        **({"base_overrides": base} if base else {}),
    }


def test_a_fit_record_carries_the_base_and_reads_under_base():
    _, based = fits.record_fit(_report("e.k=1"), capture="c", timbre="t", outcome="adopted")
    _, plain = fits.record_fit(_report(), capture="c", timbre="t", outcome="adopted")
    assert based["base_overrides"] == "e.k=1" and "base_overrides" not in plain
    got = fits.axis(["brass"], {"brass": based}, "d", set())
    assert got["state"] == fits.UNDER_BASE and got["base_overrides"] == ["e.k=1"]
    assert fits.axis(["brass"], {"brass": plain}, "d", set())["state"] == fits.CURRENT
    # A fit under another objective is stale whatever it stood on.
    assert fits.axis(["brass"], {"brass": based}, "new", set())["state"] == fits.STALE
