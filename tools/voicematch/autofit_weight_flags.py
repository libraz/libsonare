"""Argument definitions for autofit loss weights."""

from __future__ import annotations

import argparse

_WEIGHT_FLAGS_1: tuple[tuple[str, str], ...] = (
    (
        "--w-harm",
        "weight on the harmonic-profile L1 term",
    ),
    (
        "--w-cents",
        "weight on the intonation (cents) term",
    ),
    (
        "--w-tnr",
        "weight on the noise-floor (TNR shortfall) term",
    ),
    (
        "--w-env",
        (
            "weight on the temporal-envelope term (sustain slope / release / "
            "attack; attack / decay / crest for a drum). Unset, the weight "
            "comes from the tone class: half for a sustained voice, where the "
            "spectrum is the identity, and 1 for a struck, plucked or modal "
            "one and for a drum, where the gesture is"
        ),
    ),
    (
        "--w-band",
        (
            "drum fits: weight on the 1/3-octave level-profile term, the "
            "percussion analogue of the harmonic ladder"
        ),
    ),
    (
        "--w-bdecay",
        "drum fits: weight on the per-octave-band decay-slope term",
    ),
    (
        "--w-tilt",
        (
            "drum fits: weight on how far the hit's 2 kHz-and-up level "
            "sits from its 500 Hz-and-down level, against the same "
            "difference in the reference. A direction, which --w-band "
            "has none of"
        ),
    ),
    (
        "--w-bright",
        (
            "weight on the spectral centroid as a percentage of the "
            "reference's, per note. The gate's own arithmetic, so a fit "
            "moves the quantity the voice is judged on; default on drums only"
        ),
    ),
    (
        "--w-arrival",
        (
            "pitched fits: weight on the time to the note's arrival, the gate's "
            "first point within 3 dB of the held maximum, per note. Sees a level "
            "still climbing after the half-second `env` reads its attack over"
        ),
    ),
    (
        "--w-body",
        (
            "pitched fits: weight on the energy under the fundamental against "
            "the fundamental, the gate's body reading, per note from 120 Hz up. "
            "Sees a string that has lost its fundamental to a higher regime"
        ),
    ),
    (
        "--w-tonal",
        (
            "drum fits: weight on the hit's spectral flatness against the "
            "reference's, in dB — the gate's `tonality`. Whether the hit "
            "stands in lines or in a continuum, which no band level can see"
        ),
    ),
    (
        "--w-rise",
        (
            "drum fits: weight on how far the hit's tilt moves from its first "
            "15 ms to 30-60 ms in, against the reference's — the gate's "
            "`tilt_rise`. The order the energy arrives in, which every "
            "integrated spectral term has summed away"
        ),
    ),
    (
        "--w-strike",
        (
            "drum fits: weight on the hit's tilt over its first 15 ms against "
            "the reference's. `--w-rise` is a difference and cannot see a strike "
            "and a body that are too dark together"
        ),
    ),
    (
        "--w-ring",
        (
            "drum fits: weight on how long the hit takes to fall 20 dB, in "
            "doublings of the reference's — the module grids' `ring`, which "
            "`--w-env` prices at a hundredth of a unit per millisecond"
        ),
    ),
    (
        "--w-density",
        (
            "drum fits: weight on the spectral hit-density term; unset, "
            "the drum metric defaults decide"
        ),
    ),
    (
        "--w-prompt",
        (
            "drum fits: weight on the prompt/early ring-down term; unset, "
            "the drum metric defaults decide"
        ),
    ),
    (
        "--w-kit",
        (
            "drum fits: weight on the RELATIONS INSIDE THE KIT — the tom "
            "series, the hi-hat trio, the cymbals — as the sorted contrasts "
            "in pitch, decay, colour and level within each family the "
            "capture declares. Every other percussion term scores one hit "
            "against its own reference row and is capped, so a kit whose "
            "members are each individually plausible and collectively in "
            "the wrong relation reads as correct; worse, once the members "
            "are far enough out the per-hit terms saturate and the repair "
            "has no gradient to follow. Needs a corpus whose capture names "
            "its families AND a grid that covers one: --drum-note narrows "
            "to a single note by itself, so name the family with --notes"
        ),
    ),
    (
        "--w-init",
        "weight on the per-harmonic ONSET-ladder term (excitation evidence)",
    ),
    (
        "--w-slope",
        "weight on the per-harmonic decay-slope term (loop evidence)",
    ),
    (
        "--w-tail",
        (
            "weight on the per-harmonic decay slope 2-6 s in, which only has "
            "frames to fit when the probe holds a note that long. This is "
            "the aftersound: on a piano it is most of the note, and no "
            "two-second probe can reach it"
        ),
    ),
    (
        "--w-hf",
        (
            "weight on the attack's high-band balance, measured in 20 ms "
            "slices over the first 120 ms. Catches a strike-noise or "
            "excitation path whose top end is wrong for a few tens of "
            "milliseconds — a tick, which the ear finds instantly and a "
            "whole-timeline spectral distance averages away"
        ),
    ),
    (
        "--w-lf",
        (
            "weight on the attack's low- and mid-band balance, 20 Hz to "
            "4 kHz over one 50 ms window. The bass counterpart of --w-hf: "
            "catches an excitation that dumps its energy below where the "
            "instrument radiates, which reads as a note with no onset and "
            "which every sustain-window term here scores as correct"
        ),
    ),
    (
        "--w-stiff",
        (
            "weight on STRING STIFFNESS: how far the model stretches its "
            "twelfth partial against how far the reference does, in cents. "
            "The ladder is now measured along each string's own partial "
            "series, which is what makes it correct and also what leaves "
            "the series itself unpriced — a voice twice as stiff as its "
            "reference otherwise scores a clean sheet"
        ),
    ),
    (
        "--w-dyn",
        (
            "weight on the DYNAMICS CURVE: how brightness tracks velocity, "
            "fitted per pitch so register is held fixed. The only term "
            "that lives in the relation between notes rather than inside "
            "one — a model can match every note of a grid one at a time "
            "and still get the trend between them wrong. Needs a probe "
            "with a velocity axis (--pattern velocity, or a drum probe)"
        ),
    ),
    (
        "--w-hfdyn",
        (
            "weight on the ATTACK DYNAMICS CURVE: how the 4-8 kHz share of "
            "the first 120 ms tracks velocity, fitted per pitch. `--w-dyn` reads "
            "the held ladder and cannot see a hammer whose attack brightens with "
            "force while the ring does not. Needs a probe with a velocity axis"
        ),
    ),
    (
        "--w-modedyn",
        (
            "weight on the MODAL DYNAMICS CURVE: how the measured modes' share "
            "above 1.75x the played note tracks velocity, fitted per pitch. "
            "`--w-dyn` reads the harmonic ladder, which on a bar or a bell sits "
            "between the modes. Needs a probe with a velocity axis"
        ),
    ),
    (
        "--w-onsetvel",
        (
            "weight on the ONSET VELOCITY CURVE: how the fine attack time and the "
            "onset overshoot track log velocity, fitted per pitch. Needs a probe with "
            "a velocity axis"
        ),
    ),
    (
        "--w-onset",
        (
            "weight on the ONSET SHAPE: each cell's overshoot over the sustain (dB) "
            "and its settle time (doublings), against the reference. Needs a capture "
            "whose committed profile has an onset-eligible cell"
        ),
    ),
    (
        "--w-velspan",
        (
            "weight on the VELOCITY SPAN: the softest-to-hardest range of the peak "
            "level per pitch, the quantity the gate's `vel_range` reads. Needs a probe "
            "with a velocity axis"
        ),
    ),
    (
        "--w-velcurve",
        (
            "weight on the VELOCITY CURVE: per-note peak-level differences across "
            "velocity after removing that note's common gain offset. Finds a wrong "
            "middle-velocity response even when the softest-to-hardest span matches. "
            "Needs a probe whose velocities span at least 16 on one pitch"
        ),
    ),
    (
        "--w-level",
        (
            "weight on the level BALANCE across the probe grid: how loud each "
            "note is relative to the others, with the grid's own median "
            "offset removed so an output-gain difference is not fitted. "
            "Needs a probe with more than one note to mean anything"
        ),
    ),
)

_WEIGHT_FLAGS_2: tuple[tuple[str, str], ...] = (
    (
        "--w-crest",
        (
            "weight on peak-minus-held-RMS per note. Gain-invariant, and the "
            "one term that sees a note whose envelope never falls after its "
            "attack — every other term here is normalised past it"
        ),
    ),
    (
        "--w-mss",
        (
            "weight on the multi-scale STFT distance over the whole render "
            "(sees what the per-note metric set does not model)"
        ),
    ),
    (
        "--w-modes",
        (
            "weight on the MEASURED PARTIAL SERIES: the partials as found, "
            "paired against the reference's by frequency, priced in cents "
            "and dB. The harmonic ladder searches n*f0*sqrt(1+B*n^2), which "
            "describes a stiff string and nothing else, so on a bar, a bell, "
            "a plate or a membrane every bin above the fundamental reads the "
            "render's own noise floor - on BOTH sides. That covers GM 8-14, "
            "108 and 112-118, and every drum note with a definite pitch. "
            "Weighted by default for those, and available for any voice"
        ),
    ),
    (
        "--w-mod",
        (
            "weight on MOVEMENT: vibrato depth and rate, tremolo, the slow "
            "beat of an ensemble or a unison pair, and how wide the "
            "fundamental is. A sampled reference is a recording of a player "
            "and carries all of it; a physical model renders a still note "
            "unless told otherwise, and every other term here reads that "
            "stillness as cleanliness - --w-tnr charges the model only for "
            "being NOISIER, so nothing could ever ask for vibrato"
        ),
    ),
)


def _add_weight_flags(parser: argparse.ArgumentParser, flags: tuple[tuple[str, str], ...]) -> None:
    """Register optional float loss-weight flags (``--w-x`` stores into ``w_x``, default unset)."""
    for flag, help_text in flags:
        parser.add_argument(
            flag,
            type=float,
            default=None,
            dest=flag[2:].replace("-", "_"),
            help=help_text,
        )
