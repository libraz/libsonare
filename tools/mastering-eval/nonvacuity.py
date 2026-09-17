"""Show that each metric moves for its own degradation and not for the others.

The contract's gate: a set of metrics that all move together is one metric
reported four times, so it is not enough to see a metric respond -- every metric
has to be measured against every degradation. This module builds that full cross
table.

Three tables, because no one material carries every question.

**Home** runs each degradation on the corpus item its expectation is defined
against: a noise item for the denoiser knobs, a click item for the declicker, a
long programme item for the chain. It answers "does the metric respond to its
own degradation", and it is where the expected magnitudes are read.

**Common** and **speech** run every degradation again on one shared material
each, so a whole column comes from one signal and one reference. That is what
answers "do these metrics move together", which a table whose rows each use a
different material cannot settle. The speech table is the one that settles it:
its material is the only corpus item both speech-bearing and longer than the 3.1
s short-term window, so no column on it is out of domain.

**Feeds.** Most cases take a mono feed, which is what every restoration entry
point accepts, and the shared materials are restaged on one. A metric whose
reading combines channels cannot be exercised there at all, so a case may instead
name the ``stereo`` feed and be handed the item's own channels; the restoration
metrics are defined on one channel and read null with their reason on such a row.
A case on a multi-channel feed is not restaged, because the shared tables are
mono and moving it there would drop the dimension it exists to exercise.

Every restoration metric needs a reference signal. A corpus item with a clean
reference supplies one; an item without (the long dynamics beds carry no defect
and no clean twin) uses the processor's own input, recorded per row as
``reference``. The substitute changes what segmental SNR *means* on that row --
it becomes "how far the processing moved the signal" rather than "how much of
the clean signal survived" -- but it does not change what the level-independence
check asks, which is only that a fixed reference and a scaled output give the
same number.

Run from ``bindings/python`` so rye resolves numpy and libsonare, and point
``SONARE_LIB_PATH`` at the build being measured::

    SONARE_LIB_PATH=.../build-mr-w2-shared/lib/libsonare.dylib \\
        rye run python ../../tools/mastering-eval/nonvacuity.py
"""

from __future__ import annotations

import argparse
import hashlib
import inspect
import json
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import libsonare
import metrics_chain
import metrics_repair
from run import DOWNMIX, _as_stereo, load_item, mono_feeds, provenance

SCHEMA = 1

# The gain a level-only degradation applies. A power of two scales a float
# sample exactly, so anything that moves under it moved for its own reasons
# rather than through a rounding difference.
GAIN_ONLY = 0.5

# Which band the single-band cut removes, as an index into the 32 log-spaced
# centres metrics_chain lays out. 20 sits near 1.9 kHz at 48 kHz.
CUT_BAND_INDEX = 20
CUT_GAIN_DB = -24.0
CUT_Q = 4.0

PRESET = "pop"

# The two shared materials every degradation is restaged on.
#
# The speech bed is the one that settles the co-movement question, because it is
# the only item satisfying both domain limits at once: STOI may be read only on
# speech, short-term loudness spread exists only past the 3.1 s window, and this
# is the single corpus item that is both. Every column is live on it.
#
# The programme bed is kept beside it because it is broadband and transient
# where the speech bed is neither, so a per-band or a dynamics reading has
# somewhere to move that is not one talker.
COMMON_ITEM = "program_build"
COMMON_FEED = "downmix"
SPEECH_ITEM = "speech_dynamic_hum50"
SPEECH_FEED = "downmix"

# Where the gain-only case is repeated on a multi-channel feed. Short-term spread
# is a channel-summed reading, and a one-channel feed has nothing to sum, so the
# level-independence check cannot reach that arithmetic on a mono row.
#
# The programme bed rather than the corpus's stereo mix: what the sum needs is
# per-channel loudness envelopes that are not proportional, which decorrelated
# waveforms do not imply. Over the chain's output the summed series departs from a
# constant offset of the left channel's by 0.029 LU here against 0.0007 LU on
# stereo_mix_dry, whose channels carry one programme at a fixed width. It is also
# the material the mono gain-only case runs on, so the two differ in the feed's
# channel count alone.
STEREO_ITEM = COMMON_ITEM
STEREO_FEED = "stereo"

# The one-step table's extra materials. The noise bed is the only speech item
# that carries an ensemble, so it is the only place a one-step response can be
# read against a redraw floor of the same material; the click bed is where a
# detector threshold has anything to detect.
STEP_NOISE_ITEM = "speech_pink_noise"
STEP_CLICK_ITEM = "speech_click"

# The two reverberant materials the dereverb knobs are swept on. Both, because a
# knob is being asked whether it reaches the output at all, and one material
# cannot separate a knob that does nothing from a knob whose one material gave it
# nothing to do. The speech bed is where STOI is readable; the tonal bed is
# sustained, so its tail accumulates where the speech bed's decays into syllable
# gaps.
STEP_REVERB_ITEM = "speech_reverb_long"
STEP_REVERB_TONAL_ITEM = "chord_reverb_long"

NOT_SPEECH_BEARING = "STOI is defined on speech; this material carries none"
NOT_ONE_CHANNEL = "defined here on one channel; this feed carries {channels}"

# The metrics that take a mono signal, named so a multi-channel row can record
# their absence rather than leave a caller to infer it from four nulls.
ONE_CHANNEL_METRICS = ("seg_snr_db", "log_kurtosis_ratio", "stoi", "log_spectral_distance")

# The cases that repeat an earlier degradation on speech-bearing material. They
# belong to the home table only: restaging them on a shared material would run
# the same degradation twice under two names.
SPEECH_TWINS = frozenset({"D1s", "D2s", "D3s", "D7rs", "D8sa", "D8sb"})

SCALAR_METRICS = (
    "seg_snr_db",
    "log_kurtosis_ratio",
    "stoi",
    "log_spectral_distance",
    "integrated_lufs",
    "true_peak_dbtp",
    "short_term_spread",
)
VECTOR_METRICS = ("band_energy_delta",)


def _f32(x: np.ndarray) -> np.ndarray:
    return np.ascontiguousarray(x, dtype=np.float32)


def feeds(audio: np.ndarray) -> dict[str, np.ndarray]:
    """The runner's own mono feeds, plus the item's channels as one feed.

    The mono feeds and their downmix are the runner's, not a second copy: this
    module checks whether the harness's metrics can fail, so it has to be handed
    the samples the harness hands them. ``stereo`` is the same audio uncollapsed,
    which is what the runner's own stereo mastering row is measured on.
    """
    named = dict(mono_feeds(audio))
    stereo = _as_stereo(audio)
    if stereo.shape[1] > 1:
        named["stereo"] = stereo
    return named


def _channels(feed: np.ndarray) -> int:
    return 1 if feed.ndim == 1 else int(feed.shape[1])


def _rms_matched(reference: np.ndarray, other: np.ndarray) -> np.ndarray:
    """Scale `other` by one global factor so its RMS equals the reference's."""
    rms = float(np.sqrt(np.mean(other**2)))
    if rms <= 0.0:
        return other
    return other * (float(np.sqrt(np.mean(reference**2))) / rms)


# ------------------------------------------------------------------ processors


def _denoise(feed: np.ndarray, sample_rate: int, params: dict) -> np.ndarray:
    return np.asarray(
        libsonare.mastering_repair_denoise_classical(_f32(feed), sample_rate, **params),
        dtype=np.float64,
    )


def _declick(feed: np.ndarray, sample_rate: int, params: dict) -> np.ndarray:
    return np.asarray(
        libsonare.mastering_repair_declick(_f32(feed), sample_rate, **params),
        dtype=np.float64,
    )


def _dereverb(feed: np.ndarray, sample_rate: int, params: dict) -> np.ndarray:
    return np.asarray(
        libsonare.mastering_repair_dereverb_classical(_f32(feed), sample_rate, **params),
        dtype=np.float64,
    )


def _chain(feed: np.ndarray, sample_rate: int, overrides: dict | None) -> np.ndarray:
    result = libsonare.master_audio(_f32(feed), sample_rate, PRESET, overrides or None)
    return np.asarray(result.samples, dtype=np.float64)


def _chain_stereo(feed: np.ndarray, sample_rate: int) -> np.ndarray:
    result = libsonare.master_audio_stereo(
        _f32(feed[:, 0]), _f32(feed[:, 1]), sample_rate, PRESET
    )
    return np.stack(
        [np.asarray(result.left, dtype=np.float64), np.asarray(result.right, dtype=np.float64)],
        axis=1,
    )


def _band_cut_params(sample_rate: int) -> dict[str, float | bool]:
    centre = float(metrics_chain.band_center_frequencies(sample_rate)[CUT_BAND_INDEX])
    return {
        "band0.type": 0,  # Peak
        "band0.enabled": True,
        "band0.frequencyHz": centre,
        "band0.gainDb": CUT_GAIN_DB,
        "band0.q": CUT_Q,
    }


def _cut_band(x: np.ndarray, sample_rate: int) -> np.ndarray:
    result = libsonare.mastering_process(
        "eq.parametric", _f32(x), sample_rate, _band_cut_params(sample_rate)
    )
    return np.asarray(result.samples, dtype=np.float64)


# ------------------------------------------------------------------- the cases


@dataclass(frozen=True)
class Case:
    """One deliberate degradation: what it is, where it is run, what it should move."""

    key: str
    title: str
    family: str
    item: str
    expected: tuple[str, ...]
    baseline: Callable[[np.ndarray, int], np.ndarray]
    degraded: Callable[[np.ndarray, int], np.ndarray]
    settings: dict[str, Any] = field(default_factory=dict)
    feed: str = "downmix"


def _declick_defaults(defect: dict | None) -> dict:
    """The declicker aimed under the quietest planted click, as run.py aims it."""
    if defect is None:
        return {"threshold": 0.8, "max_click_samples": 8}
    quietest = min(abs(a) for a in defect["amplitudes"])
    return {
        "threshold": round(0.9 * quietest, 6),
        "max_click_samples": max(8, 4 * int(defect["width_samples"])),
    }


def build_cases(manifest: dict) -> list[Case]:
    by_id = {item["id"]: item for item in manifest["items"]}

    def click_params(item_id: str) -> dict:
        return _declick_defaults((by_id[item_id]["defects"] or {}).get("click"))

    spectral = {"mode": "spectralSubtraction"}

    def denoise_case(key, title, item, base, deg, expected) -> Case:
        return Case(
            key=key,
            title=title,
            family="restoration",
            item=item,
            expected=expected,
            baseline=lambda x, sr, p=base: _denoise(x, sr, p),
            degraded=lambda x, sr, p=deg: _denoise(x, sr, p),
            settings={"entry_point": "denoise_classical", "baseline": base, "degraded": deg},
        )

    def chain_case(key, title, item, deg_overrides, expected, tail=None) -> Case:
        def degraded(x: np.ndarray, sr: int) -> np.ndarray:
            out = _chain(x, sr, deg_overrides)
            return tail(out, sr) if tail is not None else out

        return Case(
            key=key,
            title=title,
            family="mastering",
            item=item,
            expected=expected,
            baseline=lambda x, sr: _chain(x, sr, None),
            degraded=degraded,
            settings={
                "entry_point": "master_audio",
                "preset": PRESET,
                "overrides": deg_overrides,
                "post": ("eq.parametric band cut" if tail is not None else None),
            },
        )

    cases: list[Case] = [
        denoise_case(
            "D1",
            "denoise reduction_db 26 -> 10.46",
            "sine_white_noise",
            {},
            {"reduction_db": 10.46},
            ("seg_snr_db",),
        ),
        denoise_case(
            "D2",
            "denoise over_subtraction 2.0 -> 6.0 (SpectralSubtraction)",
            "chord_pink_noise",
            dict(spectral, over_subtraction=2.0),
            dict(spectral, over_subtraction=6.0),
            ("log_kurtosis_ratio",),
        ),
        denoise_case(
            "D3",
            "denoise gain_smoothing on -> off",
            "chord_pink_noise",
            {},
            {"gain_smoothing": False},
            ("log_kurtosis_ratio", "log_spectral_distance"),
        ),
        chain_case(
            "D4",
            "chain over-compressed (ratio 20, threshold -40 dB)",
            COMMON_ITEM,
            {
                "dynamics": {
                    "compressor": {
                        "enabled": True,
                        "thresholdDb": -40.0,
                        "ratio": 20.0,
                        "attackMs": 1.0,
                        "releaseMs": 50.0,
                    }
                }
            },
            ("short_term_spread",),
        ),
        chain_case(
            "D5a",
            "chain ceiling raised to +6 dBTP, target unchanged",
            COMMON_ITEM,
            {"loudness": {"ceilingDb": 6.0}},
            ("true_peak_dbtp",),
        ),
        chain_case(
            "D5b",
            "chain ceiling raised to +6 dBTP and target to -6 LUFS",
            COMMON_ITEM,
            {"loudness": {"ceilingDb": 6.0, "targetLufs": -6.0}},
            ("true_peak_dbtp", "integrated_lufs"),
        ),
        chain_case(
            "D6",
            f"chain output band {CUT_BAND_INDEX} cut by {CUT_GAIN_DB:g} dB",
            COMMON_ITEM,
            None,
            ("band_energy_delta",),
            tail=_cut_band,
        ),
        Case(
            key="D7",
            title=f"chain output scaled by {GAIN_ONLY:g}, processing identical",
            family="mastering",
            item=COMMON_ITEM,
            expected=("integrated_lufs", "true_peak_dbtp"),
            baseline=lambda x, sr: _chain(x, sr, None),
            degraded=lambda x, sr: _chain(x, sr, None) * GAIN_ONLY,
            settings={"entry_point": "master_audio", "preset": PRESET, "gain": GAIN_ONLY},
        ),
        Case(
            key="D7st",
            title=f"stereo chain output scaled by {GAIN_ONLY:g}, processing identical",
            family="mastering",
            item=STEREO_ITEM,
            expected=("integrated_lufs", "true_peak_dbtp"),
            baseline=lambda x, sr: _chain_stereo(x, sr),
            degraded=lambda x, sr: _chain_stereo(x, sr) * GAIN_ONLY,
            settings={
                "entry_point": "master_audio_stereo",
                "preset": PRESET,
                "gain": GAIN_ONLY,
            },
            feed=STEREO_FEED,
        ),
        Case(
            key="D7r",
            title=f"denoise output scaled by {GAIN_ONLY:g}, processing identical",
            family="restoration",
            item="sine_white_noise",
            expected=("integrated_lufs", "true_peak_dbtp"),
            baseline=lambda x, sr: _denoise(x, sr, {}),
            degraded=lambda x, sr: _denoise(x, sr, {}) * GAIN_ONLY,
            settings={"entry_point": "denoise_classical", "gain": GAIN_ONLY},
        ),
        Case(
            key="D8a",
            title="declick gate raised to 1.0, so no click is detected",
            family="restoration",
            item="sine_click",
            expected=("seg_snr_db",),
            baseline=lambda x, sr: _declick(x, sr, click_params("sine_click")),
            degraded=lambda x, sr: _declick(x, sr, {"threshold": 1.0, "max_click_samples": 8}),
            settings={"entry_point": "declick", "baseline": click_params("sine_click")},
        ),
        Case(
            key="D8b",
            title="declick over-aggressive (gate 0.02, runs up to 256 samples)",
            family="restoration",
            item="sine_click",
            expected=("seg_snr_db",),
            baseline=lambda x, sr: _declick(x, sr, click_params("sine_click")),
            degraded=lambda x, sr: _declick(x, sr, {"threshold": 0.02, "max_click_samples": 256}),
            settings={"entry_point": "declick", "baseline": click_params("sine_click")},
        ),
        Case(
            key="D8c",
            title="declick gate raised to 1.0 on the 40-click bed",
            family="restoration",
            item="chord_click",
            expected=("seg_snr_db",),
            baseline=lambda x, sr: _declick(x, sr, click_params("chord_click")),
            degraded=lambda x, sr: _declick(x, sr, {"threshold": 1.0, "max_click_samples": 8}),
            settings={"entry_point": "declick", "baseline": click_params("chord_click")},
        ),
        # The speech twins. Each repeats the restoration degradation above on the
        # speech-bearing item of the same defect, which is the only place STOI's
        # response to that degradation can be read.
        denoise_case(
            "D1s",
            "denoise reduction_db 26 -> 10.46, speech bed",
            "speech_white_noise",
            {},
            {"reduction_db": 10.46},
            ("seg_snr_db",),
        ),
        denoise_case(
            "D2s",
            "denoise over_subtraction 2.0 -> 6.0 (SpectralSubtraction), speech bed",
            "speech_pink_noise",
            dict(spectral, over_subtraction=2.0),
            dict(spectral, over_subtraction=6.0),
            ("log_kurtosis_ratio",),
        ),
        denoise_case(
            "D3s",
            "denoise gain_smoothing on -> off, speech bed",
            "speech_pink_noise",
            {},
            {"gain_smoothing": False},
            ("log_kurtosis_ratio", "log_spectral_distance"),
        ),
        Case(
            key="D7rs",
            title=f"denoise output scaled by {GAIN_ONLY:g} on the speech bed, processing identical",
            family="restoration",
            item="speech_white_noise",
            expected=("integrated_lufs", "true_peak_dbtp"),
            baseline=lambda x, sr: _denoise(x, sr, {}),
            degraded=lambda x, sr: _denoise(x, sr, {}) * GAIN_ONLY,
            settings={"entry_point": "denoise_classical", "gain": GAIN_ONLY},
        ),
        Case(
            key="D8sa",
            title="declick gate raised to 1.0 on the speech bed",
            family="restoration",
            item="speech_click",
            expected=("seg_snr_db",),
            baseline=lambda x, sr: _declick(x, sr, click_params("speech_click")),
            degraded=lambda x, sr: _declick(x, sr, {"threshold": 1.0, "max_click_samples": 8}),
            settings={"entry_point": "declick", "baseline": click_params("speech_click")},
        ),
        Case(
            key="D8sb",
            title="declick over-aggressive on the speech bed",
            family="restoration",
            item="speech_click",
            expected=("seg_snr_db",),
            baseline=lambda x, sr: _declick(x, sr, click_params("speech_click")),
            degraded=lambda x, sr: _declick(x, sr, {"threshold": 0.02, "max_click_samples": 256}),
            settings={"entry_point": "declick", "baseline": click_params("speech_click")},
        ),
    ]
    return cases


def restaged(manifest: dict, item: str, feed: str) -> list[Case]:
    """Every degradation again, all on one shared material.

    A speech twin is the same degradation as its non-speech original, so only
    the original is restaged; keeping both would report one measurement twice
    under two names and inflate any agreement read off the table. A case on a
    multi-channel feed is left where it is for a different reason: the shared
    materials are taken as mono feeds here, so restaging it would drop the
    channel dimension it exists to exercise.
    """
    return [
        Case(
            key=case.key,
            title=case.title,
            family=case.family,
            item=item,
            expected=case.expected,
            baseline=case.baseline,
            degraded=case.degraded,
            settings=case.settings,
            feed=feed,
        )
        for case in build_cases(manifest)
        if case.key not in SPEECH_TWINS and case.feed == "downmix"
    ]


# ---------------------------------------------------------------- one step out


# What counts as one step, fixed here before anything was measured so that the
# answer cannot be chosen after seeing it.
#
# A deliberate degradation says how far a metric moves when a processor is
# broken. A threshold has to sit under the smallest change worth claiming, which
# is a different and much smaller quantity, and no fraction of the first
# estimates the second. So each knob is nudged by one step in both directions --
# both, because a knob's response need not be symmetric and a threshold is
# placed on the improving side.
#
# Two kinds of knob, because one rule does not fit both:
#
# * **Ratio-like** (gain floors, the Berouti alpha and beta, a quantile, a
#   detector threshold): one step is +/-20% of the value, multiplicatively. That
#   is scale-free, so it means the same thing at 0.05 and at 2.0, and it needs no
#   published range for the knob -- which the library does not give.
# * **Logarithmic** (anything in dB or LUFS): one step is +/-0.5 dB, absolutely.
#   A dB value is already a logarithm, so scaling it by 20% is not a scale-free
#   change of anything; 0.5 dB is the smallest increment a mastering decision is
#   normally made in.
#
# * **Ladder-like** (an FFT size, a hop, an iteration or a tap count): the
#   admissible values are rungs rather than a line, so one step is one rung --
#   the next power of two for a size, plus or minus one for a count. The relative
#   rule cannot be used on these: 20% of 1024 is not a power of two, and 20% of 2
#   iterations rounds back to 2 and would measure nothing while reporting a step.
#
# A boolean knob has no step. Its only change is a flip, which is what D3 already
# measured, so the booleans are flipped rather than given an invented step.
STEP_RELATIVE = 0.20
STEP_DECIBELS = 0.5

# Which rule each dereverb knob takes. Every knob the config carries is here,
# including the two the audit found unread, because a sweep that leaves a knob
# out cannot report on it either way -- and `output_identical` on these rows is
# what says whether a knob reaches the output at all, which no metric delta can
# say on its own.
DEREVERB_STEP_KIND = {
    "threshold": "relative",
    "attenuation": "relative",
    "n_fft": "ladder",
    "hop_length": "ladder",
    "t60_sec": "relative",
    "late_delay_ms": "relative",
    "over_subtraction": "relative",
    "spectral_floor": "relative",
    "wpe_enabled": "flip",
    "wpe_iterations": "count",
    "wpe_taps": "count",
    "wpe_strength": "relative",
}

# The four knobs the WPE stage owns are read only when it runs, exactly as the
# Berouti knobs are read only under spectral subtraction. Swept under a config
# that turns it on, or they would be measured where the code never reaches them.
WPE_ON = {"wpe_enabled": True}

# Two configurations the validator accepts and the subtraction stage is an
# identity map under: the stage takes `max(power - over_subtraction * late,
# spectral_floor * power)`, so a floor of 1 clamps the subtraction away and an
# over-subtraction of 0 subtracts nothing. Either way every bin's gain is 1.
#
# They matter to a sweep because `t60_sec` and `late_delay_ms` reach the output
# only through the late power that stage subtracts. Based on one of these, a
# sweep would report four inert knobs and could not tell the two that are never
# read from the two it had disabled itself. The base is checked against them
# rather than assumed clear of them.
DEREVERB_INERT_CONFIGS = ({"spectral_floor": 1.0}, {"over_subtraction": 0.0})

# The knobs an inert configuration disables, swept under one as a positive
# control. A column that reports inertness has to be shown reporting it where the
# cause is known, or an inert reading elsewhere cannot be told from a column that
# is stuck.
DEREVERB_DISABLED_BY_INERT = ("t60_sec", "late_delay_ms")

# The chain values the explicit baseline asserts are the preset's own. Asserted
# rather than assumed: PRESET-CHECK below runs the chain with these set and with
# nothing set, and its `output_identical` says whether they match.
PRESET_CEILING_DB = -1.0
PRESET_TARGET_LUFS = -14.0


def _ratio_steps(value: float) -> tuple[float, float]:
    return value * (1.0 - STEP_RELATIVE), value * (1.0 + STEP_RELATIVE)


def dereverb_defaults() -> dict[str, Any]:
    """The dereverberator's own defaults, read off the binding's signature.

    Read rather than copied: a sweep that steps from a hand-written default is a
    step from a value the processor may never have held, which is the question
    PRESET-CHECK asks on the chain side and which introspection answers outright
    here. The resolved values go into every row's settings.
    """
    parameters = inspect.signature(libsonare.mastering_repair_dereverb_classical).parameters
    return {
        name: parameter.default
        for name, parameter in parameters.items()
        if parameter.kind is inspect.Parameter.KEYWORD_ONLY
    }


def _dereverb_step(knob: str, default: Any) -> tuple[Any, Any, str]:
    """One step either side of a dereverb knob's default, by the knob's kind."""
    kind = DEREVERB_STEP_KIND[knob]
    if kind == "relative":
        low, high = _ratio_steps(float(default))
        return low, high, f"+/-{STEP_RELATIVE:.0%}"
    if kind == "ladder":
        return int(default) // 2, int(default) * 2, "one power of two"
    if kind == "count":
        return int(default) - 1, int(default) + 1, "one step"
    raise ValueError(f"{knob} takes no step: it is {kind}")


def build_steps(manifest: dict) -> list[Case]:
    """One knob moved one step, in each direction, with everything else held.

    Holding everything else is also this table's blind spot: one knob's setting can
    disable another, and a single-knob sweep sees the disabled knob as inert
    without saying which of the two caused it. The dereverb rows carry controls
    for the two cases known here; finding such a pair in general is not something
    this shape of sweep can do.
    """
    by_id = {item["id"]: item for item in manifest["items"]}
    spectral = {"mode": "spectralSubtraction"}

    def denoise_steps(knob: str, default: float, item: str, extra: dict) -> list[Case]:
        low, high = _ratio_steps(default)
        out = []
        for sign, value in (("-", low), ("+", high)):
            base = dict(extra, **{knob: default})
            stepped = dict(extra, **{knob: value})
            out.append(
                Case(
                    key=f"{knob}{sign}",
                    title=f"denoise {knob} {default:g} -> {value:g} ({sign}{STEP_RELATIVE:.0%})",
                    family="restoration",
                    item=item,
                    expected=(),
                    baseline=lambda x, sr, p=base: _denoise(x, sr, p),
                    degraded=lambda x, sr, p=stepped: _denoise(x, sr, p),
                    settings={"entry_point": "denoise_classical", "knob": knob, "step": "relative"},
                )
            )
        return out

    def chain_steps(field: str, default: float, item: str) -> list[Case]:
        out = []
        for sign, value in (("-", default - STEP_DECIBELS), ("+", default + STEP_DECIBELS)):
            base = {"loudness": {"ceilingDb": PRESET_CEILING_DB, "targetLufs": PRESET_TARGET_LUFS}}
            stepped = {"loudness": dict(base["loudness"], **{field: value})}
            out.append(
                Case(
                    key=f"{field}{sign}",
                    title=(
                        f"chain loudness.{field} {default:g} -> "
                        f"{value:g} ({sign}{STEP_DECIBELS} dB)"
                    ),
                    family="mastering",
                    item=item,
                    expected=(),
                    baseline=lambda x, sr, o=base: _chain(x, sr, o),
                    degraded=lambda x, sr, o=stepped: _chain(x, sr, o),
                    settings={"entry_point": "master_audio", "knob": field, "step": "decibels"},
                )
            )
        return out

    defaults = dereverb_defaults()

    def dereverb_case(knob: str, item: str, sign: str, base: dict, stepped: dict, how: str) -> Case:
        return Case(
            key=f"dereverb.{knob}{sign}",
            title=(
                f"dereverb {knob} {base[knob]!r} -> {stepped[knob]!r} ({how})"
                + (", WPE on" if base.get("wpe_enabled") else "")
            ),
            family="restoration",
            item=item,
            expected=(),
            baseline=lambda x, sr, p=base: _dereverb(x, sr, p),
            degraded=lambda x, sr, p=stepped: _dereverb(x, sr, p),
            settings={
                "entry_point": "dereverb_classical",
                "knob": knob,
                "step": DEREVERB_STEP_KIND[knob],
                "baseline": base,
                "degraded": stepped,
            },
        )

    def dereverb_steps(knob: str, item: str) -> list[Case]:
        extra = dict(WPE_ON) if knob.startswith("wpe_") and knob != "wpe_enabled" else {}
        default = defaults[knob]
        if DEREVERB_STEP_KIND[knob] == "flip":
            base = dict(extra, **{knob: default})
            flipped = dict(extra, **{knob: not default})
            return [dereverb_case(knob, item, "~", base, flipped, "flip")]
        low, high, how = _dereverb_step(knob, default)
        return [
            dereverb_case(
                knob, item, sign, dict(extra, **{knob: default}), dict(extra, **{knob: value}), how
            )
            for sign, value in (("-", low), ("+", high))
        ]

    def dereverb_control(key: str, title: str, item: str, base: dict, other: dict) -> Case:
        """One dereverb row whose answer is `output_identical`, not a metric delta."""
        return Case(
            key=f"dereverb.{key}",
            title=title,
            family="restoration",
            item=item,
            expected=(),
            baseline=lambda x, sr, p=base: _dereverb(x, sr, p),
            degraded=lambda x, sr, p=other: _dereverb(x, sr, p),
            settings={
                "entry_point": "dereverb_classical",
                "knob": None,
                "step": "control",
                "baseline": base,
                "degraded": other,
            },
        )

    def dereverb_controls(item: str) -> list[Case]:
        """Whether the sweep's base subtracts anything, and whether inert is reachable.

        Three questions, all answered by `output_identical` rather than by a delta:

        * ``BASE-VS-INERT`` -- the defaults against a configuration whose
          subtraction stage is an identity map. Identical outputs would mean the
          base is inert too, and every dereverb row above it vacuous.
        * ``INERT-AGREE`` -- the two inert configurations against each other. They
          are different knobs reaching the same identity map, so identical is the
          expected answer, and this is where the column is seen saying True for a
          fully understood reason.
        * ``inert[...]`` -- the two knobs an inert configuration disables, stepped
          under it. Inert here and live in the rows above is what says the column
          reports inertness rather than being stuck on one answer.
        """
        out = [
            dereverb_control(
                f"BASE-VS-INERT[{knob}={value:g}]",
                f"dereverb defaults against {knob}={value:g}, which makes the subtraction "
                f"stage an identity map",
                item,
                {},
                dict(inert),
            )
            for inert in DEREVERB_INERT_CONFIGS
            for knob, value in inert.items()
        ]
        first, second = DEREVERB_INERT_CONFIGS
        out.append(
            dereverb_control(
                "INERT-AGREE",
                "dereverb's two identity-map configurations against each other",
                item,
                dict(first),
                dict(second),
            )
        )
        for inert in DEREVERB_INERT_CONFIGS:
            held = next(iter(inert))
            for knob in DEREVERB_DISABLED_BY_INERT:
                low, high, how = _dereverb_step(knob, defaults[knob])
                for sign, value in (("-", low), ("+", high)):
                    out.append(
                        dereverb_control(
                            f"inert[{held}].{knob}{sign}",
                            f"dereverb {knob} {defaults[knob]:g} -> {value:g} ({how}) under "
                            f"{held}={inert[held]:g}",
                            item,
                            dict(inert, **{knob: defaults[knob]}),
                            dict(inert, **{knob: value}),
                        )
                    )
        return out

    cases: list[Case] = []
    for item in (STEP_NOISE_ITEM, SPEECH_ITEM):
        cases += denoise_steps("reduction_db", 26.0, item, {})
        cases += denoise_steps("over_subtraction", 2.0, item, spectral)
        cases += denoise_steps("spectral_floor", 0.05, item, spectral)
        cases += denoise_steps("noise_estimation_quantile", 0.1, item, {})
    for item in (STEP_REVERB_ITEM, STEP_REVERB_TONAL_ITEM):
        for knob in DEREVERB_STEP_KIND:
            cases += dereverb_steps(knob, item)
        cases += dereverb_controls(item)
    for item in (SPEECH_ITEM, COMMON_ITEM):
        cases += chain_steps("ceilingDb", PRESET_CEILING_DB, item)
        cases += chain_steps("targetLufs", PRESET_TARGET_LUFS, item)

    click = _declick_defaults((by_id[STEP_CLICK_ITEM]["defects"] or {}).get("click"))
    low, high = _ratio_steps(click["threshold"])
    for sign, value in (("-", low), ("+", high)):
        stepped = dict(click, threshold=round(value, 6))
        cases.append(
            Case(
                key=f"declick.threshold{sign}",
                title=(
                    f"declick threshold {click['threshold']:g} -> "
                    f"{stepped['threshold']:g} ({sign}{STEP_RELATIVE:.0%})"
                ),
                family="restoration",
                item=STEP_CLICK_ITEM,
                expected=(),
                baseline=lambda x, sr, p=click: _declick(x, sr, p),
                degraded=lambda x, sr, p=stepped: _declick(x, sr, p),
                settings={"entry_point": "declick", "knob": "threshold", "step": "relative"},
            )
        )

    # Does the explicit chain baseline reproduce the preset, or is "one step from
    # the default" a step from a value the preset never held? Read
    # `output_identical` on this row for the answer.
    cases.append(
        Case(
            key="PRESET-CHECK",
            title="chain with the asserted preset values set explicitly, against no overrides",
            family="mastering",
            item=SPEECH_ITEM,
            expected=(),
            baseline=lambda x, sr: _chain(x, sr, None),
            degraded=lambda x, sr: _chain(
                x,
                sr,
                {"loudness": {"ceilingDb": PRESET_CEILING_DB, "targetLufs": PRESET_TARGET_LUFS}},
            ),
            settings={"entry_point": "master_audio", "knob": None, "step": None},
        )
    )
    return cases


# -------------------------------------------------------------------- measuring


def evaluate(
    reference: np.ndarray,
    source: np.ndarray,
    output: np.ndarray,
    sample_rate: int,
    *,
    speech_bearing: bool,
) -> dict:
    """All eight metrics for one processed signal.

    STOI is computed only on speech-bearing material. Outside speech it is not a
    conservative reading but a wrong one -- the contract records that it has
    already called a correct restoration a regression on a tonal item -- so a
    non-speech row carries null and the reason rather than a number that would
    then have to be argued away.

    The four restoration metrics are defined here on one channel and abstain the
    same way on a multi-channel feed. Averaging them over channels would be a
    definition this harness invented, and the C++ twin they are pinned against
    takes one channel too. The four chain metrics take the feed whole.
    """
    n = min(reference.shape[0], source.shape[0], output.shape[0])
    ref, src, out = reference[:n], source[:n], output[:n]

    one_channel = _channels(out) == 1
    report = metrics_repair.segmental_snr_report(ref, out, sample_rate) if one_channel else None
    values: dict[str, Any] = {
        "seg_snr_db": float(report.value) if report is not None else None,
        "log_kurtosis_ratio": (
            float(metrics_repair.log_kurtosis_ratio(src, out, sample_rate))
            if one_channel
            else None
        ),
        "stoi": (
            _maybe(
                lambda: float(
                    metrics_repair.short_time_objective_intelligibility(ref, out, sample_rate)
                )
            )
            if speech_bearing and one_channel
            else None
        ),
        "log_spectral_distance": (
            float(metrics_repair.log_spectral_distance(ref, out, sample_rate))
            if one_channel
            else None
        ),
        "integrated_lufs": float(metrics_chain.integrated_loudness(out, sample_rate)),
        "true_peak_dbtp": float(metrics_chain.true_peak_dbtp(out, sample_rate)),
        "short_term_spread": float(metrics_chain.short_term_spread(out, sample_rate)),
        "band_energy_delta": [
            float(v) for v in metrics_chain.band_energy_delta(src, out, sample_rate)
        ],
    }
    if report is None:
        values["seg_snr_saturation"] = None
        return values
    values["seg_snr_saturation"] = {
        "active_frames": int(report.active_frames),
        "ceiling_frames": int(report.ceiling_frames),
        "floor_frames": int(report.floor_frames),
        "ceiling_fraction": round(float(report.ceiling_fraction), 6),
        "floor_fraction": round(float(report.floor_fraction), 6),
        "saturated": bool(report.saturated),
    }
    return values


def _maybe(call: Callable[[], float]) -> float | None:
    """A metric whose implementation is not there yet reads null, never a number."""
    try:
        return call()
    except NotImplementedError:
        return None


def _inapplicable(speech_bearing: bool, channels: int) -> dict[str, str]:
    """Which metrics this row could not read, each with the reason it could not."""
    absent: dict[str, str] = {}
    if not speech_bearing:
        absent["stoi"] = NOT_SPEECH_BEARING
    if channels > 1:
        for metric in ONE_CHANNEL_METRICS:
            absent[metric] = NOT_ONE_CHANNEL.format(channels=channels)
    return absent


def _delta(before: Any, after: Any) -> Any:
    if before is None or after is None:
        return None
    if isinstance(before, list):
        return [b - a for a, b in zip(before, after, strict=True)]
    if not (np.isfinite(before) and np.isfinite(after)):
        return None
    return after - before


def measure(case: Case, manifest: dict, root: Path, draw: int = 0) -> dict:
    # The runner's own loader and the runner's own downmix, not a second copy of
    # either: this module checks whether the harness's metrics can fail, so it
    # has to be handed the same samples the harness hands them. A private reader
    # here would leave the check measuring a signal the harness never sees.
    item = next(i for i in manifest["items"] if i["id"] == case.item)
    draws, clean, sample_rate = load_item(root, item)
    audio = draws[draw]
    feed = feeds(audio)[case.feed]
    reference = feeds(clean)[case.feed] if clean is not None else feed
    reference_kind = "clean_reference" if clean is not None else "processor_input"
    speech = bool(item.get("speech_bearing", False))

    started = time.perf_counter()
    base_out = case.baseline(feed, sample_rate)
    deg_out = case.degraded(feed, sample_rate)
    processed_seconds = time.perf_counter() - started

    before = evaluate(reference, feed, base_out, sample_rate, speech_bearing=speech)
    after = evaluate(reference, feed, deg_out, sample_rate, speech_bearing=speech)
    deltas = {m: _delta(before[m], after[m]) for m in SCALAR_METRICS + VECTOR_METRICS}

    # The same degradation with its level change taken back out. Two of the
    # metrics are level-sensitive by construction, so a degradation that also
    # moves the output level leaves their response inseparable from it; this
    # column says which part of the movement was the defect.
    matched_out = _rms_matched(base_out, deg_out)
    matched = evaluate(reference, feed, matched_out, sample_rate, speech_bearing=speech)
    matched_deltas = {m: _delta(before[m], matched[m]) for m in SCALAR_METRICS + VECTOR_METRICS}

    return {
        "case": case.key,
        "title": case.title,
        "family": case.family,
        "item": case.item,
        "feed": case.feed,
        "channels": _channels(feed),
        "sample_rate": sample_rate,
        "reference": reference_kind,
        "draw": draw,
        "draw_count": len(draws),
        "downmix": DOWNMIX if case.feed == "downmix" else None,
        "speech_bearing": speech,
        "inapplicable_metrics": _inapplicable(speech, _channels(feed)),
        "expected": list(case.expected),
        "settings": case.settings,
        "seconds": round(processed_seconds, 3),
        "output_identical": bool(
            base_out.shape == deg_out.shape and np.array_equal(base_out, deg_out)
        ),
        "before": before,
        "after": after,
        "delta": deltas,
        "after_level_matched": matched,
        "delta_level_matched": matched_deltas,
    }


# ------------------------------------------------------------------- the floor


def ensemble(case: Case, manifest: dict, root: Path) -> dict | None:
    """How far a metric moves between draws that differ only in their noise seed.

    This is the floor a threshold has to clear and the one number the degradation
    tables cannot supply. Those say how much a metric moves when the processing
    is deliberately broken; this says how much it moves when *nothing* changed
    except which realization of the same statistical material was fed in. An
    improvement smaller than this is indistinguishable from having redrawn the
    corpus, however deterministic the arithmetic is on one draw.

    Only the baseline processing is run, on every draw the item carries.
    """
    item = next(i for i in manifest["items"] if i["id"] == case.item)
    draws, clean, sample_rate = load_item(root, item)
    if len(draws) < 2:
        return None
    speech = bool(item.get("speech_bearing", False))

    per_draw = []
    for audio in draws:
        feed = feeds(audio)[case.feed]
        reference = feeds(clean)[case.feed] if clean is not None else feed
        out = case.baseline(feed, sample_rate)
        per_draw.append(evaluate(reference, feed, out, sample_rate, speech_bearing=speech))

    spread: dict[str, Any] = {}
    for metric in SCALAR_METRICS:
        values = [d[metric] for d in per_draw]
        if any(v is None or not np.isfinite(v) for v in values):
            spread[metric] = None
            continue
        spread[metric] = {
            "min": min(values),
            "max": max(values),
            "range": max(values) - min(values),
            "std": float(np.std(values)),
        }
    bands = np.array([d["band_energy_delta"] for d in per_draw])
    spread["band_energy_delta"] = {
        "max_range_over_bands": float(np.max(bands.max(axis=0) - bands.min(axis=0))),
        "max_std_over_bands": float(np.max(np.std(bands, axis=0))),
    }
    return {
        "case": case.key,
        "item": case.item,
        "feed": case.feed,
        "channels": _channels(feed),
        "draws": len(draws),
        "speech_bearing": speech,
        "settings": case.settings,
        "spread": spread,
        "per_draw": per_draw,
    }


# --------------------------------------------------------------------- printing


def _cell(metric: str, delta: Any) -> str:
    if delta is None:
        return "n/a"
    if metric == "band_energy_delta":
        worst = int(np.argmax(np.abs(delta)))
        return f"{delta[worst]:+.4f}@{worst}"
    return f"{delta:+.6g}"


def print_table(rows: list[dict], title: str, delta_key: str = "delta") -> None:
    print(f"\n## {title}")
    header = ["case", *(m[:14] for m in SCALAR_METRICS), "band_delta"]
    print(" | ".join(header))
    for row in rows:
        cells = [_cell(m, row[delta_key][m]) for m in SCALAR_METRICS + VECTOR_METRICS]
        print(" | ".join([row["case"], *cells]))


CORPUS_TREE_RECIPE = "find . -type f | sort | xargs shasum -a 256 | shasum -a 256"


def corpus_tree_digest(root: Path) -> str | None:
    """Digest of the whole corpus directory under the recipe the freeze publishes.

    Runs :data:`CORPUS_TREE_RECIPE` rather than reproducing it, because the
    number is only useful if it equals the published one, and a reimplementation
    that merely ought to agree is the thing being guarded against -- a first
    attempt here differed from the shell's answer on the same bytes, which as a
    hand-rolled "equivalent" would have read as a broken freeze.
    """
    result = subprocess.run(
        ["sh", "-c", CORPUS_TREE_RECIPE],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        return None
    return result.stdout.split()[0]


def corpus_paths(manifest_path: Path) -> list[Path]:
    """Every audio file the manifest names, in manifest order and deduplicated.

    An ensemble item names its draws under ``realizations[*].audio`` and repeats
    the first of them as ``audio``, so reading only ``audio`` and
    ``clean_reference`` leaves every draw past the first uncovered -- sixteen
    files on this corpus, and exactly the ones an ensemble measurement reads.
    """
    manifest = json.loads(manifest_path.read_text())
    root = manifest_path.parent
    seen: dict[Path, None] = {}
    for item in manifest["items"]:
        names = [item.get("audio"), item.get("clean_reference")]
        names += [draw.get("audio") for draw in item.get("realizations", [])]
        for name in names:
            if name:
                seen.setdefault(root / name, None)
    return list(seen)


def corpus_digest(manifest_path: Path) -> str:
    """One digest over the manifest and every audio file it names.

    The generator can rewrite the corpus while a run is in flight, and a run
    whose early rows read one corpus and whose late rows read another is not a
    cross table of anything. Taking this before and after says which of the two
    happened instead of leaving it to mtimes.

    This covers only what the manifest names, where :func:`corpus_tree_digest`
    covers the directory; a file the manifest stopped naming moves one and not
    the other, which is worth being able to tell apart.

    A named file that is missing raises. Folding it in as empty bytes would give
    a deleted file and an empty one the same digest, and a digest whose job is to
    say whether the contents are the same must not absorb an absence quietly.
    """
    digest = hashlib.sha256(manifest_path.read_bytes())
    for path in corpus_paths(manifest_path):
        digest.update(path.read_bytes())
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--manifest",
        type=Path,
        default=HERE / "audio" / "manifest.json",
        help="the corpus manifest corpus.py wrote",
    )
    parser.add_argument("--out", type=Path, default=HERE / "runs" / "nonvacuity.json")
    parser.add_argument(
        "--table",
        choices=["home", "common", "speech", "steps", "all"],
        default="all",
        help="which table to run",
    )
    parser.add_argument("--case", action="append", default=[], help="restrict to these case keys")
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text())
    root = args.manifest.parent
    digest_before = corpus_digest(args.manifest)
    tree_before = corpus_tree_digest(root)

    tables: dict[str, list[dict]] = {}
    for name, cases in (
        ("home", build_cases(manifest)),
        ("common", restaged(manifest, COMMON_ITEM, COMMON_FEED)),
        ("speech", restaged(manifest, SPEECH_ITEM, SPEECH_FEED)),
        ("steps", build_steps(manifest)),
    ):
        if args.table not in (name, "all"):
            continue
        rows = []
        for case in cases:
            if args.case and case.key not in args.case:
                continue
            print(f"{name}/{case.key:<5} {case.item:<20}", flush=True)
            rows.append(measure(case, manifest, root))
        tables[name] = rows
        print_table(rows, name)
        print_table(
            rows, f"{name} (degraded output level-matched to baseline)", "delta_level_matched"
        )

    # The redraw floor, from the baseline processing alone on every draw an item
    # carries. Restricted to the cases whose material is an ensemble.
    ensembles = []
    if args.table == "all":
        for case in build_cases(manifest):
            if args.case and case.key not in args.case:
                continue
            spread = ensemble(case, manifest, root)
            if spread is not None:
                print(f"ensemble/{case.key:<5} {case.item:<20} {spread['draws']} draws", flush=True)
                ensembles.append(spread)

    payload = {
        "schema": SCHEMA,
        "contract": "tools/mastering-eval/docs/objective.md",
        "corpus_manifest": str(args.manifest),
        "corpus_seed": manifest.get("seed"),
        "corpus_digest": digest_before,
        "corpus_digest_after": (digest_after := corpus_digest(args.manifest)),
        "corpus_stable_during_run": digest_before == digest_after,
        "corpus_digest_paths": len(corpus_paths(args.manifest)),
        "corpus_tree_digest": tree_before,
        "corpus_tree_digest_after": (tree_after := corpus_tree_digest(root)),
        "corpus_tree_stable_during_run": tree_before == tree_after,
        "gain_only": GAIN_ONLY,
        "band_cut": {"index": CUT_BAND_INDEX, "gain_db": CUT_GAIN_DB, "q": CUT_Q},
        "provenance": provenance(),
        "tables": tables,
        "ensembles": ensembles,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(payload, indent=2) + "\n")
    print(f"\n-> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
