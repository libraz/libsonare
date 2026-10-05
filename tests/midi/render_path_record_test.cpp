/// @file render_path_record_test.cpp
/// @brief The render-path recorder: attaching it changes no sample, the
///        topology it records is the chain the player built, an OD Sw edit
///        lands as a param event at the frame it took effect, and the record
///        stays inside the render in frame order.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(SONARE_WITH_MASTERING)

#include "mastering/api/insert_factory.h"
#include "midi/midi_event.h"
#include "midi/prepared_sysex.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/render_path_record.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/midi_render.h"
#include "transport/transport_state.h"

namespace {

using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::RenderPathEvent;
using sonare::midi::synth::RenderPathRecorder;
using sonare::midi::synth::RenderPathTopology;
using sonare::midi::synth::RenderPathUnit;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::event;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kBlocks = 24;
constexpr int64_t kRenderFrames = static_cast<int64_t>(kBlock) * kBlocks;

/// GTR Multi 1 on unit 0, its OD Sw at parameter slot 10 (address 40 03 0D).
constexpr uint8_t kOdSwSlot = 10;
constexpr int kRouteBlock = 4;
constexpr int kOdOffBlock = 10;
constexpr int kProgramBlock = 16;

std::vector<uint8_t> dt1(uint8_t a1, uint8_t a2, std::vector<uint8_t> data) {
  std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, a1, a2};
  int sum = 0x40 + a1 + a2;
  for (const uint8_t d : data) sum += d;
  msg.insert(msg.end(), data.begin(), data.end());
  msg.push_back(static_cast<uint8_t>((128 - (sum & 0x7F)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

Sf2PlayerConfig player_config() {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  cfg.realize_efx_inline = true;
  return cfg;
}

/// A scheduled SysEx as the engine delivers it in a bounce: prepared for this
/// player, then fed through on_event at its frame.
void deliver(Sf2Player& player, const std::vector<uint8_t>& msg, int64_t frame) {
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> token;
  REQUIRE(player.prepare_sysex(msg.data(), msg.size(), token));
  sonare::midi::MidiEvent e;
  e.ump = sonare::midi::make_sysex_handle(0, 1);
  e.render_frame = frame;
  e.sysex_payload = msg.data();
  e.sysex_payload_size = msg.size();
  e.prepared_sysex = token.get();
  player.on_event(0, e);
}

struct Take {
  std::vector<float> left;
  std::vector<float> right;
};

/// An electric guitar on program 29 (a bank rig), routed mid-file into unit 0
/// running GTR Multi 1, OD Sw switched off later, then a program change to 27.
Take play(Sf2Player& player) {
  player.prepare(kRate, kBlock);
  Take take;
  take.left.assign(static_cast<size_t>(kRenderFrames), 0.0f);
  take.right.assign(static_cast<size_t>(kRenderFrames), 0.0f);
  for (int b = 0; b < kBlocks; ++b) {
    const int64_t frame = static_cast<int64_t>(b) * kBlock;
    sonare::transport::TransportState state;
    state.playing = true;
    state.render_frame = frame;
    player.set_transport(state);
    if (b == 0) {
      player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 29)));
      player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 52, 110)));
    } else if (b == kRouteBlock) {
      deliver(player, dt1(0x41, 0x22, {0x01}), frame);
      deliver(player, dt1(0x03, 0x00, {0x04, 0x00}), frame);
      deliver(player, dt1(0x03, static_cast<uint8_t>(3 + kOdSwSlot), {0x01}), frame);
    } else if (b == kOdOffBlock) {
      deliver(player, dt1(0x03, static_cast<uint8_t>(3 + kOdSwSlot), {0x00}), frame);
    } else if (b == kProgramBlock) {
      player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 27)));
      player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 55, 110)));
    }
    float* chans[2] = {take.left.data() + frame, take.right.data() + frame};
    player.process(chans, 2, kBlock);
  }
  return take;
}

const RenderPathTopology* last_topology(const RenderPathRecorder& recorder) {
  const auto& events = recorder.events();
  for (auto it = events.rbegin(); it != events.rend(); ++it) {
    if (it->kind == RenderPathEvent::Kind::kTopology) return &it->topology;
  }
  return nullptr;
}

/// The topology in force at @p frame: the last one recorded at or before it.
const RenderPathTopology* topology_at(const RenderPathRecorder& recorder, int64_t frame) {
  const RenderPathTopology* found = nullptr;
  for (const RenderPathEvent& e : recorder.events()) {
    if (e.frame > frame) break;
    if (e.kind == RenderPathEvent::Kind::kTopology) found = &e.topology;
  }
  return found;
}

const RenderPathUnit* unit0(const RenderPathTopology& topology) {
  for (const RenderPathUnit& u : topology.units) {
    if (u.unit == 0) return &u;
  }
  return nullptr;
}

long enabled_count(const RenderPathUnit& unit) {
  return std::count(unit.enabled.begin(), unit.enabled.end(), true);
}

}  // namespace

TEST_CASE("A render-path recorder changes no sample", "[midi][rig][render-path]") {
  Sf2Player plain(player_config());
  const Take reference = play(plain);
  Sf2Player watched(player_config());
  RenderPathRecorder recorder;
  REQUIRE(watched.set_render_path_recorder(&recorder));
  const Take recorded = play(watched);
  REQUIRE_FALSE(recorder.events().empty());
  // Same code path on the same host, so identity is the claim, not a tolerance.
  CHECK(recorded.left == reference.left);
  CHECK(recorded.right == reference.right);
  // Positive control: the take is not silent.
  CHECK(
      std::any_of(reference.left.begin(), reference.left.end(), [](float s) { return s != 0.0f; }));
}

TEST_CASE("Recorded rig stages are the chains the player published", "[midi][rig][render-path]") {
  Sf2Player player(player_config());
  RenderPathRecorder recorder;
  REQUIRE(player.set_render_path_recorder(&recorder));
  (void)play(player);
  const RenderPathTopology* topology = last_topology(recorder);
  REQUIRE(topology != nullptr);
  REQUIRE(topology->parts.size() == 16);
  for (uint8_t part = 0; part < 16; ++part) {
    CAPTURE(part);
    CHECK(topology->parts[part].stages == player.part_rig_stage_names(part));
  }
  // Non-vacuity: the guitar part carries a bank rig and is routed into unit 0.
  CHECK_FALSE(topology->parts[0].stages.empty());
  CHECK(std::string_view(topology->parts[0].rig_source) == "bank");
  CHECK(topology->parts[0].program == 27);
  CHECK(topology->parts[0].unit == 0);
  const RenderPathUnit* unit = unit0(*topology);
  REQUIRE(unit != nullptr);
  CHECK(unit->type == 0x0400);
  CHECK(recorder.complete());
}

TEST_CASE("An OD Sw edit is recorded at the frame it took effect", "[midi][rig][render-path]") {
  Sf2Player player(player_config());
  RenderPathRecorder recorder;
  REQUIRE(player.set_render_path_recorder(&recorder));
  (void)play(player);
  const int64_t off_frame = static_cast<int64_t>(kOdOffBlock) * kBlock;
  const auto& events = recorder.events();
  const auto od_off = std::find_if(events.begin(), events.end(), [](const RenderPathEvent& e) {
    return e.kind == RenderPathEvent::Kind::kParam && e.unit == 0 && e.slot == kOdSwSlot &&
           e.value == 0;
  });
  REQUIRE(od_off != events.end());
  CHECK(od_off->frame == off_frame);
  // The enable layer moves with it: fewer of unit 0's stages are switched in
  // after the edit than while the block was on.
  const RenderPathTopology* before = topology_at(recorder, off_frame - 1);
  const RenderPathTopology* after = topology_at(recorder, off_frame);
  REQUIRE(before != nullptr);
  REQUIRE(after != nullptr);
  const RenderPathUnit* on = unit0(*before);
  const RenderPathUnit* off = unit0(*after);
  REQUIRE(on != nullptr);
  REQUIRE(off != nullptr);
  CHECK(on->stages == off->stages);
  CHECK(enabled_count(*off) < enabled_count(*on));
}

TEST_CASE("Render-path frames are ordered and inside the render", "[midi][rig][render-path]") {
  Sf2Player player(player_config());
  RenderPathRecorder recorder;
  REQUIRE(player.set_render_path_recorder(&recorder));
  (void)play(player);
  const auto& events = recorder.events();
  REQUIRE(events.size() > 2);
  int64_t previous = 0;
  for (const RenderPathEvent& e : events) {
    CHECK(e.frame >= previous);
    CHECK(e.frame >= 0);
    CHECK(e.frame < kRenderFrames);
    previous = e.frame;
  }
  // A second pass from frame 0 is not appended to the first.
  sonare::transport::TransportState rewind;
  player.set_transport(rewind);
  CHECK_FALSE(recorder.recording());
  CHECK_FALSE(recorder.complete());
  const std::string json = recorder.to_json("test");
  CHECK(json.find("\"schema\":1") != std::string::npos);
  CHECK(json.find("\"complete\":false") != std::string::npos);
}

TEST_CASE("NativeSynth records the rig it published", "[midi][rig][render-path]") {
  NativeSynthConfig cfg;
  cfg.gain = 1.0f;
  cfg.use_gm_programs = true;
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  cfg.realize_efx_inline = true;
  NativeSynth synth(cfg);
  RenderPathRecorder recorder;
  REQUIRE(synth.set_render_path_recorder(&recorder));
  synth.prepare(kRate, kBlock);
  synth.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 29)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 52, 110)));
  (void)sonare::test::render_stereo(synth, kBlock);
  const RenderPathTopology* topology = last_topology(recorder);
  REQUIRE(topology != nullptr);
  CHECK_FALSE(topology->parts[0].stages.empty());
  CHECK(topology->parts[0].stages == synth.part_rig_stage_names(0));
}

TEST_CASE("A live player refuses a render-path recorder", "[midi][rig][render-path]") {
  Sf2PlayerConfig cfg = player_config();
  cfg.realize_efx_inline = false;
  Sf2Player player(cfg);
  RenderPathRecorder recorder;
  CHECK_FALSE(player.set_render_path_recorder(&recorder));
}

#endif  // SONARE_WITH_MASTERING
