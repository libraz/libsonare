"""Minimal type-0 Standard MIDI File writer for the voice-match harness.

Only what the harness needs: one channel, an optional program change, and a
list of timed note on/off events. Both the reference renderer (fluidsynth) and
the model renderer are driven from the same note list, so the .mid this writes
is the single source of truth for the reference side.

The channel matters for one thing: 9 (channel 10 in one-based numbering) is the
GM drum channel, where a note number selects a percussion instrument rather than
a pitch and the program change selects a kit. libsonare and a GM reference synth
both honour that convention, so a drum probe is the same file with a different
channel nibble.
"""

from __future__ import annotations

from dataclasses import dataclass

TPQ = 480          # ticks per quarter note
TEMPO_US = 500000  # microseconds per quarter (120 BPM)
TICKS_PER_SEC = TPQ * 1_000_000 // TEMPO_US  # 960 at 120 BPM


@dataclass(frozen=True)
class Note:
    """A single timed note: MIDI number, velocity, onset and duration (seconds)."""

    note: int
    velocity: int
    start: float
    dur: float


def _vlq(value: int) -> bytes:
    """Encode an unsigned int as a MIDI variable-length quantity."""
    if value < 0:
        raise ValueError("VLQ cannot encode a negative value")
    out = bytearray([value & 0x7F])
    value >>= 7
    while value:
        out.insert(0, (value & 0x7F) | 0x80)
        value >>= 7
    return bytes(out)


def _sec_to_ticks(sec: float) -> int:
    return int(round(sec * TICKS_PER_SEC))


def write_smf(
    notes: list[Note],
    *,
    program: int = 0,
    channel: int = 0,
    end_pad: float = 0.0,
    sends: tuple[int | None, int | None, int | None] = (0, 0, 0),
    cc_events: tuple[tuple[float, int, int], ...] = (),
) -> bytes:
    """Serialize `notes` to a type-0 SMF byte string.

    A tempo meta-event and (unless `program` is negative) a program change are
    emitted first so a GM reference synth selects the intended instrument.
    `end_pad` delays the end-of-track marker past the last event, which keeps a
    fast-render reference synth (fluidsynth -F) running long enough to capture
    the release tail.

    `sends` is the (reverb, chorus, delay) controller values written at tick 0
    as CC91 / CC93 / CC94; `None` for one of them leaves that controller alone,
    so the module keeps its power-on value. The default zeroes all three, which
    is what a dry-versus-dry comparison needs: libsonare powers on at the GS
    default CC91 of 40, weighted per program by `gm_fallback_sends`, so a
    church organ would arrive with a cathedral on it while the dry reference
    render has none — and every release / tone-to-noise metric would read that
    room as timbre. Pass a non-zero reverb when the reference itself is wet
    (see `room.py`).

    `cc_events` are (second, controller, value) triples placed in the timeline
    alongside the notes. What needs them is the sustain pedal: on a piano CC64
    is not an effect but part of the instrument — it lifts the dampers off
    every string, so the notes ring past their note-offs and the strings nobody
    played ring in sympathy. A piano reference compared without it is compared
    on half of its behaviour.
    """
    # (absolute_tick, status, data1, data2) event tuples, then delta-encoded.
    events: list[tuple[int, int, int, int]] = []
    for n in notes:
        on = _sec_to_ticks(n.start)
        off = _sec_to_ticks(n.start + n.dur)
        events.append((on, 0x90 | channel, n.note, n.velocity))
        events.append((off, 0x80 | channel, n.note, 0))
    for at, cc, value in cc_events:
        events.append((_sec_to_ticks(at), 0xB0 | channel, cc & 0x7F, max(0, min(127, int(value)))))
    # Stable sort by tick keeps note-off before a same-tick note-on of the next
    # event only if ordered; ties are fine for distinct notes here.
    events.sort(key=lambda e: e[0])

    track = bytearray()
    # Tempo meta-event at tick 0.
    track += _vlq(0) + bytes([0xFF, 0x51, 0x03]) + TEMPO_US.to_bytes(3, "big")
    if program >= 0:
        track += _vlq(0) + bytes([0xC0 | channel, program & 0x7F])
    for cc, value in zip((91, 93, 94), sends):
        if value is not None:
            track += _vlq(0) + bytes([0xB0 | channel, cc, max(0, min(127, int(value)))])

    prev = 0
    for tick, status, d1, d2 in events:
        track += _vlq(tick - prev) + bytes([status, d1, d2])
        prev = tick
    track += _vlq(_sec_to_ticks(max(0.0, end_pad))) + bytes([0xFF, 0x2F, 0x00])  # end of track

    header = b"MThd" + (6).to_bytes(4, "big") + (0).to_bytes(2, "big") + (1).to_bytes(2, "big") + TPQ.to_bytes(2, "big")
    track_chunk = b"MTrk" + len(track).to_bytes(4, "big") + bytes(track)
    return header + track_chunk
