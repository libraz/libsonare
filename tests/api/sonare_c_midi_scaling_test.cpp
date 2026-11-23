/// @file sonare_c_midi_scaling_test.cpp
/// @brief The C-ABI MIDI 1.0 <-> MIDI 2.0 value scalers: anchors, exact widen-then-narrow round
///        trips, refusals, agreement with the stateless MIDI 1.0 -> 2.0 translation, and agreement
///        with the bytes sonare_project_export_smf writes for the same MIDI 2.0 values.

#include <sonare/sonare_c.h>

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "midi/ump.h"

namespace {

struct ChannelMessage {
  uint8_t status;
  uint8_t data1;
  uint8_t data2;
};

uint32_t read_be32(const std::vector<uint8_t>& bytes, size_t at) {
  return (uint32_t{bytes[at]} << 24u) | (uint32_t{bytes[at + 1]} << 16u) |
         (uint32_t{bytes[at + 2]} << 8u) | uint32_t{bytes[at + 3]};
}

uint32_t read_vlq(const std::vector<uint8_t>& bytes, size_t* at) {
  uint32_t value = 0;
  uint8_t byte = 0;
  do {
    byte = bytes.at((*at)++);
    value = (value << 7u) | (byte & 0x7Fu);
  } while ((byte & 0x80u) != 0);
  return value;
}

// Every channel voice message in every track of a Standard MIDI File, in file order.
std::vector<ChannelMessage> channel_messages(const std::vector<uint8_t>& smf) {
  std::vector<ChannelMessage> out;
  REQUIRE(smf.size() >= 14);
  REQUIRE(std::string(smf.begin(), smf.begin() + 4) == "MThd");
  size_t at = 8 + read_be32(smf, 4);
  while (at + 8 <= smf.size()) {
    const bool is_track = std::string(smf.begin() + at, smf.begin() + at + 4) == "MTrk";
    const size_t end = at + 8 + read_be32(smf, at + 4);
    at += 8;
    if (!is_track) {
      at = end;
      continue;
    }
    uint8_t running = 0;
    while (at < end) {
      read_vlq(smf, &at);
      uint8_t status = smf.at(at);
      if ((status & 0x80u) != 0) {
        ++at;
      } else {
        status = running;
      }
      if (status == 0xFF) {
        ++at;  // meta type
        at += read_vlq(smf, &at);
        continue;
      }
      if (status == 0xF0 || status == 0xF7) {
        at += read_vlq(smf, &at);
        continue;
      }
      running = status;
      const uint8_t kind = status & 0xF0u;
      const bool one_data_byte = kind == 0xC0 || kind == 0xD0;
      ChannelMessage message{status, smf.at(at++), 0};
      if (!one_data_byte) message.data2 = smf.at(at++);
      out.push_back(message);
    }
    at = end;
  }
  return out;
}

// Exports a project holding one MIDI clip with @p events and returns its channel messages.
std::vector<ChannelMessage> exported_messages(const std::vector<SonareMidiEventPod>& events) {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  uint32_t track_id = 0;
  uint32_t clip_id = 0;
  const double length = events.empty() ? 1.0 : events.back().ppq + 1.0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, length, &track_id, &clip_id) == SONARE_OK);
  REQUIRE(sonare_project_set_midi_events(project, clip_id, events.data(), events.size()) ==
          SONARE_OK);
  uint8_t* bytes = nullptr;
  size_t len = 0;
  REQUIRE(sonare_project_export_smf(project, &bytes, &len) == SONARE_OK);
  std::vector<uint8_t> smf(bytes, bytes + len);
  sonare_free_bytes(bytes);
  sonare_project_destroy(project);
  return channel_messages(smf);
}

std::vector<ChannelMessage> of_kind(const std::vector<ChannelMessage>& messages, uint8_t kind) {
  std::vector<ChannelMessage> out;
  for (const ChannelMessage& m : messages) {
    if ((m.status & 0xF0u) == kind) out.push_back(m);
  }
  return out;
}

// The 32-bit inputs the export comparisons use: a stepped sweep of 4097 values whose odd step
// varies the low bits, every 7-bit step boundary k<<25 and its neighbours, and the 14-bit step
// boundaries k<<18 and their neighbours for every 64th k.
std::vector<uint32_t> sweep_32() {
  std::vector<uint32_t> values;
  constexpr uint64_t kStep = (uint64_t{1} << 20u) + 1u;
  for (uint64_t v = 0; v <= 0xFFFFFFFFu; v += kStep) values.push_back(static_cast<uint32_t>(v));
  values.push_back(0xFFFFFFFFu);
  for (uint32_t k = 0; k < 128; ++k) {
    const uint32_t edge = k << 25u;
    if (k != 0) values.push_back(edge - 1u);
    values.push_back(edge);
    values.push_back(edge + 1u);
  }
  for (uint32_t k = 0; k < 16384; k += 64) {
    const uint32_t edge = k << 18u;
    if (k != 0) values.push_back(edge - 1u);
    values.push_back(edge);
    values.push_back(edge + 1u);
  }
  return values;
}

}  // namespace

TEST_CASE("MIDI scaling anchors land min, center and max", "[c_api][midi]") {
  uint16_t v16 = 1;
  REQUIRE(sonare_midi_velocity_to_midi2(0, &v16) == SONARE_OK);
  CHECK(v16 == 0);
  REQUIRE(sonare_midi_velocity_to_midi2(64, &v16) == SONARE_OK);
  CHECK(v16 == 0x8000);
  REQUIRE(sonare_midi_velocity_to_midi2(127, &v16) == SONARE_OK);
  CHECK(v16 == 0xFFFF);
  REQUIRE(sonare_midi_note_on_velocity_to_midi2(1, &v16) == SONARE_OK);
  CHECK(v16 == 0x0200);

  uint32_t v32 = 1;
  REQUIRE(sonare_midi_control_to_midi2(0, &v32) == SONARE_OK);
  CHECK(v32 == 0);
  REQUIRE(sonare_midi_control_to_midi2(64, &v32) == SONARE_OK);
  CHECK(v32 == 0x80000000u);
  REQUIRE(sonare_midi_control_to_midi2(127, &v32) == SONARE_OK);
  CHECK(v32 == 0xFFFFFFFFu);
  REQUIRE(sonare_midi_control14_to_midi2(8192, &v32) == SONARE_OK);
  CHECK(v32 == 0x80000000u);
  REQUIRE(sonare_midi_control14_to_midi2(16383, &v32) == SONARE_OK);
  CHECK(v32 == 0xFFFFFFFFu);
  REQUIRE(sonare_midi_pitch_bend_to_midi2(0, &v32) == SONARE_OK);
  CHECK(v32 == 0);
  REQUIRE(sonare_midi_pitch_bend_to_midi2(8192, &v32) == SONARE_OK);
  CHECK(v32 == 0x80000000u);
  REQUIRE(sonare_midi_pitch_bend_to_midi2(16383, &v32) == SONARE_OK);
  CHECK(v32 == 0xFFFFFFFFu);

  uint8_t v7 = 9;
  REQUIRE(sonare_midi2_velocity_to_midi1(0, &v7) == SONARE_OK);
  CHECK(v7 == 0);
  REQUIRE(sonare_midi2_note_on_velocity_to_midi1(0, &v7) == SONARE_OK);
  CHECK(v7 == 1);
  REQUIRE(sonare_midi2_note_on_velocity_to_midi1(0x01FF, &v7) == SONARE_OK);
  CHECK(v7 == 1);
  REQUIRE(sonare_midi2_note_on_velocity_to_midi1(0xFFFF, &v7) == SONARE_OK);
  CHECK(v7 == 127);
}

TEST_CASE("MIDI scaling widen-then-narrow returns every input exactly", "[c_api][midi]") {
  for (unsigned v = 0; v < 128; ++v) {
    const auto v7 = static_cast<uint8_t>(v);
    uint16_t v16 = 0;
    uint8_t back = 0xFF;
    REQUIRE(sonare_midi_velocity_to_midi2(v7, &v16) == SONARE_OK);
    REQUIRE(sonare_midi2_velocity_to_midi1(v16, &back) == SONARE_OK);
    CHECK(back == v7);
    if (v != 0) {
      REQUIRE(sonare_midi_note_on_velocity_to_midi2(v7, &v16) == SONARE_OK);
      REQUIRE(sonare_midi2_note_on_velocity_to_midi1(v16, &back) == SONARE_OK);
      CHECK(back == v7);
    }
    uint32_t v32 = 0;
    REQUIRE(sonare_midi_control_to_midi2(v7, &v32) == SONARE_OK);
    REQUIRE(sonare_midi2_control_to_midi1(v32, &back) == SONARE_OK);
    CHECK(back == v7);
  }
  for (unsigned v = 0; v < 16384; ++v) {
    const auto v14 = static_cast<uint16_t>(v);
    uint32_t v32 = 0;
    uint16_t back = 0xFFFF;
    REQUIRE(sonare_midi_control14_to_midi2(v14, &v32) == SONARE_OK);
    REQUIRE(sonare_midi2_control_to_midi1_control14(v32, &back) == SONARE_OK);
    CHECK(back == v14);
    REQUIRE(sonare_midi_pitch_bend_to_midi2(v14, &v32) == SONARE_OK);
    REQUIRE(sonare_midi2_pitch_bend_to_midi1(v32, &back) == SONARE_OK);
    CHECK(back == v14);
  }
}

TEST_CASE("MIDI scaling narrow-then-widen is not exact", "[c_api][midi]") {
  // 0x8001 narrows to 64, which widens back to the center 0x8000.
  uint8_t v7 = 0;
  uint16_t v16 = 0;
  REQUIRE(sonare_midi2_velocity_to_midi1(0x8001, &v7) == SONARE_OK);
  REQUIRE(sonare_midi_velocity_to_midi2(v7, &v16) == SONARE_OK);
  CHECK(v16 == 0x8000);
}

TEST_CASE("MIDI scaling refuses out-of-range input by name and zeroes the out parameter",
          "[c_api][midi]") {
  uint16_t v16 = 7;
  uint32_t v32 = 7;
  for (unsigned v = 128; v < 256; ++v) {
    const auto bad = static_cast<uint8_t>(v);
    v16 = 7;
    REQUIRE(sonare_midi_velocity_to_midi2(bad, &v16) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(v16 == 0);
    CHECK(std::string(sonare_last_error_message()) == "velocity must be an integer in [0, 127]");
    v16 = 7;
    REQUIRE(sonare_midi_note_on_velocity_to_midi2(bad, &v16) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(v16 == 0);
    v32 = 7;
    REQUIRE(sonare_midi_control_to_midi2(bad, &v32) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(v32 == 0);
    CHECK(std::string(sonare_last_error_message()) == "value must be an integer in [0, 127]");
  }
  v16 = 7;
  REQUIRE(sonare_midi_note_on_velocity_to_midi2(0, &v16) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(v16 == 0);
  CHECK(std::string(sonare_last_error_message()) == "velocity must be an integer in [1, 127]");

  for (const uint16_t bad : {uint16_t{16384}, uint16_t{0x7FFF}, uint16_t{0xFFFF}}) {
    v32 = 7;
    REQUIRE(sonare_midi_control14_to_midi2(bad, &v32) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(v32 == 0);
    CHECK(std::string(sonare_last_error_message()) == "value14 must be an integer in [0, 16383]");
    v32 = 7;
    REQUIRE(sonare_midi_pitch_bend_to_midi2(bad, &v32) == SONARE_ERROR_INVALID_PARAMETER);
    CHECK(v32 == 0);
    CHECK(std::string(sonare_last_error_message()) == "bend14 must be an integer in [0, 16383]");
  }

  CHECK(sonare_midi_velocity_to_midi2(1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_midi_note_on_velocity_to_midi2(1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_midi2_velocity_to_midi1(1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_midi2_note_on_velocity_to_midi1(1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_midi_control_to_midi2(1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_midi_control14_to_midi2(1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_midi2_control_to_midi1(1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_midi2_control_to_midi1_control14(1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_midi_pitch_bend_to_midi2(1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_midi2_pitch_bend_to_midi1(1, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
}

TEST_CASE("MIDI scaling widens as the MIDI 1.0 to 2.0 translation does", "[c_api][midi]") {
  namespace midi = sonare::midi;
  for (unsigned v = 1; v < 128; ++v) {
    const auto v7 = static_cast<uint8_t>(v);
    uint16_t v16 = 0;
    REQUIRE(sonare_midi_note_on_velocity_to_midi2(v7, &v16) == SONARE_OK);
    const midi::Ump note_on = midi::midi1_to_midi2(midi::make_midi1_note_on(0, 0, 60, v7));
    CHECK(v16 == static_cast<uint16_t>(note_on.words[1] >> 16u));
  }
  for (unsigned v = 0; v < 128; ++v) {
    const auto v7 = static_cast<uint8_t>(v);
    uint32_t v32 = 0;
    REQUIRE(sonare_midi_control_to_midi2(v7, &v32) == SONARE_OK);
    const midi::Ump control = midi::midi1_to_midi2(midi::make_midi1_control_change(0, 0, 7, v7));
    CHECK(v32 == control.words[1]);
  }
  for (unsigned v = 0; v < 16384; ++v) {
    const auto v14 = static_cast<uint16_t>(v);
    uint32_t v32 = 0;
    REQUIRE(sonare_midi_pitch_bend_to_midi2(v14, &v32) == SONARE_OK);
    const midi::Ump bend = midi::midi1_to_midi2(midi::make_midi1_pitch_bend(0, 0, v14));
    CHECK(v32 == bend.words[1]);
  }
}

TEST_CASE("MIDI scaling narrows as sonare_project_export_smf writes", "[c_api][midi][project]") {
  SECTION("note-on and note-off velocities, every 16-bit value") {
    std::vector<SonareMidiEventPod> events(2 * 65536);
    for (uint32_t v = 0; v < 65536; ++v) {
      const double ppq = v * 0.25;
      REQUIRE(sonare_midi2_note_on(ppq, 0, 0, 60, static_cast<uint16_t>(v), 0, 0, &events[2 * v]) ==
              SONARE_OK);
      REQUIRE(sonare_midi2_note_off(ppq + 0.125, 0, 0, 60, static_cast<uint16_t>(v),
                                    &events[2 * v + 1]) == SONARE_OK);
    }
    const std::vector<ChannelMessage> messages = exported_messages(events);
    const std::vector<ChannelMessage> ons = of_kind(messages, 0x90);
    const std::vector<ChannelMessage> offs = of_kind(messages, 0x80);
    REQUIRE(ons.size() == 65536);
    REQUIRE(offs.size() == 65536);
    for (uint32_t v = 0; v < 65536; ++v) {
      uint8_t on = 0;
      uint8_t off = 0;
      REQUIRE(sonare_midi2_note_on_velocity_to_midi1(static_cast<uint16_t>(v), &on) == SONARE_OK);
      REQUIRE(sonare_midi2_velocity_to_midi1(static_cast<uint16_t>(v), &off) == SONARE_OK);
      CHECK(ons[v].data2 == on);
      CHECK(offs[v].data2 == off);
    }
  }

  const std::vector<uint32_t> values = sweep_32();

  SECTION("7-bit controller values") {
    std::vector<SonareMidiEventPod> events(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
      REQUIRE(sonare_midi2_cc(i * 0.25, 0, 0, 7, values[i], &events[i]) == SONARE_OK);
    }
    const std::vector<ChannelMessage> controls = of_kind(exported_messages(events), 0xB0);
    REQUIRE(controls.size() == values.size());
    for (size_t i = 0; i < values.size(); ++i) {
      uint8_t v7 = 0;
      REQUIRE(sonare_midi2_control_to_midi1(values[i], &v7) == SONARE_OK);
      CHECK(controls[i].data2 == v7);
    }
  }

  SECTION("14-bit registered controller values") {
    std::vector<SonareMidiEventPod> events(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
      REQUIRE(sonare_midi2_registered_controller(i * 0.25, 0, 0, 0, 1, values[i], &events[i]) ==
              SONARE_OK);
    }
    const std::vector<ChannelMessage> controls = of_kind(exported_messages(events), 0xB0);
    std::vector<uint16_t> written;
    for (size_t i = 0; i + 1 < controls.size(); ++i) {
      if (controls[i].data1 == 6 && controls[i + 1].data1 == 38) {
        written.push_back(static_cast<uint16_t>((controls[i].data2 << 7u) | controls[i + 1].data2));
      }
    }
    REQUIRE(written.size() == values.size());
    for (size_t i = 0; i < values.size(); ++i) {
      uint16_t v14 = 0;
      REQUIRE(sonare_midi2_control_to_midi1_control14(values[i], &v14) == SONARE_OK);
      CHECK(written[i] == v14);
    }
  }

  SECTION("pitch bends") {
    std::vector<SonareMidiEventPod> events(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
      REQUIRE(sonare_midi2_pitch_bend(i * 0.25, 0, 0, values[i], &events[i]) == SONARE_OK);
    }
    const std::vector<ChannelMessage> bends = of_kind(exported_messages(events), 0xE0);
    REQUIRE(bends.size() == values.size());
    for (size_t i = 0; i < values.size(); ++i) {
      uint16_t v14 = 0;
      REQUIRE(sonare_midi2_pitch_bend_to_midi1(values[i], &v14) == SONARE_OK);
      CHECK(static_cast<uint16_t>(bends[i].data1 | (bends[i].data2 << 7u)) == v14);
    }
  }
}
