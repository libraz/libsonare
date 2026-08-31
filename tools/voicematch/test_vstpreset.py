"""Tests for the `.vstpreset` reader and the audio-unit state it builds.

The conversion these cover fails silently in production — a plugin handed a
blob it does not recognise renders a default instrument at a normal level —
so every step is checked here against bytes assembled by hand rather than
against a plugin that has to be installed.

    rye run --pyproject bindings/python/pyproject.toml \
        python -m pytest tools/voicematch/test_vstpreset.py
"""

from __future__ import annotations

import plistlib
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from vstpreset import (  # noqa: E402
    VstPresetError,
    build_aupreset,
    component_codes,
    read_vstpreset,
    state_sizes,
)

#: An instrument triple no plugin claims, so nothing here depends on what is installed.
TRIPLE = "aumu:Tst1:Acme"
#: `VST3`, a format version and a 32-character class id: 48 bytes with the
#: chunk-list offset that follows, which is where a chunk may first begin.
HEADER = b"VST3" + struct.pack("<i", 1) + b"V" * 32


def write_vstpreset(path: Path, chunks: dict[str, bytes]) -> Path:
    """A `.vstpreset` carrying @p chunks, laid out as the format specifies."""
    body = b""
    placed: list[tuple[str, int, int]] = []
    for chunk_id, payload in chunks.items():
        placed.append((chunk_id, 48 + len(body), len(payload)))
        body += payload
    entries = b"List" + struct.pack("<i", len(placed))
    for chunk_id, offset, size in placed:
        entries += chunk_id.encode("ascii") + struct.pack("<qq", offset, size)
    path.write_bytes(HEADER + struct.pack("<q", 48 + len(body)) + body + entries)
    return path


def test_reads_every_chunk_by_id(tmp_path):
    path = write_vstpreset(
        tmp_path / "rack.vstpreset",
        {"Comp": b"component state", "Cont": b"", "Info": b"<xml/>"},
    )
    chunks = read_vstpreset(path)
    assert chunks["Comp"] == b"component state"
    assert chunks["Cont"] == b""
    assert chunks["Info"] == b"<xml/>"


def test_a_file_that_is_not_a_vstpreset_is_refused(tmp_path):
    path = tmp_path / "not.vstpreset"
    path.write_bytes(b"RIFF" + b"\x00" * 64)
    with pytest.raises(VstPresetError, match="not a .vstpreset"):
        read_vstpreset(path)


def test_component_codes_pack_four_characters_big_endian():
    au_type, subtype, manufacturer = component_codes(TRIPLE)
    assert au_type == 0x61756D75  # 'aumu'
    assert subtype == 0x54737431  # 'Tst1'
    assert manufacturer == 0x41636D65  # 'Acme'


@pytest.mark.parametrize("bad", ["aumu:Tst1", "aumu:Tst1:Acme:x", "au:Tst1:Acme"])
def test_a_malformed_triple_is_refused(bad):
    with pytest.raises(VstPresetError, match="four-character"):
        component_codes(bad)


def test_the_aupreset_carries_the_state_under_the_named_key():
    plist = plistlib.loads(build_aupreset(b"blob", triple=TRIPLE, name="rack", key="vstdata"))
    assert plist["vstdata"] == b"blob"
    assert plist["name"] == "rack"
    assert plist["type"] == 0x61756D75
    # Only the key that was asked for. An empty second key would load as an
    # empty rack in a plugin that reads it, which looks like a preset that took.
    assert "data" not in plist


def test_the_state_key_is_selectable():
    plist = plistlib.loads(build_aupreset(b"blob", triple=TRIPLE, name="r", key="data"))
    assert plist["data"] == b"blob"
    assert "vstdata" not in plist


def test_state_sizes_reads_an_aubounce_report():
    report = """Acme: Test Sampler
  triple    aumu:Tst1:Acme
  state     7 entries
    manufacturer  1097101413
    data  (0 bytes)
    vstdata  (4013818 bytes)
    name  Test Sampler
"""
    sizes = state_sizes(report)
    assert sizes == {"data": 0, "vstdata": 4013818}
    # An integer-valued entry is not a size and must not read as one.
    assert "manufacturer" not in sizes
