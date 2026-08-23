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
    bank: int = 0,
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

    `bank` is the GS variation number, written as Bank Select MSB (CC0) with the
    LSB zeroed, ahead of the program change. It is what reaches a capital tone's
    variations at all: libsonare voices program 19 as a six-rank principal
    chorus at bank 0, a three-rank flute registration at bank 8 and a full organ
    with reeds at bank 16, and a harness that emits no bank select can render
    only the first of the three. Zero is the capital tone and emits nothing, so
    a caller that does not care is unaffected.

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
        # Bank select precedes the program change: a module latches the bank and
        # applies it when the program arrives, so the other order selects the
        # capital tone and leaves the bank for whatever program comes next.
        if bank:
            track += _vlq(0) + bytes([0xB0 | channel, 0, bank & 0x7F])
            track += _vlq(0) + bytes([0xB0 | channel, 32, 0])
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


def _read_vlq(data: bytes, pos: int) -> tuple[int, int]:
    """Decode the variable-length quantity at `pos`; returns (value, next position)."""
    value = 0
    while True:
        byte = data[pos]
        pos += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, pos


def strip_program_changes(data: bytes) -> bytes:
    """Return the same file with every program change removed, timing intact.

    A program change is an instruction to a General MIDI synth and a question to
    anything else. A plugin selected by a preset already holds the sound the
    probe is meant to record, and what it does with a program number on top of
    that is its own business: a multitimbral rack loads a different program into
    the slot, so the file that comes back is the right length, the right shape,
    free of dropouts and playing an instrument nobody asked for.

    Measured on a a multitimbral rack: a snare struck at 0.1 s renders identically
    with and without a leading program change, and the same snare struck at 1.0 s
    or later renders at 0.365 peak instead of 0.898, because the program loads
    asynchronously and the first note beats it. So a one-note probe agrees with
    itself and a real probe does not, which is why this is stripped rather than
    left to be noticed.

    The event's delta time is carried onto the next event rather than dropped, so
    nothing after it moves.
    """
    if not data.startswith(b"MThd"):
        raise ValueError("not a Standard MIDI File")
    out = bytearray(data[: 8 + int.from_bytes(data[4:8], "big")])
    pos = len(out)
    while pos < len(data):
        if data[pos : pos + 4] != b"MTrk":
            raise ValueError(f"expected a track chunk at byte {pos}")
        length = int.from_bytes(data[pos + 4 : pos + 8], "big")
        body, pos = data[pos + 8 : pos + 8 + length], pos + 8 + length
        kept = _strip_track(body)
        out += b"MTrk" + len(kept).to_bytes(4, "big") + kept
    return bytes(out)


def _strip_track(body: bytes) -> bytearray:
    """One track's events, minus the program changes, with their deltas carried on."""
    kept = bytearray()
    pos, status, carry = 0, 0, 0
    while pos < len(body):
        delta, pos = _read_vlq(body, pos)
        delta += carry
        start = pos
        if body[pos] & 0x80:
            status = body[pos]
            pos += 1
        if status in (0xFF, 0xF0, 0xF7):
            if status == 0xFF:
                pos += 1  # meta type
            size, pos = _read_vlq(body, pos)
            pos += size
        elif 0xC0 <= status < 0xE0:  # program change and channel pressure carry one byte
            pos += 1
        else:
            pos += 2
        if 0xC0 <= status < 0xD0:
            carry = delta
            continue
        carry = 0
        kept += _vlq(delta) + body[start:pos]
    return kept
