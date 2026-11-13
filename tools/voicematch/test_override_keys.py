"""An override key the library does not know stops the run before any render."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import profile as profile_module

from _repo import REPO_ROOT
from build_lib import dylib_path
from catalogue import Catalogue, dump_catalogue
from knobs import refuse_unknown_override_keys

TUNING_BUILD = REPO_ROOT / "build-tuning"


def _catalogue(*keys: str) -> Catalogue:
    return Catalogue(defaults=dict.fromkeys(keys, 1.0), programs={}, modes={}, bounds={})


def test_an_unknown_key_is_refused_by_name_and_known_keys_pass():
    cat = _catalogue("electric_guitar.ks.exc_brightness", "piano_voice.kHammerWidthHarmonics")
    refuse_unknown_override_keys("electric_guitar.ks.exc_brightness=0.4", cat)
    with pytest.raises(ValueError, match=r"does not know: electric_guitar\.bogus_key, x\.y\.z"):
        refuse_unknown_override_keys(
            "electric_guitar.ks.exc_brightness=0.4,electric_guitar.bogus_key=1,x.y.z=2", cat
        )


def test_profile_refuses_the_environments_unknown_key_before_rendering(monkeypatch):
    asked = []

    def fake_dump(program, pattern, lib, *, sr, bank=0):
        asked.append((program, bank, sr))
        return _catalogue("electric_guitar.ks.exc_brightness")

    monkeypatch.setattr(profile_module, "dump_catalogue", fake_dump)
    cfg = {"program": 27, "sample_rate": 48000}
    monkeypatch.delenv("SONARE_TUNING_OVERRIDES", raising=False)
    profile_module.check_override_keys(cfg)
    assert asked == []
    monkeypatch.setenv("SONARE_TUNING_OVERRIDES", "electric_guitar.bogus_key=1")
    with pytest.raises(ValueError, match=r"electric_guitar\.bogus_key"):
        profile_module.check_override_keys(cfg)
    assert asked == [(27, 0, 48000)]


def test_the_librarys_own_catalogue_refuses_a_bogus_electric_guitar_key():
    lib = dylib_path(TUNING_BUILD)
    if lib is None:
        pytest.skip("build-tuning/lib/libsonare.dylib is unavailable")
    cat = dump_catalogue(27, "sustain", str(lib), sr=48000)
    patch = cat.patch_for(27)
    real = next(k for k in cat.defaults if k.startswith(f"{patch}."))
    refuse_unknown_override_keys(f"{real}=0.5", cat)
    with pytest.raises(ValueError, match=r"electric_guitar\.bogus_key"):
        refuse_unknown_override_keys("electric_guitar.bogus_key=1", cat)
