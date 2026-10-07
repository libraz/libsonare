/// @file gs_efx_controller_settle_test.cpp
/// @brief A GS EFX unit is never observed ahead of the controllers it holds:
///        the tail a player publishes, and the tail an offline host sizes a
///        render from, already cover the EFX CONTROL modulation, and a unit
///        rebuilt under the other realisation keeps following its source.
///
/// Every case compares against a player that reaches the same slot byte with
/// no controller at all, so a tail or an output that ignored the controller
/// shows as a mismatch rather than as a plausible number.

#include <catch2/catch_test_macros.hpp>

#if defined(SONARE_MIDI_WITH_FX) && defined(SONARE_WITH_MASTERING)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "midi/instrument.h"
#include "midi/midi_event.h"
#include "midi/synth/gs_classic/model_registry.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/midi_render.h"
#include "support/sf2_builder.h"
#include "util/constants.h"

namespace {

namespace s = sonare::midi::synth;
namespace m = sonare::midi;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

constexpr uint16_t kStereoDelay = 0x0150;  // `+` is slot 2, the feedback
constexpr uint16_t kStereoEq = 0x0101;     // `#` is slot 19, the level
constexpr uint8_t kSourceCc20 = 0x14;
constexpr uint8_t kDepthPlus = 0x7F;
constexpr uint8_t kDepthZero = 0x40;
constexpr uint8_t kCc = 20;
/// Where CONTROL at full positive depth and a full controller puts a base of
/// 40 on the feedback slot: the top of its printed range.
constexpr uint8_t kModulatedFeedback = 0x71;

using Message = std::vector<uint8_t>;

/// A GS DT1 write of @p value at 40 03 @p offset.
Message efx_write(uint8_t offset, uint8_t value) {
  Message msg = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, offset, value, 0x00, 0xF7};
  const uint32_t sum = msg[5] + msg[6] + msg[7] + msg[8];
  msg[9] = static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
  return msg;
}

Message type_write(uint16_t type) {
  Message msg = {0xF0,
                 0x41,
                 0x10,
                 0x42,
                 0x12,
                 0x40,
                 0x03,
                 0x00,
                 static_cast<uint8_t>(type >> 8),
                 static_cast<uint8_t>(type & 0x7F),
                 0x00,
                 0xF7};
  const uint32_t sum = msg[5] + msg[6] + msg[7] + msg[8] + msg[9];
  msg[10] = static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
  return msg;
}

/// Part 1 (channel 0) routed into the spec unit.
const Message kPartOn = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x01, 0x5C, 0xF7};

uint8_t slot_offset(uint8_t slot) { return static_cast<uint8_t>(0x03 + slot); }
uint8_t source_offset(size_t control) { return static_cast<uint8_t>(0x1B + 2 * control); }
uint8_t depth_offset(size_t control) { return static_cast<uint8_t>(0x1C + 2 * control); }

/// The unit-0 setup a file sends: the type, the controlled slot's base, and
/// CONTROL @p control's source and depth (source 0 leaves it off).
std::vector<Message> unit_setup(uint16_t type, uint8_t slot, uint8_t base, size_t control,
                                uint8_t source, uint8_t depth) {
  return {kPartOn,
          type_write(type),
          efx_write(0x17, 0x00),  // no EFX reverb send
          efx_write(slot_offset(slot), base),
          efx_write(source_offset(control), source),
          efx_write(depth_offset(control), depth)};
}

s::GsEfxStageFactory factory() {
  return [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
}

/// Feeds @p msg the way an offline host's event replay does: prepared first,
/// then delivered with its token, which @p tokens keeps alive.
void replay_sysex(m::MidiInstrument& instrument, const Message& msg,
                  std::vector<std::shared_ptr<const m::PreparedMidiSysEx>>& tokens) {
  std::shared_ptr<const m::PreparedMidiSysEx> token;
  REQUIRE(instrument.prepare_sysex(msg.data(), msg.size(), token));
  m::MidiEvent ev;
  ev.ump = m::make_sysex_handle(0, 1);
  ev.sysex_payload = msg.data();
  ev.sysex_payload_size = msg.size();
  ev.prepared_sysex = token.get();
  tokens.push_back(std::move(token));
  instrument.on_event(0, ev);
}

void send_cc(m::MidiInstrument& instrument, uint8_t value) {
  instrument.on_event(0, sonare::test::event(m::make_midi1_control_change(0, 0, kCc, value)));
}

void render_blocks(m::MidiInstrument& instrument, int blocks,
                   std::vector<float>* capture = nullptr) {
  std::vector<float> left(kBlock);
  std::vector<float> right(kBlock);
  for (int b = 0; b < blocks; ++b) {
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    float* chans[2] = {left.data(), right.data()};
    instrument.process(chans, 2, kBlock);
    for (int i = 0; i < kBlock; ++i) {
      REQUIRE(std::isfinite(left[static_cast<size_t>(i)]));
      REQUIRE(std::isfinite(right[static_cast<size_t>(i)]));
    }
    if (capture != nullptr) {
      capture->insert(capture->end(), left.begin(), left.end());
      capture->insert(capture->end(), right.begin(), right.end());
    }
  }
}

s::Sf2PlayerConfig sf2_config(bool inline_efx, s::GsEfxRealization realization) {
  s::Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.dc_block = false;
  cfg.insert_factory = factory();
  cfg.realize_efx_inline = inline_efx;
  cfg.gs_efx_realization = realization;
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
  return cfg;
}

s::NativeSynthConfig native_config() {
  s::NativeSynthConfig cfg;
  cfg.gain = 1.0f;
  cfg.use_gm_programs = true;
  cfg.insert_factory = factory();
  cfg.bank_rig_binding = false;
  cfg.realize_efx_inline = true;
  return cfg;
}

/// The tail an offline host reads after replaying @p setup and then moving the
/// controller to @p cc, each event followed by the probe, as a bounce does.
template <typename Instrument, typename Config>
int probed_tail(const Config& cfg, const std::vector<Message>& setup, int cc) {
  Instrument instrument(cfg);
  instrument.prepare(kRate, kBlock);
  std::vector<std::shared_ptr<const m::PreparedMidiSysEx>> tokens;
  for (const Message& msg : setup) {
    replay_sysex(instrument, msg, tokens);
    REQUIRE(instrument.materialize_tail_probe());
  }
  if (cc >= 0) {
    send_cc(instrument, static_cast<uint8_t>(cc));
    REQUIRE(instrument.materialize_tail_probe());
  }
  return instrument.tail_samples();
}

TEST_CASE("the native synth's tail probe covers a controller-moved feedback",
          "[gs][gs-efx][gs-efx-control][native]") {
  const s::NativeSynthConfig cfg = native_config();
  const auto controlled = unit_setup(kStereoDelay, 2, 0x40, 0, kSourceCc20, kDepthPlus);
  const auto raw_top = unit_setup(kStereoDelay, 2, kModulatedFeedback, 0, 0x00, kDepthZero);
  const auto raw_base = unit_setup(kStereoDelay, 2, 0x40, 0, 0x00, kDepthZero);

  const int moved = probed_tail<s::NativeSynth>(cfg, controlled, 127);
  const int at_top = probed_tail<s::NativeSynth>(cfg, raw_top, 127);
  CHECK(moved == at_top);
  // A controller left at rest probes as the unmodulated byte, and the two
  // references are far enough apart that the comparison can fail.
  const int resting = probed_tail<s::NativeSynth>(cfg, controlled, 0);
  const int at_base = probed_tail<s::NativeSynth>(cfg, raw_base, 0);
  CHECK(resting == at_base);
  CHECK(at_top > 2 * at_base);
}

TEST_CASE("the SoundFont player's tail probe covers a controller-moved feedback",
          "[gs][gs-efx][gs-efx-control]") {
  const s::Sf2PlayerConfig cfg = sf2_config(true, s::GsEfxRealization::kModern);
  const auto controlled = unit_setup(kStereoDelay, 2, 0x40, 0, kSourceCc20, kDepthPlus);
  const auto raw_top = unit_setup(kStereoDelay, 2, kModulatedFeedback, 0, 0x00, kDepthZero);
  const auto raw_base = unit_setup(kStereoDelay, 2, 0x40, 0, 0x00, kDepthZero);

  const int moved = probed_tail<s::Sf2Player>(cfg, controlled, 127);
  const int at_top = probed_tail<s::Sf2Player>(cfg, raw_top, 127);
  CHECK(moved == at_top);
  const int resting = probed_tail<s::Sf2Player>(cfg, controlled, 0);
  const int at_base = probed_tail<s::Sf2Player>(cfg, raw_base, 0);
  CHECK(resting == at_base);
  CHECK(at_top > 2 * at_base);
}

TEST_CASE("a block that applies a controller publishes the tail of what it rendered",
          "[gs][gs-efx][gs-efx-control]") {
  const s::Sf2PlayerConfig cfg = sf2_config(true, s::GsEfxRealization::kModern);
  const auto run = [&cfg](const std::vector<Message>& setup, int cc) {
    s::Sf2Player player(cfg);
    player.prepare(kRate, kBlock);
    std::vector<std::shared_ptr<const m::PreparedMidiSysEx>> tokens;
    for (const Message& msg : setup) replay_sysex(player, msg, tokens);
    send_cc(player, 0);
    render_blocks(player, 1);
    send_cc(player, static_cast<uint8_t>(cc));
    render_blocks(player, 1);
    return player.tail_samples();
  };
  const auto controlled = unit_setup(kStereoDelay, 2, 0x40, 0, kSourceCc20, kDepthPlus);
  const int moved = run(controlled, 127);
  const int at_top = run(unit_setup(kStereoDelay, 2, kModulatedFeedback, 0, 0x00, kDepthZero), 127);
  CHECK(moved == at_top);
  const int resting = run(controlled, 0);
  const int at_base = run(unit_setup(kStereoDelay, 2, 0x40, 0, 0x00, kDepthZero), 0);
  CHECK(resting == at_base);
  CHECK(at_top > 2 * at_base);
}

std::shared_ptr<s::Sf2File> sine_font() {
  static const std::shared_ptr<s::Sf2File> cached = [] {
    sonare::test::Sf2Builder b;
    std::vector<float> sine(96);
    for (size_t i = 0; i < sine.size(); ++i) {
      sine[i] = 0.5f * static_cast<float>(
                           std::sin(sonare::constants::kTwoPiD * static_cast<double>(i) / 32.0));
    }
    const int id = b.add_sample("sine", sine, 32000, 60, 32, 96);
    sonare::test::Sf2Builder::ZoneSpec looped;
    looped.gens.push_back({54 /*sampleModes*/, 1});
    looped.target = id;
    const int inst = b.add_instrument("sine", {looped});
    sonare::test::Sf2Builder::ZoneSpec pz;
    pz.target = inst;
    b.add_preset("Sine", 0, 0, {pz});
    const auto bytes = b.build();
    auto sf2 = std::make_shared<s::Sf2File>();
    std::string error;
    REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
    return sf2;
  }();
  return cached;
}

float max_difference(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  float worst = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::fabs(a[i] - b[i]));
  return worst;
}

/// A live player holding a sine note through unit 0, its EFX sent from the
/// control thread as a host does.
class LivePlayer {
 public:
  explicit LivePlayer(s::GsEfxRealization realization) : player_(sf2_config(false, realization)) {
    player_.set_soundfont(sine_font());
    player_.prepare(kRate, kBlock);
    for (const Message& msg : unit_setup(kStereoEq, 19, 0x40, 1, kSourceCc20, kDepthPlus)) {
      control(msg);
    }
    send_cc(player_, 0);
    player_.on_event(0, sonare::test::event(m::make_midi1_note_on(0, 0, 60, 100)));
    render_blocks(player_, 4);
  }

  void control(const Message& msg) { player_.on_control_sysex(msg.data(), msg.size()); }
  void cc(uint8_t value) { send_cc(player_, value); }
  void switch_to(s::GsEfxRealization realization) { player_.set_gs_efx_realization(realization); }
  /// Settles @p settle blocks, then returns the next eight.
  std::vector<float> listen(int settle) {
    render_blocks(player_, settle);
    std::vector<float> out;
    render_blocks(player_, 8, &out);
    return out;
  }

 private:
  s::Sf2Player player_;
};

TEST_CASE("a unit rebuilt as classic keeps following its EFX CONTROL",
          "[gs][gs-efx][gs-efx-control][gs-efx-realization]") {
  REQUIRE(s::gs_classic::gs_classic_default_registry().find(kStereoEq) != nullptr);
  constexpr int kSettle = 64;
  LivePlayer subject(s::GsEfxRealization::kModern);
  LivePlayer reference(s::GsEfxRealization::kClassic);
  subject.switch_to(s::GsEfxRealization::kClassic);
  std::vector<float> before = reference.listen(kSettle);
  CHECK(max_difference(subject.listen(kSettle), before) < 1e-5f);

  // Every step below is sent to both; the reference never changed realisation.
  struct Step {
    const char* what;
    bool is_cc;
    Message msg;
    uint8_t cc;
  };
  const std::vector<Step> steps = {
      {"controller to the top", true, {}, 127},
      {"neutral depth", false, efx_write(depth_offset(1), kDepthZero), 0},
      {"base moved", false, efx_write(slot_offset(19), 0x20), 0},
      {"full depth again", false, efx_write(depth_offset(1), kDepthPlus), 0},
      {"source off", false, efx_write(source_offset(1), 0x00), 0},
      {"source on", false, efx_write(source_offset(1), kSourceCc20), 0},
  };
  for (const Step& step : steps) {
    INFO(step.what);
    if (step.is_cc) {
      subject.cc(step.cc);
      reference.cc(step.cc);
    } else {
      subject.control(step.msg);
      reference.control(step.msg);
    }
    const std::vector<float> expected = reference.listen(kSettle);
    const std::vector<float> heard = subject.listen(kSettle);
    CHECK(max_difference(heard, expected) < 1e-5f);
    // Each step moves the level, so a subject stuck on the old byte would fail.
    CHECK(max_difference(expected, before) > 1e-3f);
    before = expected;
  }
}

}  // namespace

#endif  // SONARE_MIDI_WITH_FX && SONARE_WITH_MASTERING
