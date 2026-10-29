#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "midi/capture.h"
#include "midi/midi_clip.h"
#include "midi/smf2.h"
#include "midi/ump.h"
#include "transport/tempo_map.h"

using Catch::Matchers::WithinAbs;

namespace {

using namespace sonare::midi;

Ump midi1_with_stale_group(uint8_t group, UmpStatus status, uint8_t channel, uint8_t data1,
                           uint8_t data2) {
  Ump message;
  message.words[0] = (uint32_t{0x2} << 28) | (static_cast<uint32_t>(group & 0x0Fu) << 24) |
                     (static_cast<uint32_t>(status) << 20) |
                     (static_cast<uint32_t>(channel & 0x0Fu) << 16) |
                     (static_cast<uint32_t>(data1 & 0x7Fu) << 8) | (data2 & 0x7Fu);
  message.word_count = 1;
  message.group = 0;
  return message;
}

MidiEvent captured(int64_t frame, Ump ump) {
  MidiEvent event;
  event.render_frame = frame;
  event.ump = ump;
  return event;
}

}  // namespace

TEST_CASE("MIDI channel-voice conversions take group from word zero", "[midi][ump][group]") {
  const Ump note_on = midi1_with_stale_group(3, UmpStatus::kNoteOn, 2, 60, 100);
  const Ump midi2 = midi1_to_midi2(note_on);
  REQUIRE(midi2.message_type() == UmpMessageType::kMidi2ChannelVoice);
  REQUIRE(ump_group_from_word0(midi2.words[0]) == 3);

  Ump midi1_packet = note_on;
  midi1_packet.group = 0xFF;
  const Midi1MessageList midi1_messages = midi2_to_midi1_messages(midi1_packet);
  REQUIRE(midi1_messages.count == 1);
  REQUIRE(midi1_messages.messages[0].words[0] == midi1_packet.words[0]);
  REQUIRE(midi1_messages.messages[0].group == 3);

  const Ump note_off = make_midi2_note_off(3, 2, 60, 0x8000u);
  Ump stale_midi2 = note_off;
  stale_midi2.group = 0;
  const Ump midi1 = midi2_to_midi1(stale_midi2);
  REQUIRE(midi1.message_type() == UmpMessageType::kMidi1ChannelVoice);
  REQUIRE(ump_group_from_word0(midi1.words[0]) == 3);

  const Ump rpn = make_midi2_registered_controller(3, 2, 0, 1, 0x12345678u);
  Ump stale_rpn = rpn;
  stale_rpn.group = 0;
  const Midi1MessageList messages = midi2_to_midi1_messages(stale_rpn);
  REQUIRE(messages.count == 4);
  for (uint8_t i = 0; i < messages.count; ++i) {
    REQUIRE(ump_group_from_word0(messages.messages[i].words[0]) == 3);
  }
}

TEST_CASE("Stateful MIDI translation isolates channels by wire group", "[midi][ump][group]") {
  Midi1ToMidi2Translator translator;
  const auto send = [&](uint8_t group, UmpStatus status, uint8_t data1, uint8_t data2) {
    return translator.translate(midi1_with_stale_group(group, status, 0, data1, data2));
  };

  REQUIRE(send(3, UmpStatus::kControlChange, 0, 2).count == 0);
  const Midi2MessageList other_group = send(4, UmpStatus::kProgramChange, 7, 0);
  REQUIRE(other_group.count == 1);
  REQUIRE((other_group.messages[0].words[0] & 1u) == 0);
  REQUIRE(ump_group_from_word0(other_group.messages[0].words[0]) == 4);

  const Midi2MessageList selected_group = send(3, UmpStatus::kProgramChange, 8, 0);
  REQUIRE(selected_group.count == 1);
  REQUIRE((selected_group.messages[0].words[0] & 1u) != 0);
  REQUIRE(static_cast<uint8_t>((selected_group.messages[0].words[1] >> 8u) & 0x7Fu) == 2);
  REQUIRE(ump_group_from_word0(selected_group.messages[0].words[0]) == 3);
}

TEST_CASE("Clip note pairing uses wire groups even when group caches disagree",
          "[midi][ump][group]") {
  MidiClip separate_groups;
  Ump on = make_midi1_note_on(3, 0, 60, 100);
  Ump off = make_midi1_note_off(4, 0, 60, 0);
  on.group = 0;
  off.group = 0;
  separate_groups.add_event({0.0, on});
  separate_groups.add_event({1.0, off});
  const NotePairValidation unmatched = separate_groups.validate_note_pairs();
  REQUIRE_FALSE(unmatched.ok);
  REQUIRE(unmatched.unmatched_note_ons == 1);
  REQUIRE(unmatched.unmatched_note_offs == 1);

  MidiClip same_group;
  Ump matching_on = make_midi1_note_on(5, 0, 62, 100);
  Ump matching_off = make_midi1_note_off(5, 0, 62, 0);
  matching_on.group = 1;
  matching_off.group = 7;
  same_group.add_event({0.0, matching_on});
  same_group.add_event({1.0, matching_off});
  REQUIRE(same_group.validate_note_pairs().ok);
}

TEST_CASE("Capture note-shift matching uses wire groups", "[midi][ump][group][capture]") {
  sonare::transport::TempoMap tempo;
  tempo.prepare(48000.0);
  tempo.set_segments({{0.0, 120.0, 0.0}});
  MidiCapture capture;
  capture.prepare(&tempo, 16);

  Ump group3_on = make_midi1_note_on(3, 2, 68, 100);
  Ump group4_on = make_midi1_note_on(4, 2, 68, 100);
  group3_on.group = 0;
  group4_on.group = 0;
  REQUIRE(capture.push(captured(10800, group3_on)));
  REQUIRE(capture.push(captured(14400, group4_on)));

  MidiClip clip;
  CaptureConfig config;
  config.quantize.enabled = true;
  config.quantize.grid_ppq = 0.5;
  config.quantize.strength = 1.0;
  REQUIRE(capture.drain(config, &clip) == 2);

  Ump group3_off = make_midi1_note_off(3, 2, 68, 0);
  Ump group4_off = make_midi1_note_off(4, 2, 68, 0);
  group3_off.group = 7;
  group4_off.group = 7;
  REQUIRE(capture.push(captured(13200, group3_off)));
  REQUIRE(capture.push(captured(15600, group4_off)));
  REQUIRE(capture.drain(config, &clip) == 2);

  REQUIRE(clip.events().size() == 4);
  for (const MidiClipEvent& event : clip.events()) {
    REQUIRE(event.ump.group == ump_group_from_word0(event.ump.words[0]));
    if (event.ump.is_note_on()) REQUIRE(event.ppq == 0.5);
  }
  double group3_off_ppq = -1.0;
  double group4_off_ppq = -1.0;
  for (const MidiClipEvent& event : clip.events()) {
    if (!event.ump.is_note_off()) continue;
    const uint8_t wire_group = ump_group_from_word0(event.ump.words[0]);
    if (wire_group == 3) group3_off_ppq = event.ppq;
    if (wire_group == 4) group4_off_ppq = event.ppq;
  }
  REQUIRE_THAT(group3_off_ppq, WithinAbs(0.6, 1.0e-12));
  REQUIRE_THAT(group4_off_ppq, WithinAbs(0.55, 1.0e-12));
}

TEST_CASE("SMF2 SysEx export uses the group encoded in its UMP", "[midi][smf2][group]") {
  SysExStore store;
  const SysExHandle handle = store.add(std::vector<uint8_t>{0x7E, 0x7F, 0x09, 0x01});
  REQUIRE(handle != 0);

  Ump sysex = make_sysex_handle(3, handle);
  sysex.group = 0;
  MidiClip clip;
  clip.add_event({0.0, sysex});
  Smf2ExportOptions options;
  options.sysex_store = &store;
  const Smf2ExportResult exported = export_clip_file(clip, {}, {}, options);
  REQUIRE(exported.ok());
  REQUIRE(exported.skipped_events == 0);

  const Smf2ImportResult imported = import_clip_file(exported.bytes);
  REQUIRE(imported.ok());
  REQUIRE(imported.clips.size() == 1);
  REQUIRE(imported.clips.front().events().size() == 1);
  REQUIRE(ump_group_from_word0(imported.clips.front().events().front().ump.words[0]) == 3);
}
