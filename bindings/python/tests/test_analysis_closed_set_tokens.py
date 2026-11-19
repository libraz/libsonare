"""Closed-set string parameters of the analysis functions take their tokens only.

A refused value is a ``TypeError`` when it is not a string at all and a
``SonareValueError`` naming the accepted tokens otherwise, including spellings
an earlier release folded (case, abbreviations) and names the core would have
read as its default without complaint.
"""

from __future__ import annotations

import math
from collections.abc import Callable
from typing import Any, get_args

import numpy as np
import pytest

import libsonare
from libsonare import SonareValueError
from libsonare.types import ChordChromaMethod, GenreHint, NormType, TempogramMode

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050


def _chord_tone(seconds: float = 1.0) -> np.ndarray:
    t = np.arange(int(SR * seconds)) / SR
    tone = sum(np.sin(2 * math.pi * f * t) for f in (261.63, 329.63, 392.0))
    return (0.25 * tone).astype(np.float32)


def _features() -> np.ndarray:
    rng = np.random.default_rng(7)
    return rng.standard_normal((6, 24)).astype(np.float32)


def test_the_exported_token_types_spell_the_javascript_unions() -> None:
    assert get_args(NormType) == ("max", "l1", "l2", "power")
    assert get_args(TempogramMode) == ("autocorrelation", "cosine")
    assert get_args(ChordChromaMethod) == ("stft", "nnls", "cqt")
    assert get_args(GenreHint) == (
        "auto",
        "edm",
        "electronic",
        "dance",
        "pop",
        "classical",
        "jazz",
    )


@pytest.mark.parametrize(
    ("norm", "expected"),
    [("max", [0.75, 1.0]), ("l1", [3.0 / 7.0, 4.0 / 7.0]), ("l2", [0.6, 0.8])],
)
def test_vector_normalize_applies_the_named_norm(norm: NormType, expected: list[float]) -> None:
    np.testing.assert_allclose(
        libsonare.vector_normalize([3.0, 4.0], norm=norm), expected, rtol=1e-6
    )


def test_vector_normalize_defaults_to_the_maximum_magnitude() -> None:
    np.testing.assert_allclose(libsonare.vector_normalize([3.0, 4.0]), [0.75, 1.0], rtol=1e-6)


def test_tempogram_modes_compute_different_results() -> None:
    onset = np.abs(np.sin(np.linspace(0.0, 40.0, 200))).astype(np.float32)
    auto = libsonare.tempogram(onset, sr=SR, win_length=16, mode="autocorrelation")
    cosine = libsonare.tempogram(onset, sr=SR, win_length=16, mode="cosine")
    assert auto.shape == cosine.shape
    assert not np.allclose(auto, cosine)


@pytest.mark.parametrize("method", get_args(ChordChromaMethod))
def test_every_chroma_method_token_runs_chord_detection(method: ChordChromaMethod) -> None:
    result = libsonare.detect_chords(_chord_tone(), SR, chroma_method=method)
    assert isinstance(result.chords, list)


@pytest.mark.parametrize("hint", get_args(GenreHint))
def test_every_genre_hint_token_runs_key_detection(hint: GenreHint) -> None:
    key = libsonare.detect_key(_chord_tone(), SR, genre_hint=hint)
    assert key.mode in ("major", "minor")


def test_segment_functions_take_their_tokens() -> None:
    data = _features()
    for metric in ("cosine", "euclidean"):
        for mode in ("connectivity", "affinity"):
            matrix = libsonare.segment_recurrence_matrix(data, k=3, metric=metric, mode=mode)
            assert matrix.shape == (24, 24)
            cross = libsonare.segment_cross_similarity(data, data, k=3, metric=metric, mode=mode)
            assert cross.shape == (24, 24)
    for linkage in ("average", "single", "complete", "ward"):
        labels = libsonare.segment_agglomerative(data, 3, linkage=linkage)
        assert labels.shape == (24,)


def test_the_recurrence_metric_reaches_the_core() -> None:
    data = _features()
    cosine = libsonare.segment_recurrence_matrix(data, k=3, metric="cosine", mode="affinity")
    euclidean = libsonare.segment_recurrence_matrix(data, k=3, metric="euclidean", mode="affinity")
    assert not np.array_equal(cosine, euclidean)


REFUSALS: dict[str, tuple[Callable[[Any], object], tuple[object, ...], tuple[object, ...]]] = {
    "norm": (
        lambda v: libsonare.vector_normalize([3.0, 4.0], norm=v),
        ("inf", "L2", "maximum"),
        (0, 2, True, None),
    ),
    "tempogram mode": (
        lambda v: libsonare.tempogram(np.ones(64, dtype=np.float32), sr=SR, win_length=8, mode=v),
        ("auto", "ac", "Cosine"),
        (0, 1),
    ),
    "chroma_method": (
        lambda v: libsonare.detect_chords(_chord_tone(0.5), SR, chroma_method=v),
        ("CQT", "chroma", ""),
        (2, None),
    ),
    "genre_hint": (
        lambda v: libsonare.detect_key(_chord_tone(0.5), SR, genre_hint=v),
        ("rock", "EDM", ""),
        (1,),
    ),
    "metric": (
        lambda v: libsonare.segment_recurrence_matrix(_features(), k=3, metric=v),
        ("manhattan", "Cosine"),
        (0,),
    ),
    "segment mode": (
        lambda v: libsonare.segment_cross_similarity(_features(), _features(), k=3, mode=v),
        ("distance",),
        (1,),
    ),
    "linkage": (
        lambda v: libsonare.segment_agglomerative(_features(), 3, linkage=v),
        ("Ward", "centroid"),
        (0,),
    ),
}


@pytest.mark.parametrize("name", sorted(REFUSALS))
def test_an_unknown_name_is_refused_naming_the_tokens(name: str) -> None:
    call, unknown, _ = REFUSALS[name]
    for value in unknown:
        with pytest.raises(SonareValueError, match=" must be one of "):
            call(value)


@pytest.mark.parametrize("name", sorted(REFUSALS))
def test_a_non_string_is_a_type_error(name: str) -> None:
    call, _, non_strings = REFUSALS[name]
    for value in non_strings:
        with pytest.raises(TypeError):
            call(value)
