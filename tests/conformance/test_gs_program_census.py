"""Stdlib self-tests for the GS program census, over synthetic SMFs.

The corpus is not in the repository and cannot be, so every case here builds
the few bytes it is about. What is worth testing is not the walking — the
address census shares that — but the three rules that decide what a program
change *means*, each of which silently misattributes a whole corpus when it is
wrong:

* which part is a rhythm part, since a program change there selects a kit out
  of a different 128-number space;
* that the block nibble of a GS part write is not the channel;
* that a byte which cannot be a data byte is refused rather than counted, which
  is how the first run reported a 128th kit.

Each is asserted against a control that would pass without the rule.
"""

from __future__ import annotations

import importlib.util
import json
import struct
import tempfile
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "extract_programs",
    Path(__file__).resolve().parents[2] / "tools/gs/extract_programs.py",
)
assert _SPEC and _SPEC.loader
census = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(census)


def vlq(value: int) -> bytes:
    out = bytearray([value & 0x7F])
    value >>= 7
    while value:
        out.insert(0, (value & 0x7F) | 0x80)
        value >>= 7
    return bytes(out)


def event(delta: int, payload: bytes) -> bytes:
    return vlq(delta) + payload


def track(*events: bytes) -> bytes:
    body = b"".join(events) + event(0, b"\xff\x2f\x00")
    return b"MTrk" + struct.pack(">I", len(body)) + body


def smf(*tracks: bytes) -> bytes:
    head = b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks), 480)
    return head + b"".join(tracks)


def program_change(channel: int, program: int) -> bytes:
    return bytes([0xC0 | channel, program])


def control(channel: int, controller: int, value: int) -> bytes:
    return bytes([0xB0 | channel, controller, value])


def gs_write(block: int, low: int, value: int) -> bytes:
    """`F0 41 10 42 12 40 1<block> <low> <value> <sum> F7`, checksum included."""
    body = bytes([0x40, 0x10 | block, low, value])
    checksum = (128 - sum(body) % 128) % 128
    return b"\xf0" + vlq(9) + bytes([0x41, 0x10, 0x42, 0x12]) + body + bytes([checksum, 0xF7])


def sysex_payload(message: bytes) -> bytes:
    """What `iter_events` yields for @p message: past the `F0` and its length."""
    return message[2:]


def rhythm_write(block: int, value: int) -> bytes:
    """`40 1x 15` USE FOR RHYTHM PART, for the part @p block selects."""
    return gs_write(block, census.RHYTHM_PART_LOW, value)


class _Corpus(unittest.TestCase):
    def census_of(self, *files: bytes) -> dict:
        root = Path(tempfile.mkdtemp())
        self.addCleanup(__import__("shutil").rmtree, root)
        for index, data in enumerate(files):
            (root / f"{index:03d}.mid").write_bytes(data)
        paths, duplicates = census.unique_paths(str(root))
        scan = census.Census()
        scan.duplicate_files = duplicates
        for index, path in enumerate(paths):
            scan.scan(path, index)
        return json.loads(json.dumps(scan.to_json("synthetic")))


class SelectionTest(_Corpus):
    def test_a_program_change_is_recorded_with_the_bank_that_was_latched(self) -> None:
        got = self.census_of(smf(track(
            event(0, control(0, 0, 8)),
            event(0, control(0, 32, 3)),
            event(0, program_change(0, 48)),
        )))
        self.assertEqual(got["melodic"], [[48, 8, 3, 1, 1]])
        self.assertEqual(got["rhythm"], [])

    def test_a_bank_select_persists_until_the_next_one(self) -> None:
        """A latch, not a prefix: the second program change takes the same bank."""
        got = self.census_of(smf(track(
            event(0, control(0, 0, 8)),
            event(0, program_change(0, 48)),
            event(480, program_change(0, 49)),
        )))
        self.assertEqual(got["melodic"], [[48, 8, 0, 1, 1], [49, 8, 0, 1, 1]])

    def test_a_bank_select_on_one_channel_does_not_reach_another(self) -> None:
        got = self.census_of(smf(track(
            event(0, control(0, 0, 8)),
            event(0, program_change(1, 48)),
        )))
        self.assertEqual(got["melodic"], [[48, 0, 0, 1, 1]])

    def test_files_and_count_are_different_numbers(self) -> None:
        """A file selecting one tone forty times must not outweigh forty files."""
        busy = smf(track(*(event(index, program_change(0, 48)) for index in range(40))))
        got = self.census_of(busy)
        self.assertEqual(got["melodic"], [[48, 0, 0, 40, 1]])
        self.assertEqual(got["melodic_by_program"], [[48, 40, 1]])

    def test_a_duplicate_file_is_not_two_files(self) -> None:
        one = smf(track(event(0, program_change(0, 48))))
        got = self.census_of(one, one, one)
        self.assertEqual(got["files_scanned"], 1)
        self.assertEqual(got["files_duplicate"], 2)
        self.assertEqual(got["melodic"], [[48, 0, 0, 1, 1]])


class RhythmPartTest(_Corpus):
    def test_channel_ten_powers_on_as_the_rhythm_part(self) -> None:
        """Program 48 on channel 10 is the Orchestra kit, not String Ensemble 1."""
        got = self.census_of(smf(track(event(0, program_change(9, 48)))))
        self.assertEqual(got["rhythm"], [[48, 0, 0, 1, 1]])
        self.assertEqual(got["melodic"], [])

    def test_every_other_channel_powers_on_melodic(self) -> None:
        """The control: without the rule the two would land in one table."""
        got = self.census_of(smf(track(*(
            event(0, program_change(channel, 48))
            for channel in range(16) if channel != 9))))
        self.assertEqual(got["melodic"], [[48, 0, 0, 15, 1]])
        self.assertEqual(got["rhythm"], [])

    def test_a_part_can_become_a_rhythm_part(self) -> None:
        """Block 1 is part 1, which is channel 1 — zero-based channel 0."""
        got = self.census_of(smf(track(
            event(0, rhythm_write(1, 1)),
            event(0, program_change(0, 48)),
        )))
        self.assertEqual(got["rhythm"], [[48, 0, 0, 1, 1]])
        self.assertEqual(got["melodic"], [])
        self.assertEqual(got["messages"]["use_for_rhythm_part"], 1)

    def test_a_part_can_stop_being_one_and_zero_is_the_only_value_that_says_so(self) -> None:
        """The value is one-based: 0 is off and anything above it is a drum map."""
        off = self.census_of(smf(track(
            event(0, rhythm_write(0, 0)),
            event(0, program_change(9, 48)),
        )))
        self.assertEqual(off["melodic"], [[48, 0, 0, 1, 1]])
        self.assertEqual(off["rhythm"], [])
        map2 = self.census_of(smf(track(
            event(0, rhythm_write(0, 2)),
            event(0, program_change(9, 48)),
        )))
        self.assertEqual(map2["rhythm"], [[48, 0, 0, 1, 1]])

    def test_the_block_nibble_is_not_the_channel(self) -> None:
        """Block 0 is part 10. Read as a channel it would move part 1 instead."""
        self.assertEqual(census.part_block_to_channel(0), 9)
        self.assertEqual(census.part_block_to_channel(1), 0)
        self.assertEqual(census.part_block_to_channel(9), 8)
        self.assertEqual(census.part_block_to_channel(10), 10)
        self.assertEqual(census.part_block_to_channel(15), 15)

    def test_a_write_with_a_bad_checksum_does_not_move_the_part(self) -> None:
        """A corpus off the open web is garbled, and one accepted here would
        move a part into the rhythm role for the rest of the file."""
        good = rhythm_write(1, 1)
        broken = good[:-2] + bytes([(good[-2] + 1) % 128, 0xF7])
        got = self.census_of(smf(track(
            event(0, broken),
            event(0, program_change(0, 48)),
        )))
        self.assertEqual(got["melodic"], [[48, 0, 0, 1, 1]])
        self.assertEqual(got["messages"].get("use_for_rhythm_part", 0), 0)

    def test_a_write_to_another_address_in_the_same_block_is_not_read_as_this_one(self) -> None:
        """`40 1x 16` is PITCH KEY SHIFT and sits one byte along. Its checksum is
        its own, so this fails on the address rather than on the framing."""
        neighbour = gs_write(1, census.RHYTHM_PART_LOW + 1, 1)
        self.assertIsNone(census.rhythm_part_write(sysex_payload(neighbour)))
        got = self.census_of(smf(track(
            event(0, neighbour),
            event(0, program_change(0, 48)),
        )))
        self.assertEqual(got["messages"].get("use_for_rhythm_part", 0), 0)
        self.assertEqual(got["melodic"], [[48, 0, 0, 1, 1]])

    def test_the_control_for_it_is_the_same_write_at_the_right_address(self) -> None:
        """Otherwise the case above passes on framing the test itself broke."""
        wanted = gs_write(1, census.RHYTHM_PART_LOW, 1)
        self.assertEqual(census.rhythm_part_write(sysex_payload(wanted)), (0, 1))


class MalformedByteTest(_Corpus):
    def test_a_data_byte_with_its_high_bit_set_is_refused_and_counted(self) -> None:
        """Taken at face value it becomes a program number no file can carry."""
        got = self.census_of(smf(track(
            event(0, b"\xc0\x80"),
            event(0, program_change(0, 48)),
        )))
        self.assertEqual(got["melodic"], [[48, 0, 0, 1, 1]])
        self.assertEqual(got["messages"]["data_byte_high_bit"], 1)
        for program, *_ in got["melodic"] + got["rhythm"]:
            self.assertLessEqual(program, 127)

    def test_the_shipped_census_carries_no_program_outside_the_range(self) -> None:
        committed = census.__file__.rsplit("/", 1)[0] + "/program-census.json"
        payload = json.loads(Path(committed).read_text(encoding="utf-8"))
        for table in ("melodic", "rhythm"):
            for program, msb, lsb, _count, _files in payload[table]:
                self.assertTrue(0 <= program <= 127, (table, program))
                self.assertTrue(0 <= msb <= 127 and 0 <= lsb <= 127, (table, msb, lsb))


class OrderingTest(_Corpus):
    def test_a_switch_in_one_track_governs_a_program_change_in_another(self) -> None:
        """Replayed in tick order, because a sequencer's tracks are simultaneous."""
        got = self.census_of(smf(
            track(event(480, program_change(0, 48))),
            track(event(0, rhythm_write(1, 1))),
        ))
        self.assertEqual(got["rhythm"], [[48, 0, 0, 1, 1]])

    def test_a_switch_after_the_program_change_does_not_reach_back(self) -> None:
        """The control for the case above: order is what decides, not presence."""
        got = self.census_of(smf(
            track(event(0, program_change(0, 48))),
            track(event(480, rhythm_write(1, 1))),
        ))
        self.assertEqual(got["melodic"], [[48, 0, 0, 1, 1]])
        self.assertEqual(got["rhythm"], [])


class ShippedCensusTest(unittest.TestCase):
    """The committed histogram carries counts and nothing a file could be from."""

    def setUp(self) -> None:
        path = Path(census.__file__).parent / "program-census.json"
        self.payload = json.loads(path.read_text(encoding="utf-8"))

    def test_it_carries_no_filenames_no_timing_and_no_note_data(self) -> None:
        allowed = {
            "source", "files_scanned", "files_duplicate", "files_with_program_change",
            "files_unparsed", "messages", "_selection_columns", "selection_columns",
            "program_columns", "melodic", "melodic_by_program", "rhythm", "rhythm_by_program",
        }
        self.assertEqual(set(self.payload), allowed)
        for table in ("melodic", "rhythm"):
            for row in self.payload[table]:
                self.assertEqual(len(row), 5)
                self.assertTrue(all(isinstance(value, int) for value in row))

    def test_the_two_tables_are_counted_apart(self) -> None:
        """A kit and a melodic tone share a number and are different things."""
        melodic = {row[0] for row in self.payload["melodic_by_program"]}
        rhythm = {row[0] for row in self.payload["rhythm_by_program"]}
        self.assertTrue(melodic & rhythm, "the number spaces do overlap, which is the point")
        self.assertNotEqual(self.payload["melodic"], self.payload["rhythm"])

    def test_the_rhythm_rule_is_exercised_by_the_corpus_rather_than_hypothetical(self) -> None:
        self.assertGreater(self.payload["messages"]["use_for_rhythm_part"], 0)


if __name__ == "__main__":
    unittest.main()
