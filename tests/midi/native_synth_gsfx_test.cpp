/// @file native_synth_gsfx_test.cpp
/// @brief GS insertion effects on NativeSynth: the EFX block and the resets are
///        the only GS it reads, only under GM program resolution, offline from
///        the event stream and live from the host's pushed SysEx alone.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
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
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "rt/command.h"
#include "support/midi_render.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::MidiInstrumentSourceOutput;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::SynthEngineMode;
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
const Bytes kPart0OnUnit1 = dt1(0x404122u, {0x02});
const Bytes kPart1OnUnit1 = dt1(0x404222u, {0x02});
// Start one byte before PART EFX ASSIGN so the assignment is the third
// decoded write, after PART EQ SWITCH and the output-pair byte.
const Bytes kPart0OnBulk = dt1(0x404120u, {0x01, 0x00, 0x01});
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

TEST_CASE("native: a later EFX assignment in a bulk DT1 reaches the inline path",
          "[midi][native][gsfx]") {
  const Lanes single = render_lanes(offline_config(), {kDistortion, kPart0On});
  const Lanes bulk = render_lanes(offline_config(), {kDistortion, kPart0OnBulk});
  // The assignment must be audible, or bulk == single holds with it dropped.
  const Lanes unassigned = render_lanes(offline_config(), {kDistortion});
  REQUIRE(energy(single.assigned_l) > 1.0e-6);
  CHECK_FALSE(static_cast<bool>(single.assigned_l == unassigned.assigned_l));
  CHECK(same(bulk, single));
}

TEST_CASE("native: one bulk run realizes every EFX unit before its assignments",
          "[midi][native][gsfx][bulk]") {
  std::vector<uint8_t> data(130, 0x00);
  data[0] = 0x01;
  data[1] = 0x11;  // unit 0: Distortion
  data[0x20] = 0x01;
  data[0x21] = 0x50;  // reserved 40 30 20: must not alter unit 0
  data[128] = 0x01;
  data[129] = 0x10;  // unit 1: Overdrive
  const Bytes bulk_units = dt1(0x403000u, data);
  const Bytes split_unit0 = dt1(0x403000u, std::vector<uint8_t>(data.begin(), data.begin() + 32));
  const Bytes split_unit1 = dt1(0x403100u, {0x01, 0x10});

  const Lanes dry = render_lanes(offline_config(), {});
  const Lanes split =
      render_lanes(offline_config(), {split_unit0, split_unit1, kPart0On, kPart1OnUnit1});
  const Lanes bulk = render_lanes(offline_config(), {bulk_units, kPart0On, kPart1OnUnit1});
  REQUIRE(energy(dry.assigned_l) > 1.0e-6);
  REQUIRE(energy(dry.other_l) > 1.0e-6);
  REQUIRE(energy(bulk.assigned_l) > 1.0e-6);
  REQUIRE(energy(bulk.other_l) > 1.0e-6);
  CHECK(static_cast<bool>(bulk.assigned_l != dry.assigned_l));
  CHECK(static_cast<bool>(bulk.other_l != dry.other_l));
  CHECK(same(bulk, split));

  // The same bulk walk must reach the live control path. Part 0 is routed to
  // unit 1 here so the second block is audible rather than merely held.
  const std::vector<float> live_dry = render_pushed({});
  const std::vector<float> live_split = render_pushed({split_unit0, split_unit1, kPart0OnUnit1});
  const std::vector<float> live_bulk = render_pushed({bulk_units, kPart0OnUnit1});
  REQUIRE(energy(live_dry) > 1.0e-6);
  REQUIRE(energy(live_bulk) > 1.0e-6);
  CHECK(static_cast<bool>(live_bulk != live_dry));
  CHECK(static_cast<bool>(live_bulk == live_split));
}

TEST_CASE("native: a bussed piano's board return is silenced by its part rig",
          "[midi][native][gsfx][piano]") {
  NativeSynthConfig dry_cfg = offline_config();
  dry_cfg.use_gm_programs = false;
  dry_cfg.patch.mode = SynthEngineMode::kPiano;

  NativeSynth dry(dry_cfg);
  dry.prepare(kRate, 256);
  dry.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 48, 110)));
  std::vector<float> dry_l(kSamples, 0.0f), dry_r(kSamples, 0.0f);
  float* dry_channels[] = {dry_l.data(), dry_r.data()};
  dry.process(dry_channels, 2, kSamples);

  NativeSynthConfig bussed_cfg = dry_cfg;
  NativeSynth bussed(bussed_cfg);
  sonare::midi::PartRig mute_rig;
  mute_rig.mode = sonare::midi::PartRigMode::kChain;
  mute_rig.stages = {{"utility.gain", R"({"levelDb":-1000})"}};
  REQUIRE(bussed.set_part_rig(0, mute_rig));
  bussed.prepare(kRate, 256);
  bussed.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 48, 110)));
  std::vector<float> bussed_l(kSamples, 0.0f), bussed_r(kSamples, 0.0f);
  float* bussed_channels[] = {bussed_l.data(), bussed_r.data()};
  bussed.process(bussed_channels, 2, kSamples);

  REQUIRE(energy(dry_l) > 1.0e-6);
  REQUIRE(energy(dry_r) > 1.0e-6);
  REQUIRE(energy(bussed_l) == 0.0);
  REQUIRE(energy(bussed_r) == 0.0);
}

TEST_CASE("native: reset adopts the final part rig after a full publication burst",
          "[midi][native][gsfx][quiescent]") {
  NativeSynthConfig cfg = offline_config();
  cfg.use_gm_programs = false;

  sonare::midi::PartRig identity;
  identity.mode = sonare::midi::PartRigMode::kChain;
  identity.stages = {{"utility.gain", R"({"levelDb":0})"}};
  sonare::midi::PartRig mute;
  mute.mode = sonare::midi::PartRigMode::kChain;
  mute.stages = {{"utility.gain", R"({"levelDb":-1000})"}};

  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  // Establish an audio-owned snapshot before filling the hand-off ring. With
  // no current snapshot, a burst can be drained without exercising retirement.
  const auto silent = sonare::test::render_stereo(synth, 256);
  REQUIRE(energy(silent.left) == 0.0);
  REQUIRE(energy(silent.right) == 0.0);

  constexpr int kRigPublishes = 64;
  for (int i = 0; i < kRigPublishes; ++i) REQUIRE(synth.set_part_rig(0, identity));
  REQUIRE(synth.set_part_rig(0, mute));
  synth.reset();
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 48, 110)));
  const auto after_reset = sonare::test::render_stereo(synth, kSamples);

  NativeSynth mute_oracle(cfg);
  REQUIRE(mute_oracle.set_part_rig(0, mute));
  mute_oracle.prepare(kRate, 256);
  mute_oracle.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 48, 110)));
  const auto expected_mute = sonare::test::render_stereo(mute_oracle, kSamples);

  NativeSynth identity_oracle(cfg);
  REQUIRE(identity_oracle.set_part_rig(0, identity));
  identity_oracle.prepare(kRate, 256);
  identity_oracle.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 48, 110)));
  const auto expected_identity = sonare::test::render_stereo(identity_oracle, kSamples);

  REQUIRE(energy(expected_identity.left) > 1.0e-6);
  REQUIRE(energy(expected_identity.right) > 1.0e-6);
  REQUIRE(energy(expected_mute.left) == 0.0);
  REQUIRE(energy(expected_mute.right) == 0.0);
  CHECK(static_cast<bool>(after_reset.left == expected_mute.left));
  CHECK(static_cast<bool>(after_reset.right == expected_mute.right));
  CHECK(energy(after_reset.left) != energy(expected_identity.left));
  CHECK(energy(after_reset.right) != energy(expected_identity.right));
}

TEST_CASE("native: physical bodies follow a held note through rig changes",
          "[midi][native][gsfx][gs-physical-review]") {
  for (const uint8_t program : {uint8_t{0}, uint8_t{25}}) {
    CAPTURE(program);
    NativeSynthConfig cfg = offline_config();
    cfg.use_gm_programs = false;
    cfg.patch = sonare::midi::synth::gm_fallback_patch(0, program);
    NativeSynth reference(cfg), changed(cfg);
    for (NativeSynth* synth : {&reference, &changed}) {
      synth->prepare(kRate, 256);
      synth->on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 48, 110)));
      sonare::test::render_stereo(*synth, 2048);
    }
    sonare::midi::PartRig mute;
    mute.mode = sonare::midi::PartRigMode::kChain;
    mute.stages = {{"utility.gain", R"({"levelDb":-1000})"}};
    REQUIRE(changed.set_part_rig(0, mute));
    const auto muted = sonare::test::render_stereo(changed, 4096);
    sonare::test::render_stereo(reference, 4096);
    REQUIRE(energy(muted.left) == 0.0);
    REQUIRE(energy(muted.right) == 0.0);
    sonare::midi::PartRig direct;
    direct.mode = sonare::midi::PartRigMode::kNone;
    REQUIRE(changed.set_part_rig(0, direct));
    const auto expected = sonare::test::render_stereo(reference, 4096);
    const auto resumed = sonare::test::render_stereo(changed, 4096);
    REQUIRE(energy(expected.left) > 1e-6);
    REQUIRE(static_cast<bool>(resumed.left == expected.left));
    REQUIRE(static_cast<bool>(resumed.right == expected.right));
  }
}

TEST_CASE("native: output gain scales the entire piano body",
          "[midi][native][piano][gs-physical-review]") {
  for (const bool factory : {false, true}) {
    CAPTURE(factory);
    const auto play = [factory](float gain) {
      NativeSynthConfig cfg = offline_config();
      cfg.use_gm_programs = false;
      cfg.patch = sonare::midi::synth::gm_fallback_patch(0, 0);
      cfg.gain = gain;
      if (!factory) cfg.insert_factory = nullptr;
      NativeSynth synth(cfg);
      synth.prepare(kRate, 256);
      synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 48, 110)));
      return sonare::test::render_stereo(synth, 12000);
    };
    const auto full = play(1.0f);
    const auto half = play(0.5f);
    const auto zero = play(0.0f);
    REQUIRE(energy(full.left) > 1e-6);
    float worst = 0.0f;
    for (size_t i = 0; i < full.left.size(); ++i) {
      worst = std::max(worst, std::fabs(half.left[i] - 0.5f * full.left[i]));
      worst = std::max(worst, std::fabs(half.right[i] - 0.5f * full.right[i]));
    }
    CAPTURE(worst);
    CHECK(worst < 1e-7f);
    CHECK(energy(zero.left) == 0.0);
    CHECK(energy(zero.right) == 0.0);
  }
}

TEST_CASE("native: All Sound Off preserves another part's body-only tail",
          "[midi][native][piano][gs-physical-review]") {
  for (const bool dc_block : {false, true}) {
    CAPTURE(dc_block);
    NativeSynthConfig cfg = offline_config();
    cfg.use_gm_programs = false;
    cfg.dc_block = dc_block;
    cfg.patch = sonare::midi::synth::gm_fallback_patch(0, 0);
    cfg.patch.amp_env.release_ms = 1.0f;
    NativeSynth reference(cfg), changed(cfg);
    for (NativeSynth* synth : {&reference, &changed}) {
      synth->prepare(kRate, 256);
      synth->on_event(0, event(sonare::midi::make_midi1_note_on(0, 1, 48, 110)));
      sonare::test::render_stereo(*synth, 4096);
      synth->on_event(0, event(sonare::midi::make_midi1_note_off(0, 1, 48, 0)));
      sonare::test::render_stereo(*synth, 4096);
      REQUIRE(synth->active_voice_count() == 0);
    }
    changed.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
    const auto expected = sonare::test::render_stereo(reference, 4096);
    const auto actual = sonare::test::render_stereo(changed, 4096);
    REQUIRE(energy(expected.left) > 1e-6);
    CHECK(static_cast<bool>(actual.left == expected.left));
    CHECK(static_cast<bool>(actual.right == expected.right));
    changed.on_event(0, event(sonare::midi::make_midi1_control_change(0, 1, 120, 0)));
    const auto silenced = sonare::test::render_stereo(changed, 4096);
    CHECK(energy(silenced.left) == 0.0);
    CHECK(energy(silenced.right) == 0.0);
  }
}

TEST_CASE("native: a body-only tail keeps its source when its rig changes",
          "[midi][native][gsfx][gs-physical-review]") {
  for (const bool start_bussed : {false, true}) {
    CAPTURE(start_bussed);
    NativeSynthConfig cfg = offline_config();
    cfg.use_gm_programs = false;
    cfg.patch = sonare::midi::synth::gm_fallback_patch(0, 0);
    cfg.patch.amp_env.release_ms = 1.0f;
    NativeSynth synth(cfg);
    sonare::midi::PartRig identity;
    identity.mode = sonare::midi::PartRigMode::kChain;
    identity.stages = {{"utility.gain", R"({"levelDb":0})"}};
    sonare::midi::PartRig direct;
    direct.mode = sonare::midi::PartRigMode::kNone;
    REQUIRE(synth.set_part_rig(0, start_bussed ? identity : direct));
    synth.prepare(kRate, 256);
    MidiEvent strike = event(sonare::midi::make_midi1_note_on(0, 0, 48, 110));
    strike.source_track_id = 1;
    synth.on_event(0, strike);
    const auto render_lanes = [&]() {
      std::array<std::vector<float>, 4> lanes;
      for (auto& lane : lanes) lane.assign(4096, 0.0f);
      float* fallback[] = {lanes[0].data(), lanes[1].data()};
      float* source[] = {lanes[2].data(), lanes[3].data()};
      const MidiInstrumentSourceOutput outputs[] = {{0, fallback}, {1, source}};
      REQUIRE(synth.process_source_tracks(outputs, 2, 2, 4096));
      return lanes;
    };
    render_lanes();
    MidiEvent release = event(sonare::midi::make_midi1_note_off(0, 0, 48, 0));
    release.source_track_id = 1;
    synth.on_event(0, release);
    render_lanes();
    REQUIRE(synth.active_voice_count() == 0);
    REQUIRE(synth.set_part_rig(0, start_bussed ? direct : identity));
    const auto tail = render_lanes();
    REQUIRE(energy(tail[2]) > 1e-6);
    CHECK(energy(tail[0]) < 1e-12);
    CHECK(energy(tail[1]) < 1e-12);
  }
}

TEST_CASE("native: absolute pitch selects the GM drum target piece",
          "[midi][native][midi2][gs-physical-review]") {
  for (const bool gm : {false, true}) {
    CAPTURE(gm);
    const auto play = [gm](uint8_t note, bool absolute) {
      NativeSynthConfig cfg = offline_config();
      cfg.use_gm_programs = gm;
      cfg.patch.mode = SynthEngineMode::kPercussion;
      cfg.patch.percussion.gm_kit = true;
      NativeSynth synth(cfg);
      synth.prepare(kRate, 256);
      if (absolute) {
        synth.on_event(0,
                       event(sonare::midi::make_midi2_per_note_controller(0, 9, 60, 3, 42u << 25)));
      }
      synth.on_event(0, event(sonare::midi::make_midi2_note_on(0, 9, note, 0xC000)));
      return sonare::test::render_stereo(synth, 8192);
    };
    const auto direct = play(42, false);
    const auto absolute = play(60, true);
    REQUIRE(energy(direct.left) > 1e-6);
    CHECK(static_cast<bool>(absolute.left == direct.left));
    CHECK(static_cast<bool>(absolute.right == direct.right));
  }
}

TEST_CASE("physical hosts: inactive EFX buses age source ownership before reuse",
          "[midi][native][sf2][gsfx][gs-physical-review]") {
  const auto exercise = [](auto& synth, bool unit, int idle_frames) {
    synth.prepare(kRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, kLead)));
    sonare::midi::PartRig identity;
    identity.mode = sonare::midi::PartRigMode::kChain;
    identity.stages = {{"utility.gain", R"({"levelDb":0})"}};
    sonare::midi::PartRig direct;
    direct.mode = sonare::midi::PartRigMode::kNone;
    if (unit) synth.on_event(0, sysex_event(kDistortion));
    const auto route = [&](bool enabled) {
      if (unit) {
        const auto msg = dt1(0x404122u, {static_cast<uint8_t>(enabled)});
        synth.on_event(0, sysex_event(msg));
      } else {
        REQUIRE(synth.set_part_rig(0, enabled ? identity : direct));
      }
    };
    const auto render = [&](int frames) {
      std::array<std::vector<float>, 6> lanes;
      for (auto& lane : lanes) lane.assign(frames, 0.0f);
      float* fallback[] = {lanes[0].data(), lanes[1].data()};
      float* old_source[] = {lanes[2].data(), lanes[3].data()};
      float* new_source[] = {lanes[4].data(), lanes[5].data()};
      const MidiInstrumentSourceOutput outputs[] = {
          {0, fallback}, {1, old_source}, {2, new_source}};
      REQUIRE(synth.process_source_tracks(outputs, 3, 2, frames));
      return lanes;
    };
    route(true);
    auto note = event(sonare::midi::make_midi1_note_on(0, 0, 48, 110));
    note.source_track_id = 1;
    synth.on_event(0, note);
    render(4096);
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 120, 0)));
    route(false);
    for (int i = 0; i < idle_frames; i += 256) render(256);
    route(true);
    note = event(sonare::midi::make_midi1_note_on(0, 0, 67, 110));
    note.source_track_id = 2;
    synth.on_event(0, note);
    const auto result = render(4096);
    REQUIRE(energy(result[4]) > 1e-6);
    return energy(result[2]);
  };
  for (const bool sf2 : {false, true}) {
    for (const bool unit : {false, true}) {
      CAPTURE(sf2, unit);
      const auto run = [&](int idle_frames) {
        if (sf2) {
          sonare::midi::synth::Sf2PlayerConfig cfg;
          cfg.gain = 1.0f;
          cfg.dc_block = false;
          cfg.realize_efx_inline = true;
          cfg.insert_factory = offline_config().insert_factory;
#if defined(SONARE_MIDI_WITH_FX)
          cfg.effects.enable_reverb = false;
          cfg.effects.enable_chorus = false;
          cfg.effects.enable_delay = false;
#endif
          sonare::midi::synth::Sf2Player synth(cfg);
          return exercise(synth, unit, idle_frames);
        }
        NativeSynth synth(offline_config());
        return exercise(synth, unit, idle_frames);
      };
      const double immediate = run(0);
      const double aged = run(96000);
      REQUIRE(immediate > 1e-8);
      CHECK(aged < immediate * 0.1);
    }
  }
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

TEST_CASE("native live: a later EFX assignment in a bulk DT1 reaches the control path",
          "[midi][native][gsfx][live]") {
  const std::vector<float> single = render_pushed({kDistortion, kPart0On});
  const std::vector<float> bulk = render_pushed({kDistortion, kPart0OnBulk});
  const std::vector<float> unassigned = render_pushed({kDistortion});
  REQUIRE(energy(single) > 1.0e-6);
  CHECK_FALSE(static_cast<bool>(single == unassigned));
  CHECK(bulk == single);
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
