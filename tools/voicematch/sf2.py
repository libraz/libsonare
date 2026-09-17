"""Read a SoundFont as a corpus of recorded notes, never as an instrument to play.

A SoundFont holds two things: recorded audio, and the generators a player applies
to it. This module reads the first and **refuses** a file that needs the second.
That refusal is the whole point. A reference capture is what the source actually
produced; running the samples through a half-implemented player would make it what
this file's author guessed the source produced, and nothing downstream could tell
the two apart — every metric would still compute, every gate would still pass.

So `extraction_report` walks every generator the file sets and names the ones that
would change what a note sounds like. A file that sets any of them is not a corpus
and is refused rather than approximated. A file that sets none is a set of
recordings in a RIFF container, and reading it is exact.

Two further refusals, both for the same reason:

- **A note is extracted only where the file holds a sample recorded at that
  pitch.** A player would stretch the neighbour; a capture that did so would
  report a resampled E as a measurement of E. A note with no sample of its own
  comes back missing, and the caller reports it as un-reached.
- **A velocity axis is never invented.** Where the file declares no `velRange`,
  what it holds is one layer at an unknown velocity, and writing it out under
  several velocity labels would manufacture the axis a dynamics fit reads.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Self

import numpy as np

#: SF2 generator operators this module understands as structure rather than as
#: sound: the zone map, and the fields that say which sample a zone plays.
_STRUCTURAL = {43, 53, 41}  # keyRange, sampleID, instrument

#: Generators that change what a note sounds like, with what a player would do.
#: A file setting any of these needs a player, so it is not a corpus. `pan` is
#: here too: a corpus's channel count is part of its identity.
_SHAPING = {
    5: "modLfoToPitch", 6: "vibLfoToPitch", 7: "modEnvToPitch",
    8: "initialFilterFc", 9: "initialFilterQ", 10: "modLfoToFilterFc",
    11: "modEnvToFilterFc", 13: "modLfoToVolume", 15: "chorusEffectsSend",
    16: "reverbEffectsSend", 17: "pan",
    25: "delayModEnv", 26: "attackModEnv", 27: "holdModEnv", 28: "decayModEnv",
    29: "sustainModEnv", 30: "releaseModEnv",
    44: "velRange", 46: "keynum", 47: "velocity", 48: "initialAttenuation",
    51: "coarseTune", 52: "fineTune", 54: "sampleModes", 56: "scaleTuning",
    58: "overridingRootKey",
}

#: The volume envelope, judged rather than refused outright — every SoundFont
#: writes one and most write a pass-through. `_envelope_shapes` decides.
_VOL_ENV = {33: "delayVolEnv", 34: "attackVolEnv", 35: "holdVolEnv",
            36: "decayVolEnv", 37: "sustainVolEnv", 38: "releaseVolEnv"}

#: Attack past this is audible shaping rather than a click guard.
_ATTACK_CEILING_MS = 20.0
#: Sustain below full level means the envelope pulls the note down under the
#: recording, in decibels of attenuation (the SF2 unit is 0.1 dB).
_SUSTAIN_CEILING_DB = 6.0
#: Decay faster than this reaches that sustain inside a note worth measuring.
_DECAY_FLOOR_S = 20.0


def _timecents_s(tc: int) -> float:
    return float(2.0 ** (tc / 1200.0))


@dataclass(frozen=True)
class Sample:
    """One recorded note: where it sits in the `smpl` chunk, and what it is."""

    name: str
    start: int
    end: int
    sample_rate: int
    original_pitch: int
    #: SF2 `sampleType`. 1 is mono; a linked pair is 2/4 and carries a partner.
    kind: int
    link: int

    @property
    def frames(self) -> int:
        return self.end - self.start

    @property
    def seconds(self) -> float:
        return self.frames / float(self.sample_rate)


@dataclass(frozen=True)
class Preset:
    name: str
    bank: int
    program: int
    #: `(instrument index, ...)` this preset plays, in the order it lists them.
    instruments: tuple[int, ...]


class UnplayableAsCorpus(ValueError):
    """The file needs a player, so reading its samples would not be a measurement."""


class SoundFont:
    """A parsed SoundFont, kept open so samples are read by seek rather than held.

    The audio of one family file here runs to 862 MB, so the `smpl` chunk is
    never loaded: `extract` seeks to a sample's own frames and reads those.
    """

    def __init__(self, path: Path | str) -> None:
        self.path = Path(path).expanduser()
        self._fh = self.path.open("rb")
        self._smpl_off = 0
        self._pdta: dict[bytes, bytes] = {}
        self.info: dict[str, str] = {}
        self._read_chunks()
        self.samples = self._read_samples()
        self.presets = self._read_presets()

    def close(self) -> None:
        self._fh.close()

    def __enter__(self) -> Self:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    # ---------------------------------------------------------------- parsing

    def _read_chunks(self) -> None:
        fh = self._fh
        size = self.path.stat().st_size
        if fh.read(4) != b"RIFF":
            raise UnplayableAsCorpus(f"{self.path.name}: not a RIFF file")
        fh.seek(8)
        if fh.read(4) != b"sfbk":
            raise UnplayableAsCorpus(f"{self.path.name}: RIFF form is not sfbk")
        fh.seek(12)
        while fh.tell() < size - 8:
            head = fh.read(8)
            if len(head) < 8:
                break
            cid, csize = head[:4], struct.unpack("<I", head[4:])[0]
            pos = fh.tell()
            if cid == b"LIST":
                kind = fh.read(4)
                end = pos + csize
                while fh.tell() < end - 8:
                    sub = fh.read(8)
                    sid, ssize = sub[:4], struct.unpack("<I", sub[4:])[0]
                    at = fh.tell()
                    if kind == b"INFO":
                        raw = fh.read(min(ssize, 256))
                        self.info[sid.decode("latin1")] = _cstr(raw)
                    elif kind == b"sdta" and sid == b"smpl":
                        self._smpl_off = at
                    elif kind == b"pdta":
                        self._pdta[sid] = fh.read(ssize)
                    fh.seek(at + ssize + (ssize & 1))
            fh.seek(pos + csize + (csize & 1))
        missing = [n for n in (b"phdr", b"pbag", b"pgen", b"inst", b"ibag", b"igen", b"shdr")
                   if n not in self._pdta]
        if missing:
            raise UnplayableAsCorpus(
                f"{self.path.name}: pdta is missing "
                f"{', '.join(m.decode() for m in missing)}"
            )

    def _read_samples(self) -> list[Sample]:
        shdr = self._pdta[b"shdr"]
        out = []
        # The last record is the `EOS` terminal and is not a sample.
        for i in range(len(shdr) // 46 - 1):
            rec = shdr[i * 46 : (i + 1) * 46]
            start, end, _ls, _le, rate, pitch, _corr, link, kind = struct.unpack(
                "<IIIIIBbHH", rec[20:46]
            )
            out.append(Sample(_cstr(rec[:20]), start, end, rate, pitch, kind, link))
        return out

    def _read_presets(self) -> list[Preset]:
        phdr, pbag, pgen = self._pdta[b"phdr"], self._pdta[b"pbag"], self._pdta[b"pgen"]
        out = []
        count = len(phdr) // 38 - 1  # the last record is the `EOP` terminal
        for i in range(count):
            rec = phdr[i * 38 : (i + 1) * 38]
            program, bank = struct.unpack("<HH", rec[20:24])
            bag_lo = struct.unpack("<H", rec[24:26])[0]
            bag_hi = struct.unpack("<H", phdr[(i + 1) * 38 + 24 : (i + 1) * 38 + 26])[0]
            instruments = []
            for b in range(bag_lo, bag_hi):
                gen_lo = struct.unpack("<H", pbag[b * 4 : b * 4 + 2])[0]
                gen_hi = struct.unpack("<H", pbag[(b + 1) * 4 : (b + 1) * 4 + 2])[0]
                for g in range(gen_lo, gen_hi):
                    op, amount = struct.unpack("<Hh", pgen[g * 4 : (g + 1) * 4])
                    if op == 41:  # instrument — terminal generator of a preset zone
                        instruments.append(amount)
            out.append(Preset(_cstr(rec[:20]), bank, program, tuple(instruments)))
        return out

    def _instrument_zones(self, index: int) -> list[dict[int, int]]:
        """One dict of generator -> amount per zone of an instrument."""
        inst, ibag, igen = self._pdta[b"inst"], self._pdta[b"ibag"], self._pdta[b"igen"]
        if index + 1 >= len(inst) // 22:
            return []
        bag_lo = struct.unpack("<H", inst[index * 22 + 20 : index * 22 + 22])[0]
        bag_hi = struct.unpack("<H", inst[(index + 1) * 22 + 20 : (index + 1) * 22 + 22])[0]
        zones = []
        for b in range(bag_lo, bag_hi):
            gen_lo = struct.unpack("<H", ibag[b * 4 : b * 4 + 2])[0]
            gen_hi = struct.unpack("<H", ibag[(b + 1) * 4 : (b + 1) * 4 + 2])[0]
            zone: dict[int, int] = {}
            for g in range(gen_lo, gen_hi):
                op, amount = struct.unpack("<Hh", igen[g * 4 : (g + 1) * 4])
                zone[op] = amount
            zones.append(zone)
        return zones

    # ----------------------------------------------------------- the refusals

    def extraction_report(self) -> dict[str, object]:
        """What this file sets that a corpus reader would have to ignore.

        Returned rather than raised so a caller can print it: a refusal that only
        says "unsupported" sends whoever hit it back to a hex editor.
        """
        igen, pgen = self._pdta[b"igen"], self._pdta[b"pgen"]
        seen: dict[int, set[int]] = {}
        for blob in (igen, pgen):
            for i in range(len(blob) // 4):
                op, amount = struct.unpack("<Hh", blob[i * 4 : (i + 1) * 4])
                seen.setdefault(op, set()).add(amount)

        shaping = {
            _SHAPING[op]: sorted(vals)[:4]
            for op, vals in sorted(seen.items())
            if op in _SHAPING and not _inert(op, vals)
        }
        envelope = {_VOL_ENV[op]: sorted(vals) for op, vals in sorted(seen.items())
                    if op in _VOL_ENV}
        unknown = sorted(op for op in seen if op not in _SHAPING and op not in _VOL_ENV
                         and op not in _STRUCTURAL and op not in _IGNORABLE)
        return {
            "shaping": shaping,
            "envelope": envelope,
            "envelope_shapes": _envelope_shapes(seen),
            "unknown_generators": unknown,
            "samples": len(self.samples),
            "presets": len(self.presets),
            "sample_rates": sorted({s.sample_rate for s in self.samples}),
            "sample_kinds": sorted({s.kind for s in self.samples}),
        }

    def require_corpus(self) -> None:
        """Refuse a file whose samples are not what the source produced."""
        report = self.extraction_report()
        problems = []
        if report["shaping"]:
            problems.append(
                "it sets generators a player would apply and this reader will not: "
                + ", ".join(f"{k}={v}" for k, v in report["shaping"].items())  # type: ignore[union-attr]
            )
        if report["envelope_shapes"]:
            problems.append(f"its volume envelope shapes the note ({report['envelope_shapes']})")
        if report["unknown_generators"]:
            problems.append(f"it sets generators this reader does not model: "
                            f"{report['unknown_generators']}")
        if problems:
            raise UnplayableAsCorpus(
                f"{self.path.name} is an instrument to be played, not a corpus of "
                f"recordings — " + "; and ".join(problems) + ". Extracting its samples "
                "would report this reader's guess as a measurement of the source."
            )

    # ---------------------------------------------------------- the extractor

    def find(self, bank: int, program: int) -> Preset | None:
        for p in self.presets:
            if p.bank == bank and p.program == program:
                return p
        return None

    def note_map(self, preset: Preset) -> dict[int, Sample]:
        """Note -> the sample recorded AT that note, for the notes that have one.

        A zone whose sample was recorded at a different pitch is left out: a
        player would resample it and a capture that did so would report the
        stretch as the instrument.
        """
        out: dict[int, Sample] = {}
        for index in preset.instruments:
            for zone in self._instrument_zones(index):
                sid = zone.get(53)
                if sid is None or sid >= len(self.samples):
                    continue  # a global zone, or a dangling reference
                sample = self.samples[sid]
                key = zone.get(43)
                if key is None:
                    continue
                lo, hi = key & 0xFF, key >> 8
                if lo <= sample.original_pitch <= hi:
                    out[sample.original_pitch] = sample
        return out

    def extract(self, sample: Sample) -> tuple[np.ndarray, int]:
        """The recorded frames of one sample, as float in [-1, 1]."""
        self._fh.seek(self._smpl_off + sample.start * 2)
        raw = self._fh.read(sample.frames * 2)
        audio = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
        return audio, sample.sample_rate


#: Generators that cannot change what one note sounds like on their own: the
#: sample-address offsets, the LFO rates and delays (whose destinations are all
#: in `_SHAPING` and checked there, so an LFO nothing routes is silent), and the
#: drum mute group, which is a relation between notes rather than a note.
_IGNORABLE = {0, 1, 2, 3, 4, 12, 45, 50, 21, 22, 23, 24, 57}


def _inert(op: int, values: set[int]) -> bool:
    """Whether a generator is set only to the amount that means "do nothing"."""
    return op in _NEUTRAL and values == {_NEUTRAL[op]}


#: The amount at which a shaping generator changes nothing. Absent from this
#: table means any value shapes.
_NEUTRAL = {
    5: 0, 6: 0, 7: 0, 8: 13500, 9: 0, 10: 0, 11: 0, 13: 0,
    15: 0, 16: 0, 17: 0, 48: 0, 51: 0, 52: 0, 54: 0, 56: 100,
}


def _envelope_shapes(seen: dict[int, set[int]]) -> str:
    """Whether the volume envelope does more than guard the ends of the sample.

    Every SoundFont writes an envelope and most write a pass-through, so the
    presence of one says nothing. What matters is whether it moves the note
    inside the window a metric is read from.
    """
    reasons = []
    for amount in seen.get(34, set()):  # attackVolEnv
        ms = _timecents_s(amount) * 1000.0
        if ms > _ATTACK_CEILING_MS:
            reasons.append(f"attack {ms:.0f} ms")
    for amount in seen.get(37, set()):  # sustainVolEnv, in 0.1 dB of attenuation
        if amount / 10.0 > _SUSTAIN_CEILING_DB:
            reasons.append(f"sustain -{amount / 10.0:.1f} dB")
    for amount in seen.get(36, set()):  # decayVolEnv
        s = _timecents_s(amount)
        if s < _DECAY_FLOOR_S:
            reasons.append(f"decay {s:.1f} s")
    return ", ".join(sorted(set(reasons)))


def _cstr(raw: bytes) -> str:
    return raw.split(b"\x00")[0].decode("latin1", "replace").strip()
