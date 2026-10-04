/// @file native_synth_gsfx_test.cpp
/// @brief GS insertion effects on NativeSynth: the EFX block and the resets are
///        the only GS it reads, only under GM program resolution, offline from
///        the event stream and live from the host's pushed SysEx alone.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#if defined(SONARE_WITH_MASTERING)

#include "engine/realtime_engine.h"
#include "mastering/api/insert_factory.h"
#include "midi/instrument.h"
#include "midi/midi_clip.h"
#include "midi/midi_event.h"
#include "midi/part_rig.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "rt/command.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::MidiInstrumentSourceOutput;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::test::event;

constexpr double kRate = 48000.0;
constexpr int kSamples = 9600;
constexpr uint32_t kAssignedTrack = 1;
constexpr uint32_t kOtherTrack = 2;
/// A subtractive lead: no default rig, no shared body, so whatever moves a
/// part's output here is the insertion effect.
constexpr uint8_t kLead = 80;

using Bytes = std::vector<uint8_t>;

Bytes dt1(uint32_t addr, std::vector<uint8_t> data) {
  const uint8_t a0 = static_cast<uint8_t>((addr >> 16) & 0x7Fu);
  const uint8_t a1 = static_cast<uint8_t>((addr >> 8) & 0x7Fu);
  const uint8_t a2 = static_cast<uint8_t>(addr & 0x7Fu);
  Bytes msg{0xF0, 0x41, 0x10, 0x42, 0x12, a0, a1, a2};
  msg.insert(msg.end(), data.begin(), data.end());
  int sum = a0 + a1 + a2;
  for (const uint8_t b : data) sum += b;
  msg.push_back(static_cast<uint8_t>((128 - (sum % 128)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

/// Distortion in the spec unit, and part slot 0 (block 1) routed into it.
const Bytes kDistortion = dt1(0x400300u, {0x01, 0x11});
const Bytes kPart0On = dt1(0x404122u, {0x01});
const Bytes kGsReset = dt1(0x40007Fu, {0x00});

NativeSynthConfig offline_config() {
  NativeSynthConfig cfg;
  cfg.gain = 1.0f;
  cfg.use_gm_programs = true;
  // Without the blocker the shared remainder is exactly zero, so the lanes
  // carry nothing but their own voices and buses.
  cfg.dc_block = false;
  cfg.realize_efx_inline = true;
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  return cfg;
}

MidiEvent sysex_event(const Bytes& msg) {
  MidiEvent e;
  e.ump = sonare::midi::make_sysex_handle(0, 1);
  e.sysex_payload = msg.data();
  e.sysex_payload_size = msg.size();
  return e;
}

struct Lanes {
  std::vector<float> fallback_l, fallback_r;
  std::vector<float> assigned_l, assigned_r;
  std::vector<float> other_l, other_r;
};

/// Parts 0 and 1 on the lead, each note on its own lane, after @p prelude.
Lanes render_lanes(const NativeSynthConfig& cfg, const std::vector<Bytes>& prelude) {
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  for (const Bytes& msg : prelude) synth.on_event(0, sysex_event(msg));
  synth.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, kLead)));
  synth.on_event(0, event(sonare::midi::make_midi1_program_change(0, 1, kLead)));
  MidiEvent a = event(sonare::midi::make_midi1_note_on(0, 0, 60, 110));
  a.source_track_id = kAssignedTrack;
  MidiEvent b = event(sonare::midi::make_midi1_note_on(0, 1, 67, 110));
  b.source_track_id = kOtherTrack;
  synth.on_event(0, a);
  synth.on_event(0, b);
  Lanes out;
  for (std::vector<float>* v : {&out.fallback_l, &out.fallback_r, &out.assigned_l, &out.assigned_r,
                                &out.other_l, &out.other_r}) {
    v->assign(kSamples, 0.0f);
  }
  float* fallback[] = {out.fallback_l.data(), out.fallback_r.data()};
  float* assigned[] = {out.assigned_l.data(), out.assigned_r.data()};
  float* other[] = {out.other_l.data(), out.other_r.data()};
  const MidiInstrumentSourceOutput outputs[] = {
      {0, fallback}, {kAssignedTrack, assigned}, {kOtherTrack, other}};
  REQUIRE(synth.process_source_tracks(outputs, 3, 2, kSamples));
  return out;
}

bool same(const Lanes& a, const Lanes& b) {
  return a.fallback_l == b.fallback_l && a.fallback_r == b.fallback_r &&
         a.assigned_l == b.assigned_l && a.assigned_r == b.assigned_r && a.other_l == b.other_l &&
         a.other_r == b.other_r;
}

double energy(const std::vector<float>& v) {
  double acc = 0.0;
  for (const float s : v) acc += static_cast<double>(s) * s;
  return acc;
}

// --- live engine -----------------------------------------------------------

constexpr uint32_t kDest = 0;
constexpr int kBlock = 256;
constexpr int64_t kLiveFrames = 8192;

NativeSynthConfig live_config() {
  NativeSynthConfig cfg = offline_config();
  cfg.realize_efx_inline = false;
  return cfg;
}

/// A clip holding the lead on part 0, preceded at frame 0 by @p sysex.
sonare::midi::MidiClipSchedule lead_clip(const std::vector<Bytes>& sysex) {
  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.destination_id = kDest;
  clip.start_sample = 0;
  clip.length_samples = kLiveFrames;
  for (const Bytes& msg : sysex) {
    MidiEvent e = sysex_event(msg);
    e.render_frame = 0;
    clip.events.push_back(e);
  }
  MidiEvent program = event(sonare::midi::make_midi1_program_change(0, 0, kLead));
  program.render_frame = 0;
  clip.events.push_back(program);
  MidiEvent on = event(sonare::midi::make_midi1_note_on(0, 0, 60, 110));
  on.render_frame = 0;
  clip.events.push_back(on);
  return clip;
}

std::vector<float> render_clip(const NativeSynthConfig& cfg, const std::vector<Bytes>& sysex) {
  NativeSynth synth(cfg);
  sonare::engine::RealtimeEngine engine;
  engine.prepare(kRate, kBlock);
  REQUIRE(engine.set_midi_instrument(kDest, &synth));
  engine.set_midi_clips({lead_clip(sysex)});
  std::vector<float> left(kLiveFrames, 0.0f);
  std::vector<float> right(kLiveFrames, 0.0f);
  float* channels[] = {left.data(), right.data()};
  engine.render_offline(channels, 2, kLiveFrames, kBlock);
  engine.set_midi_instrument(kDest, nullptr);
  return left;
}

/// The lead on part 0 played straight into the synth, with @p pushed handed to
/// the engine's live SysEx path first.
std::vector<float> render_pushed(const std::vector<Bytes>& pushed) {
  NativeSynth synth(live_config());
  sonare::engine::RealtimeEngine engine;
  engine.prepare(kRate, kBlock);
  REQUIRE(engine.set_midi_instrument(kDest, &synth));
  for (const Bytes& msg : pushed)
    REQUIRE(engine.push_midi_sysex(kDest, msg.data(), msg.size(), -1));
  synth.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, kLead)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
  std::vector<float> left(kLiveFrames, 0.0f);
  std::vector<float> right(kLiveFrames, 0.0f);
  for (int64_t offset = 0; offset < kLiveFrames; offset += kBlock) {
    float* channels[] = {left.data() + offset, right.data() + offset};
    engine.process(channels, 2, kBlock);
  }
  engine.set_midi_instrument(kDest, nullptr);
  return left;
}

}  // namespace

TEST_CASE("native: a GS insertion effect reaches the part it is assigned to and no other",
          "[midi][native][gsfx]") {
  const Lanes dry = render_lanes(offline_config(), {});
  const Lanes wet = render_lanes(offline_config(), {kDistortion, kPart0On});
  REQUIRE(energy(dry.assigned_l) > 1.0e-6);
  REQUIRE(wet.assigned_l != dry.assigned_l);
  REQUIRE(wet.other_l == dry.other_l);
  REQUIRE(wet.other_r == dry.other_r);
  REQUIRE(wet.fallback_l == dry.fallback_l);
}

TEST_CASE("native: a GS reset clears the insertion effect", "[midi][native][gsfx]") {
  const Lanes dry = render_lanes(offline_config(), {});
  const Lanes reset = render_lanes(offline_config(), {kDistortion, kPart0On, kGsReset});
  REQUIRE(same(reset, dry));
}

TEST_CASE("native: without GM program resolution GS SysEx is ignored", "[midi][native][gsfx]") {
  NativeSynthConfig cfg = offline_config();
  cfg.use_gm_programs = false;
  const Lanes dry = render_lanes(cfg, {});
  const Lanes sent = render_lanes(cfg, {kDistortion, kPart0On});
  REQUIRE(energy(dry.assigned_l) > 1.0e-6);
  REQUIRE(same(sent, dry));
}

TEST_CASE("native live: a host-pushed EFX SysEx takes effect", "[midi][native][gsfx][live]") {
  const std::vector<float> dry = render_pushed({});
  const std::vector<float> wet = render_pushed({kDistortion, kPart0On});
  REQUIRE(energy(dry) > 1.0e-6);
  REQUIRE(wet != dry);
}

TEST_CASE("native live: an EFX SysEx scheduled inside a clip is not applied",
          "[midi][native][gsfx][live]") {
  const std::vector<float> dry = render_clip(live_config(), {});
  const std::vector<float> scheduled = render_clip(live_config(), {kDistortion, kPart0On});
  REQUIRE(energy(dry) > 1.0e-6);
  REQUIRE(scheduled == dry);
  // Control: the same clip does reach the synth and would apply offline.
  const std::vector<float> inline_dry = render_clip(offline_config(), {});
  const std::vector<float> inline_wet = render_clip(offline_config(), {kDistortion, kPart0On});
  REQUIRE(inline_wet != inline_dry);
}

TEST_CASE("native live: a program change keeps the rig until the control thread rebuilds",
          "[midi][native][gsfx][live]") {
  NativeSynth synth(live_config());
  synth.prepare(kRate, kBlock);
  synth.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 29)));
  (void)sonare::test::render_stereo(synth, kBlock);
  REQUIRE(synth.part_rig_stage_names(0).empty());
  // Any control-thread rebuild picks the bound rig up.
  synth.on_control_sysex(kGsReset.data(), kGsReset.size());
  std::vector<std::string> bank;
  for (const auto& stage :
       sonare::midi::synth::gm_rig_chain(sonare::midi::synth::gm_fallback_rig(0, 29).id)) {
    bank.push_back(stage.name);
  }
  REQUIRE_FALSE(bank.empty());
  REQUIRE(synth.part_rig_stage_names(0) == bank);
}

#endif  // SONARE_WITH_MASTERING
