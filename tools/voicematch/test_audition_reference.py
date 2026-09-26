"""The audition page's reference must be the capture policy aims a voice at.

`make_audition.py` used to move a different, playable capture to the front of
a voice whenever the one `policy.json` aims it at could render no reference —
the standard kit's page played the sampled library's kit while the fit and the
gates were reading the module. These guard the fix: no reordering happens, and
a module capture's reference is played at the FONT's own address.

    rye run --pyproject bindings/python/pyproject.toml \\
        python -m pytest tools/voicematch/test_audition_reference.py -q
"""

from __future__ import annotations

import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import bank
import make_audition
from smf import Note
from test_smf import events


def _args(**overrides) -> SimpleNamespace:
    base = {
        "preset": "",
        "config": "",
        "program": None,
        "programs": "",
        "kits": "",
        "banks": "",
        "lib": "",
    }
    base.update(overrides)
    return SimpleNamespace(**base)


def test_resolve_voices_does_not_reorder_or_wrap_the_bank(monkeypatch):
    """Whatever `bank.voices` returns is exactly what a run presents.

    A capture that could render no reference used to be swapped for a
    different layer's capture entirely (`playable_first`); this guards against
    that reappearing under a new name.
    """
    sentinel = [object(), object()]
    monkeypatch.setattr(make_audition, "voices", lambda *a, **k: sentinel)
    assert make_audition.resolve_voices(_args(programs="all")) is sentinel


def test_every_bank_voice_gets_the_capture_policy_aims_it_at(monkeypatch):
    """Voice.capture, as a real run resolves it, is `capture_for`'s own answer.

    Catalogue lookup is disabled so the comparison does not depend on whether
    a `-DBUILD_TUNING=ON` library happens to be on disk where the test runs.
    Holds with `comparison_captures` in play too, on any voice that has one:
    offering the other captures beside the reference must never move which
    one the reference itself is.
    """
    monkeypatch.setattr(make_audition, "load_catalogue", lambda lib: None)
    kits = ",".join(str(k) for k in sorted(bank.KIT_NAMES))
    voices = make_audition.resolve_voices(_args(programs="all", kits=kits))
    pool = bank.captures()
    for voice in voices:
        expected = bank.capture_for(voice.program, voice.bank, kit=voice.kit, pool=pool)
        # `==` rather than `is`: `bank.voices` re-reads `capture/` for its own
        # pool, so an equal `Capture` from a second read is a distinct object.
        assert voice.capture == expected, voice.slug
        comparisons = make_audition.comparison_captures(voice)
        assert all(c != voice.capture for c in comparisons), voice.slug


def test_the_standard_kit_gets_the_library_kit_as_its_only_comparison():
    """The kit's three module grids are one set of recordings split by decay
    window for gating, not three things to hear -- only the sampled library
    kit, which the page used to substitute in as the reference outright
    (`bf04145c`), is a genuinely different capture to offer beside it."""
    pool = bank.captures()
    voice = bank.Voice(
        program=0, kit=True, captures=tuple(bank.captures_for(0, kit=True, pool=pool))
    )
    assert voice.capture.id == "drums_module"
    assert [c.id for c in make_audition.comparison_captures(voice)] == ["drums"]


def test_electric_grand_gets_the_library_capture_as_its_only_comparison():
    pool = bank.captures()
    voice = bank.Voice(program=2, captures=tuple(bank.captures_for(2, 0, pool=pool)))
    assert voice.capture.id == "electric_grand_module"
    assert [c.id for c in make_audition.comparison_captures(voice)] == ["electric_grand"]


def test_build_sources_gives_a_comparison_its_own_role_and_keeps_one_reference():
    """A module-aimed voice with a library capture gets a `comparison` source
    and still exactly one `reference` capture: the policy's own."""
    module_cap = bank.load_capture(bank.CAPTURE_DIR / "drums_module.json")
    library_cap = bank.load_capture(bank.CAPTURE_DIR / "drums.json")
    voice = bank.Voice(program=0, kit=True, captures=(module_cap, library_cap))
    timbres = list(module_cap.timbres)
    comparisons = [(library_cap, list(library_cap.timbres))]

    sources = make_audition.build_sources(voice, timbres, [], comparisons)
    roles = {k: v["role"] for k, v in sources.items()}

    assert [k for k, r in roles.items() if r == "reference"] == [module_cap.timbres[0]["id"]]
    assert sorted(k for k, r in roles.items() if r == "comparison") == sorted(
        t["id"] for t in library_cap.timbres
    )
    for t in library_cap.timbres:
        assert sources[t["id"]]["detail"].startswith("a modern recording of the same instrument")


def _fake_font(monkeypatch, tmp_path, *, bank: int, program: int, name: str = "KIT A"):
    """Wire `resolve_font` and `SoundFont` to answer one preset, without a real file."""

    class FakePreset:
        pass

    FakePreset.bank, FakePreset.program = bank, program

    class FakeFont:
        def __init__(self, path):
            self.path = path

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

        def find_by_name(self, want):
            assert want == name
            return FakePreset()

    font_path = tmp_path / "FONT.sf2"
    monkeypatch.setattr(
        make_audition, "resolve_font", lambda cfg, timbre: (font_path, timbre["preset"])
    )
    monkeypatch.setattr(make_audition, "SoundFont", FakeFont)
    return font_path


def _fake_render(monkeypatch):
    captured: dict = {}

    def fake(smf_bytes, total_seconds, sr, soundfont=None):
        captured["smf"], captured["soundfont"] = smf_bytes, soundfont
        captured["total"], captured["sr"] = total_seconds, sr
        return np.zeros((1, 2), dtype=np.float32)

    monkeypatch.setattr(make_audition, "render_oracle_fluidsynth", fake)
    return captured


def test_render_module_reference_addresses_a_percussion_preset_via_the_drum_channel(
    monkeypatch, tmp_path
):
    """A drum kit's presets sit at SF2 bank 128, which the drum channel alone reaches.

    Measured against the module's own drum file: channel 10 selects bank 128
    whatever the bank-select bytes say, and nothing sent on any other channel
    reaches it — so no bank-select is the right thing to send here, and the
    channel is doing the addressing instead.
    """
    font_path = _fake_font(monkeypatch, tmp_path, bank=128, program=3)
    captured = _fake_render(monkeypatch)

    cfg = {"_path": str(tmp_path / "drums_module.json")}
    timbre = {"id": "t0", "preset": "KIT A"}
    out = make_audition.render_module_reference(
        cfg, timbre, [Note(38, 100, 0.1, 0.2)], (), 9, 0.5, 1.5, 48000
    )

    assert out.shape == (1, 2)
    assert captured["soundfont"] == font_path
    assert (captured["total"], captured["sr"]) == (1.5, 48000)
    raw = [e for _, e in events(captured["smf"])]
    assert [e for e in raw if e[0] & 0xF0 == 0xC0] == [bytes([0xC9, 3])]
    bank_select = [e for e in raw if e[0] & 0xF0 == 0xB0 and e[1] in (0, 32)]
    assert not bank_select, "no bank-select on the drum channel"


def test_render_module_reference_sends_bank_select_for_a_nonzero_melodic_bank(
    monkeypatch, tmp_path
):
    """A bank under 128 is a plain Bank-Select-MSB, sent as any GS variation is."""
    _fake_font(monkeypatch, tmp_path, bank=9, program=7, name="TONE X")
    captured = _fake_render(monkeypatch)

    cfg = {"_path": str(tmp_path / "electric_grand_module.json")}
    timbre = {"id": "t0", "preset": "TONE X"}
    make_audition.render_module_reference(
        cfg, timbre, [Note(60, 100, 0.1, 1.0)], (), 0, 0.5, 2.0, 48000
    )

    raw = [e for _, e in events(captured["smf"])]
    assert bytes([0xB0, 0, 9]) in raw
    assert bytes([0xB0, 32, 0]) in raw
    assert [e for e in raw if e[0] & 0xF0 == 0xC0] == [bytes([0xC0, 7])]


def test_render_module_reference_refuses_a_percussion_bank_preset_off_the_drum_channel(
    monkeypatch, tmp_path
):
    """SF2 bank 128 is unreachable anywhere but channel 10, so addressing it
    elsewhere must fail rather than silently render whatever channel 0 has."""
    _fake_font(monkeypatch, tmp_path, bank=128, program=0)
    _fake_render(monkeypatch)

    cfg = {"_path": str(tmp_path / "drums_module.json")}
    timbre = {"id": "t0", "preset": "KIT A"}
    with pytest.raises(ValueError, match="percussion"):
        make_audition.render_module_reference(
            cfg, timbre, [Note(38, 100, 0.1, 0.2)], (), 0, 0.5, 1.5, 48000
        )


def test_render_module_reference_fails_loudly_on_an_unresolvable_font(monkeypatch, tmp_path):
    """A missing overlay or font is a `ValueError` from `resolve_font`, uncaught here.

    `render_take` is what turns this into `ReferenceUnavailable`; this function
    itself must not swallow it into a quiet skip.
    """
    monkeypatch.setattr(
        make_audition,
        "resolve_font",
        lambda cfg, timbre: (_ for _ in ()).throw(ValueError("no overlay")),
    )
    with pytest.raises(ValueError, match="no overlay"):
        make_audition.render_module_reference(
            {"_path": "x.json"}, {"id": "t0"}, [Note(60, 100, 0.0, 0.1)], (), 0, 0.1, 0.5, 48000
        )
