"""Fit-profile tests for the bank's independent excitation axes."""

from __future__ import annotations

import dataclasses
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from toneclass import ExcitationFamily, FitProfile, ToneClass, default_weights, fit_profile


@pytest.mark.parametrize(
    ("program", "excitation", "keyboard"),
    [
        (0, ExcitationFamily.HAMMERED, True),  # concert piano
        (4, ExcitationFamily.HAMMERED, True),  # FM/tine electric piano
        (5, ExcitationFamily.HAMMERED, True),
        (6, ExcitationFamily.PLUCKED, True),  # harpsichord jack
        (7, ExcitationFamily.PLUCKED, True),  # clavi high-ratio pluck
        (8, ExcitationFamily.STRUCK_MODAL, True),  # celesta: keyed, not hammered string
        (9, ExcitationFamily.STRUCK_MODAL, False),  # hand-played glockenspiel
        (15, ExcitationFamily.HAMMERED, False),  # hammered dulcimer
        (24, ExcitationFamily.PLUCKED, False),  # nylon guitar
        (46, ExcitationFamily.PLUCKED, False),  # orchestral harp
        (40, ExcitationFamily.BOWED, False),  # violin
        (48, ExcitationFamily.BOWED, False),  # physical string ensemble
        (49, ExcitationFamily.BOWED, False),
        (110, ExcitationFamily.BOWED, False),  # fiddle
        (16, ExcitationFamily.OTHER, True),  # drawbar organ: keyboard interface
        (22, ExcitationFamily.OTHER, False),  # harmonica: wind instrument
        (44, ExcitationFamily.OTHER, False),  # subtractive tremolo section patch
        (108, ExcitationFamily.STRUCK_MODAL, False),  # modal steel tine
    ],
)
def test_program_profiles_follow_the_bank_patch(program, excitation, keyboard):
    profile = fit_profile(program)
    assert profile.excitation is excitation
    assert profile.keyboard is keyboard


def test_tone_topology_stays_independent_from_physical_excitation():
    # Harpsichord/clavi are still the existing stiff-string topology, even
    # though their profile uses the plucked excitation metrics.
    assert fit_profile(6).tone_class is ToneClass.STRUCK_STRING
    assert fit_profile(7).tone_class is ToneClass.STRUCK_STRING
    assert fit_profile(6).excitation is ExcitationFamily.PLUCKED
    assert fit_profile(0).tone_class is ToneClass.STRUCK_STRING
    assert fit_profile(8).tone_class is ToneClass.MODAL


def test_drum_channel_overrides_the_melodic_program_without_calling_it_a_hammer():
    for note in (36, 38, 49):
        profile = fit_profile(0, drum_note=note)
        assert profile == FitProfile(ToneClass.MODAL, ExcitationFamily.PERCUSSION, False)
    assert fit_profile(40, percussive=True).excitation is ExcitationFamily.PERCUSSION


def test_profile_is_frozen_and_has_no_shared_mutable_weight_state():
    profile = fit_profile(40)
    with pytest.raises(dataclasses.FrozenInstanceError):
        profile.keyboard = True

    first = default_weights(24)
    second = default_weights(24)
    first["level"] = 0.0
    first["tail"] = 0.0
    assert second["level"] == pytest.approx(1.0)
    assert second["tail"] == pytest.approx(1.0)


def test_family_defaults_add_only_the_axes_the_excitation_can_explain():
    bowed = default_weights(40)
    plucked = default_weights(24)
    hammered = default_weights(0)
    modal = default_weights(8)
    drum = default_weights(0, drum_note=38)

    assert bowed["level"] > 0.0 and bowed["mod"] > 0.0
    assert default_weights(48)["level"] > 0.0
    assert default_weights(49)["level"] > 0.0
    assert plucked["level"] > 0.0 and plucked["tail"] > 0.0
    assert hammered["level"] > 0.0 and hammered["tail"] > 0.0 and hammered["hfdyn"] > 0.0
    assert modal["level"] > 0.0 and "harm" not in modal
    assert drum["level"] > 0.0 and "mss" not in drum

    # Harpsichord and clavi preserve their topology but use the plucked metric
    # profile, so they do not inherit the piano-only high-frequency dynamics.
    assert default_weights(6) == plucked
    assert default_weights(7) == plucked
    assert "hfdyn" not in default_weights(6)
    assert "hfdyn" not in default_weights(7)


def test_unrelated_sustained_and_noise_defaults_remain_unchanged():
    organ = default_weights(19)
    wind = default_weights(73)
    noise = default_weights(120)
    assert fit_profile(16).excitation is ExcitationFamily.OTHER
    assert fit_profile(16).keyboard is True
    assert "level" not in organ and organ["mod"] > 0.0
    assert "level" not in wind and wind["mod"] > 0.0
    assert noise["mss"] == pytest.approx(1.0)
