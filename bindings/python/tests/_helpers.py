"""Shared helpers for libsonare Python binding tests."""

from __future__ import annotations

import dataclasses
import json
from collections.abc import Iterator
from contextlib import contextmanager
from typing import Any

import numpy as np
import pytest


def sine(freq: float, duration_sec: float, sr: int = 22050, amp: float = 0.5) -> np.ndarray:
    """Return a mono float32 sine tone.

    Shared by the offline-DSP test suites so the tone formula lives in one
    place. The samples are ``amp * sin(2*pi*freq*n/sr)`` for ``n`` in
    ``range(int(sr * duration_sec))``, computed in float64 and cast to float32.
    """
    n = int(sr * duration_sec)
    return (amp * np.sin(2.0 * np.pi * freq * np.arange(n) / sr)).astype(np.float32)


def scene_strip(scene_json: str) -> dict[str, Any]:
    """The first strip of a mixer scene given as JSON text, for ``RealtimeEngine.set_strip``."""
    strip: dict[str, Any] = json.loads(scene_json)["strips"][0]
    return strip


def scene_bus(scene_json: str) -> dict[str, Any]:
    """The first bus of a mixer scene given as JSON text, for ``RealtimeEngine.set_bus_strip``."""
    bus: dict[str, Any] = json.loads(scene_json)["buses"][0]
    return bus


def same_value(a: Any, b: Any) -> bool:
    """Structural equality for result values: dataclasses field by field, arrays by content.

    Result dataclasses holding ndarrays are ``eq=False``, so ``==`` is identity;
    this compares what two results actually carry (NaN equal to NaN).
    """
    if isinstance(a, np.ndarray) or isinstance(b, np.ndarray):
        return (
            isinstance(a, np.ndarray)
            and isinstance(b, np.ndarray)
            and a.dtype == b.dtype
            and np.array_equal(a, b, equal_nan=a.dtype.kind == "f")
        )
    if dataclasses.is_dataclass(a) and not isinstance(a, type):
        return type(a) is type(b) and all(
            same_value(getattr(a, f.name), getattr(b, f.name)) for f in dataclasses.fields(a)
        )
    if isinstance(a, list | tuple):
        return (
            type(a) is type(b)
            and len(a) == len(b)
            and all(same_value(x, y) for x, y in zip(a, b, strict=True))
        )
    return bool(a == b)


def is_lib_available() -> bool:
    """Returns True if the libsonare shared library is loadable.

    This is the ONLY skip mechanism, not a final guard behind another one. The
    conftest neither skips nor filters collection: it only points
    ``SONARE_LIB_PATH`` at a development build when the checkout has one, and
    leaves an installed wheel to find its own bundled library. Every module
    that touches the C layer therefore has to declare its own::

        pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason=...)

    A module that omits it fails at collection with an import error rather than
    skipping, because nothing upstream is watching for it.
    """
    try:
        from libsonare._runtime import _get_lib

        _get_lib()
        return True
    except Exception:
        return False


LIB_AVAILABLE: bool = is_lib_available()


def _midi1_word(status: int, channel: int, word0: int, word1: int) -> int:
    return (0x2 << 28) | ((status & 0xF) << 20) | ((channel & 0xF) << 16) | (word0 << 8) | word1


def patch_mono_decode(monkeypatch, loader) -> None:
    """Route the CLI's mono decode through ``loader(path) -> (samples, rate)``.

    The one seam under both the downmixing and the channel-preserving loaders:
    a mono or downmixed source reaches ``_decode_mono``, so a test
    replaces the decode itself rather than a loader the production path
    selects between.
    """
    from libsonare import _cli_common

    monkeypatch.setattr(
        _cli_common,
        "_decode_mono",
        loader,
    )


@contextmanager
def raises_coded(code: int | None = None, match: str | None = None) -> Iterator[None]:
    """Assert the block raises a ``SonareError`` of exactly the class and code the cause maps to.

    ``code`` defaults to ``INVALID_PARAMETER``, which is always a ``SonareValueError``
    (a refused argument); every other code must be a plain coded ``SonareError``, so
    neither a generic ``RuntimeError`` nor the wrong class passes.
    """
    from libsonare import ErrorCode, SonareError, SonareValueError

    expected = int(ErrorCode.INVALID_PARAMETER if code is None else code)
    with pytest.raises(SonareError, match=match) as excinfo:
        yield
    error = excinfo.value
    assert error.code == expected, f"expected code {expected}, got {error.code}: {error}"
    if expected == int(ErrorCode.INVALID_PARAMETER):
        assert isinstance(error, SonareValueError), type(error)
    else:
        assert not isinstance(error, SonareValueError), type(error)


# The keys a mastering chain reader accepts, in the order its unknown-key refusal lists them.
_CHAIN_ACCEPTED_KEYS = (
    "dynamics.compressor.attackMs",
    "dynamics.compressor.autoMakeup",
    "dynamics.compressor.detector",
    "dynamics.compressor.enabled",
    "dynamics.compressor.kneeDb",
    "dynamics.compressor.makeupGainDb",
    "dynamics.compressor.pdrReleaseScale",
    "dynamics.compressor.pdrTimeMs",
    "dynamics.compressor.ratio",
    "dynamics.compressor.releaseMs",
    "dynamics.compressor.sidechainHighPassHz",
    "dynamics.compressor.thresholdDb",
    "dynamics.deesser.attackMs",
    "dynamics.deesser.bandPassQ",
    "dynamics.deesser.enabled",
    "dynamics.deesser.frequencyHz",
    "dynamics.deesser.rangeDb",
    "dynamics.deesser.ratio",
    "dynamics.deesser.releaseMs",
    "dynamics.deesser.thresholdDb",
    "dynamics.multibandComp.bands.{i}.attackMs",
    "dynamics.multibandComp.bands.{i}.autoMakeup",
    "dynamics.multibandComp.bands.{i}.detector",
    "dynamics.multibandComp.bands.{i}.kneeDb",
    "dynamics.multibandComp.bands.{i}.makeupGainDb",
    "dynamics.multibandComp.bands.{i}.pdrReleaseScale",
    "dynamics.multibandComp.bands.{i}.pdrTimeMs",
    "dynamics.multibandComp.bands.{i}.ratio",
    "dynamics.multibandComp.bands.{i}.releaseMs",
    "dynamics.multibandComp.bands.{i}.sidechainHighPassHz",
    "dynamics.multibandComp.bands.{i}.thresholdDb",
    "dynamics.multibandComp.crossover.cutoffsHz.{i}",
    "dynamics.multibandComp.crossover.firKernelSize",
    "dynamics.multibandComp.crossover.mode",
    "dynamics.multibandComp.crossover.slope",
    "dynamics.multibandComp.cutoff{i}Hz",
    "dynamics.multibandComp.enabled",
    "dynamics.transientShaper.attackGainDb",
    "dynamics.transientShaper.enabled",
    "dynamics.transientShaper.fastAttackMs",
    "dynamics.transientShaper.fastReleaseMs",
    "dynamics.transientShaper.gainSmoothingMs",
    "dynamics.transientShaper.lookaheadMs",
    "dynamics.transientShaper.maxGainDb",
    "dynamics.transientShaper.sensitivity",
    "dynamics.transientShaper.slowAttackMs",
    "dynamics.transientShaper.slowReleaseMs",
    "dynamics.transientShaper.sustainGainDb",
    "eq.tilt.enabled",
    "eq.tilt.pivotHz",
    "eq.tilt.tiltDb",
    "loudness.applyGainAtInputRate",
    "loudness.ceilingDbtp",
    "loudness.enabled",
    "loudness.maxLimiterGainReductionDb",
    "loudness.releaseMs",
    "loudness.targetLufs",
    "loudness.truePeakOversample",
    "maximizer.truePeakLimiter.applyGainAtInputRate",
    "maximizer.truePeakLimiter.ceilingDbtp",
    "maximizer.truePeakLimiter.enabled",
    "maximizer.truePeakLimiter.lookaheadMs",
    "maximizer.truePeakLimiter.oversampleFactor",
    "maximizer.truePeakLimiter.releaseMs",
    "repair.declick.enabled",
    "repair.declick.lpcOrder",
    "repair.declick.maxClickSamples",
    "repair.declick.neighborRatio",
    "repair.declick.residualRatio",
    "repair.declick.threshold",
    "repair.declip.clipThresholdDb",
    "repair.declip.enabled",
    "repair.declip.iterations",
    "repair.declip.linkChannels",
    "repair.declip.lpcOrder",
    "repair.decrackle.enabled",
    "repair.decrackle.levels",
    "repair.decrackle.mode",
    "repair.decrackle.threshold",
    "repair.dehum.adaptation",
    "repair.dehum.adaptive",
    "repair.dehum.enabled",
    "repair.dehum.frameSize",
    "repair.dehum.fundamentalHz",
    "repair.dehum.harmonics",
    "repair.dehum.mode",
    "repair.dehum.pllBandwidth",
    "repair.dehum.q",
    "repair.dehum.searchRangeHz",
    "repair.denoise.ddAlpha",
    "repair.denoise.enabled",
    "repair.denoise.gainSmoothing",
    "repair.denoise.hopLength",
    "repair.denoise.mode",
    "repair.denoise.nFft",
    "repair.denoise.noiseEstimationQuantile",
    "repair.denoise.noiseEstimator",
    "repair.denoise.overSubtraction",
    "repair.denoise.reductionDb",
    "repair.denoise.spectralFloor",
    "repair.denoise.speechPresenceGain",
    "repair.dereverb.attenuation",
    "repair.dereverb.enabled",
    "repair.dereverb.hopLength",
    "repair.dereverb.lateDelayMs",
    "repair.dereverb.nFft",
    "repair.dereverb.overSubtraction",
    "repair.dereverb.spectralFloor",
    "repair.dereverb.t60Sec",
    "repair.dereverb.threshold",
    "repair.dereverb.wpeEnabled",
    "repair.dereverb.wpeIterations",
    "repair.dereverb.wpeStrength",
    "repair.dereverb.wpeTaps",
    "saturation.exciter.aliasing",
    "saturation.exciter.amount",
    "saturation.exciter.driveDb",
    "saturation.exciter.enabled",
    "saturation.exciter.evenOddMix",
    "saturation.exciter.frequencyHz",
    "saturation.exciter.q",
    "saturation.tape.bias",
    "saturation.tape.driveDb",
    "saturation.tape.enabled",
    "saturation.tape.gapLoss",
    "saturation.tape.headBumpDb",
    "saturation.tape.hysteresis",
    "saturation.tape.outputGainDb",
    "saturation.tape.oversampleFactor",
    "saturation.tape.saturation",
    "saturation.tape.speedIps",
    "spectral.airBand.amount",
    "spectral.airBand.dynamicRangeDb",
    "spectral.airBand.dynamicThresholdDb",
    "spectral.airBand.enabled",
    "spectral.airBand.shelfFrequencyHz",
    "stereo.imager.decorrelationAmount",
    "stereo.imager.enabled",
    "stereo.imager.outputGainDb",
    "stereo.imager.preserveEnergy",
    "stereo.imager.width",
    "stereo.monoMaker.amount",
    "stereo.monoMaker.enabled",
    "stereo.monoMaker.frequencyHz",
)


def chain_unknown_key_text(head: str) -> str:
    """The whole unknown-key refusal of a mastering chain reader whose message starts with head."""
    return f"{head} (expected {', '.join(_CHAIN_ACCEPTED_KEYS)})"
