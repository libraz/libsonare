/// @file part_rig_test.cpp
/// @brief Per-part rig selection (src/midi/part_rig.h) on the players: which
///        chain a part is built with, in what precedence, and the mono input
///        rule a chain carrying an amplifier takes.

#include "midi/part_rig.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#if defined(SONARE_WITH_MASTERING)

#include "mastering/api/insert_factory.h"
#include "midi/midi_event.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/midi_render.h"
#include "util/constants.h"

namespace {

using sonare::midi::kPartRigAllParts;
using sonare::midi::MidiEvent;
using sonare::midi::PartRig;
using sonare::midi::PartRigMode;
using sonare::midi::PartRigStage;
using sonare::midi::synth::gm_fallback_rig;
using sonare::midi::synth::gm_rig_chain;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::event;
using sonare::test::StereoRender;

constexpr double kOutRate = 48000.0;
using Names = std::vector<std::string>;

Sf2PlayerConfig with_factory() {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.realize_efx_inline = true;
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
#if defined(SONARE_MIDI_WITH_FX)
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
#endif
  return cfg;
}

PartRig chain(std::vector<PartRigStage> stages) {
  PartRig rig;
  rig.mode = PartRigMode::kChain;
  rig.stages = std::move(stages);
  return rig;
}

PartRig mode(PartRigMode m) {
  PartRig rig;
  rig.mode = m;
  return rig;
}

const PartRigStage kClipper{"saturation.softClipper", "{}"};
const PartRigStage kTube{"saturation.tube", R"({"driveDb":30})"};
const PartRigStage kCleanAmp{"saturation.ampSim", R"({"preset":"cleanCombo"})"};
const PartRigStage kDelay{"effects.delay.stereo", "{}"};

/// The bank's own default chain for @p program, by stage name.
Names bank_names(uint8_t program) {
  Names out;
  for (const auto& stage : gm_rig_chain(gm_fallback_rig(0, program).id)) out.push_back(stage.name);
  return out;
}

template <typename Player>
void send(Player& player, const sonare::midi::Ump& ump) {
  player.on_event(0, event(ump));
}

template <typename Player>
void program(Player& player, uint8_t channel, uint8_t value) {
  send(player, sonare::midi::make_midi1_program_change(0, channel, value));
}

/// Processes one block, which realises whatever the events above made pending.
template <typename Player>
void settle(Player& player) {
  (void)sonare::test::render_stereo(player, 256);
}

std::vector<uint8_t> dt1(uint32_t addr, std::vector<uint8_t> data) {
  const uint8_t a0 = static_cast<uint8_t>((addr >> 16) & 0x7Fu);
  const uint8_t a1 = static_cast<uint8_t>((addr >> 8) & 0x7Fu);
  const uint8_t a2 = static_cast<uint8_t>(addr & 0x7Fu);
  std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, a0, a1, a2};
  msg.insert(msg.end(), data.begin(), data.end());
  int sum = a0 + a1 + a2;
  for (const uint8_t b : data) sum += b;
  msg.push_back(static_cast<uint8_t>((128 - (sum % 128)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

/// The part-block nibble of part slot @p part (block 0 is the rhythm part).
uint32_t part_block(uint8_t part) {
  if (part == 9) return 0;
  return part < 9 ? static_cast<uint32_t>(part) + 1u : part;
}

template <typename Player>
void sysex(Player& player, const std::vector<uint8_t>& msg) {
  MidiEvent e;
  e.ump = sonare::midi::make_sysex_handle(0, 1);
  e.sysex_payload = msg.data();
  e.sysex_payload_size = msg.size();
  player.on_event(0, e);
}

/// Routes @p part into the spec insertion unit with an overdrive in it.
template <typename Player>
void route_to_unit(Player& player, uint8_t part) {
  sysex(player, dt1(0x400300u, {0x01, 0x10}));
  sysex(player, dt1(0x404022u | (part_block(part) << 8), {0x01}));
}

Sf2Player make(const Sf2PlayerConfig& cfg) {
  Sf2Player player(cfg);
  player.prepare(kOutRate, 256);
  return player;
}

/// GM playback with the factory wired, realised on the render thread.
NativeSynthConfig native_with_factory() {
  NativeSynthConfig cfg;
  cfg.gain = 1.0f;
  cfg.use_gm_programs = true;
  cfg.realize_efx_inline = true;
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  return cfg;
}

template <typename Player, typename Config>
StereoRender render_panned_on(const Config& cfg, const PartRig& rig, uint8_t pan) {
  Player player(cfg);
  REQUIRE(player.set_part_rig(0, rig));
  player.prepare(kOutRate, 256);
  program(player, 0, 29);
  send(player, sonare::midi::make_midi1_control_change(0, 0, 10, pan));
  send(player, sonare::midi::make_midi1_note_on(0, 0, 52, 110));
  return sonare::test::render_stereo(player, 24000);
}

StereoRender render_panned(const Sf2PlayerConfig& cfg, const PartRig& rig, uint8_t pan) {
  return render_panned_on<Sf2Player>(cfg, rig, pan);
}

StereoRender render_panned(const NativeSynthConfig& cfg, const PartRig& rig, uint8_t pan) {
  return render_panned_on<NativeSynth>(cfg, rig, pan);
}

double relative_rms_difference(const std::vector<float>& reference,
                               const std::vector<float>& candidate, float candidate_scale) {
  double error = 0.0;
  double energy = 0.0;
  for (size_t i = 0; i < reference.size(); ++i) {
    const double delta =
        static_cast<double>(reference[i]) - static_cast<double>(candidate[i]) * candidate_scale;
    error += delta * delta;
    energy += static_cast<double>(reference[i]) * reference[i];
  }
  return energy > 0.0 ? std::sqrt(error / energy) : 0.0;
}

}  // namespace

TEST_CASE("sf2: with no entry a part gets the bank rig for its program", "[midi][rig][part]") {
  Sf2Player player = make(with_factory());
  program(player, 0, 29);
  program(player, 1, 0);
  settle(player);
  REQUIRE_FALSE(bank_names(29).empty());
  REQUIRE(player.part_rig_stage_names(0) == bank_names(29));
  REQUIRE(player.part_rig_stage_names(1).empty());
}

TEST_CASE("sf2: an explicit chain replaces the bank rig rather than stacking on it",
          "[midi][rig][part]") {
  Sf2Player player = make(with_factory());
  REQUIRE(player.set_part_rig(0, chain({kCleanAmp, kDelay})));
  REQUIRE(player.set_part_rig(1, chain({kClipper})));
  program(player, 0, 29);
  program(player, 1, 29);
  program(player, 2, 0);
  REQUIRE(player.set_part_rig(2, chain({kClipper})));
  settle(player);
  // One amplifier, the host's: no second ampSim from the bank preset.
  REQUIRE(player.part_rig_stage_names(0) == Names{kCleanAmp.processor, kDelay.processor});
  REQUIRE(player.part_rig_stage_names(1) == Names{kClipper.processor});
  // A chain applies on a program the bank binds nothing to as well.
  REQUIRE(player.part_rig_stage_names(2) == Names{kClipper.processor});
}

TEST_CASE("sf2: kNone gives the DI and an explicit kBank gives the default back",
          "[midi][rig][part]") {
  Sf2Player player = make(with_factory());
  program(player, 0, 29);
  REQUIRE(player.set_part_rig(0, mode(PartRigMode::kNone)));
  settle(player);
  REQUIRE(player.part_rig_stage_names(0).empty());
  REQUIRE(player.set_part_rig(0, mode(PartRigMode::kBank)));
  settle(player);
  REQUIRE(player.part_rig_stage_names(0) == bank_names(29));
}

TEST_CASE("sf2: the destination default applies to every part an entry does not name",
          "[midi][rig][part]") {
  SECTION("kNone everywhere, one part opted back into the bank") {
    Sf2Player player = make(with_factory());
    REQUIRE(player.set_part_rig(kPartRigAllParts, mode(PartRigMode::kNone)));
    REQUIRE(player.set_part_rig(0, mode(PartRigMode::kBank)));
    program(player, 0, 29);
    program(player, 1, 29);
    settle(player);
    REQUIRE(player.part_rig_stage_names(0) == bank_names(29));
    REQUIRE(player.part_rig_stage_names(1).empty());
  }
  SECTION("a chain everywhere, one part cleared") {
    Sf2Player player = make(with_factory());
    REQUIRE(player.set_part_rig(kPartRigAllParts, chain({kClipper})));
    REQUIRE(player.set_part_rig(3, mode(PartRigMode::kNone)));
    program(player, 0, 0);
    program(player, 3, 29);
    settle(player);
    REQUIRE(player.part_rig_stage_names(0) == Names{kClipper.processor});
    REQUIRE(player.part_rig_stage_names(5) == Names{kClipper.processor});
    REQUIRE(player.part_rig_stage_names(3).empty());
  }
}

TEST_CASE("sf2: clear_bank_rig ranks below every entry and above the bank", "[midi][rig][part]") {
  Sf2PlayerConfig cleared = with_factory();
  cleared.bank_rig_binding = false;
  SECTION("no entry: no rig") {
    Sf2Player player = make(cleared);
    program(player, 0, 29);
    settle(player);
    REQUIRE(player.part_rig_stage_names(0).empty());
  }
  SECTION("a part entry of kBank outranks it") {
    Sf2Player player = make(cleared);
    REQUIRE(player.set_part_rig(0, mode(PartRigMode::kBank)));
    program(player, 0, 29);
    program(player, 1, 29);
    settle(player);
    REQUIRE(player.part_rig_stage_names(0) == bank_names(29));
    REQUIRE(player.part_rig_stage_names(1).empty());
  }
  SECTION("a destination default outranks it") {
    Sf2Player player = make(cleared);
    REQUIRE(player.set_part_rig(kPartRigAllParts, chain({kClipper})));
    program(player, 0, 29);
    settle(player);
    REQUIRE(player.part_rig_stage_names(0) == Names{kClipper.processor});
  }
}

TEST_CASE("sf2: a GS route retires the bank rig but runs in series with a chain",
          "[midi][rig][part]") {
  SECTION("bank rig") {
    Sf2Player player = make(with_factory());
    program(player, 0, 29);
    route_to_unit(player, 0);
    settle(player);
    REQUIRE(player.part_rig_stage_names(0).empty());
  }
  SECTION("explicit chain") {
    Sf2Player player = make(with_factory());
    REQUIRE(player.set_part_rig(0, chain({kClipper})));
    program(player, 0, 29);
    route_to_unit(player, 0);
    settle(player);
    REQUIRE(player.part_rig_stage_names(0) == Names{kClipper.processor});
  }
}

TEST_CASE("sf2: a rig belongs to the part slot, not to the channel it listens on",
          "[midi][rig][part]") {
  Sf2Player player = make(with_factory());
  REQUIRE(player.set_part_rig(0, chain({kClipper})));
  // Part 0 now listens on channel 3 alongside part 2; a program change there
  // reaches both slots and each keeps the rig its own slot was given.
  sysex(player, dt1(0x401002u | (part_block(0) << 8), {0x02}));
  program(player, 2, 29);
  settle(player);
  REQUIRE(player.part_rig_stage_names(0) == Names{kClipper.processor});
  REQUIRE(player.part_rig_stage_names(2) == bank_names(29));
}

TEST_CASE("sf2: an invalid entry is refused and changes nothing", "[midi][rig][part]") {
  Sf2Player player = make(with_factory());
  program(player, 0, 29);
  REQUIRE_FALSE(player.set_part_rig(16, mode(PartRigMode::kNone)));
  REQUIRE_FALSE(player.set_part_rig(0, chain({})));
  PartRig stray = mode(PartRigMode::kNone);
  stray.stages.push_back(kClipper);
  REQUIRE_FALSE(player.set_part_rig(0, stray));
  settle(player);
  REQUIRE(player.part_rig_stage_names(0) == bank_names(29));
}

TEST_CASE("sf2: a chain carrying an amplifier feeds it a mono pickup ahead of CC10",
          "[midi][rig][part]") {
  const PartRig amped =
      chain({{"saturation.ampSim", R"({"preset":"britStack","inputDb":24})"}, kDelay});
  const StereoRender center = render_panned(with_factory(), amped, 64);
  const StereoRender left = render_panned(with_factory(), amped, 0);
  // Pan sits after the amplifier, so the hard-left leg is the centred one
  // louder by the pan law and otherwise the same waveform.
  REQUIRE(relative_rms_difference(center.left, left.left, sonare::constants::kInvSqrt2) < 1.0e-4);
}

TEST_CASE("sf2: a chain without an amplifier stays stereo", "[midi][rig][part]") {
  const PartRig driven = chain({kTube});
  const StereoRender center = render_panned(with_factory(), driven, 64);
  const StereoRender left = render_panned(with_factory(), driven, 0);
  // The tube is fed the panned legs, so the hard-left one is driven harder and
  // is not the centred waveform scaled.
  REQUIRE(relative_rms_difference(center.left, left.left, sonare::constants::kInvSqrt2) > 1.0e-2);
}

TEST_CASE("native: with no entry a part gets the bank rig for its program", "[midi][rig][part]") {
  NativeSynth synth(native_with_factory());
  synth.prepare(kOutRate, 256);
  program(synth, 0, 29);
  program(synth, 1, 0);
  settle(synth);
  REQUIRE_FALSE(bank_names(29).empty());
  REQUIRE(synth.part_rig_stage_names(0) == bank_names(29));
  REQUIRE(synth.part_rig_stage_names(1).empty());
}

TEST_CASE("native: an explicit chain replaces the bank rig rather than stacking on it",
          "[midi][rig][part]") {
  NativeSynth synth(native_with_factory());
  synth.prepare(kOutRate, 256);
  REQUIRE(synth.set_part_rig(0, chain({kCleanAmp, kDelay})));
  REQUIRE(synth.set_part_rig(1, chain({kClipper})));
  program(synth, 0, 29);
  program(synth, 1, 29);
  program(synth, 2, 0);
  REQUIRE(synth.set_part_rig(2, chain({kClipper})));
  settle(synth);
  REQUIRE(synth.part_rig_stage_names(0) == Names{kCleanAmp.processor, kDelay.processor});
  REQUIRE(synth.part_rig_stage_names(1) == Names{kClipper.processor});
  REQUIRE(synth.part_rig_stage_names(2) == Names{kClipper.processor});
}

TEST_CASE("native: kNone gives the DI and an explicit kBank gives the default back",
          "[midi][rig][part]") {
  NativeSynth synth(native_with_factory());
  synth.prepare(kOutRate, 256);
  program(synth, 0, 29);
  REQUIRE(synth.set_part_rig(0, mode(PartRigMode::kNone)));
  settle(synth);
  REQUIRE(synth.part_rig_stage_names(0).empty());
  REQUIRE(synth.set_part_rig(0, mode(PartRigMode::kBank)));
  settle(synth);
  REQUIRE(synth.part_rig_stage_names(0) == bank_names(29));
}

TEST_CASE("native: the destination default applies to every part an entry does not name",
          "[midi][rig][part]") {
  SECTION("kNone everywhere, one part opted back into the bank") {
    NativeSynth synth(native_with_factory());
    synth.prepare(kOutRate, 256);
    REQUIRE(synth.set_part_rig(kPartRigAllParts, mode(PartRigMode::kNone)));
    REQUIRE(synth.set_part_rig(0, mode(PartRigMode::kBank)));
    program(synth, 0, 29);
    program(synth, 1, 29);
    settle(synth);
    REQUIRE(synth.part_rig_stage_names(0) == bank_names(29));
    REQUIRE(synth.part_rig_stage_names(1).empty());
  }
  SECTION("a chain everywhere, one part cleared") {
    NativeSynth synth(native_with_factory());
    synth.prepare(kOutRate, 256);
    REQUIRE(synth.set_part_rig(kPartRigAllParts, chain({kClipper})));
    REQUIRE(synth.set_part_rig(3, mode(PartRigMode::kNone)));
    program(synth, 0, 0);
    program(synth, 3, 29);
    settle(synth);
    REQUIRE(synth.part_rig_stage_names(0) == Names{kClipper.processor});
    REQUIRE(synth.part_rig_stage_names(5) == Names{kClipper.processor});
    REQUIRE(synth.part_rig_stage_names(3).empty());
  }
}

TEST_CASE("native: an unbound bank rig ranks below every entry", "[midi][rig][part]") {
  NativeSynthConfig cleared = native_with_factory();
  cleared.bank_rig_binding = false;
  SECTION("no entry: no rig") {
    NativeSynth synth(cleared);
    synth.prepare(kOutRate, 256);
    program(synth, 0, 29);
    settle(synth);
    REQUIRE(synth.part_rig_stage_names(0).empty());
  }
  SECTION("a part entry of kBank outranks it") {
    NativeSynth synth(cleared);
    synth.prepare(kOutRate, 256);
    REQUIRE(synth.set_part_rig(0, mode(PartRigMode::kBank)));
    program(synth, 0, 29);
    program(synth, 1, 29);
    settle(synth);
    REQUIRE(synth.part_rig_stage_names(0) == bank_names(29));
    REQUIRE(synth.part_rig_stage_names(1).empty());
  }
  SECTION("a destination default outranks it") {
    NativeSynth synth(cleared);
    synth.prepare(kOutRate, 256);
    REQUIRE(synth.set_part_rig(kPartRigAllParts, chain({kClipper})));
    program(synth, 0, 29);
    settle(synth);
    REQUIRE(synth.part_rig_stage_names(0) == Names{kClipper.processor});
  }
}

TEST_CASE("native: a GS route retires the bank rig but runs in series with a chain",
          "[midi][rig][part]") {
  SECTION("bank rig") {
    NativeSynth synth(native_with_factory());
    synth.prepare(kOutRate, 256);
    program(synth, 0, 29);
    route_to_unit(synth, 0);
    settle(synth);
    REQUIRE(synth.part_rig_stage_names(0).empty());
  }
  SECTION("explicit chain") {
    NativeSynth synth(native_with_factory());
    synth.prepare(kOutRate, 256);
    REQUIRE(synth.set_part_rig(0, chain({kClipper})));
    program(synth, 0, 29);
    route_to_unit(synth, 0);
    settle(synth);
    REQUIRE(synth.part_rig_stage_names(0) == Names{kClipper.processor});
  }
}

TEST_CASE("native: without GM programs kBank binds nothing and a chain still applies",
          "[midi][rig][part]") {
  NativeSynthConfig fixed = native_with_factory();
  fixed.use_gm_programs = false;
  NativeSynth synth(fixed);
  synth.prepare(kOutRate, 256);
  REQUIRE(synth.set_part_rig(1, chain({kClipper})));
  program(synth, 0, 29);
  // A GS route needs the GS layer, which this synth reads only for GM playback.
  route_to_unit(synth, 2);
  settle(synth);
  REQUIRE(synth.part_rig_stage_names(0).empty());
  REQUIRE(synth.part_rig_stage_names(1) == Names{kClipper.processor});
  REQUIRE(synth.part_rig_stage_names(2).empty());
}

TEST_CASE("native: an invalid entry is refused and changes nothing", "[midi][rig][part]") {
  NativeSynth synth(native_with_factory());
  synth.prepare(kOutRate, 256);
  program(synth, 0, 29);
  REQUIRE_FALSE(synth.set_part_rig(16, mode(PartRigMode::kNone)));
  REQUIRE_FALSE(synth.set_part_rig(0, chain({})));
  PartRig stray = mode(PartRigMode::kNone);
  stray.stages.push_back(kClipper);
  REQUIRE_FALSE(synth.set_part_rig(0, stray));
  settle(synth);
  REQUIRE(synth.part_rig_stage_names(0) == bank_names(29));
}

TEST_CASE("native: a chain carrying an amplifier feeds it a mono pickup ahead of CC10",
          "[midi][rig][part]") {
  const PartRig amped =
      chain({{"saturation.ampSim", R"({"preset":"britStack","inputDb":24})"}, kDelay});
  const StereoRender center = render_panned(native_with_factory(), amped, 64);
  const StereoRender left = render_panned(native_with_factory(), amped, 0);
  REQUIRE(relative_rms_difference(center.left, left.left, sonare::constants::kInvSqrt2) < 1.0e-4);
}

TEST_CASE("native: a chain without an amplifier stays stereo", "[midi][rig][part]") {
  const PartRig driven = chain({kTube});
  const StereoRender center = render_panned(native_with_factory(), driven, 64);
  const StereoRender left = render_panned(native_with_factory(), driven, 0);
  REQUIRE(relative_rms_difference(center.left, left.left, sonare::constants::kInvSqrt2) > 1.0e-2);
}

#endif  // SONARE_WITH_MASTERING
