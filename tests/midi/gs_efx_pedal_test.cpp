/// @file gs_efx_pedal_test.cpp
/// @brief The GS overdrive/distortion block as a pedal ahead of four amp stages.
///
/// Every OD/DS block realises as a pedal (overdrive, distortion, or both behind
/// an OD Sel) followed by one amp stage per printed Amp Type state. The Drive
/// byte is the pedal's gain, Amp Type picks one amp, Amp Sw takes the cabinet
/// off every amp and leaves the amps themselves in place, and OD Sw bypasses
/// the whole block. The distortion case renders the chain through the insert
/// factory, so the pedal has to be what distorts once the cabinet is off.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "midi/midi_event.h"
#include "midi/prepared_sysex.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/gs_efx_processor.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "rt/processor_base.h"
#include "support/midi_render.h"
#include "util/constants.h"

namespace {

using sonare::constants::kTwoPiD;
using sonare::midi::synth::gs_efx_insert_chain;
using sonare::midi::synth::gs_efx_type_defaults;
using sonare::midi::synth::GsEfx;
using sonare::midi::synth::GsEfxStage;
using sonare::test::event;

constexpr std::string_view kOverdrive = "saturation.overdrive";
constexpr std::string_view kDistortion = "saturation.distortion";
constexpr std::string_view kAmp = "saturation.ampSim";

constexpr double kRate = 48000.0;
constexpr int kBlockSize = 512;
constexpr double kProbeHz = 1000.0;
// A DI-level guitar signal, -20 dBFS.
constexpr float kProbeAmplitude = 0.1f;
constexpr double kSettleSeconds = 0.5;
// A whole number of 1 kHz periods.
constexpr double kWindowSeconds = 0.1;

/// @p type at its power-on bytes.
GsEfx efx_of(uint16_t type) {
  GsEfx efx;
  efx.type = type;
  efx.type_msb = static_cast<uint8_t>(type >> 8);
  const auto* defaults = gs_efx_type_defaults(type);
  REQUIRE(defaults != nullptr);
  efx.params = defaults->params;
  return efx;
}

GsEfx with_byte(GsEfx efx, int slot, uint8_t value) {
  efx.params[static_cast<size_t>(slot)] = value;
  return efx;
}

const GsEfxStage* find(const std::vector<GsEfxStage>& chain, std::string_view name,
                       uint8_t ordinal) {
  for (const GsEfxStage& stage : chain) {
    if (stage.name == name && stage.ordinal == ordinal) return &stage;
  }
  return nullptr;
}

/// The stage at (@p name, @p ordinal), which has to exist.
const GsEfxStage& stage_at(const std::vector<GsEfxStage>& chain, std::string_view name,
                           uint8_t ordinal) {
  const GsEfxStage* stage = find(chain, name, ordinal);
  CAPTURE(name, ordinal);
  REQUIRE(stage != nullptr);
  return *stage;
}

int count(const std::vector<GsEfxStage>& chain, std::string_view name) {
  return static_cast<int>(std::count_if(chain.begin(), chain.end(),
                                        [name](const GsEfxStage& s) { return s.name == name; }));
}

bool carries(const GsEfxStage& stage, std::string_view fragment) {
  return stage.params_json.find(fragment) != std::string::npos;
}

/// The steady-state left channel of a 1 kHz probe through every enabled stage.
std::vector<float> render(const std::vector<GsEfxStage>& chain) {
  const int settle = static_cast<int>(kSettleSeconds * kRate);
  const int window = static_cast<int>(std::lround(kWindowSeconds * kRate));
  std::vector<std::unique_ptr<sonare::rt::ProcessorBase>> stages;
  for (const GsEfxStage& stage : chain) {
    if (!stage.enabled) continue;
    auto processor = sonare::mastering::api::make_insert(stage.name, stage.params_json);
    CAPTURE(stage.name, stage.params_json);
    REQUIRE(processor != nullptr);
    processor->prepare(kRate, kBlockSize);
    stages.push_back(std::move(processor));
  }
  const int total = settle + window;
  std::vector<float> left(static_cast<size_t>(total));
  std::vector<float> right(static_cast<size_t>(total));
  for (int i = 0; i < total; ++i) {
    const float s =
        kProbeAmplitude *
        static_cast<float>(std::sin(kTwoPiD * kProbeHz * static_cast<double>(i) / kRate));
    left[static_cast<size_t>(i)] = s;
    right[static_cast<size_t>(i)] = s;
  }
  for (int offset = 0; offset < total; offset += kBlockSize) {
    const int n = std::min(kBlockSize, total - offset);
    float* channels[] = {left.data() + offset, right.data() + offset};
    for (auto& processor : stages) processor->process(channels, 2, n);
  }
  return std::vector<float>(left.begin() + settle, left.end());
}

double power_at(const std::vector<float>& window, double hz) {
  double re = 0.0;
  double im = 0.0;
  for (size_t n = 0; n < window.size(); ++n) {
    const double phase = kTwoPiD * hz * static_cast<double>(n) / kRate;
    re += static_cast<double>(window[n]) * std::cos(phase);
    im += static_cast<double>(window[n]) * std::sin(phase);
  }
  const double scale = 2.0 / static_cast<double>(window.size());
  return (re * re + im * im) * scale * scale * 0.5;
}

double thd_db(const std::vector<float>& window) {
  const double fundamental = power_at(window, kProbeHz);
  REQUIRE(fundamental > 0.0);
  double harmonics = 0.0;
  for (int k = 2; k * kProbeHz < kRate * 0.5; ++k) harmonics += power_at(window, k * kProbeHz);
  return 10.0 * std::log10(harmonics / fundamental + 1e-30);
}

/// A drive type: its pedal and its Drive / Amp Type / Amp Sw slots.
struct DriveType {
  uint16_t type;
  std::string_view pedal;
  int drive_slot;
  int amp_type_slot;
  int amp_sw_slot;
};

constexpr DriveType kDriveTypes[] = {
    {0x0110, kOverdrive, 0, 1, 2},
    {0x0111, kDistortion, 0, 1, 2},
};

}  // namespace

TEST_CASE("Overdrive and Distortion run a pedal ahead of four amp stages",
          "[midi][gs][gsefx][efxpedal]") {
  for (const DriveType& t : kDriveTypes) {
    CAPTURE(t.type);
    const auto chain = gs_efx_insert_chain(efx_of(t.type));
    REQUIRE(chain.size() > 5);
    CHECK(chain[0].name == t.pedal);
    for (uint8_t ordinal = 0; ordinal < 4; ++ordinal) {
      CAPTURE(ordinal);
      CHECK(chain[1 + ordinal].name == kAmp);
      CHECK(chain[1 + ordinal].ordinal == ordinal);
    }
    CHECK(count(chain, kAmp) == 4);
    // The Drive byte is the pedal's gain, and no longer the amp's input trim.
    CHECK(carries(chain[0], "\"gainDb\""));
    for (uint8_t ordinal = 0; ordinal < 4; ++ordinal) {
      CHECK_FALSE(carries(stage_at(chain, kAmp, ordinal), "\"gainDb\""));
    }
  }
}

TEST_CASE("Amp Type turns exactly one amp on and Amp Sw takes every cabinet off",
          "[midi][gs][gsefx][efxpedal]") {
  for (const DriveType& t : kDriveTypes) {
    CAPTURE(t.type);
    for (uint8_t amp = 0; amp < 4; ++amp) {
      CAPTURE(amp);
      const auto chain = gs_efx_insert_chain(with_byte(efx_of(t.type), t.amp_type_slot, amp));
      for (uint8_t ordinal = 0; ordinal < 4; ++ordinal) {
        CAPTURE(ordinal);
        CHECK(stage_at(chain, kAmp, ordinal).enabled == (ordinal == amp));
      }
      CHECK(stage_at(chain, t.pedal, 0).enabled);
    }
    const auto off = gs_efx_insert_chain(with_byte(efx_of(t.type), t.amp_sw_slot, 0));
    const auto on = gs_efx_insert_chain(with_byte(efx_of(t.type), t.amp_sw_slot, 1));
    for (uint8_t ordinal = 0; ordinal < 4; ++ordinal) {
      CAPTURE(ordinal);
      CHECK(carries(stage_at(off, kAmp, ordinal), "\"cab\":0"));
      CHECK(carries(stage_at(on, kAmp, ordinal), "\"cab\":1"));
    }
    // The cabinet goes; the selected amp stays in the chain.
    CHECK(count(off, kAmp) == 4);
  }
}

TEST_CASE("with Amp Sw off the pedal still distorts as Drive rises",
          "[midi][gs][gsefx][efxpedal]") {
  // A 1 kHz sine: Drive 7F has to sit at least 20 dB above Drive 00 in THD.
  constexpr double kMinThdRiseDb = 20.0;
  // The Small amp, whose own breakup leaves the pedal's audible; a stack amp
  // saturates a DI-level tone whatever the pedal does.
  constexpr uint8_t kSmallAmp = 0;
  for (const DriveType& t : kDriveTypes) {
    CAPTURE(t.type);
    const GsEfx cab_off =
        with_byte(with_byte(efx_of(t.type), t.amp_sw_slot, 0), t.amp_type_slot, kSmallAmp);
    const double low = thd_db(render(gs_efx_insert_chain(with_byte(cab_off, t.drive_slot, 0))));
    const double high = thd_db(render(gs_efx_insert_chain(with_byte(cab_off, t.drive_slot, 0x7F))));
    CAPTURE(low, high);
    CHECK(high - low >= kMinThdRiseDb);
  }
}

TEST_CASE("OD Sel picks the overdrive or the distortion pedal", "[midi][gs][gsefx][efxpedal]") {
  struct Selector {
    uint16_t type;
    int slot;
    uint8_t ordinal;  ///< Which overdrive/distortion pair the selector owns.
  };
  constexpr Selector kSelectors[] = {
      {0x0400, 4, 0}, {0x0401, 4, 0}, {0x0402, 4, 0}, {0x0405, 4, 0}, {0x1103, 0, 0},
      {0x1103, 5, 1}, {0x1104, 0, 0}, {0x1105, 0, 0}, {0x1106, 0, 0},
  };
  for (const Selector& s : kSelectors) {
    CAPTURE(s.type, s.slot);
    const auto odrv = gs_efx_insert_chain(with_byte(efx_of(s.type), s.slot, 0));
    const auto dist = gs_efx_insert_chain(with_byte(efx_of(s.type), s.slot, 1));
    CHECK(stage_at(odrv, kOverdrive, s.ordinal).enabled);
    CHECK_FALSE(stage_at(odrv, kDistortion, s.ordinal).enabled);
    CHECK_FALSE(stage_at(dist, kOverdrive, s.ordinal).enabled);
    CHECK(stage_at(dist, kDistortion, s.ordinal).enabled);
  }
}

TEST_CASE("each OD block has one amp per printed Amp Type state", "[midi][gs][gsefx][efxpedal]") {
  // OD1 / OD2 carries two blocks; Bass Multi prints three amp types; Rotary
  // Multi prints none and runs a single amp.
  CHECK(count(gs_efx_insert_chain(efx_of(0x1103)), kAmp) == 8);
  CHECK(count(gs_efx_insert_chain(efx_of(0x0405)), kAmp) == 3);
  CHECK(count(gs_efx_insert_chain(efx_of(0x020C)), kAmp) == 1);
  for (const uint16_t type : {0x0200, 0x0201, 0x0202, 0x020C}) {
    CAPTURE(type);
    const auto chain = gs_efx_insert_chain(efx_of(type));
    CHECK(count(chain, kOverdrive) == 1);
    CHECK(count(chain, kDistortion) == 0);
  }
  for (const uint16_t type : {0x0203, 0x0204, 0x0205}) {
    CAPTURE(type);
    const auto chain = gs_efx_insert_chain(efx_of(type));
    CHECK(count(chain, kOverdrive) == 0);
    CHECK(count(chain, kDistortion) == 1);
    CHECK(count(chain, kAmp) == 4);
  }
}

TEST_CASE("OD Sw bypasses the pedal and every amp", "[midi][gs][gsefx][efxpedal]") {
  struct Switch {
    uint16_t type;
    int slot;
  };
  constexpr Switch kSwitches[] = {
      {0x0400, 10}, {0x0401, 8}, {0x0402, 10}, {0x0405, 8}, {0x020C, 1}};
  for (const Switch& s : kSwitches) {
    CAPTURE(s.type);
    const auto off = gs_efx_insert_chain(with_byte(efx_of(s.type), s.slot, 0));
    for (const GsEfxStage& stage : off) {
      if (stage.name == kOverdrive || stage.name == kDistortion || stage.name == kAmp) {
        CAPTURE(stage.name, stage.ordinal);
        CHECK_FALSE(stage.enabled);
      }
    }
    CHECK(count(off, kAmp) >= 1);
  }
}

namespace {

/// A stand-in stage that silences its input where it is a pedal or an amp and
/// passes it through untouched otherwise. Every binding key is published as
/// realtime-safe, so no path rejects the graph for want of a control.
class MuteDriveStage final : public sonare::rt::ProcessorBase {
 public:
  MuteDriveStage(std::string_view name, bool mute_drive)
      : mute_(mute_drive && (name == kOverdrive || name == kDistortion || name == kAmp)) {}
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    if (!mute_) return;
    for (int ch = 0; ch < num_channels; ++ch) std::fill_n(channels[ch], num_samples, 0.0f);
  }
  void reset() override {}
  bool parameter_is_realtime_safe(unsigned int id) const noexcept override {
    return id < sonare::midi::synth::kGsEfxRowKeys.size();
  }
  std::vector<sonare::rt::ParamDescriptor> parameter_descriptors() const override {
    std::vector<sonare::rt::ParamDescriptor> out;
    for (unsigned int id = 0; id < sonare::midi::synth::kGsEfxRowKeys.size(); ++id) {
      out.push_back({std::string(sonare::midi::synth::kGsEfxRowKeys[id]), id});
    }
    return out;
  }

 protected:
  bool set_parameter_impl(unsigned int id, float) override {
    return parameter_is_realtime_safe(id);
  }

 private:
  bool mute_;
};

std::unique_ptr<sonare::rt::ProcessorBase> stand_in(std::string_view name, bool mute_drive) {
  return std::make_unique<MuteDriveStage>(name, mute_drive);
}

/// GTR Multi 1 (04 00): six OD stages behind OD Sw, the last amp selected so a
/// switch reaching only the first four stages leaves it running.
constexpr uint16_t kGtrMulti1 = 0x0400;
constexpr int kOdAmpSlot = 6;
constexpr int kOdSwSlot = 10;
constexpr uint8_t kLastAmp = 3;

std::vector<uint8_t> dt1(uint8_t a1, uint8_t a2, std::vector<uint8_t> data) {
  std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, a1, a2};
  int sum = 0x40 + a1 + a2;
  for (const uint8_t d : data) sum += d;
  msg.insert(msg.end(), data.begin(), data.end());
  msg.push_back(static_cast<uint8_t>((128 - (sum & 0x7F)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

/// Part 1 on unit 1 with GTR Multi 1, its last amp chosen and OD Sw at @p od_sw.
std::vector<std::vector<uint8_t>> od_block_messages(uint8_t od_sw) {
  return {dt1(0x41, 0x22, {0x01}), dt1(0x03, 0x00, {0x04, 0x00}),
          dt1(0x03, static_cast<uint8_t>(3 + kOdAmpSlot), {kLastAmp}),
          dt1(0x03, static_cast<uint8_t>(3 + kOdSwSlot), {od_sw})};
}

double relative_difference(const sonare::test::StereoRender& a,
                           const sonare::test::StereoRender& b) {
  double error = 0.0;
  double power = 0.0;
  for (size_t i = 0; i < a.left.size(); ++i) {
    const double dl = static_cast<double>(a.left[i]) - b.left[i];
    const double dr = static_cast<double>(a.right[i]) - b.right[i];
    error += dl * dl + dr * dr;
    power +=
        static_cast<double>(b.left[i]) * b.left[i] + static_cast<double>(b.right[i]) * b.right[i];
  }
  REQUIRE(power > 1e-8);
  return std::sqrt(error / power);
}

enum class Delivery { kDirect, kPrepared };

/// A note through an Sf2Player whose unit is the stand-in graph.
sonare::test::StereoRender render_part(bool mute_drive, uint8_t od_sw, Delivery delivery) {
  sonare::midi::synth::Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.bank_rig_binding = false;
  // A system tail or the DC blocker's memory would carry the fade window into
  // the compared one.
  cfg.dc_block = false;
#if defined(SONARE_MIDI_WITH_FX)
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
#endif
  cfg.insert_factory = [mute_drive](std::string_view name, std::string_view) {
    return stand_in(name, mute_drive);
  };
  sonare::midi::synth::Sf2Player player(cfg);
  player.prepare(kRate, 256);
  for (const auto& msg : od_block_messages(od_sw)) {
    if (delivery == Delivery::kDirect) {
      player.on_control_sysex(msg.data(), msg.size());
      continue;
    }
    std::shared_ptr<const sonare::midi::PreparedMidiSysEx> token;
    REQUIRE(player.prepare_sysex(msg.data(), msg.size(), token));
    sonare::midi::MidiEvent prepared;
    prepared.ump = sonare::midi::make_sysex_handle(0, 1);
    prepared.sysex_payload = msg.data();
    prepared.sysex_payload_size = msg.size();
    prepared.prepared_sysex = token.get();
    player.on_event(0, prepared);
  }
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 110)));
  // The switch reaches the running unit through the stage fade; compare past it.
  (void)sonare::test::render_stereo(player, 4096);
  return sonare::test::render_stereo(player, 4096);
}

}  // namespace

TEST_CASE("OD Sw off bypasses the pedals and every amp on the processor path",
          "[midi][gs][gsefx][efxpedal]") {
  const auto render = [](uint8_t od_sw) {
    GsEfx state = with_byte(with_byte(efx_of(kGtrMulti1), kOdAmpSlot, kLastAmp), kOdSwSlot, od_sw);
    state.assigned = true;
    sonare::midi::synth::GsEfxProcessor processor(
        state, sonare::midi::synth::GsEfxRealization::kModern,
        [](std::string_view name, std::string_view) { return stand_in(name, true); });
    processor.prepare(kRate, kBlockSize, 2);
    std::vector<float> left(kBlockSize, 0.5f);
    std::vector<float> right(kBlockSize, 0.5f);
    float* channels[] = {left.data(), right.data()};
    processor.process(channels, 2, kBlockSize);
    return left;
  };
  // Every other stage is a pass-through, so with the block bypassed the input survives.
  const std::vector<float> off = render(0);
  CHECK(*std::min_element(off.begin(), off.end()) == 0.5f);
  CHECK(*std::max_element(off.begin(), off.end()) == 0.5f);
  // Positive control: switched on, the selected amp silences the unit.
  const std::vector<float> on = render(1);
  CHECK(*std::max_element(on.begin(), on.end()) == 0.0f);
}

TEST_CASE("OD Sw off bypasses the pedals and every amp on the part path",
          "[midi][gs][gsefx][efxpedal]") {
  for (const Delivery delivery : {Delivery::kDirect, Delivery::kPrepared}) {
    CAPTURE(delivery == Delivery::kDirect ? "direct" : "prepared");
    const auto pass = render_part(false, 0, delivery);
    // Bypassed, the muting stand-ins change nothing.
    CHECK(relative_difference(render_part(true, 0, delivery), pass) < 1e-6);
    // Positive control: switched on, they silence the part's unit output.
    CHECK(relative_difference(render_part(true, 1, delivery), pass) > 0.1);
  }
}
