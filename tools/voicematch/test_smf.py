"""Tests for the probe's Standard MIDI File writer and its program-change strip.

The strip is what keeps a probe from reprogramming the plugin it is meant to
record, and what it must not do is move a note. A shifted probe is not a failure
anyone sees: every analysis window is derived from the score, so a file whose
notes drifted is measured at the wrong instants and reads as an instrument with
a slow attack.

    rye run --pyproject bindings/python/pyproject.toml \
        python -m pytest tools/voicematch/test_smf.py
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from smf import Note, _read_vlq, _vlq, strip_program_changes, write_smf  # noqa: E402


def events(data: bytes) -> list[tuple[int, bytes]]:
    """Every event in a single-track file as (absolute tick, raw bytes)."""
    assert data.startswith(b"MThd")
    pos = 8 + int.from_bytes(data[4:8], "big")
    assert data[pos : pos + 4] == b"MTrk"
    length = int.from_bytes(data[pos + 4 : pos + 8], "big")
    body = data[pos + 8 : pos + 8 + length]

    out: list[tuple[int, bytes]] = []
    pos, tick, status = 0, 0, 0
    while pos < len(body):
        delta, pos = _read_vlq(body, pos)
        tick += delta
        start = pos
        if body[pos] & 0x80:
            status = body[pos]
            pos += 1
        if status == 0xFF:
            pos += 1
            size, pos = _read_vlq(body, pos)
            pos += size
        elif 0xC0 <= status < 0xE0:
            pos += 1
        else:
            pos += 2
        out.append((tick, body[start:pos]))
    return out


def a_probe(*, program: int = 12) -> bytes:
    return write_smf(
        [Note(38, 100, 0.5, 0.05), Note(42, 64, 1.5, 0.05)],
        program=program,
        channel=9,
        end_pad=1.0,
        cc_events=((0.25, 64, 127),),
    )


@pytest.mark.parametrize("value", [0, 1, 127, 128, 8192, 0x0FFFFFFF])
def test_a_variable_length_quantity_round_trips(value):
    decoded, end = _read_vlq(_vlq(value), 0)
    assert (decoded, end) == (value, len(_vlq(value)))


def test_a_program_change_is_written_only_when_one_is_asked_for():
    assert any(e[0] & 0xF0 == 0xC0 for _, e in events(a_probe(program=12)))
    assert not any(e[0] & 0xF0 == 0xC0 for _, e in events(a_probe(program=-1)))


def test_stripping_removes_the_program_change():
    stripped = strip_program_changes(a_probe())
    assert not any(e[0] & 0xF0 == 0xC0 for _, e in events(stripped))


def test_stripping_leaves_every_other_event_where_it_was():
    before = [(tick, raw) for tick, raw in events(a_probe()) if raw[0] & 0xF0 != 0xC0]
    assert events(strip_program_changes(a_probe())) == before


def test_stripping_a_file_that_has_none_changes_nothing():
    probe = a_probe(program=-1)
    assert strip_program_changes(probe) == probe


def test_stripping_rejects_something_that_is_not_a_midi_file():
    with pytest.raises(ValueError):
        strip_program_changes(b"RIFF....WAVEfmt ")
