"""The phrases an audition page plays, and which set a voice gets.

A take is chosen for what it catches by ear that no metric here reports. The
per-note analysis in `metrics.py` measures one isolated note at a time, so
everything that lives BETWEEN notes is invisible to it — two strings beating
against each other, a note struck again before it has stopped, a damper landing,
a hi-hat choked by the next one, the join in a slur. Those are the takes.

Two kinds of set live here. An instrument set (`piano`, `harpsichord`, `drums`)
is written for one instrument and names its notes: a harpsichord has no pedal to
lift and a piano has no stops to draw, so the phrases cannot be shared. A
generic set (`sustained`, `struck`, `plucked`, `modal`, `noise`) is a shape
whose notes are filled in from `registers_for_program`, and there is one per
`ToneClass` — which is what lets every GM program have a set without a capture
existing for it.

A capture definition names its set explicitly and keeps whichever it names;
`take_set_for` is only consulted for a program no capture covers.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

from patterns import registers_for_program
from smf import Note
from toneclass import ToneClass, tone_class

PEDAL = 64
#: Continuous-controller number for expression, the swell gesture's control.
EXPRESSION = 11
#: The GM drum channel, zero-based. A note number selects an instrument there
#: rather than a pitch, which is the whole of what makes the kit set a kit.
DRUM_CHANNEL = 9


@dataclass
class Take:
    """One phrase, rendered once per version."""

    id: str
    label: str
    group: str
    sub: str
    notes: list[Note]
    tail_s: float = 4.0
    cc_events: tuple[tuple[float, int, int], ...] = field(default=())
    #: MIDI channel. 9 is the GM drum channel, on which a note number selects a
    #: percussion instrument rather than a pitch — the kit set needs it and
    #: nothing else does.
    channel: int = 0

    def duration(self) -> float:
        end = max((n.start + n.dur) for n in self.notes)
        return end + self.tail_s


# --------------------------------------------------------------------------- #
# Instrument sets — written for one instrument, notes named
# --------------------------------------------------------------------------- #

def piano_takes(program: int = 0) -> list[Take]:
    """The piano set: one phrase per thing that is hard to hear any other way."""
    out: list[Take] = []

    out.append(Take(
        "single-c4", "Single note — C4, mf", "one note at a time",
        "attack, free decay, damper",
        [Note(60, 96, 0.3, 4.0)], tail_s=4.0,
    ))

    out.append(Take(
        "dynamics-c4", "Dynamics — C4 at five velocities", "one note at a time",
        "vel 16 / 40 / 72 / 100 / 127",
        [Note(60, v, 0.3 + i * 2.2, 1.6) for i, v in enumerate((16, 40, 72, 100, 127))],
        tail_s=3.5,
    ))

    out.append(Take(
        "register-sweep", "Register — A0 to C8", "one note at a time",
        "A0 C2 C4 C6 C8, vel 96",
        [Note(n, 96, 0.3 + i * 2.6, 2.0) for i, n in enumerate((21, 36, 60, 84, 108))],
        tail_s=4.0,
    ))

    # Two strings a third apart beat against each other in a way that depends on
    # every partial being in the right place, not just the fundamental. A model
    # with the right harmonic ladder and the wrong inharmonicity passes note by
    # note and falls apart here.
    out.append(Take(
        "chord-sustained", "Chord — C major triad held", "notes together",
        "C3 E3 G3, vel 88, 6 s",
        [Note(n, 88, 0.3, 6.0) for n in (48, 52, 55)], tail_s=4.0,
    ))

    out.append(Take(
        "arpeggio-legato", "Arpeggio — overlapping, no pedal", "notes together",
        "C3 E3 G3 C4 E4 G4 C5, each held past the next",
        [Note(n, 84, 0.3 + i * 0.42, 1.9)
         for i, n in enumerate((48, 52, 55, 60, 64, 67, 72))],
        tail_s=4.0,
    ))

    # The pedal is the piano behaviour a physical model is most likely to be
    # missing entirely: it lifts every damper, so the strings that were never
    # struck ring in sympathy with the ones that were.
    out.append(Take(
        "pedal-resonance", "Pedal — staccato notes under a held pedal", "the pedal",
        "CC64 down at 0.2 s, up at 7.0 s",
        [Note(n, 92, 0.5 + i * 0.8, 0.12)
         for i, n in enumerate((36, 43, 48, 52, 55, 60, 64, 67))],
        tail_s=4.0,
        cc_events=((0.2, PEDAL, 127), (7.0, PEDAL, 0)),
    ))

    out.append(Take(
        "repeated-note", "Repeated note — eight strikes on a ringing string", "the pedal",
        "C4 vel 100, 190 ms apart",
        [Note(60, 100, 0.3 + i * 0.19, 0.14) for i in range(8)], tail_s=4.0,
    ))

    # Something to just listen to. Everything above is a probe; this is the one
    # that says whether the result is an instrument.
    phrase = [
        (60, 100, 0.00, 0.85), (64, 88, 0.85, 0.85), (67, 92, 1.70, 0.85),
        (72, 104, 2.55, 1.30), (71, 84, 3.85, 0.60), (67, 80, 4.45, 0.60),
        (64, 76, 5.05, 1.60),
        (36, 78, 0.00, 1.70), (43, 70, 1.70, 1.70), (41, 72, 3.40, 1.70),
        (36, 74, 5.05, 1.60),
    ]
    out.append(Take(
        "phrase-ballad", "Phrase — melody over a bass line, pedalled", "a phrase",
        "with the pedal changed on each bass note",
        [Note(n, v, 0.4 + s, d) for n, v, s, d in phrase], tail_s=5.0,
        cc_events=(
            (0.45, PEDAL, 127), (2.05, PEDAL, 0), (2.15, PEDAL, 127),
            (3.75, PEDAL, 0), (3.85, PEDAL, 127), (5.40, PEDAL, 0),
            (5.50, PEDAL, 127), (7.20, PEDAL, 0),
        ),
    ))
    return out


def harpsichord_takes(program: int = 6) -> list[Take]:
    """The harpsichord set.

    Almost nothing the piano set probes transfers. There is no pedal to lift and
    no una corda; what there is instead is a mechanism that refuses to obey the
    key, and a top octave that used to stop sounding long before it should.
    """
    out: list[Take] = []

    out.append(Take(
        "single-c4", "Single note - C4, mf", "one note at a time",
        "pluck, free decay, damper on release",
        [Note(60, 96, 0.3, 4.0)], tail_s=3.0,
    ))

    # The instrument's defining trait, and the one a listener catches in a
    # second: five velocities across the whole MIDI range should arrive at very
    # nearly the same loudness. A model that spends a dynamic range here is not
    # a harpsichord however good its single note sounds.
    out.append(Take(
        "dynamics-c4", "Dynamics - C4 at five velocities", "one note at a time",
        "vel 16 / 40 / 72 / 100 / 127, and they should barely differ",
        [Note(60, v, 0.3 + i * 1.7, 1.2) for i, v in enumerate((16, 40, 72, 100, 127))],
        tail_s=3.0,
    ))

    # A harpsichord's peak level is set by the plectrum rather than by the
    # string, so the compass should be flat. A register-balance error is
    # inaudible note by note and obvious the moment the notes are played in a
    # row.
    out.append(Take(
        "register-sweep", "Register - the whole compass", "one note at a time",
        "F1 C2 C3 C4 C5 C6 F6, vel 96",
        [Note(n, 96, 0.3 + i * 1.5, 1.2)
         for i, n in enumerate((29, 36, 48, 60, 72, 84, 89))],
        tail_s=3.5,
    ))

    # The treble is where the engine this replaced fell apart: the note died in
    # a fifth of a second. Held four seconds, alone, with nothing to hide it.
    out.append(Take(
        "treble-sustain", "Treble - the top note held four seconds", "one note at a time",
        "the top of the compass has to keep sounding",
        [Note(89, 96, 0.3, 4.0)], tail_s=3.0,
    ))

    out.append(Take(
        "chord-sustained", "Chord - C major triad held", "notes together",
        "C3 E3 G3, vel 88, 5 s",
        [Note(n, 88, 0.3, 5.0) for n in (48, 52, 55)], tail_s=3.5,
    ))

    # Baroque keyboard writing is mostly this: overlapping lines on an
    # instrument with no pedal, where every note stops when its key does.
    out.append(Take(
        "arpeggio-legato", "Arpeggio - overlapping, held past each other", "notes together",
        "C3 E3 G3 C4 E4 G4 C5, each held past the next",
        [Note(n, 84, 0.3 + i * 0.34, 1.5)
         for i, n in enumerate((48, 52, 55, 60, 64, 67, 72))],
        tail_s=3.5,
    ))

    # The ornament the instrument is played with, fast enough that the damper's
    # speed and the jack's reset decide whether it reads as notes at all.
    out.append(Take(
        "trill", "Trill - C4 to D4, sixteen notes", "the mechanism",
        "each key released before the next, 90 ms apart",
        [Note(60 if i % 2 == 0 else 62, 92, 0.3 + i * 0.09, 0.075) for i in range(16)],
        tail_s=3.0,
    ))

    # Release is a mechanism, not a fade: the jack drops, the tongue pivots past
    # the string without plucking it again, and the felt lands.
    out.append(Take(
        "staccato-release", "Staccato - eight short notes", "the mechanism",
        "the jack and the damper are the sound between the notes",
        [Note(n, 96, 0.3 + i * 0.55, 0.10)
         for i, n in enumerate((48, 55, 60, 64, 67, 64, 60, 55))],
        tail_s=3.0,
    ))

    # Something to just listen to, as the piano set has.
    out.append(Take(
        "phrase-baroque", "Phrase - two voices", "a phrase",
        "a right hand over a walking bass, no pedal anywhere",
        [Note(n, v, 0.4 + st, d) for n, v, st, d in (
            (72, 96, 0.00, 0.38), (71, 88, 0.38, 0.38), (69, 90, 0.76, 0.38),
            (67, 88, 1.14, 0.38), (65, 92, 1.52, 0.38), (64, 88, 1.90, 0.38),
            (62, 90, 2.28, 0.38), (60, 96, 2.66, 0.90),
            (64, 88, 3.60, 0.38), (65, 86, 3.98, 0.38), (67, 92, 4.36, 0.76),
            (72, 98, 5.12, 1.20),
            (48, 88, 0.00, 0.72), (55, 82, 0.76, 0.72), (53, 84, 1.52, 0.72),
            (52, 82, 2.28, 0.72), (48, 86, 3.04, 0.72), (50, 82, 3.80, 0.72),
            (52, 84, 4.56, 0.52), (48, 90, 5.12, 1.20),
        )], tail_s=3.5,
    ))
    return out


def drum_takes(program: int = 0) -> list[Take]:
    """The kit set: what a drum part is, which single hits are not.

    The single-hit probe strikes one instrument every two seconds, which is the
    right stimulus for measuring one drum and the wrong one for hearing a kit.
    Everything below lives in the relation between hits, and a fit that never
    plays two hits close together cannot produce any of it — not because the
    search failed but because the question was never put.

    Written on the drum channel, so a note number selects an instrument. The
    take set is `analysis_notes`-free by nature: every per-hit measurement
    assumes an isolated strike.
    """
    out: list[Take] = []

    # The mute group. 42, 44 and 46 share a GM exclusive class, so an open hat
    # left ringing is cut the instant a closed one or the pedal arrives. That is
    # the pedal, it is most of what a hi-hat part sounds like, and no probe in
    # this harness has ever fired it.
    out.append(Take(
        "hh-choke", "Hi-hat — open, choked, open, pedal", "the mute group",
        "46 open / 42 closed / 46 open / 44 pedal",
        [Note(46, 110, 0.3, 0.05), Note(42, 100, 0.8, 0.05),
         Note(46, 110, 1.8, 0.05), Note(44, 90, 2.3, 0.05)], tail_s=4.0,
        channel=DRUM_CHANNEL,
    ))

    # At speed each strike lands on the last one's ring. A voice that sounds
    # right alone and identical every time reads as a machine here, which is
    # what a fixed one-shot with no round robin and no choke sounds like.
    out.append(Take(
        "hh-sixteenths", "Hi-hat — sixteenths at 120", "at speed",
        "42 closed, accented on the beat",
        [Note(42, 112 if i % 4 == 0 else 96, 0.3 + i * 0.125, 0.05)
         for i in range(16)], tail_s=2.5, channel=DRUM_CHANNEL,
    ))

    # Two strikes close enough to fuse into one event. The ear hears a flam as
    # thickness rather than as two notes, and whether it does depends entirely
    # on what the second strike finds the first one doing.
    out.append(Take(
        "snare-flam", "Snare — flams and a roll", "at speed",
        "grace note 28 ms ahead, then a 12-stroke roll",
        [Note(38, 60, 0.3, 0.05), Note(38, 110, 0.328, 0.05),
         Note(38, 60, 1.3, 0.05), Note(38, 110, 1.328, 0.05)]
        + [Note(38, 90, 2.4 + i * 0.06, 0.05) for i in range(12)], tail_s=3.0,
        channel=DRUM_CHANNEL,
    ))

    # The tom fill is where the kit's tuning is audible as a kit rather than as
    # six separate instruments. The order is the MEASURED pitch order of the
    # capture, which is not the order General MIDI names the keys in.
    out.append(Take(
        "tom-fill", "Toms — a descending fill", "the kit as a kit",
        "43 41 50 48 47 45, the capture's measured pitch order, high to low",
        [Note(n, 104, 0.3 + i * 0.22, 0.05)
         for i, n in enumerate((43, 41, 50, 48, 47, 45))], tail_s=3.5,
        channel=DRUM_CHANNEL,
    ))

    # A cymbal's wash is four to ten seconds and every measurement in this
    # harness stops at 1.8. This is the take that hears what none of them reach.
    out.append(Take(
        "cymbal-wash", "Cymbals — crash and ride, let ring", "the long tail",
        "49 crash, then 51 ride, ten seconds",
        [Note(49, 120, 0.3, 0.05), Note(51, 100, 3.0, 0.05)], tail_s=10.0,
        channel=DRUM_CHANNEL,
    ))

    out.append(Take(
        "groove", "Groove — a bar of eights", "the kit as a kit",
        "kick, snare, closed hat, 100 bpm",
        [Note(42, 92 if i % 2 else 104, 0.3 + i * 0.3, 0.05) for i in range(8)]
        + [Note(36, 110, 0.3, 0.05), Note(36, 100, 1.5, 0.05)]
        + [Note(38, 108, 0.9, 0.05), Note(38, 108, 2.1, 0.05)], tail_s=3.5,
        channel=DRUM_CHANNEL,
    ))
    return out


# --------------------------------------------------------------------------- #
# Generic sets — one per ToneClass, notes filled in from the program's compass
# --------------------------------------------------------------------------- #

def sustained_takes(program: int = 40) -> list[Take]:
    """The set for anything bowed or blown: what happens BETWEEN notes.

    The whole harness measures isolated held notes, and on a wind or a bowed
    string that is the least characteristic thing the instrument does. Tonguing,
    a bow change, the join in a slur and the shape of a swell are where the
    playing is, and none of them exists inside one note.
    """
    lo, mid, hi = registers_for_program(program)
    out: list[Take] = []

    out.append(Take(
        "single-long", "Single note — held six seconds", "one note at a time",
        "the vibrato, if there is one, and how it starts",
        [Note(mid, 96, 0.3, 6.0)], tail_s=2.5,
    ))

    # A slur is one continuous sound with the pitch changed inside it. A model
    # that retriggers its excitation on every note-on produces a series of
    # separate notes, which is audible immediately and invisible to every
    # measurement here.
    out.append(Take(
        "legato-scale", "Legato — a scale, slurred", "between notes",
        "overlapping note-ons, no gap anywhere",
        [Note(mid + d, 92, 0.3 + i * 0.38, 0.44)
         for i, d in enumerate((0, 2, 4, 5, 7, 9, 11, 12))], tail_s=2.5,
    ))

    out.append(Take(
        "tongued", "Repeated notes — separated", "between notes",
        "the same pitch eight times, each articulated",
        [Note(mid, 100, 0.3 + i * 0.35, 0.22) for i in range(8)], tail_s=2.5,
    ))

    # The floor is 16 rather than 0, which is about -18 dB, because that is what
    # the gesture is: a swell box closed attenuates a chorus by 15 to 20 dB and
    # a player at the bottom of a diminuendo is still sounding. Expression taken
    # to 0 is a mute, and a mute is the one part of the phrase that cannot show
    # a difference — both sides render digital silence there, so a quarter of a
    # second of the take was carrying no comparison at all. It is also the part
    # a sampled reference cannot be recorded through: `au_oracle` reads silence
    # inside a sounding note as the dropout it guards against, which it is right
    # to do, and which no evidence in the render can distinguish from this.
    out.append(Take(
        "swell", "Swell — one note from a whisper and back", "one note at a time",
        "expression from 16 to 127 and back over 6 s",
        [Note(mid, 100, 0.3, 6.0)], tail_s=2.5,
        cc_events=tuple((0.3 + i * 0.25, EXPRESSION, v) for i, v in enumerate(
            list(range(16, 128, 11)) + list(range(127, 15, -11)))),
    ))

    out.append(Take(
        "leap", "Leaps — across the compass", "between notes",
        "low, high, low, slurred",
        [Note(n, 96, 0.3 + i * 0.6, 0.7)
         for i, n in enumerate((lo, hi, mid, hi, lo))], tail_s=3.0,
    ))
    return out


def struck_takes(program: int = 0) -> list[Take]:
    """The generic hammered-string set: an electric piano, a clav, a dulcimer.

    The piano set is this shape with the notes named and a pedal added. What is
    kept here is everything a struck string has whether or not it has dampers —
    the decay, the register balance, and what a second strike finds the first
    one doing.
    """
    lo, mid, hi = registers_for_program(program)
    out: list[Take] = []

    out.append(Take(
        "single-mid", "Single note — held four seconds", "one note at a time",
        "strike, free decay, damper on release",
        [Note(mid, 96, 0.3, 4.0)], tail_s=4.0,
    ))

    out.append(Take(
        "dynamics", "Dynamics — five velocities", "one note at a time",
        "vel 16 / 40 / 72 / 100 / 127",
        [Note(mid, v, 0.3 + i * 2.0, 1.4) for i, v in enumerate((16, 40, 72, 100, 127))],
        tail_s=3.5,
    ))

    # A struck string's decay rate is a function of its length, so the compass is
    # where a model with one decay law for every note gives itself away.
    out.append(Take(
        "register-sweep", "Register — across the compass", "one note at a time",
        "low, low-mid, mid, high-mid, high, vel 96",
        [Note(n, 96, 0.3 + i * 2.2, 1.8) for i, n in enumerate(
            (lo, (lo + mid) // 2, mid, (mid + hi) // 2, hi))], tail_s=4.0,
    ))

    # Partials that are each in the right place note by note can still be in the
    # wrong place relative to each other, and a held third is where that is
    # heard: the beating rate is set by the inharmonicity, not the fundamental.
    out.append(Take(
        "chord-sustained", "Chord — a major triad held", "notes together",
        "root, third, fifth at vel 88, 6 s",
        [Note(mid + d, 88, 0.3, 6.0) for d in (0, 4, 7)], tail_s=4.0,
    ))

    out.append(Take(
        "arpeggio-legato", "Arpeggio — overlapping", "notes together",
        "an octave and a half, each note held past the next",
        [Note(mid + d, 84, 0.3 + i * 0.42, 1.9)
         for i, d in enumerate((0, 4, 7, 12, 16, 19, 24))], tail_s=4.0,
    ))

    # Re-striking a string that is already moving is not the same as striking a
    # still one, and a voice that simply retriggers its envelope says so at once.
    out.append(Take(
        "repeated-note", "Repeated note — eight strikes on a ringing string", "at speed",
        "vel 100, 190 ms apart",
        [Note(mid, 100, 0.3 + i * 0.19, 0.14) for i in range(8)], tail_s=4.0,
    ))

    # Everything above is a probe; this is the one that says whether the result
    # is an instrument.
    out.append(Take(
        "phrase", "Phrase — melody over a bass line", "a phrase",
        "two hands, nothing held by a pedal",
        [Note(mid + d, v, 0.4 + s, dur) for d, v, s, dur in (
            (0, 100, 0.00, 0.85), (4, 88, 0.85, 0.85), (7, 92, 1.70, 0.85),
            (12, 104, 2.55, 1.30), (11, 84, 3.85, 0.60), (7, 80, 4.45, 0.60),
            (4, 76, 5.05, 1.60),
        )] + [Note(lo + d, v, 0.4 + s, dur) for d, v, s, dur in (
            (0, 78, 0.00, 1.70), (7, 70, 1.70, 1.70), (5, 72, 3.40, 1.70),
            (0, 74, 5.05, 1.60),
        )], tail_s=5.0,
    ))
    return out


def plucked_takes(program: int = 24) -> list[Take]:
    """The generic plucked set: a guitar, a bass, a harp, a koto.

    A plucked string is a struck one whose excitation sets the whole spectrum
    and then feeds it nothing, so the takes lean on the onset and on what stops
    a note — the damped stroke, the re-pluck, the strum that is not one event.
    """
    lo, mid, hi = registers_for_program(program)
    out: list[Take] = []

    out.append(Take(
        "single-mid", "Single note — plucked, let ring", "one note at a time",
        "the pluck, then nothing feeding it",
        [Note(mid, 96, 0.3, 3.0)], tail_s=3.5,
    ))

    # On a plucked string velocity moves the pluck point and the plectrum's
    # hardness as much as the level, so a voice that only scales its gain here
    # sounds like one note played louder.
    out.append(Take(
        "dynamics", "Dynamics — five velocities", "one note at a time",
        "vel 16 / 40 / 72 / 100 / 127",
        [Note(mid, v, 0.3 + i * 1.8, 1.2) for i, v in enumerate((16, 40, 72, 100, 127))],
        tail_s=3.0,
    ))

    out.append(Take(
        "register-sweep", "Register — across the compass", "one note at a time",
        "low, low-mid, mid, high-mid, high, vel 96",
        [Note(n, 96, 0.3 + i * 1.6, 1.3) for i, n in enumerate(
            (lo, (lo + mid) // 2, mid, (mid + hi) // 2, hi))], tail_s=3.5,
    ))

    # A strum is not a chord: the strings are plucked in sequence over 20 to 40
    # ms, and that spread is most of why a strummed chord sounds like one hand.
    out.append(Take(
        "strum", "Chord — strummed, not struck", "notes together",
        "five strings 25 ms apart, let ring",
        [Note(mid + d, 92, 0.3 + i * 0.025, 3.0)
         for i, d in enumerate((0, 7, 12, 16, 19))], tail_s=3.5,
    ))

    out.append(Take(
        "arpeggio-legato", "Arpeggio — overlapping, all ringing", "notes together",
        "an octave and a half, nothing damped",
        [Note(mid + d, 84, 0.3 + i * 0.34, 2.0)
         for i, d in enumerate((0, 4, 7, 12, 16, 19, 24))], tail_s=3.5,
    ))

    out.append(Take(
        "repeated-note", "Repeated note — re-plucked while ringing", "at speed",
        "vel 100, 160 ms apart, eight times",
        [Note(mid, 100, 0.3 + i * 0.16, 0.12) for i in range(8)], tail_s=3.0,
    ))

    # A plucked string stops because a hand stops it, and the damped stroke is
    # a mechanism rather than a fade. The sound between the notes is the take.
    out.append(Take(
        "muted-stroke", "Staccato — damped strokes", "the mechanism",
        "eight short notes, each stopped by the release",
        [Note(mid + d, 96, 0.3 + i * 0.45, 0.09)
         for i, d in enumerate((0, 7, 12, 16, 19, 16, 12, 7))], tail_s=3.0,
    ))

    out.append(Take(
        "phrase", "Phrase — a line over open strings", "a phrase",
        "a melody with the low notes left ringing under it",
        [Note(mid + d, v, 0.4 + s, dur) for d, v, s, dur in (
            (12, 96, 0.00, 0.40), (11, 88, 0.40, 0.40), (9, 90, 0.80, 0.40),
            (7, 88, 1.20, 0.40), (5, 92, 1.60, 0.40), (4, 88, 2.00, 0.40),
            (2, 90, 2.40, 0.40), (0, 96, 2.80, 1.00),
            (4, 88, 3.80, 0.40), (5, 86, 4.20, 0.40), (7, 92, 4.60, 0.80),
            (12, 98, 5.40, 1.40),
        )] + [Note(lo + d, 84, 0.4 + s, 2.4) for d, s in (
            (0, 0.00), (7, 1.60), (5, 3.20), (0, 4.80),
        )], tail_s=3.5,
    ))
    return out


def modal_takes(program: int = 11) -> list[Take]:
    """The generic bar/bell/membrane set: a vibraphone, a bell, a timpano.

    A modal voice's partials sit at ratios no formula predicts, and those ratios
    do not hold as the note moves: a bar bank tuned at one pitch and transposed
    everywhere else is the standard way a modal voice goes wrong, and the only
    take that shows it is the compass played in a row. Two bars sounding
    together is the other one — inharmonic partials beat at rates a harmonic
    model never produces.
    """
    lo, mid, hi = registers_for_program(program)
    out: list[Take] = []

    # A bell's ring is most of its identity and lasts longer than any window the
    # metrics take, so the single note is held until it has actually stopped.
    out.append(Take(
        "single-mid", "Single strike — let ring", "one note at a time",
        "the whole ring, well past where any metric stops looking",
        [Note(mid, 100, 0.3, 0.2)], tail_s=9.0,
    ))

    # Mallet hardness moves with velocity on a real instrument, so the high
    # partials should arrive at a different rate rather than simply louder.
    out.append(Take(
        "dynamics", "Dynamics — five velocities", "one note at a time",
        "vel 16 / 40 / 72 / 100 / 127",
        [Note(mid, v, 0.3 + i * 1.8, 0.2) for i, v in enumerate((16, 40, 72, 100, 127))],
        tail_s=4.0,
    ))

    # Where a transposed mode bank gives itself away.
    out.append(Take(
        "register-sweep", "Register — across the compass", "one note at a time",
        "low, low-mid, mid, high-mid, high, vel 100",
        [Note(n, 100, 0.3 + i * 1.8, 0.2) for i, n in enumerate(
            (lo, (lo + mid) // 2, mid, (mid + hi) // 2, hi))], tail_s=4.0,
    ))

    # Two bars a third apart. On a harmonic voice this beats at the difference
    # of two fundamentals; on a real bar bank the upper modes beat too, at rates
    # that have nothing to do with the interval.
    out.append(Take(
        "interval", "Two bars together — a third", "notes together",
        "struck at once, let ring",
        [Note(mid + d, 96, 0.3, 0.2) for d in (0, 4)], tail_s=8.0,
    ))

    # The roll is how a mallet instrument sustains at all, and it is a stack of
    # overlapping rings rather than a held note.
    out.append(Take(
        "roll", "Roll — sixteen strokes on one bar", "at speed",
        "80 ms apart, alternating accents",
        [Note(mid, 104 if i % 2 == 0 else 88, 0.3 + i * 0.08, 0.06)
         for i in range(16)], tail_s=5.0,
    ))

    out.append(Take(
        "phrase", "Phrase — a line with the bars left ringing", "a phrase",
        "nothing damped, so every note is heard against the last four",
        [Note(mid + d, v, 0.4 + s, 0.2) for d, v, s in (
            (0, 96, 0.00), (4, 88, 0.35), (7, 92, 0.70), (12, 104, 1.05),
            (11, 84, 1.60), (7, 88, 1.95), (4, 90, 2.30), (0, 96, 2.65),
            (7, 88, 3.20), (12, 96, 3.55), (16, 100, 3.90), (19, 104, 4.25),
        )], tail_s=7.0,
    ))
    return out


def noise_takes(program: int = 120) -> list[Take]:
    """The set for a voice with no pitch: the sound effects, the reverse cymbal.

    There is no compass to sweep and no chord to hold, so what is left is the
    gesture itself — one trigger heard whole, the velocity response, and what
    happens when it is retriggered before it has finished.
    """
    _, mid, _ = registers_for_program(program)
    out: list[Take] = []

    out.append(Take(
        "single", "One trigger — heard whole", "one at a time",
        "held two seconds, then left to finish",
        [Note(mid, 100, 0.3, 2.0)], tail_s=6.0,
    ))

    out.append(Take(
        "dynamics", "Dynamics — five velocities", "one at a time",
        "vel 16 / 40 / 72 / 100 / 127",
        [Note(mid, v, 0.3 + i * 2.4, 1.5) for i, v in enumerate((16, 40, 72, 100, 127))],
        tail_s=4.0,
    ))

    # A one-shot retriggered before it finishes either steals its own voice,
    # layers, or restarts — three audibly different behaviours that no per-note
    # measurement here distinguishes.
    out.append(Take(
        "retrigger", "Retriggered — before it has finished", "at speed",
        "six triggers 400 ms apart",
        [Note(mid, 100, 0.3 + i * 0.4, 0.3) for i in range(6)], tail_s=5.0,
    ))
    return out


# --------------------------------------------------------------------------- #
# The musical take — the one nothing measures
# --------------------------------------------------------------------------- #
# Every other take isolates one thing. A voice can hold all of them and still be
# unpleasant across ten seconds of a real line, which is a judgement only a
# listener makes: how a chord bloom sits under a moving part, whether a decay
# leaves room for the next entry, where the register balance goes on the way up.
#
# One passage per tone class rather than per instrument, for the same reason the
# generic sets are per class: every program needs one and four have a capture. A
# capture may name its own with a `music` key, which is how the organ gets a
# chorale with a pedal part and the harpsichord a fugue.

EXCERPT_DIR = Path(__file__).resolve().parent / "excerpts"

#: Class to committed excerpt. See `extract_excerpt.py` for what each catches.
MUSIC_EXCERPTS: dict[ToneClass, str] = {
    ToneClass.STRUCK_STRING: "bwv846-prelude",
    ToneClass.PLUCKED_STRING: "bwv996-prelude",
    # One line across a wide compass, which is what a monophonic bowed or blown
    # voice is asked to do. A three-part chorale would be a better organ take
    # and a strange flute one, so the organ names that itself.
    ToneClass.SUSTAINED: "bwv1007-prelude",
    ToneClass.MODAL: "bwv847-fugue",
    # Nothing with a pitch belongs to this class, so a melodic line says nothing
    # about it. Drum kits are the same case and are covered by their own set.
    ToneClass.NOISE: "",
}


def load_excerpt(ident: str) -> dict | None:
    path = EXCERPT_DIR / f"{ident}.json"
    return json.loads(path.read_text()) if path.is_file() else None


def _octave_shift(pitches: list[int], compass: tuple[int, int, int]) -> int:
    """Whole octaves to move a passage into a program's compass.

    Whole octaves rather than semitones because the passage is in a key and a
    transposition that is not an octave puts it in a different one — which for a
    voice with fixed formants or a modal bank is a different instrument, and for
    a listener is just wrong.

    Nothing moves where the passage already fits. Centring it on the program's
    middle register instead would transpose a keyboard prelude that Bach put
    where a keyboard is, because the arpeggios reach above the middle and pull
    the median up with them.
    """
    lo, _, hi = compass
    low, high = min(pitches), max(pitches)
    if low >= lo and high <= hi:
        return 0

    def outside(k: int) -> tuple[int, int]:
        under = max(0, lo - (low + 12 * k))
        over = max(0, (high + 12 * k) - hi)
        return under + over, abs(k)

    return min(range(-4, 5), key=outside)


def music_take(program: int, *, excerpt: str = "") -> Take | None:
    """Ten seconds of real music, in this program's own compass.

    None where the class has no excerpt, and none where the committed excerpt is
    missing — a page without it is a page with one take fewer, not a failure.
    """
    ident = excerpt or MUSIC_EXCERPTS.get(tone_class(program), "")
    if not ident:
        return None
    data = load_excerpt(ident)
    if not data:
        return None
    compass = registers_for_program(program)
    shift = 12 * _octave_shift([n["pitch"] for n in data["notes"]], compass)
    notes = [Note(n["pitch"] + shift, n["velocity"], n["start"], n["duration"])
             for n in data["notes"]]
    sub = data["note"]
    if shift:
        sub += f" ({shift // 12:+d} octave{'s' if abs(shift) > 12 else ''})"
    return Take("music", data["label"], "a real line", sub, notes, tail_s=4.0)


def with_music(takes: list[Take], program: int, *, excerpt: str = "") -> list[Take]:
    """A set plus its musical take, which always comes last."""
    take = music_take(program, excerpt=excerpt)
    return takes + [take] if take else takes


#: Phrase sets by name. A capture definition names the one its instrument needs;
#: a program with no capture gets the generic set for its tone class.
TAKE_SETS = {
    "piano": piano_takes,
    "harpsichord": harpsichord_takes,
    "drums": drum_takes,
    "sustained": sustained_takes,
    "struck": struck_takes,
    "plucked": plucked_takes,
    "modal": modal_takes,
    "noise": noise_takes,
}

#: The generic set each tone class gets. One entry per `ToneClass`, so a program
#: always resolves to something — which is what lets the audition index be the
#: library's own catalogue rather than the list of captures that happen to exist.
GENERIC_SETS: dict[ToneClass, str] = {
    ToneClass.SUSTAINED: "sustained",
    ToneClass.STRUCK_STRING: "struck",
    ToneClass.PLUCKED_STRING: "plucked",
    ToneClass.MODAL: "modal",
    ToneClass.NOISE: "noise",
}


def take_set_for(program: int, *, drum_note: int | None = None) -> str:
    """The generic phrase set a program gets when no capture names one."""
    if drum_note is not None:
        return "drums"
    return GENERIC_SETS[tone_class(program)]


def build_takes(name: str, program: int, *, music: str | None = None) -> list[Take]:
    """The phrases of a named set, with a program's own compass filled in.

    `music` names the excerpt this voice's musical take uses: None leaves it
    off, empty takes the class default, an id overrides it.

    **Off by default, because the musical take is not a measurement.** Nothing
    scores it — it is ten seconds of overlapping polyphony judged by ear — and
    the take-measurement path reads every take in this set as isolated notes
    against a reference. An audition run asks for it; `profile.py` does not.
    """
    if name not in TAKE_SETS:
        raise KeyError(f"no phrase set named {name!r}; have {', '.join(TAKE_SETS)}")
    takes = TAKE_SETS[name](program)
    # A kit's note numbers select instruments rather than pitches, so a melodic
    # line played on one is 48 different drums.
    if music is None or any(t.channel == DRUM_CHANNEL for t in takes):
        return takes
    return with_music(takes, program, excerpt=music)


def is_drum_set(name: str) -> bool:
    """Whether a set's note numbers select instruments rather than pitches.

    Derived from the phrases rather than declared beside them: the channel is
    already the thing that decides it, and a second field saying the same thing
    is a field that can disagree with it.
    """
    return any(t.channel == DRUM_CHANNEL for t in TAKE_SETS[name](0))
