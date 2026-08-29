#!/usr/bin/env python3
"""Census the GS address space as real Standard MIDI Files use it.

The address table in ``src/midi/synth/gs_address_table.h`` promises that every
address is assigned, and that an address with no row is a defect rather than a
silence (``src/midi/synth/docs/gs.md``). That promise is only checkable against
data somebody else wrote: a hand-written corpus can only contain the addresses
whoever wrote it already knew about.

The files themselves cannot be committed — they are arrangements under their
authors' terms. What is committed is this census, and it deliberately carries
no expression: per address, how many messages landed on it, how many distinct
files used it, the payload lengths, and the range of values seen. No filenames,
no note data, no timing, no ordering. A histogram of 24-bit integers is not a
derivative work of a composition.

Usage::

    extract_addresses.py --corpus .cache/gs-corpus/mid --out tools/gs/address-census.json

The corpus lives outside the repository (``.cache/`` is gitignored). The census
is committed so that a fresh clone can run the coverage gate without it.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import struct
import sys
from typing import Iterator

# Roland framing.
ROLAND_ID = 0x41
GS_MODEL = 0x42
CMD_DT1 = 0x12
CMD_RQ1 = 0x11

# Address groups whose middle byte carries a variable nibble. The census folds
# the nibble away so that sixteen parts do not read as sixteen addresses; the
# fold is written as a mask so it matches the address table's own `mask` field.
#
# `41 mn rr` is two variable nibbles and a variable low byte: `m` is the drum
# map (0 = MAP1, 1 = MAP2 — not 1/2), `n` the parameter group, `rr` the note.
VARIABLE_MID_HIGH_NIBBLE = {
    (0x40, 0x1): 0x000F00,  # part parameters
    (0x40, 0x2): 0x000F00,  # part controller parameters
    (0x40, 0x3): 0x000F00,  # EFX; unit 0 is the manual's, 1-15 are libsonare's
    (0x40, 0x4): 0x000F00,  # part extension
    (0x50, 0x1): 0x000F00,  # the opposite port's blocks, same format as 40
    (0x50, 0x2): 0x000F00,
    (0x50, 0x3): 0x000F00,
    (0x50, 0x4): 0x000F00,
}


def read_vlq(data: bytes, i: int) -> tuple[int, int]:
    value = 0
    while True:
        byte = data[i]
        i += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, i


def iter_sysex(path: str) -> Iterator[bytes]:
    """Yields every SysEx payload in an SMF, framing bytes excluded.

    Tolerant by design: a corpus assembled from the open web contains files
    with truncated tracks and impossible running status, and a parser that
    refuses them censuses a corpus of the files that happen to be well-formed.
    A track that fails mid-way contributes what it yielded before failing.
    """
    with open(path, "rb") as handle:
        data = handle.read()
    pos = 0
    if data[:4] != b"MThd":
        return
    while pos + 8 <= len(data):
        tag = data[pos : pos + 4]
        length = struct.unpack(">I", data[pos + 4 : pos + 8])[0]
        body = data[pos + 8 : pos + 8 + length]
        pos += 8 + length
        if tag != b"MTrk":
            continue
        i = 0
        running = 0
        while i < len(body):
            try:
                _, i = read_vlq(body, i)
                status = body[i]
                if status in (0xF0, 0xF7):
                    i += 1
                    n, i = read_vlq(body, i)
                    yield body[i : i + n]
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
                    i += 1 if (status & 0xF0) in (0xC0, 0xD0) else 2
            except IndexError:
                break


def group_of(addr: int) -> tuple[int, int]:
    """Returns (base address with variable nibbles zeroed, mask) for reporting.

    Grouping is a presentation concern only. The census stores every CONCRETE
    address, because the gate resolves each one through the address table's own
    lookup and a wildcard has nothing to look up; a group that folded two
    addresses the table treats differently would hide exactly the drift the
    census exists to find.
    """
    hi, mid = addr >> 16, (addr >> 8) & 0xFF
    if hi in (0x41, 0x51):  # drum setup, and the opposite port's: 41 mn rr
        return addr & 0xFF0000, 0x00FFFF
    if hi in (0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27):  # user areas
        return addr & 0xFF0000, 0x00FFFF
    if hi == 0x10:  # display dot data: 10 0p xx
        return addr & 0xFFF000, 0x000FFF
    mask = VARIABLE_MID_HIGH_NIBBLE.get((hi, mid >> 4), 0)
    return addr & ~mask, mask


def format_address(addr: int, mask: int) -> str:
    out = []
    for shift in (16, 8, 0):
        byte = (addr >> shift) & 0xFF
        nibble_mask = (mask >> shift) & 0xFF
        if nibble_mask == 0xFF:
            out.append("**")
        elif nibble_mask == 0x0F:
            out.append("%X*" % (byte >> 4))
        elif nibble_mask == 0xF0:
            out.append("*%X" % (byte & 0x0F))
        else:
            out.append("%02X" % byte)
    return " ".join(out)


class Census:
    def __init__(self) -> None:
        self.rows: dict[tuple[int, int], dict] = {}
        self.files_scanned = 0
        self.files_with_gs = 0
        self.messages = collections.Counter()
        self.unparsed_files = 0
        self.duplicate_files = 0

    def observe(self, addr: int, payload: bytes, file_key: int) -> None:
        key = addr
        row = self.rows.get(key)
        if row is None:
            row = {
                "count": 0,
                "files": set(),
                "len_min": len(payload),
                "len_max": len(payload),
                "value_min": 0x7F,
                "value_max": 0x00,
            }
            self.rows[key] = row
        row["count"] += 1
        row["files"].add(file_key)
        row["len_min"] = min(row["len_min"], len(payload))
        row["len_max"] = max(row["len_max"], len(payload))
        for byte in payload:
            row["value_min"] = min(row["value_min"], byte)
            row["value_max"] = max(row["value_max"], byte)

    def scan(self, path: str, file_key: int) -> None:
        self.files_scanned += 1
        saw_gs = False
        try:
            messages = list(iter_sysex(path))
        except Exception:
            self.unparsed_files += 1
            return
        for msg in messages:
            if msg and msg[-1] == 0xF7:
                msg = msg[:-1]
            if len(msg) >= 4 and msg[0] in (0x7E, 0x7F):
                self.messages["universal"] += 1
                continue
            if not msg or msg[0] != ROLAND_ID:
                self.messages["non_roland"] += 1
                continue
            if len(msg) < 8:
                self.messages["roland_truncated"] += 1
                continue
            model, command = msg[2], msg[3]
            if model != GS_MODEL:
                self.messages["roland_model_%02X" % model] += 1
                continue
            if command == CMD_RQ1:
                self.messages["gs_rq1"] += 1
                continue
            if command != CMD_DT1:
                self.messages["gs_command_%02X" % command] += 1
                continue
            # A corpus off the open web contains garbled messages, and a census
            # that skips the checksum turns each one into a fictional address
            # the coverage gate then demands a row for. Validate exactly as the
            # decoder does: sum the address and data bytes, checksum last.
            if sum(msg[4:]) % 128 != 0:  # address + data + checksum
                self.messages["gs_dt1_bad_checksum"] += 1
                continue
            if any(byte & 0x80 for byte in msg[4:]):
                self.messages["gs_dt1_high_bit"] += 1
                continue
            addr = (msg[4] << 16) | (msg[5] << 8) | msg[6]
            payload = msg[7:-1]  # the last byte is the checksum
            if not payload:
                self.messages["gs_dt1_empty"] += 1
                continue
            self.messages["gs_dt1"] += 1
            self.observe(addr, payload, file_key)
            saw_gs = True
        if saw_gs:
            self.files_with_gs += 1

    def to_json(self, source: str) -> dict:
        addresses = []
        groups: dict[tuple[int, int], dict] = {}
        for addr, row in sorted(self.rows.items()):
            addresses.append(
                [
                    "%02X %02X %02X" % (addr >> 16, (addr >> 8) & 0xFF, addr & 0xFF),
                    row["count"],
                    len(row["files"]),
                    row["len_min"],
                    row["len_max"],
                    row["value_min"],
                    row["value_max"],
                ]
            )
            key = group_of(addr)
            group = groups.setdefault(key, {"count": 0, "addresses": 0, "files": set()})
            group["count"] += row["count"]
            group["addresses"] += 1
            group["files"] |= row["files"]
        return {
            "source": source,
            "files_scanned": self.files_scanned,
            "files_duplicate": self.duplicate_files,
            "files_with_gs": self.files_with_gs,
            "files_unparsed": self.unparsed_files,
            "messages": dict(sorted(self.messages.items())),
            "distinct_addresses": len(addresses),
            "address_columns": [
                "address",
                "count",
                "files",
                "len_min",
                "len_max",
                "value_min",
                "value_max",
            ],
            "addresses": addresses,
            "groups": [
                {
                    "group": format_address(addr, mask),
                    "addresses": g["addresses"],
                    "count": g["count"],
                    "files": len(g["files"]),
                }
                for (addr, mask), g in sorted(groups.items())
            ],
        }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", required=True, help="directory of SMF files")
    parser.add_argument("--out", required=True, help="census JSON to write")
    parser.add_argument(
        "--source",
        default="",
        help="one line naming where the corpus came from; recorded in the census",
    )
    args = parser.parse_args()

    census = Census()
    paths = []
    for root, _, names in os.walk(args.corpus):
        for name in names:
            if name.lower().endswith((".mid", ".midi", ".smf")):
                paths.append(os.path.join(root, name))
    paths.sort()
    # A corpus assembled from a distribution that ships the same tune both loose
    # and inside an archive holds each of those twice, and a file counted twice
    # is not two files: it would weight the coverage ratchet by how a collection
    # was packaged. Deduplicate by content, keeping the first path.
    seen_digests: set[str] = set()
    unique = []
    for path in paths:
        try:
            with open(path, "rb") as handle:
                digest = hashlib.sha256(handle.read()).hexdigest()
        except OSError:
            continue
        if digest in seen_digests:
            continue
        seen_digests.add(digest)
        unique.append(path)
    census.duplicate_files = len(paths) - len(unique)
    paths = unique
    if not paths:
        print("no MIDI files under %s" % args.corpus, file=sys.stderr)
        return 1
    for index, path in enumerate(paths):
        census.scan(path, index)

    payload = census.to_json(args.source)
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=1, ensure_ascii=True)
        handle.write("\n")

    print(
        "scanned %d unique files (%d duplicates dropped, %d carry GS, %d unparsed)"
        " -> %d distinct addresses"
        % (
            payload["files_scanned"],
            payload["files_duplicate"],
            payload["files_with_gs"],
            payload["files_unparsed"],
            payload["distinct_addresses"],
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
