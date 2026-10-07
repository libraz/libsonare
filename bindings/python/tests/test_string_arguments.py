"""String arguments cross the C boundary byte-for-byte or are refused, never truncated.

The C side reads a ``char *`` up to its first NUL, so a Python ``str`` holding one
would arrive silently shortened. Every encode goes through ``_utf8_arg``; these
cases hold that for representative entry points and for the source as a whole.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from libsonare import Mixer, SonareValueError, mix_stereo, mixing_scene_preset_json, note_to_hz
from libsonare._engine_conversions import _fixed_bytes
from libsonare._narrowing import _utf8_arg

from ._helpers import LIB_AVAILABLE

PACKAGE_DIR = Path(__file__).resolve().parents[1] / "src" / "libsonare"

needs_lib = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")


def test_utf8_arg_round_trips_and_names_the_argument() -> None:
    assert _utf8_arg("café", "name") == "café".encode()
    with pytest.raises(SonareValueError, match="strip_id must not contain NUL"):
        _utf8_arg("a\x00b", "strip_id")


def test_fixed_width_names_refuse_nul_before_truncating() -> None:
    assert _fixed_bytes("abc", 64, "name") == b"abc"
    with pytest.raises(SonareValueError, match="node_id must not contain NUL"):
        _fixed_bytes("a\x00b", 64, "node_id")


def test_no_encode_call_bypasses_the_shared_encoder() -> None:
    """The CLI writes files and is exempt; every other module encodes through ``_utf8_arg``."""
    offenders = []
    for path in sorted(PACKAGE_DIR.glob("*.py")):
        if path.name.startswith("_cli_") or path.name == "_narrowing.py":
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if re.search(r"\.encode\(", line) and ".decode(" not in line:
                offenders.append(f"{path.name}:{number}: {line.strip()}")
    assert not offenders, "str encoded without _utf8_arg:\n  " + "\n  ".join(offenders)


@needs_lib
@pytest.mark.parametrize(
    "call",
    [
        lambda: note_to_hz("A\x004"),
        lambda: mixing_scene_preset_json("a\x00b"),
        lambda: Mixer.from_scene_json('{"version":1}\x00trailing'),
    ],
    ids=["note_to_hz", "mixing_scene_preset_json", "Mixer.from_scene_json"],
)
def test_embedded_nul_is_refused(call) -> None:
    with pytest.raises(SonareValueError, match="must not contain NUL"):
        call()


@needs_lib
def test_embedded_nul_is_refused_on_mixer_methods() -> None:
    mixer = Mixer.from_scene_json(
        mixing_scene_preset_json(__import__("libsonare").mixing_scene_preset_names()[0]),
        sample_rate=48000,
        block_size=256,
    )
    try:
        with pytest.raises(SonareValueError, match="strip_id must not contain NUL"):
            mixer.add_strip("lead\x00hidden")
        with pytest.raises(SonareValueError, match="bus_id must not contain NUL"):
            mixer.add_bus("bus\x00hidden")
        with pytest.raises(SonareValueError, match="must not contain NUL"):
            mixer.add_vca_group("grp", members=["ok", "bad\x00member"])
    finally:
        mixer.close()


@needs_lib
def test_mix_stereo_sample_rate_range_is_enforced() -> None:
    strips = [([0.1] * 64, [0.1] * 64)]
    for bad in (7999, 384001, 0, -48000):
        with pytest.raises(SonareValueError, match=r"sample_rate out of supported range"):
            mix_stereo(strips, sample_rate=bad)
    with pytest.raises(SonareValueError):
        mix_stereo(strips, sample_rate=48000.5)  # type: ignore[arg-type]
    mix_stereo(strips, sample_rate=8000)
    mix_stereo(strips, sample_rate=384000)


@needs_lib
def test_scene_sample_rate_range_is_enforced() -> None:
    scene = mixing_scene_preset_json(__import__("libsonare").mixing_scene_preset_names()[0])
    for bad in (7999, 384001):
        with pytest.raises(SonareValueError, match=r"sample_rate out of supported range"):
            Mixer.from_scene_json(scene, sample_rate=bad)


@needs_lib
def test_mix_stereo_short_option_arrays_leave_defaults_and_long_ones_are_refused() -> None:
    strips = [([0.2] * 64, [0.2] * 64), ([0.2] * 64, [0.2] * 64)]
    short = mix_stereo(strips, fader_db=[-6.0], pan=[0.5], muted=[False])
    full = mix_stereo(strips, fader_db=[-6.0, 0.0], pan=[0.5, 0.0], muted=[False, False])
    assert short.left == pytest.approx(full.left)
    assert short.right == pytest.approx(full.right)
    for name in ("fader_db", "pan", "width", "muted", "input_trim_db", "pan_mode"):
        value = ["balance"] * 3 if name == "pan_mode" else [0.0] * 3
        if name == "muted":
            value = [False] * 3
        with pytest.raises(SonareValueError, match=f"'{name}' has more entries than strips"):
            mix_stereo(strips, **{name: value})  # type: ignore[arg-type]


@needs_lib
@pytest.mark.parametrize("field", ["azimuth", "elevation", "divergence", "lfe", "distance"])
@pytest.mark.parametrize("bad", [float("nan"), float("inf"), 1e40, True, "1"])
def test_mixer_surround_pan_names_the_refused_field(field: str, bad: object) -> None:
    import libsonare

    mixer = Mixer.from_scene_json(
        mixing_scene_preset_json(libsonare.mixing_scene_preset_names()[0]),
        sample_rate=48000,
        block_size=256,
    )
    try:
        with pytest.raises((SonareValueError, TypeError), match=field):
            mixer.set_surround_pan(0, **{field: bad})  # type: ignore[arg-type]
    finally:
        mixer.close()
