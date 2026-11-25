"""An integer keyword refuses a fraction, a NaN or a value out of range in the core's domain text.

The table is generated from the core's numeric-domain dump. Each row is driven twice:
by a number the binding refuses before the core sees it, and by an integer just outside
the domain, which the core (or the binding's own copy of the check) refuses. Both must
state the row's domain, so a row that disagrees with the core fails here.
"""

from __future__ import annotations

import inspect
import math
import re
from typing import Any

import numpy as np
import pytest

import libsonare
from libsonare import SonareError
from libsonare._integer_domains import DOMAINS

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

_SR = 22050
_TONE = (0.5 * np.sin(2 * np.pi * 440 * np.arange(_SR // 8) / _SR)).astype(np.float32)

# A valid value for each required argument a swept entry takes.
_REQUIRED: dict[str, Any] = {
    "samples": _TONE,
    "y": _TONE,
    "audio": _TONE,
    "left": _TONE,
    "right": _TONE,
    "onset_envelope": np.abs(_TONE[:2048]),
    "mel": np.ones((128, 4), np.float32),
    "coefficients": np.ones((20, 4), np.float32),
    "values": np.ones((8, 4), np.float32),
    "tempogram": np.ones((384, 4), np.float32),
    "times": [0.1],
    "frames": 1,
    "time_sec": 0.1,
    "intervals": np.array([[0, 100]]),
    "sample_rate": _SR,
    "target_sample_rate": _SR,
    "sr": _SR,
    "n_fft": 2048,
    "channels": [_TONE, _TONE],
    "magnitude": np.ones((1025, 4), np.float32),
    "s": np.ones((64, 8), np.float32),
    "data": np.ones((64, 8), np.float32),
    "frame_length": 256,
    "hop_length": 128,
    "speed_ratio": 1.0,
    "pitch_semitones": 0.0,
    "recurrence": np.eye(8, dtype=np.float32),
    "boundaries": [0, 4],
    "ops": [],
    "win": 3,
    "midi": 60.0,
    "root": 0,
    "mode_mask": 0xAB5,
    "pitch_class": 0,
    "key_root": libsonare.PitchClass.C,
    "mode": "major",
    "features": np.ones((4, 16), np.float32),
    "reference": _TONE,
    "take": _TONE,
    "frequencies": [440.0],
    "signals": [_TONE],
    "num_channels": 1,
    "num_samples": 1024,
    "page_frames": 256,
    "notes": [],
    "targets": [],
    "beats": [libsonare.Beat(time_sec=0.5 * i, strength=1.0) for i in range(8)],
    "ppq": 0.0,
    "phase": np.zeros((1025, 4), np.float32),
    "format": "wav",
    "observations": libsonare.BeatObservations(
        accent=np.ones(8, np.float32), has_attack=np.ones(8, np.bool_)
    ),
    "events": [],
    "group": 0,
    "channel": 0,
    "controller": 1,
    "value": 0,
    "value14": 0,
    "value32": 0,
    "delta32": 0,
    "note": 60,
    "velocity": 100,
    "velocity16": 0x8000,
    "pressure": 0,
    "pressure32": 0,
    "program": 0,
    "bank": 0,
    "bank_msb": 0,
    "bank_lsb": 0,
    "index": 0,
    "bend": 8192,
    "bend14": 8192,
    "bend32": 0x80000000,
    "param_id": 1,
    "unit_value": 0.5,
    "bindings": [],
    "k": 2,
    "length_m": 6.0,
    "width_m": 5.0,
    "height_m": 3.0,
}

# Arguments refused unless given together, so probing one supplies the other.
_PAIRED = {"bank_msb": "bank_lsb", "bank_lsb": "bank_msb"}

# Entries whose required argument of a shared name needs another shape.
_ENTRY_REQUIRED: dict[str, dict[str, Any]] = {
    "mix_stereo": {"strips": [(_TONE, _TONE)]},
    "mixing_assistant_suggest": {
        "tracks": [libsonare.MixAssistantTrack("a", [_TONE], sample_rate=_SR)]
    },
    "mastering_apply_pair_processor_stereo": {
        "processor": "match.abCrossfade",
        "source_left": _TONE,
        "source_right": _TONE,
        "reference_left": _TONE,
        "reference_right": _TONE,
    },
    "segment_cross_similarity": {
        "x": np.ones((8, 16), np.float32),
        "y": np.ones((8, 16), np.float32),
    },
    "render_percussive_events": {
        "events": [libsonare.PercussiveEventInput(onset_sample=0, offset_sample=64)]
    },
    "pitch_correct_timevarying": {"f0_hz": np.full(len(_TONE) // 512 + 1, 440.0, np.float32)},
    "decompose": {
        "s": np.ones(64 * 8, np.float32),
        "n_features": 64,
        "n_frames": 8,
        "n_components": 2,
    },
    "decompose_with_init": {
        "s": np.ones(64 * 8, np.float32),
        "n_features": 64,
        "n_frames": 8,
        "n_components": 2,
    },
    "nn_filter": {
        "s": np.ones(64 * 8, np.float32),
        "n_features": 64,
        "n_frames": 8,
        "n_components": 2,
    },
    "samples_to_frames": {"samples": 1024},
    "note_move": {"target_onset_sample": 0},
}

# Keywords whose explicit 0 is refused as the spelling of absence ("omit it"), so the
# out-of-domain probe steps past it to the first negative value.
_ZERO_MEANS_OMIT = {("frames_to_samples", "n_fft"), ("samples_to_frames", "n_fft")}


# The C types' own ranges, which a binding reader states when no core domain applies.
_TYPE_RANGES = {
    "in [-2147483648, 2147483647]",
    "in [0, 4294967295]",
    "in [-9223372036854775808, 9223372036854775807]",
    "in [0, 18446744073709551615]",
}


def _integer_keywords(fn: Any) -> list[str]:
    return [
        name
        for name, p in inspect.signature(fn).parameters.items()
        if p.default is not p.empty
        and "int" in str(p.annotation)
        and not any(t in str(p.annotation) for t in ("float", "bool"))
    ]


def _callable_entries() -> list[str]:
    names = []
    for name in sorted(libsonare.__all__):
        fn = getattr(libsonare, name)
        if not inspect.isfunction(fn):
            continue
        params = inspect.signature(fn).parameters.items()
        required = [
            n for n, p in params if p.default is p.empty and p.kind is p.POSITIONAL_OR_KEYWORD
        ]
        if all(n in _REQUIRED for n in required):
            names.append(name)
    return names


_KINDS = {
    None: "an integer",
    "power_of_two": "a power of two",
    "even": "an even integer",
    "odd": "an odd integer",
}


def _rows() -> list[tuple[str, str, int, int | None, str | None]]:
    return [
        (entry, keyword, lo, hi, stricter)
        for (entry, keyword), (lo, hi, stricter) in DOMAINS.items()
        if keyword in inspect.signature(getattr(libsonare, entry, lambda: None)).parameters
    ]


def _refusal(entry: str, keyword: str, value: object) -> str:
    fn = getattr(libsonare, entry)
    given = _ENTRY_REQUIRED.get(entry, {})
    parameters = inspect.signature(fn).parameters
    kwargs = {
        name: given.get(name, _REQUIRED.get(name))
        for name, p in parameters.items()
        if p.default is p.empty
        and p.kind in (p.POSITIONAL_OR_KEYWORD, p.KEYWORD_ONLY)
        and name != keyword
    }
    # An entry-specific value also stands in for an optional argument the call needs.
    kwargs.update({n: v for n, v in given.items() if n in parameters and n != keyword})
    paired = _PAIRED.get(keyword)
    if paired is not None and paired in parameters:
        kwargs[paired] = _REQUIRED[paired]
    with pytest.raises((SonareError, TypeError, ValueError)) as caught:
        fn(**kwargs, **{keyword: value})
    return str(caught.value)


def _domain_text(keyword: str, lo: int, hi: int | None, stricter: str | None = None) -> str:
    if hi is None:
        return f"{keyword} must be an integer of at least {lo}"
    return f"{keyword} must be {_KINDS[stricter]} in [{lo}, {hi}]"


def test_the_table_reaches_the_public_entries() -> None:
    # A member of an array element is keyed by its path, which no signature names.
    keywords = [key for key in DOMAINS if "[" not in key[1]]
    assert len(_rows()) >= len(keywords) // 2


@pytest.mark.parametrize("value", [7999, 384001, 8000.5, 1e100, math.inf])
def test_assistant_track_sample_rate_uses_its_array_domain(value: object) -> None:
    low, high, stricter = DOMAINS[("mixing_assistant_suggest", "track_sample_rates[]")]
    expected = _domain_text("tracks[0].sample_rate", low, high, stricter)
    track = libsonare.MixAssistantTrack("a", [_TONE], sample_rate=value)
    with pytest.raises((SonareError, TypeError, ValueError)) as caught:
        libsonare.mixing_assistant_suggest([track], sample_rate=_SR)
    assert expected in str(caught.value)


@pytest.mark.parametrize(("entry", "keyword", "lo", "hi", "stricter"), _rows())
def test_a_non_integer_is_refused_in_the_core_domain(
    entry: str, keyword: str, lo: int, hi: int | None, stricter: str | None
) -> None:
    expected = _domain_text(keyword, lo, hi, stricter)
    # 2 ** 40 is in a domain open above, so it only probes a bounded one.
    for value in (2.5, math.nan) if hi is None else (2.5, math.nan, 2.0**40, 2**40):
        assert expected in _refusal(entry, keyword, value), value


@pytest.mark.parametrize(("entry", "keyword", "lo", "hi", "stricter"), _rows())
def test_the_core_refuses_an_integer_outside_the_row_in_the_same_text(
    entry: str, keyword: str, lo: int, hi: int | None, stricter: str | None
) -> None:
    below = lo - 1
    if below == 0 and (entry, keyword) in _ZERO_MEANS_OMIT:
        below = -1
    assert _domain_text(keyword, lo, hi, stricter) in _refusal(entry, keyword, below)


@pytest.mark.slow
def test_every_integer_keyword_the_core_bounds_has_a_row() -> None:
    """Drive each integer keyword out of domain; a core domain refusal must come with a row."""
    missing = []
    for entry in _callable_entries():
        for keyword in _integer_keywords(getattr(libsonare, entry)):
            if (entry, keyword) in DOMAINS:
                continue
            for value in (0, -1):
                try:
                    text = _refusal(entry, keyword, value)
                except pytest.fail.Exception:
                    continue
                stated = re.search(
                    rf"{keyword} must be an integer (in \[[^]]*\]|of at least -?\d+)", text
                )
                if stated and stated.group(1) not in _TYPE_RANGES:
                    missing.append(f"{entry}({keyword}): {text}")
                    break
    assert missing == []
