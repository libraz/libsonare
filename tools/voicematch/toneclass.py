"""What kind of sound a GM program makes, and what follows from that.

Three decisions in this harness depend on the same property and were each made
separately: which notes to probe, which metric set to score with, and which
terms to weight. Splitting them is how program 9 came to be probed at C3/C4/C5
(the fallback nobody chose), scored with a harmonic ladder (a glockenspiel has
no harmonic series), under weights tuned for a bowed string.

So the property is named once here and the three read it.

`ToneClass` is about the PARTIAL STRUCTURE and the EXCITATION, because that is
what the measurements care about:

  sustained       energy is fed continuously; partials are integer multiples
  struck-string   a hammered stiff string: n·f0·sqrt(1+B·n^2), decaying
  plucked-string  the same series, plucked, shorter and brighter at the onset
  modal           a bar, bell, plate or membrane: partials at ratios no formula
                  predicts, which is exactly why they have to be measured
  noise           no pitch to speak of

The membrane and bar cases are one class rather than two. Their ratios differ
(1 : 2.756 : 5.404 for a free-free bar, 1 : 1.59 : 2.14 for a circular
membrane) but nothing here has to know which: both are measured by finding the
partials rather than by predicting them, and the *measurement* is identical.
"""

from __future__ import annotations

from enum import Enum


class ToneClass(str, Enum):
    SUSTAINED = "sustained"
    STRUCK_STRING = "struck-string"
    PLUCKED_STRING = "plucked-string"
    MODAL = "modal"
    NOISE = "noise"

    @property
    def stringlike(self) -> bool:
        """Whether the stiff-string partial law describes this voice.

        The harmonic ladder and the inharmonicity fit are only meaningful here;
        `MODAL` needs the measured-partial path instead.
        """
        return self in (ToneClass.STRUCK_STRING, ToneClass.PLUCKED_STRING,
                        ToneClass.SUSTAINED)

    @property
    def fast_attack(self) -> bool:
        """Whether the onset is short enough to need the fine envelope grid.

        A struck or plucked string reaches its peak in single-digit
        milliseconds, which the 5 ms envelope hop quantises rather than
        measures — the same argument the percussion path already makes for
        itself. A bowed or blown note rises over tens of milliseconds and is
        resolved fine by the coarse grid.
        """
        return self in (ToneClass.STRUCK_STRING, ToneClass.PLUCKED_STRING,
                        ToneClass.MODAL)


#: GM programs whose class is not the one their family implies. Everything else
#: falls to `_FAMILY_CLASS` below, which is right for 7 of the 8 members of most
#: families and wrong for the exceptions listed here.
_PROGRAM_CLASS: dict[int, ToneClass] = {
    # 8-15 chromatic percussion is modal, except the hammered dulcimer, which
    # libsonare voices on Karplus-Strong because it physically is a struck
    # string rather than a bar.
    15: ToneClass.STRUCK_STRING,   # dulcimer
    # 40-47 strings: bowed, except the three that are not.
    45: ToneClass.PLUCKED_STRING,  # pizzicato strings
    46: ToneClass.PLUCKED_STRING,  # orchestral harp
    47: ToneClass.MODAL,           # timpani — a tuned kettledrum, 1 : 1.5 : 2 : 2.44
    55: ToneClass.MODAL,           # orchestra hit
    # 104-111 ethnic: plucked, blown and struck in one family.
    104: ToneClass.PLUCKED_STRING,  # sitar
    105: ToneClass.PLUCKED_STRING,  # banjo
    106: ToneClass.PLUCKED_STRING,  # shamisen
    107: ToneClass.PLUCKED_STRING,  # koto
    108: ToneClass.MODAL,           # kalimba — a plucked steel tine, a bar not a string
    109: ToneClass.SUSTAINED,       # bagpipe
    110: ToneClass.SUSTAINED,       # fiddle
    111: ToneClass.SUSTAINED,       # shanai
    # 112-119 percussive: pitched, on the melodic channel, and none of them
    # harmonic. This is the range the harness used to score with a harmonic
    # ladder because `percussive` was tied to the drum channel rather than to
    # the instrument.
    119: ToneClass.NOISE,           # reverse cymbal
}

_FAMILY_CLASS: dict[int, ToneClass] = {
    0: ToneClass.STRUCK_STRING,    # 0-7    piano
    8: ToneClass.MODAL,            # 8-15   chromatic percussion
    16: ToneClass.SUSTAINED,       # 16-23  organ
    24: ToneClass.PLUCKED_STRING,  # 24-31  guitar
    32: ToneClass.PLUCKED_STRING,  # 32-39  bass
    40: ToneClass.SUSTAINED,       # 40-47  strings (bowed)
    48: ToneClass.SUSTAINED,       # 48-55  ensemble
    56: ToneClass.SUSTAINED,       # 56-63  brass
    64: ToneClass.SUSTAINED,       # 64-71  reed
    72: ToneClass.SUSTAINED,       # 72-79  pipe
    80: ToneClass.SUSTAINED,       # 80-87  synth lead
    88: ToneClass.SUSTAINED,       # 88-95  synth pad
    96: ToneClass.SUSTAINED,       # 96-103 synth effects
    104: ToneClass.PLUCKED_STRING,  # 104-111 ethnic
    112: ToneClass.MODAL,          # 112-119 percussive
    120: ToneClass.NOISE,          # 120-127 sound effects
}


def tone_class(program: int, *, drum_note: int | None = None) -> ToneClass:
    """What kind of sound `program` makes.

    A drum note is `MODAL` whatever the kit's program number is: on the drum
    channel the program selects the kit and the note selects the instrument, so
    the program says nothing about the partial structure. The cymbals and
    shakers of a kit are closer to `NOISE`, but they are voiced by the same
    percussion metric set as the membranes and nothing downstream branches on
    the difference.
    """
    if drum_note is not None:
        return ToneClass.MODAL
    if program in _PROGRAM_CLASS:
        return _PROGRAM_CLASS[program]
    return _FAMILY_CLASS.get((program // 8) * 8, ToneClass.SUSTAINED)


# --------------------------------------------------------------------------- #
# Default term weights per class
# --------------------------------------------------------------------------- #
# Why these are not one set of numbers.
#
# Leaving every weight but `harm`, `cents` and `tnr` at zero means the default
# fit scores a time-averaged spectrum and nothing else — no envelope, no decay,
# no level, no attack. That default is defensible for a sustained voice, where
# the spectrum genuinely is most of the identity, and it is close to useless for
# a struck one, where the identity is the decay and the strike.
#
# So a class carries its own. These are starting points a spec overrides, not
# claims about the one true weighting; what they buy is that a run with no
# `--w-*` flags scores something appropriate to the instrument rather than
# something appropriate to a violin.
_CLASS_WEIGHTS: dict[ToneClass, dict[str, float]] = {
    # A bowed or blown note: the spectrum is the identity, and how it moves is
    # the difference between an instrument and an organ pipe imitating one.
    ToneClass.SUSTAINED: {
        "harm": 1.0, "cents": 0.5, "tnr": 1.0, "mod": 1.0, "env": 0.5,
        "slope": 0.5,
    },
    # A hammered string: the decay and the strike carry it, and the aftersound
    # is most of the note. `stiff` is weighted because the series itself is a
    # property of the string rather than of the voicing.
    ToneClass.STRUCK_STRING: {
        "harm": 1.0, "cents": 0.5, "tnr": 0.5, "env": 1.0, "init": 1.0,
        "slope": 1.0, "tail": 1.0, "hf": 0.5, "lf": 0.5, "stiff": 0.5,
        "crest": 1.0,
    },
    # A plucked string: the same shape, weighted toward the onset, since the
    # pluck sets the spectrum and nothing feeds it afterwards.
    ToneClass.PLUCKED_STRING: {
        "harm": 1.0, "cents": 0.5, "tnr": 0.5, "env": 1.0, "init": 1.5,
        "slope": 1.0, "hf": 0.5, "crest": 1.0, "stiff": 0.5,
    },
    # A bar, bell or membrane. `harm` is deliberately absent: the ladder it
    # measures is not this instrument's series, and weighting it would score the
    # difference between two noise floors. `modes` replaces it.
    ToneClass.MODAL: {
        "modes": 1.0, "env": 1.0, "init": 1.0, "slope": 1.0, "crest": 1.0,
        "hf": 0.5,
    },
    # Nothing has a pitch, so only the whole-render measures say anything.
    ToneClass.NOISE: {
        "mss": 1.0, "env": 1.0, "crest": 1.0,
    },
}

#: A drum probe's default weights, which are per-metric-set rather than per
#: class: the percussion metric set produces `band` / `bdecay` and no ladder,
#: whatever the kit piece is.
PERCUSSION_WEIGHTS: dict[str, float] = {
    "band": 1.0, "bdecay": 1.0, "env": 1.0, "modes": 0.5, "crest": 0.5,
    # Weighted as heavily as the whole band profile it is drawn from, because
    # it is one region against that profile's twenty-five bands and the kick is
    # the loudest thing in the kit. See `loss._perc_lf_terms`.
    "lf": 1.0,
    # The relations between the kit's own families — the tom series, the hi-hat
    # trio, the cymbals — which no per-hit term can hold. Weighted level with
    # the timbre terms rather than under them: a kit whose members are each
    # individually plausible and collectively in the wrong relation is what a
    # listener calls a bad kit, and it is the failure the per-hit terms are
    # structurally unable to report. See `loss._kit_terms`.
    "kit": 1.0,
}


def default_weights(program: int, *, drum_note: int | None = None,
                    percussive: bool = False) -> dict[str, float]:
    """The term weights a run starts from when the command line names none."""
    if percussive or drum_note is not None:
        return dict(PERCUSSION_WEIGHTS)
    return dict(_CLASS_WEIGHTS[tone_class(program)])


# --------------------------------------------------------------------------- #
# Canonical gate dimensions per class
# --------------------------------------------------------------------------- #
# A capture's `dimensions` list says what its reference is judged on, and every
# capture chose one for itself: the piano names 11, the pipe organ 3, and the
# harpsichord and the drum kit name none at all. That makes a gate's size
# unreadable — three bounds on an organ and eight on a harpsichord say nothing
# about which voice is further along, because neither says how many the
# instrument HAS.
#
# So the class carries the denominator. A dimension is listed for a class when
# the measurement means something for that excitation, not when some instrument
# in the class happens to have been measured on it. A capture that cannot reach
# one says so in its own `dimensions_na`, with a reason — an organ has no
# velocity response, and that is a fact about flue pipes rather than a gap.
_MELODIC_ALL = (
    "stretch",       # cents_vs_et       — the ladder against equal temperament
    "decay",         # decay_db_s        — the free fall while the key is held
    "aftersound",    # decay_late_db_s   — what is left after the prompt stage
    "doubling",      # decay_early_db_s  — the prompt stage itself
    "body",          # body_below_f0_db  — radiated energy under the fundamental
    "attack",        # attack_ms         — time to peak
    "stereo",        # stereo_width      — the image
    "damper",        # damper_release_ms — how the note is stopped
    "balance",       # partials_db       — the partial levels against each other
    "centroid_pct",  # centroid_hz       — brightness, as a fraction
    "tnr",           # tnr_db            — tone against noise
    "vel_range",     # peak_dbfs         — the dynamic span
    "register",      # held_peak_dbfs    — level against register
)

CANONICAL_DIMENSIONS: dict[ToneClass, tuple[str, ...]] = {
    # A hammered or plucked string is measured on everything: it has a free
    # decay, a two-stage fall, a damper and a dynamic range, and the stiff-string
    # ladder is meaningful.
    ToneClass.STRUCK_STRING: _MELODIC_ALL,
    ToneClass.PLUCKED_STRING: _MELODIC_ALL,
    # Energy is fed continuously, so the three free-decay dimensions describe a
    # fall this voice does not have. The release still does — `damper` is how
    # the pipe or the bow stops, which is measurable and is where a sustained
    # voice most often gives itself away.
    ToneClass.SUSTAINED: tuple(
        d for d in _MELODIC_ALL if d not in ("decay", "aftersound", "doubling")),
    # A bar, bell or membrane has no series equal temperament predicts, so
    # `stretch` measures the distance between two things that were never meant
    # to agree.
    ToneClass.MODAL: tuple(d for d in _MELODIC_ALL if d != "stretch"),
    # No pitch, so every dimension resting on a partial or a fundamental drops.
    ToneClass.NOISE: ("attack", "stereo", "centroid_pct", "vel_range", "decay"),
}

#: A drum kit's, which are per-metric-set rather than per class for the same
#: reason `PERCUSSION_WEIGHTS` is: the percussion metric set produces a band
#: profile and no ladder, whatever the piece.
PERCUSSION_DIMENSIONS: tuple[str, ...] = (
    "band_tilt", "band_shape", "band_decay", "attack", "crest", "centroid_pct",
    "level", "vel_range",
)


def canonical_dimensions(program: int, *, drum_note: int | None = None,
                         percussive: bool = False) -> tuple[str, ...]:
    """Every dimension this voice's class can be judged on.

    The denominator of gate coverage. What a capture actually gates is its
    `dimensions`; what it could gate is this.
    """
    if percussive or drum_note is not None:
        return PERCUSSION_DIMENSIONS
    return CANONICAL_DIMENSIONS[tone_class(program)]


# --------------------------------------------------------------------------- #
# Probe registers
# --------------------------------------------------------------------------- #
# Where a program is probed, for the families `patterns.py` never listed. Its
# table covers 16-23, 32-47 and 56-79; everything else fell through to C3/C4/C5,
# which is inside the range of a guitar and nowhere near a piccolo's or a
# kalimba's. A register outside the instrument's compass makes a sampled
# reference play an extreme transposition and a physical model extrapolate, and
# neither is the thing being compared.
PROGRAM_REGISTERS: dict[int, tuple[int, int, int]] = {
    # 0-7 pianos and keyboards: the compass is the point of a keyboard voice,
    # and a physical model diverges most at the ends of it.
    **{p: (36, 60, 84) for p in range(0, 8)},
    # 8-15 chromatic percussion, each in its own written compass.
    8: (60, 72, 84),    # celesta (written C4-C8, sounds an octave up)
    9: (79, 91, 100),   # glockenspiel
    10: (72, 84, 96),   # music box
    11: (53, 65, 77),   # vibraphone (F3-F6)
    12: (45, 60, 79),   # marimba (A2-C7)
    13: (65, 77, 89),   # xylophone (F4-C8)
    14: (53, 65, 77),   # tubular bells (F3-F5, sounding)
    15: (48, 60, 72),   # dulcimer
    # 24-31 guitars: E2-E5 is the fretboard, and the top string's open E is 64.
    **{p: (40, 52, 64) for p in range(24, 32)},
    # 48-55 ensembles and voices. The choir programs sit in a singer's range
    # rather than a string section's, which runs an octave lower at the bottom.
    **{p: (48, 60, 72) for p in range(48, 52)},
    52: (48, 60, 72),   # choir aahs
    53: (48, 60, 72),   # voice oohs
    54: (48, 60, 72),   # synth voice
    55: (48, 60, 72),   # orchestra hit
    # 80-95 synth leads and pads: no acoustic compass to respect, so the probe
    # is the widest range the voices are actually written across.
    **{p: (36, 60, 84) for p in range(80, 96)},
    # 96-103 synth effects.
    **{p: (48, 60, 72) for p in range(96, 104)},
    # 104-111 ethnic, each in its own compass.
    104: (48, 60, 72),  # sitar
    105: (48, 60, 72),  # banjo
    106: (48, 60, 72),  # shamisen
    107: (48, 60, 72),  # koto
    108: (60, 72, 84),  # kalimba
    109: (62, 69, 74),  # bagpipe (a chanter's compass is barely an octave)
    110: (55, 67, 79),  # fiddle — a violin
    111: (62, 72, 82),  # shanai
    # 112-119 pitched percussion.
    112: (72, 84, 96),  # tinkle bell
    113: (72, 79, 84),  # agogo
    114: (60, 67, 74),  # steel drums
    115: (72, 79, 84),  # woodblock
    116: (36, 43, 48),  # taiko
    117: (41, 48, 55),  # melodic tom
    118: (36, 48, 60),  # synth drum
    119: (60, 60, 60),  # reverse cymbal — no pitch, one probe point
    # 120-127 sound effects: no pitch at all, probed at one point so the
    # whole-render terms have something to read.
    **{p: (60, 60, 60) for p in range(120, 128)},
}
