"""Tests for the SoundFont corpus reader.

Nothing here reads a SoundFont from disk. Every font is built by `_font` from
known samples, so a round trip has to recover the frames it was written from —
and, more importantly, every refusal gets a font that should trip it. A reader
whose job is to refuse needs its refusals exercised or it is a comment: the set
this was written against passes all seventeen of its files, which is exactly the
evidence that cannot show the refusal works.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent))

from sf2 import SoundFont, UnplayableAsCorpus

#: SF2 requires 46 zero frames between samples, so a player that runs off the
#: end of one reads silence rather than the next.
_GUARD = 46


def _font(
    tmp_path: Path,
    *,
    zones: list[tuple[int, int, int]],
    audio: list[np.ndarray],
    pitches: list[int],
    extra_igen: list[tuple[int, int]] = (),
    rate: int = 44100,
    name: str = "test",
) -> Path:
    """Write a one-preset, one-instrument SoundFont.

    `zones` is one `(key_lo, key_hi, sample index)` per instrument zone;
    `extra_igen` is appended to the FIRST zone, which is how a test asks for a
    generator the reader is meant to refuse.
    """
    smpl = bytearray()
    shdr = bytearray()
    for i, (frames, pitch) in enumerate(zip(audio, pitches)):
        start = len(smpl) // 2
        pcm = np.clip(np.asarray(frames), -1.0, 1.0)
        smpl += (np.round(pcm * 32767.0).astype("<i2")).tobytes()
        end = len(smpl) // 2
        smpl += b"\x00\x00" * _GUARD
        shdr += struct.pack(
            "<20sIIIIIBbHH", f"s{i}".encode(), start, end, start, end, rate,
            pitch, 0, 0, 1,
        )
    shdr += struct.pack("<20sIIIIIBbHH", b"EOS", 0, 0, 0, 0, 0, 0, 0, 0, 0)

    igen, ibag = bytearray(), bytearray()
    for n, (lo, hi, sample) in enumerate(zones):
        ibag += struct.pack("<HH", len(igen) // 4, 0)
        gens = [(43, (hi << 8) | lo)]
        if n == 0:
            gens += list(extra_igen)
        gens.append((53, sample))  # sampleID is the terminal generator of a zone
        for op, amount in gens:
            igen += struct.pack("<Hh", op, amount)
    ibag += struct.pack("<HH", len(igen) // 4, 0)  # terminal bag
    igen += struct.pack("<Hh", 0, 0)  # terminal gen

    inst = struct.pack("<20sH", b"inst", 0) + struct.pack("<20sH", b"EOI", len(zones))
    pgen = struct.pack("<Hh", 41, 0) + struct.pack("<Hh", 0, 0)
    pbag = struct.pack("<HH", 0, 0) + struct.pack("<HH", 1, 0)
    phdr = (
        struct.pack("<20sHHHIII", name.encode(), 0, 0, 0, 0, 0, 0)
        + struct.pack("<20sHHHIII", b"EOP", 0, 0, 1, 0, 0, 0)
    )
    terminal_mod = b"\x00" * 10

    def chunk(cid: bytes, body: bytes) -> bytes:
        pad = b"\x00" if len(body) & 1 else b""
        return cid + struct.pack("<I", len(body)) + body + pad

    info = chunk(b"ifil", struct.pack("<HH", 2, 1)) + chunk(b"INAM", name.encode() + b"\x00")
    sdta = chunk(b"smpl", bytes(smpl))
    pdta = (
        chunk(b"phdr", phdr) + chunk(b"pbag", pbag) + chunk(b"pmod", terminal_mod)
        + chunk(b"pgen", pgen) + chunk(b"inst", inst) + chunk(b"ibag", bytes(ibag))
        + chunk(b"imod", terminal_mod) + chunk(b"igen", bytes(igen))
        + chunk(b"shdr", bytes(shdr))
    )
    body = (
        b"sfbk"
        + chunk(b"LIST", b"INFO" + info)
        + chunk(b"LIST", b"sdta" + sdta)
        + chunk(b"LIST", b"pdta" + pdta)
    )
    path = tmp_path / f"{name}.sf2"
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)
    return path


def _burst(seconds: float = 0.05, freq: float = 440.0, rate: int = 44100) -> np.ndarray:
    t = np.arange(int(seconds * rate)) / rate
    return 0.5 * np.sin(2 * np.pi * freq * t)


@pytest.fixture
def clean(tmp_path: Path) -> Path:
    """A two-note font that sets nothing but the zone map: a corpus."""
    return _font(
        tmp_path,
        zones=[(58, 62, 0), (70, 74, 1)],
        audio=[_burst(freq=261.6), _burst(freq=440.0)],
        pitches=[60, 72],
    )


def test_a_font_that_sets_only_the_zone_map_is_a_corpus(clean: Path):
    with SoundFont(clean) as sf:
        sf.require_corpus()
        assert len(sf.samples) == 2
        assert sf.find(0, 0) is not None


def test_extracted_frames_are_the_frames_that_were_written(clean: Path):
    with SoundFont(clean) as sf:
        notes = sf.note_map(sf.find(0, 0))
        audio, rate = sf.extract(notes[60])
        assert rate == 44100
        # 16-bit round trip, so agreement is to a quantisation step rather than exact.
        assert np.max(np.abs(audio - _burst(freq=261.6))) < 2.0 / 32768.0


def test_a_note_without_a_sample_of_its_own_is_absent_rather_than_stretched(clean: Path):
    """The zones cover 58-62 and 70-74; only 60 and 72 were recorded."""
    with SoundFont(clean) as sf:
        notes = sf.note_map(sf.find(0, 0))
        assert sorted(notes) == [60, 72]
        for stretched in (58, 59, 61, 62, 70, 71, 73, 74):
            assert stretched not in notes


# --------------------------------------------------------------------------
# The refusals. Each font below differs from `clean` in exactly one generator.


@pytest.mark.parametrize(
    "gen,amount,wanted",
    [
        ((8, 6900), None, "initialFilterFc"),   # a lowpass at ~1 kHz
        ((9, 60), None, "initialFilterQ"),
        ((13, 120), None, "modLfoToVolume"),
        ((48, 250), None, "initialAttenuation"),
        ((51, 12), None, "coarseTune"),
        ((54, 1), None, "sampleModes"),         # the sample is meant to loop
        ((44, (100 << 8) | 0), None, "velRange"),
        ((17, 500), None, "pan"),
        ((16, 400), None, "reverbEffectsSend"),
    ],
)
def test_a_shaping_generator_is_refused(tmp_path: Path, gen, amount, wanted):
    path = _font(
        tmp_path,
        zones=[(58, 62, 0), (70, 74, 1)],
        audio=[_burst(), _burst()],
        pitches=[60, 72],
        extra_igen=[gen],
        name=f"shaped_{wanted}",
    )
    with SoundFont(path) as sf:
        assert wanted in sf.extraction_report()["shaping"]
        with pytest.raises(UnplayableAsCorpus, match=wanted):
            sf.require_corpus()


@pytest.mark.parametrize(
    "gen,wanted",
    [
        ((34, -3000), "attack"),     # ~188 ms attack
        ((37, 200), "sustain"),      # pulled 20 dB under the recording
        ((36, 1200), "decay"),       # 2 s to that sustain
    ],
)
def test_an_envelope_that_shapes_the_note_is_refused(tmp_path: Path, gen, wanted):
    path = _font(
        tmp_path,
        zones=[(58, 62, 0), (70, 74, 1)],
        audio=[_burst(), _burst()],
        pitches=[60, 72],
        extra_igen=[gen],
        name=f"env_{wanted}",
    )
    with SoundFont(path) as sf:
        assert wanted in sf.extraction_report()["envelope_shapes"]
        with pytest.raises(UnplayableAsCorpus, match="volume envelope"):
            sf.require_corpus()


@pytest.mark.parametrize(
    "gen",
    [
        (34, -8359),  # 8 ms attack: a click guard, not shaping
        (35, -1200),
        (36, 6008),   # 32 s decay, reaching a -3 dB sustain
        (37, 30),
        (38, -2084),
        (17, 0),      # centred pan
        (22, -851),   # an LFO rate with nothing routed to it
        (24, 1),
    ],
)
def test_a_pass_through_generator_is_not_refused(tmp_path: Path, gen):
    """The envelope the set this was written against carries, one field at a time.

    Without this the refusals above could be satisfied by refusing everything,
    which would be a reader that reads nothing.
    """
    path = _font(
        tmp_path,
        zones=[(58, 62, 0), (70, 74, 1)],
        audio=[_burst(), _burst()],
        pitches=[60, 72],
        extra_igen=[gen],
        name=f"inert_{gen[0]}",
    )
    with SoundFont(path) as sf:
        sf.require_corpus()


def test_an_unmodelled_generator_is_refused_rather_than_ignored(tmp_path: Path):
    """Generator 7 is modEnvToPitch; 31 is keynumToModEnvHold, which this reader
    has no opinion about at all. Both have to stop the read — silently ignoring
    a generator is how a reader reports its own guess as a measurement."""
    path = _font(
        tmp_path,
        zones=[(58, 62, 0), (70, 74, 1)],
        audio=[_burst(), _burst()],
        pitches=[60, 72],
        extra_igen=[(31, 100)],
        name="unknown_gen",
    )
    with SoundFont(path) as sf:
        assert 31 in sf.extraction_report()["unknown_generators"]
        with pytest.raises(UnplayableAsCorpus, match="does not model"):
            sf.require_corpus()


def test_a_file_that_is_not_a_soundfont_is_refused(tmp_path: Path):
    path = tmp_path / "not.sf2"
    path.write_bytes(b"RIFF" + struct.pack("<I", 4) + b"WAVE")
    with pytest.raises(UnplayableAsCorpus, match="not sfbk"):
        SoundFont(path)
