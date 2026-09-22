"""Census which programs and which variation banks real Standard MIDI Files select.

The sibling census in ``extract_addresses.py`` parses SysEx only. Bank Select
and Program Change are channel-voice messages, so nothing in this tree has ever
asked the corpus the question the bank's own working order rests on: which
voices files actually reach.

Two things in ``tools/voicematch/policy.json`` are asserted rather than measured
without it. Its core tier is "the voices most files reach", which is a
reasonable guess and, until this runs, nothing more than one. And its variation
queue is flat: nothing distinguishes a variation a hundred files select from one
nobody has ever selected, while capturing from the module is the most expensive
reference route there is.

What is committed is the histogram, on the same grounds the address census
already argues: the files are arrangements under their authors' terms and cannot
enter the repository, but a count of 128 program numbers carries no expression —
no filenames, no note data, no timing, no ordering, and nothing that says which
programs a given file used together.

Usage::

    extract_programs.py --corpus .cache/gs-corpus/mid --out tools/gs/program-census.json

## The rhythm-part rule, which is what makes the numbers mean anything

A program change means two different things depending on the part it lands on:
on a melodic part it selects a tone, and on a rhythm part it selects a drum kit
out of a different 128-number space. Counting the two together puts the Room kit
on the celesta's tally.

Channel 10 powers on as the rhythm part and every other channel powers on
melodic, but a part can *become* one: ``40 1x 15`` USE FOR RHYTHM PART is a GS
write whose value is one-based, so 0 is off and anything above it is a drum map
(``src/midi/synth/docs/gs.md``). The block nibble is not the channel either —
block 0 is part 10 — so the mapping goes through the same rule the address
table states. Both are replayed here in tick order across the tracks, because a
switch written in one track governs a program change in another.

## What a bank number is, and why both halves are kept

For a GS part CC#0 carries the variation number and CC#32 the tone map. GM2 uses
the same two controllers for different things and is told apart by an MSB of 120
or 121 before the LSB is read at all, so the two halves are recorded as they
arrived and nothing here folds them into one bank. The channel is not part of
the key: which of sixteen parts a tone was selected on says nothing about how
often the tone is reached, and keeping it would multiply the table by sixteen
for a distinction nobody downstream makes. It is read for the rhythm rule and
then dropped.

## Distinct files, not messages

A file that changes program every bar would otherwise outweigh a hundred files
that each select a tone once, which inverts exactly the ranking this exists to
supply. Both numbers are recorded; ``files`` is the one to rank by.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import struct
import sys
from collections.abc import Iterator

# Roland framing, as in the address census.
ROLAND_ID = 0x41
GS_MODEL = 0x42
CMD_DT1 = 0x12

#: `40 1x 15` USE FOR RHYTHM PART. The high byte and the block are matched apart
#: from the nibble, which selects the part.
RHYTHM_PART_HIGH = 0x40
RHYTHM_PART_BLOCK = 0x10
RHYTHM_PART_LOW = 0x15

#: Zero-based MIDI channel of the part that powers on as the rhythm part.
DEFAULT_RHYTHM_CHANNEL = 9

BANK_MSB_CC = 0
BANK_LSB_CC = 32


def read_vlq(data: bytes, i: int) -> tuple[int, int]:
    value = 0
    while True:
        byte = data[i]
        i += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, i


def part_block_to_channel(block: int) -> int:
    """GS part-parameter block nibble to zero-based channel.

    Block 0 is part 10, blocks 1-9 are parts 1-9, blocks A-F are parts 11-16 —
    `gs_part_block_to_channel` in `src/midi/synth/gs_address_table.h`.
    """
    if block == 0:
        return DEFAULT_RHYTHM_CHANNEL
    if block <= 9:
        return block - 1
    return block


def iter_events(path: str) -> Iterator[tuple[int, int, bytes]]:
    """Every channel-voice and SysEx event as (tick, track, payload), in file order.

    Tolerant in the same way and for the same reason as the address census: a
    corpus off the open web holds truncated tracks and impossible running
    status, and a parser that refuses them censuses the files that happen to be
    well-formed. A track that fails mid-way contributes what it yielded first.
    """
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:4] != b"MThd":
        return
    pos = 0
    track = 0
    while pos + 8 <= len(data):
        tag = data[pos : pos + 4]
        length = struct.unpack(">I", data[pos + 4 : pos + 8])[0]
        body = data[pos + 8 : pos + 8 + length]
        pos += 8 + length
        if tag != b"MTrk":
            continue
        track += 1
        i = 0
        tick = 0
        running = 0
        while i < len(body):
            try:
                delta, i = read_vlq(body, i)
                tick += delta
                status = body[i]
                if status in (0xF0, 0xF7):
                    i += 1
                    n, i = read_vlq(body, i)
                    yield tick, track, body[i : i + n]
                    i += n
                elif status == 0xFF:
                    i += 2
                    n, i = read_vlq(body, i)
                    i += n
                else:
                    if status & 0x80:
                        running = status
                        i += 1
                    else:
                        status = running
                    width = 1 if (status & 0xF0) in (0xC0, 0xD0) else 2
                    yield tick, track, bytes([status]) + body[i : i + width]
                    i += width
            except IndexError:
                break


def rhythm_part_write(message: bytes) -> tuple[int, int] | None:
    """(channel, value) of a `40 1x 15` USE FOR RHYTHM PART write, or None.

    The checksum is validated exactly as the decoder does. A corpus off the open
    web carries garbled messages, and one accepted here would move a part into
    or out of the rhythm role for the rest of the file.
    """
    if message and message[-1] == 0xF7:
        message = message[:-1]
    if len(message) < 9 or message[0] != ROLAND_ID or message[2] != GS_MODEL:
        return None
    if message[3] != CMD_DT1 or sum(message[4:]) % 128 != 0:
        return None
    if any(byte & 0x80 for byte in message[4:]):
        return None
    high, mid, low = message[4], message[5], message[6]
    if high != RHYTHM_PART_HIGH or (mid & 0xF0) != RHYTHM_PART_BLOCK or low != RHYTHM_PART_LOW:
        return None
    return part_block_to_channel(mid & 0x0F), message[7]


class Census:
    """Selections by (program, bank MSB, bank LSB), melodic and rhythm apart."""

    def __init__(self) -> None:
        self.melodic: dict[tuple[int, int, int], dict] = {}
        self.rhythm: dict[tuple[int, int, int], dict] = {}
        self.files_scanned = 0
        self.files_with_program_change = 0
        self.files_unparsed = 0
        self.duplicate_files = 0
        self.messages = collections.Counter()

    def observe(self, table: dict, key: tuple[int, int, int], file_key: int) -> None:
        row = table.get(key)
        if row is None:
            row = {"count": 0, "files": set()}
            table[key] = row
        row["count"] += 1
        row["files"].add(file_key)

    def scan(self, path: str, file_key: int) -> None:
        self.files_scanned += 1
        try:
            events = sorted(iter_events(path), key=lambda e: (e[0], e[1]))
        except Exception:  # noqa: BLE001 -- an unreadable file is counted, not diagnosed
            self.files_unparsed += 1
            return
        msb = [0] * 16
        lsb = [0] * 16
        drums = [channel == DEFAULT_RHYTHM_CHANNEL for channel in range(16)]
        saw = False
        for _tick, _track, message in events:
            if not message:
                continue
            status = message[0]
            if status in (0xF0, 0xF7) or not status & 0x80:
                found = rhythm_part_write(message)
                if found is not None:
                    channel, value = found
                    # The value is one-based: 0 is off and anything above it
                    # names a drum map. Out of range still means drums.
                    drums[channel] = value != 0
                    self.messages["use_for_rhythm_part"] += 1
                continue
            kind, channel = status & 0xF0, status & 0x0F
            # A data byte with the high bit set is a garbled stream, and taken
            # at face value it becomes a program number no MIDI file can carry
            # -- a 128th program that then reads as a kit no GS map defines.
            # Refused and counted, exactly as the address census refuses a
            # payload byte that cannot be one.
            if any(byte & 0x80 for byte in message[1:]):
                self.messages["data_byte_high_bit"] += 1
                continue
            if kind == 0xB0 and len(message) >= 3:
                if message[1] == BANK_MSB_CC:
                    msb[channel] = message[2]
                    self.messages["bank_select_msb"] += 1
                elif message[1] == BANK_LSB_CC:
                    lsb[channel] = message[2]
                    self.messages["bank_select_lsb"] += 1
            elif kind == 0xC0 and len(message) >= 2:
                self.messages["program_change"] += 1
                table = self.rhythm if drums[channel] else self.melodic
                self.observe(table, (message[1], msb[channel], lsb[channel]), file_key)
                saw = True
        if saw:
            self.files_with_program_change += 1

    @staticmethod
    def _rows(table: dict) -> list[list[int]]:
        return [
            [program, bank_msb, bank_lsb, row["count"], len(row["files"])]
            for (program, bank_msb, bank_lsb), row in sorted(table.items())
        ]

    @staticmethod
    def _by_program(table: dict) -> list[list[int]]:
        """Every bank of a program folded together, which is how a tier is read."""
        folded: dict[int, dict] = {}
        for (program, _msb, _lsb), row in table.items():
            entry = folded.setdefault(program, {"count": 0, "files": set()})
            entry["count"] += row["count"]
            entry["files"] |= row["files"]
        return [
            [program, entry["count"], len(entry["files"])]
            for program, entry in sorted(folded.items())
        ]

    def to_json(self, source: str) -> dict:
        return {
            "source": source,
            "files_scanned": self.files_scanned,
            "files_duplicate": self.duplicate_files,
            "files_with_program_change": self.files_with_program_change,
            "files_unparsed": self.files_unparsed,
            "messages": dict(sorted(self.messages.items())),
            "_selection_columns": (
                "A melodic program change selects a tone and a rhythm one selects a kit out "
                "of a different 128-number space, so the two are never added together. "
                "`bank_msb` is the GS variation number and `bank_lsb` the tone map; GM2 uses "
                "both differently and is told apart by an MSB of 120 or 121, so neither is "
                "folded. Rank by `files`, not by `count`."
            ),
            "selection_columns": ["program", "bank_msb", "bank_lsb", "count", "files"],
            "program_columns": ["program", "count", "files"],
            "melodic": self._rows(self.melodic),
            "melodic_by_program": self._by_program(self.melodic),
            "rhythm": self._rows(self.rhythm),
            "rhythm_by_program": self._by_program(self.rhythm),
        }


def unique_paths(corpus: str) -> tuple[list[str], int]:
    """Every SMF under @p corpus, deduplicated by content, with the drop count.

    A distribution that ships the same tune both loose and inside an archive
    holds each twice, and a file counted twice is not two files: it would weight
    the ranking by how a collection was packaged.
    """
    paths = []
    for root, _, names in os.walk(corpus):
        for name in names:
            if name.lower().endswith((".mid", ".midi", ".smf")):
                paths.append(os.path.join(root, name))
    paths.sort()
    seen: set[str] = set()
    unique = []
    for path in paths:
        try:
            with open(path, "rb") as handle:
                digest = hashlib.sha256(handle.read()).hexdigest()
        except OSError:
            continue
        if digest in seen:
            continue
        seen.add(digest)
        unique.append(path)
    return unique, len(paths) - len(unique)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--corpus", required=True, help="directory of SMF files")
    parser.add_argument("--out", required=True, help="census JSON to write")
    parser.add_argument(
        "--source",
        default="",
        help="one line naming where the corpus came from; recorded in the census",
    )
    parser.add_argument(
        "--top", type=int, default=0, help="print the N most-reached melodic programs"
    )
    args = parser.parse_args()

    paths, duplicates = unique_paths(args.corpus)
    if not paths:
        print(f"no MIDI files under {args.corpus}", file=sys.stderr)
        return 1

    census = Census()
    census.duplicate_files = duplicates
    for index, path in enumerate(paths):
        census.scan(path, index)

    payload = census.to_json(args.source)
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=1, ensure_ascii=True)
        handle.write("\n")

    print(
        f"scanned {payload['files_scanned']} unique files "
        f"({payload['files_duplicate']} duplicates dropped, "
        f"{payload['files_with_program_change']} select a program, "
        f"{payload['files_unparsed']} unparsed) -> "
        f"{len(payload['melodic_by_program'])} melodic programs, "
        f"{len(payload['rhythm_by_program'])} kits, "
        f"{len(payload['melodic'])} (program, bank) pairs"
    )
    if args.top:
        for program, count, files in sorted(payload["melodic_by_program"], key=lambda row: -row[2])[
            : args.top
        ]:
            print(f"  program {program:3}: {files:6} files, {count:7} selections")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
