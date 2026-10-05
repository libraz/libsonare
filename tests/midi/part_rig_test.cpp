/// @file part_rig_test.cpp
/// @brief Per-part rig selection (src/midi/part_rig.h) on the players: which
///        chain a part is built with, in what precedence, and the mono input
///        rule a chain carrying an amplifier takes.

#include "midi/part_rig.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(SONARE_WITH_MASTERING)

#include "mastering/api/insert_factory.h"
#include "midi/midi_event.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_efx_processor.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "rt/processor_base.h"
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

/// A no-op insert whose latency is visible through the same Q8 interface used
/// by the part-rig guard. The rate-scaled variant models an insert whose delay
/// is expressed in seconds, so a 256-sample delay at 48 kHz is 512 samples at
/// 96 kHz while still being the same physical latency.
class RigLatencyInsert final : public sonare::rt::ProcessorBase {
 public:
  RigLatencyInsert(int latency_samples_q8, bool scale_with_rate)
      : reference_latency_samples_q8_(latency_samples_q8),
        scale_with_rate_(scale_with_rate),
        latency_samples_q8_(latency_samples_q8) {}

  void prepare(double sample_rate, int) override {
    latency_samples_q8_ =
        scale_with_rate_
            ? static_cast<int>(std::lround(static_cast<double>(reference_latency_samples_q8_) *
                                           sample_rate / kOutRate))
            : reference_latency_samples_q8_;
  }
  void process(float* const*, int, int) override {}
  void reset() override {}
  int latency_samples() const noexcept override { return latency_samples_q8_ >> 8; }
  int latency_samples_q8() const noexcept override { return latency_samples_q8_; }

 private:
  int reference_latency_samples_q8_;
  bool scale_with_rate_;
  int latency_samples_q8_;
};

sonare::midi::synth::GsEfxStageFactory rig_latency_factory() {
  return [](std::string_view name, std::string_view) -> std::unique_ptr<sonare::rt::ProcessorBase> {
    if (name == "test.latency.256") {
      return std::make_unique<RigLatencyInsert>(256 << 8, false);
    }
    if (name == "test.latency.257") {
      return std::make_unique<RigLatencyInsert>(257 << 8, false);
    }
    if (name == "test.latency.256.5") {
      return std::make_unique<RigLatencyInsert>((256 << 8) + (1 << 7), false);
    }
    if (name == "test.latency.rate") {
      return std::make_unique<RigLatencyInsert>(256 << 8, true);
    }
    return nullptr;
  };
}

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

Sf2PlayerConfig with_rig_latency_factory() {
  Sf2PlayerConfig cfg = with_factory();
  cfg.insert_factory = rig_latency_factory();
  return cfg;
}

NativeSynthConfig native_with_rig_latency_factory() {
  NativeSynthConfig cfg = native_with_factory();
  cfg.insert_factory = rig_latency_factory();
  return cfg;
}

template <typename Player, typename Config>
void check_rig_latency_limit(const Config& config, bool prepared_before_install) {
  INFO("player=" << (std::is_same_v<Player, Sf2Player> ? "sf2" : "native"));
  Player player(config);
  const PartRig accepted = chain({{"test.latency.256", "{}"}});
  const PartRig rejected = chain({{"test.latency.257", "{}"}});
  const PartRig fractional = chain({{"test.latency.256.5", "{}"}});
  const Names expected_names{accepted.stages.front().processor};

  if (prepared_before_install) {
    player.prepare(kOutRate, 512);
    REQUIRE(player.set_part_rig(0, accepted));
    REQUIRE(player.part_rig_stage_names(0) == expected_names);
    CHECK_FALSE(player.set_part_rig(0, rejected));
    CHECK(player.part_rig_stage_names(0) == expected_names);
    CHECK_FALSE(player.set_part_rig(0, fractional));
    CHECK(player.part_rig_stage_names(0) == expected_names);
    return;
  }

  REQUIRE(player.set_part_rig(0, accepted));
  REQUIRE(player.part_rig_stage_names(0).empty());
  CHECK_FALSE(player.set_part_rig(0, rejected));
  CHECK(player.part_rig_stage_names(0).empty());
  CHECK_FALSE(player.set_part_rig(0, fractional));
  CHECK(player.part_rig_stage_names(0).empty());
  player.prepare(kOutRate, 512);
  CHECK(player.part_rig_stage_names(0) == expected_names);
}

template <typename Player, typename Config>
void check_reference_rate_rig_limit(const Config& config) {
  Player player(config);
  player.prepare(96000.0, 512);
  REQUIRE(player.set_part_rig(0, chain({{"test.latency.rate", "{}"}})));
  REQUIRE(player.part_rig_stage_names(0) == Names{"test.latency.rate"});
}

template <typename Player, typename Config>
void check_gs_unit_tail(const Config& input_config, bool rigged) {
  INFO("player=" << (std::is_same_v<Player, Sf2Player> ? "sf2" : "native"));
  Config config = input_config;
  config.bank_rig_binding = false;
  Player player(config);
  if (rigged) REQUIRE(player.set_part_rig(0, chain({kDelay})));
  player.prepare(kOutRate, 256);
  const int base_tail = player.tail_samples();

  const auto factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  sonare::midi::synth::GsEfxProcessor reference(
      0x0110, sonare::midi::synth::GsEfxRealization::kModern, factory);
  reference.prepare(kOutRate, 256, 2);
  REQUIRE(reference.tail_samples() > 0);

  route_to_unit(player, 0);
  settle(player);
  CHECK(player.tail_samples() == base_tail + reference.tail_samples());
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

TEST_CASE("part rig: chain latency is capped at 256 samples at 48 kHz",
          "[midi][rig][part][latency]") {
  for (const bool prepared : {false, true}) {
    INFO("prepared before install=" << prepared);
    check_rig_latency_limit<Sf2Player>(with_rig_latency_factory(), prepared);
    check_rig_latency_limit<NativeSynth>(native_with_rig_latency_factory(), prepared);
  }
}

TEST_CASE("part rig: latency validation uses the 48 kHz reference rate",
          "[midi][rig][part][latency]") {
  check_reference_rate_rig_limit<Sf2Player>(with_rig_latency_factory());
  check_reference_rate_rig_limit<NativeSynth>(native_with_rig_latency_factory());
}

TEST_CASE("part rig: routed GS units report their complete graph tail",
          "[midi][rig][part][latency][gs][efx]") {
  check_gs_unit_tail<Sf2Player>(with_factory(), false);
  check_gs_unit_tail<NativeSynth>(native_with_factory(), false);
}

TEST_CASE("part rig: routed GS units follow an explicit chain tail",
          "[midi][rig][part][latency][gs][efx]") {
  check_gs_unit_tail<Sf2Player>(with_factory(), true);
  check_gs_unit_tail<NativeSynth>(native_with_factory(), true);
}

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

namespace {

enum class RigSource : uint8_t {
  kPartChain,
  kPartNone,
  kPartBank,
  kDestNone,
  kDestChain,
  kClearBankRig,
  kNone
};

struct RigRow {
  RigSource source;
  bool efx_route;
  bool with_amp;
  uint8_t program;
  bool clear_bank_rig;
  bool gm_programs;  ///< Native only; the Sf2Player always plays GM programs.
};

const char* source_name(RigSource s) {
  switch (s) {
    case RigSource::kPartChain:
      return "part-chain";
    case RigSource::kPartNone:
      return "part-none";
    case RigSource::kPartBank:
      return "part-bank";
    case RigSource::kDestNone:
      return "dest-none";
    case RigSource::kDestChain:
      return "dest-chain";
    case RigSource::kClearBankRig:
      return "clear_bank_rig";
    case RigSource::kNone:
      return "none";
  }
  return "";
}

PartRig row_chain(const RigRow& row) {
  return row.with_amp ? chain({kCleanAmp, kDelay}) : chain({kClipper});
}

/// The stage names part 0 must realise, by the resolution order: part entry,
/// destination default, clear_bank_rig, bank. A GS route retires the bank rig.
Names expected_names(const RigRow& row) {
  switch (row.source) {
    case RigSource::kPartChain:
    case RigSource::kDestChain: {
      Names out;
      for (const auto& stage : row_chain(row).stages) out.push_back(stage.processor);
      return out;
    }
    case RigSource::kPartNone:
    case RigSource::kDestNone:
    case RigSource::kClearBankRig:
      return {};
    case RigSource::kPartBank:
    case RigSource::kNone:
      return row.gm_programs && !row.efx_route ? bank_names(row.program) : Names{};
  }
  return {};
}

/// Applies the row to a fresh player and returns part 0's realised stage names.
template <typename Player, typename Config>
Names realised_names(Config cfg, const RigRow& row) {
  cfg.bank_rig_binding = !row.clear_bank_rig;
  if constexpr (std::is_same_v<Player, NativeSynth>) cfg.use_gm_programs = row.gm_programs;
  Player player(cfg);
  player.prepare(kOutRate, 256);
  switch (row.source) {
    case RigSource::kPartChain:
      REQUIRE(player.set_part_rig(0, row_chain(row)));
      REQUIRE(player.set_part_rig(kPartRigAllParts, mode(PartRigMode::kNone)));
      break;
    case RigSource::kPartNone:
      REQUIRE(player.set_part_rig(0, mode(PartRigMode::kNone)));
      REQUIRE(player.set_part_rig(kPartRigAllParts, chain({kDelay})));
      break;
    case RigSource::kPartBank:
      REQUIRE(player.set_part_rig(0, mode(PartRigMode::kBank)));
      REQUIRE(player.set_part_rig(kPartRigAllParts, mode(PartRigMode::kNone)));
      break;
    case RigSource::kDestNone:
      REQUIRE(player.set_part_rig(kPartRigAllParts, mode(PartRigMode::kNone)));
      break;
    case RigSource::kDestChain:
      REQUIRE(player.set_part_rig(kPartRigAllParts, row_chain(row)));
      break;
    case RigSource::kClearBankRig:
    case RigSource::kNone:
      break;
  }
  program(player, 0, row.program);
  if (row.efx_route) route_to_unit(player, 0);
  settle(player);
  return player.part_rig_stage_names(0);
}

void check_row(const RigRow& row, const Names& got, const char* player) {
  INFO(player << " source=" << source_name(row.source) << " efx=" << row.efx_route
              << " amp=" << row.with_amp << " program=" << int(row.program)
              << " clear=" << row.clear_bank_rig << " gm=" << row.gm_programs);
  REQUIRE(got == expected_names(row));
  if (row.with_amp) {
    // Exactly one amplifier: the chain's, never the bank preset's on top.
    REQUIRE(std::count(got.begin(), got.end(), std::string("saturation.ampSim")) == 1);
  }
}

}  // namespace

// Rows completing pairwise coverage of {player, rig source, EFX route, amp in
// chain, program, clear_bank_rig, GM programs} beyond the cases above. A chain
// holding an amp is only meaningful for the two chain sources, and an EFX route
// exists only where the player reads GS (never native without GM programs).
TEST_CASE("part rig: sources resolve in order across the remaining combinations",
          "[midi][rig][part]") {
  const RigRow sf2_rows[] = {
      {RigSource::kDestNone, true, false, 0, true, true},
      {RigSource::kPartNone, true, false, 29, true, true},
      {RigSource::kPartNone, false, false, 0, false, true},
      {RigSource::kClearBankRig, true, false, 29, true, true},
  };
  for (const RigRow& row : sf2_rows) {
    check_row(row, realised_names<Sf2Player>(with_factory(), row), "sf2");
  }
  const RigRow native_rows[] = {
      {RigSource::kDestChain, true, true, 0, true, true},
      {RigSource::kPartBank, true, false, 0, false, true},
      {RigSource::kPartBank, false, false, 29, true, false},
      {RigSource::kClearBankRig, false, false, 0, true, false},
      {RigSource::kDestChain, false, true, 29, true, false},
      {RigSource::kPartNone, false, false, 29, false, false},
      {RigSource::kPartChain, true, false, 29, true, true},
      {RigSource::kDestNone, false, false, 29, false, false},
  };
  for (const RigRow& row : native_rows) {
    check_row(row, realised_names<NativeSynth>(native_with_factory(), row), "native");
  }
}

#endif  // SONARE_WITH_MASTERING
