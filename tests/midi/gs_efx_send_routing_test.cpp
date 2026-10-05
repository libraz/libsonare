/// @file gs_efx_send_routing_test.cpp
/// @brief GS EFX routing/automation fixes for the SC-88 SoundFont player:
///        (1) an insertion-effect part sends its POST-effect signal to the
///        system reverb, scaled by the EFX unit's send amount (GS 40 03 17),
///        instead of the clean pre-effect signal; (2) a parameter-only EFX
///        change updates the live insert processors in place rather than
///        rebuilding the chain (which would zero their DSP state).

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/realtime_engine.h"
#include "mastering/api/insert_factory.h"
#include "midi/midi_event.h"
#include "midi/synth/gs_address_table.h"
#include "midi/synth/gs_efx_bindings.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "rt/processor_base.h"
#include "support/midi_render.h"
#include "support/sf2_builder.h"
#include "util/json.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::MidiInstrumentSourceOutput;
using sonare::midi::synth::gs_efx_insert_chain;
using sonare::midi::synth::gs_efx_type_defaults;
using sonare::midi::synth::GsEfx;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;

/// A unit holding @p type with that type's own power-on parameters, which is
/// the state selecting it over the wire leaves.
GsEfx efx_holding(uint16_t type) {
  GsEfx efx;
  efx.type = type;
  efx.type_msb = static_cast<uint8_t>(type >> 8);
  const auto* defaults = gs_efx_type_defaults(type);
  if (defaults != nullptr) efx.params = defaults->params;
  efx.assigned = true;
  return efx;
}
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;

// GS SysEx (Roland DT1, framed): enable EFX on part 1 (channel 0), select
// Overdrive (01 10), write EFX PARAMETER 1 (40 03 03, the drive) at max.
// Checksums per the DT1 rule.
constexpr uint8_t kPartOn[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x01, 0x5C, 0xF7};
// The same EFX assignment for part 2 (MIDI channel 1). The control fanout
// must follow this routed part even when part 1 carries a different CC value.
#if defined(SONARE_MIDI_WITH_FX) && defined(SONARE_WITH_MASTERING)
constexpr uint8_t kPartOnChannel1[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                       0x42, 0x22, 0x01, 0x5B, 0xF7};
#endif
constexpr uint8_t kOdType[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                               0x03, 0x00, 0x01, 0x10, 0x2C, 0xF7};
constexpr uint8_t kOdDrive[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, 0x03, 0x7F, 0x3B, 0xF7};
// A genuine TYPE change: select Stereo Chorus (01 42).
constexpr uint8_t kChorusType[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                   0x03, 0x00, 0x01, 0x42, 0x7A, 0xF7};

// Only used by the post-effect reverb-routing test below, which is itself
// gated on SONARE_MIDI_WITH_FX and SONARE_WITH_MASTERING (it needs the EFX
// insertion chain, built through mastering::api::make_insert, to build).
#if defined(SONARE_MIDI_WITH_FX) && defined(SONARE_WITH_MASTERING)
using sonare::test::event;

/// Framed EFX -> reverb send write (GS address 40 03 17) with the DT1 checksum
/// computed for @p value.
std::array<uint8_t, 11> efx_reverb_send(uint8_t value) {
  std::array<uint8_t, 11> m = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, 0x17, value, 0x00, 0xF7};
  // Checksum sums the address + data bytes (40 03 17 value = indices 5..8).
  const uint32_t sum = m[5] + m[6] + m[7] + m[8];
  m[9] = static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
  return m;
}

/// Fixture: program 1 = a short one-shot burst (so the dry signal ends well
/// before the reverb tail is measured).
std::shared_ptr<Sf2File> make_fixture() {
  constexpr double kTwoPi = 6.28318530717958647692;
  Sf2Builder b;
  std::vector<float> burst(256);
  for (size_t i = 0; i < burst.size(); ++i) {
    const float envl = 1.0f - static_cast<float>(i) / 256.0f;
    burst[i] = envl * static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / 16.0));
  }
  const int burst_id = b.add_sample("burst", burst, 48000, 60, 0, 256);
  Sf2Builder::ZoneSpec oneshot;
  oneshot.target = burst_id;
  const int burst_inst = b.add_instrument("burst", {oneshot});
  Sf2Builder::ZoneSpec pz;
  pz.target = burst_inst;
  b.add_preset("Burst", 0, 1, {pz});
  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

/// A sustained sample for the engine timing case: the insertion event is due
/// halfway through the note, so its first block is an observable dry baseline
/// and its later block can prove that the prepared operation took effect.
std::shared_ptr<Sf2File> make_sustained_fixture() {
  constexpr double kTwoPi = 6.28318530717958647692;
  Sf2Builder b;
  constexpr size_t kSamples = 8192;
  std::vector<float> tone(kSamples);
  for (size_t i = 0; i < tone.size(); ++i) {
    const double phase = kTwoPi * static_cast<double>(i) / 64.0;
    const float envelope = 0.8f + 0.2f * static_cast<float>(i) / kSamples;
    tone[i] = envelope * static_cast<float>(std::sin(phase));
  }
  const int sample_id = b.add_sample("sustained", tone, 48000, 60, 0, kSamples);
  Sf2Builder::ZoneSpec zone;
  zone.target = sample_id;
  const int instrument = b.add_instrument("sustained", {zone});
  Sf2Builder::ZoneSpec preset_zone;
  preset_zone.target = instrument;
  b.add_preset("Sustained", 0, 1, {preset_zone});
  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

float rms(const std::vector<float>& buf, size_t from, size_t to) {
  double acc = 0.0;
  size_t n = 0;
  to = std::min(to, buf.size());
  for (size_t i = from; i < to; ++i) {
    acc += static_cast<double>(buf[i]) * buf[i];
    ++n;
  }
  return n > 0 ? static_cast<float>(std::sqrt(acc / static_cast<double>(n))) : 0.0f;
}
#endif  // SONARE_MIDI_WITH_FX && SONARE_WITH_MASTERING

/// A minimal insert processor that only counts lifecycle/parameter calls, so a
/// test can tell an in-place parameter update (set_parameter, no prepare/reset)
/// apart from a full rebuild (a fresh processor, prepare()). The named
/// parameters are supplied by the case, so each one stands in for the insert the
/// type it selects would really build.
struct EfxCounters {
  int prepares = 0;
  int resets = 0;
  int set_params = 0;
  bool has_value = false;
  float last_value = 0.0f;
  std::array<float, 128> last_value_by_id{};
  std::array<bool, 128> has_value_by_id{};
};

class CountingInsert final : public sonare::rt::ProcessorBase {
 public:
  CountingInsert(std::shared_ptr<EfxCounters> counters, std::vector<std::string> keys)
      : counters_(std::move(counters)), keys_(std::move(keys)) {}
  void prepare(double, int) override { ++counters_->prepares; }
  void process(float* const*, int, int) override {}
  void reset() override { ++counters_->resets; }
  bool set_parameter_impl(unsigned int id, float value) override {
    ++counters_->set_params;
    counters_->has_value = true;
    counters_->last_value = value;
    if (id < counters_->last_value_by_id.size()) {
      counters_->last_value_by_id[id] = value;
      counters_->has_value_by_id[id] = true;
    }
    return true;
  }
  std::vector<sonare::rt::ParamDescriptor> parameter_descriptors() const override {
    std::vector<sonare::rt::ParamDescriptor> out;
    for (size_t i = 0; i < keys_.size(); ++i) {
      out.push_back({keys_[i], static_cast<unsigned int>(i)});
    }
    return out;
  }

 private:
  std::shared_ptr<EfxCounters> counters_;
  std::vector<std::string> keys_;
};

/// A real streaming stand-in with one realtime-safe descriptor followed by a
/// non-realtime descriptor. It makes a failed multi-key publication audible:
/// the first descriptor must not reach the old graph by itself.
class MixedSafetyInsert final : public sonare::rt::ProcessorBase {
 public:
  explicit MixedSafetyInsert(std::shared_ptr<int> set_count) : set_count_(std::move(set_count)) {}

  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    if (channels == nullptr) return;
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] == nullptr) continue;
      for (int i = 0; i < num_samples; ++i) channels[ch][i] *= gain_;
    }
  }
  void reset() override {}
  bool set_parameter_impl(unsigned int id, float) override {
    if (set_count_ != nullptr) ++*set_count_;
    if (id == 0) gain_ = 3.0f;
    return id < 2;
  }
  bool parameter_is_realtime_safe(unsigned int id) const noexcept override { return id == 0; }
  std::vector<sonare::rt::ParamDescriptor> parameter_descriptors() const override {
    return {{"gainDb", 0}, {"cab", 1}};
  }

 private:
  std::shared_ptr<int> set_count_;
  float gain_ = 1.0f;
};

/// The stereo delay's automatable keys, which the delay types' translations now
/// write into.
const std::vector<std::string> kStereoDelayKeys = {"delayTimeLMs", "delayTimeRMs", "feedback",
                                                   "pingPong",     "dryWet",       "dampingHz"};

/// The rotary's automatable keys. Its acceleration fields are deliberately NOT
/// among them: they size and shape the glide rather than ride it, so the rotary
/// translation's edits fall back to a rebuild by construction.
const std::vector<std::string> kRotaryKeys = {"rateHz", "depthMs", "tremolo", "dryWet",
                                              "drumRateHz"};

std::vector<std::string> all_efx_binding_keys() {
  std::vector<std::string> keys;
  keys.reserve(sonare::midi::synth::kGsEfxRowKeys.size());
  for (const std::string_view key : sonare::midi::synth::kGsEfxRowKeys) {
    keys.emplace_back(key);
  }
  return keys;
}

/// A framed GS DT1 write of one byte into the EFX parameter block at @p offset,
/// with the checksum. Offset 0x03 is EFX PARAMETER 1.
std::array<uint8_t, 11> efx_param_write(uint8_t offset, uint8_t value) {
  std::array<uint8_t, 11> m = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, offset, value, 0x00, 0xF7};
  const uint32_t sum = m[5] + m[6] + m[7] + m[8];
  m[9] = static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
  return m;
}

/// A framed GS DT1 write at an arbitrary EFX extension address. The extension
/// tests need to cross the 40 30/40 31 boundary while keeping one valid frame.
std::vector<uint8_t> efx_extension_write(uint32_t addr, const std::vector<uint8_t>& values) {
  std::vector<uint8_t> m = {0xF0, 0x41, 0x10, 0x42, 0x12};
  uint32_t sum = 0;
  for (int shift = 16; shift >= 0; shift -= 8) {
    const uint8_t byte = static_cast<uint8_t>((addr >> shift) & 0x7Fu);
    m.push_back(byte);
    sum += byte;
  }
  for (const uint8_t value : values) {
    const uint8_t byte = static_cast<uint8_t>(value & 0x7Fu);
    m.push_back(byte);
    sum += byte;
  }
  m.push_back(static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu));
  m.push_back(0xF7);
  return m;
}

/// A contiguous EFX block write, used to exercise the parameter+send
/// transaction at 40 03 16..19.
#if defined(SONARE_MIDI_WITH_FX) && defined(SONARE_WITH_MASTERING)
std::array<uint8_t, 14> efx_bulk_write(uint8_t offset, const std::array<uint8_t, 4>& values) {
  std::array<uint8_t, 14> m = {0xF0,   0x41,      0x10,      0x42,      0x12,      0x40, 0x03,
                               offset, values[0], values[1], values[2], values[3], 0x00, 0xF7};
  const uint32_t sum = m[5] + m[6] + m[7] + m[8] + m[9] + m[10] + m[11];
  m[12] = static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
  return m;
}
#endif

/// A framed EFX type selection.
std::array<uint8_t, 12> efx_type_write(uint8_t msb, uint8_t lsb) {
  std::array<uint8_t, 12> m = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                               0x03, 0x00, msb,  lsb,  0x00, 0xF7};
  const uint32_t sum = m[5] + m[6] + m[7] + m[8] + m[9];
  m[10] = static_cast<uint8_t>((128u - (sum & 0x7Fu)) & 0x7Fu);
  return m;
}

}  // namespace

#if defined(SONARE_MIDI_WITH_FX) && defined(SONARE_WITH_MASTERING)
TEST_CASE("a GS EFX part sends its post-effect signal to reverb", "[midi][sf2][gsefx]") {
  // The insertion-effect (Overdrive) stage must actually build for the part to
  // be bussed. This is an integration case, so an unavailable stock factory is
  // a test failure rather than a silently skipped assertion.
  REQUIRE(sonare::mastering::api::make_insert("saturation.ampSim", "{}") != nullptr);

  auto reverb_tail = [](uint8_t send) {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.insert_factory = [](std::string_view name, std::string_view json) {
      return sonare::mastering::api::make_insert(std::string(name), std::string(json));
    };
    Sf2Player player(cfg);
    player.set_soundfont(make_fixture());
    player.prepare(kOutRate, 256);
    // Route part 1 (channel 0) through the Overdrive insertion effect.
    player.on_control_sysex(kPartOn, sizeof(kPartOn));
    player.on_control_sysex(kOdType, sizeof(kOdType));
    player.on_control_sysex(kOdDrive, sizeof(kOdDrive));
    // Set the EFX -> reverb send amount (GS 40 03 17).
    const std::array<uint8_t, 11> rev = efx_reverb_send(send);
    player.on_control_sysex(rev.data(), rev.size());

    player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));

    std::vector<float> left(24000, 0.0f);
    std::vector<float> right(24000, 0.0f);
    float* chans[2] = {left.data(), right.data()};
    player.process(chans, 2, 24000);
    // Well after the dry burst (~5 ms): only the post-effect reverb tail is here.
    return rms(left, 4800, 24000) + rms(right, 4800, 24000);
  };

  // The dry bus (send 0) is identical for every send amount, so any increase in
  // the measured tail is purely the reverb return derived from the POST-effect
  // signal. Before the fix the EFX send was never applied and all three would be
  // equal (the pre-effect CC path only).
  const float dry = reverb_tail(0);
  const float mid = reverb_tail(64);
  const float full = reverb_tail(127);
  REQUIRE(mid > dry + 1e-6f);
  REQUIRE(full > mid + 1e-6f);
}

TEST_CASE("prepared GS EFX wet return stays on its source lane", "[midi][sf2][gsefx][prepared]") {
  REQUIRE(sonare::mastering::api::make_insert("saturation.ampSim", "{}") != nullptr);

  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.bank_rig_binding = false;
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  Sf2Player player(cfg);
  player.set_soundfont(make_fixture());
  player.prepare(kOutRate, 256);

  const std::array<uint8_t, 11> send = efx_reverb_send(127);
  const std::array<uint8_t, 11> dry_send = efx_reverb_send(0);
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> assign_token;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> type_token;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> send_token;
  REQUIRE(player.prepare_sysex(kPartOn, sizeof(kPartOn), assign_token));
  REQUIRE(player.prepare_sysex(kOdType, sizeof(kOdType), type_token));
  REQUIRE(player.prepare_sysex(send.data(), send.size(), send_token));
  // Leave the legacy control mirror at its dry default while the scheduled
  // token carries the wet send. The render path must use the prepared raw
  // state for both the unit bus and source attribution.
  player.on_control_sysex(dry_send.data(), dry_send.size());
  REQUIRE(player.gs_efx().send_reverb == 0);

  const auto dispatch = [](Sf2Player& target, const uint8_t* data, size_t size,
                           const std::shared_ptr<const sonare::midi::PreparedMidiSysEx>& token) {
    MidiEvent event;
    event.ump = sonare::midi::make_sysex_handle(0, 1);
    event.sysex_payload = data;
    event.sysex_payload_size = size;
    event.prepared_sysex = token.get();
    target.on_event(0, event);
  };
  dispatch(player, kPartOn, sizeof(kPartOn), assign_token);
  dispatch(player, kOdType, sizeof(kOdType), type_token);
  dispatch(player, send.data(), send.size(), send_token);
  MidiEvent note = event(sonare::midi::make_midi1_note_on(0, 0, 60, 127));
  constexpr uint32_t kSourceTrack = 4242;
  note.source_track_id = kSourceTrack;
  player.on_event(0, note);

  constexpr int kSamples = 24000;
  std::vector<float> fallback_left(kSamples, 0.0f);
  std::vector<float> fallback_right(kSamples, 0.0f);
  std::vector<float> source_left(kSamples, 0.0f);
  std::vector<float> source_right(kSamples, 0.0f);
  float* fallback_channels[2] = {fallback_left.data(), fallback_right.data()};
  float* source_channels[2] = {source_left.data(), source_right.data()};
  const MidiInstrumentSourceOutput outputs[] = {{0, fallback_channels},
                                                {kSourceTrack, source_channels}};
  REQUIRE(player.process_source_tracks(outputs, std::size(outputs), 2, kSamples));

  const float source_tail = rms(source_left, 4800, kSamples) + rms(source_right, 4800, kSamples);
  const float fallback_tail =
      rms(fallback_left, 4800, kSamples) + rms(fallback_right, 4800, kSamples);
  REQUIRE(source_tail > 1e-8f);
  REQUIRE(fallback_tail < source_tail * 1e-3f);
}

TEST_CASE("prepared GS EFX type, assignment, and parameter render at dispatch",
          "[midi][sf2][gsefx][prepared]") {
  // This uses the real mastering factory and the real SF2 player. Every token
  // is prepared before any one is dispatched, so a parameter token cannot
  // accidentally resolve against a control-thread projection of a later type.
  REQUIRE(sonare::mastering::api::make_insert("saturation.ampSim", "{}") != nullptr);

  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.bank_rig_binding = false;
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  Sf2Player prepared(cfg);
  Sf2Player legacy(cfg);
  const std::shared_ptr<Sf2File> sf2 = make_fixture();
  prepared.set_soundfont(sf2);
  legacy.set_soundfont(sf2);
  prepared.prepare(kOutRate, 256);
  legacy.prepare(kOutRate, 256);

  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> type_token;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> assign_token;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> parameter_token;
  REQUIRE(prepared.prepare_sysex(kOdType, sizeof(kOdType), type_token));
  REQUIRE(prepared.prepare_sysex(kPartOn, sizeof(kPartOn), assign_token));
  REQUIRE(prepared.prepare_sysex(kOdDrive, sizeof(kOdDrive), parameter_token));
  REQUIRE(type_token != nullptr);
  REQUIRE(assign_token != nullptr);
  REQUIRE(parameter_token != nullptr);

  const auto dispatch = [](Sf2Player& player, const uint8_t* payload, size_t size,
                           const std::shared_ptr<const sonare::midi::PreparedMidiSysEx>& token) {
    MidiEvent event;
    event.ump = sonare::midi::make_sysex_handle(0, 1);
    event.sysex_payload = payload;
    event.sysex_payload_size = size;
    event.prepared_sysex = token.get();
    player.on_event(0, event);
  };
  dispatch(prepared, kOdType, sizeof(kOdType), type_token);
  dispatch(prepared, kPartOn, sizeof(kPartOn), assign_token);
  dispatch(prepared, kOdDrive, sizeof(kOdDrive), parameter_token);

  // The legacy path is the oracle for the same exact operation order.
  legacy.on_control_sysex(kOdType, sizeof(kOdType));
  legacy.on_control_sysex(kPartOn, sizeof(kPartOn));
  legacy.on_control_sysex(kOdDrive, sizeof(kOdDrive));

  for (Sf2Player* player : {&prepared, &legacy}) {
    player->on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
    player->on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  }
  std::vector<float> prepared_left(2048, 0.0f);
  std::vector<float> prepared_right(2048, 0.0f);
  std::vector<float> legacy_left(2048, 0.0f);
  std::vector<float> legacy_right(2048, 0.0f);
  float* prepared_channels[2] = {prepared_left.data(), prepared_right.data()};
  float* legacy_channels[2] = {legacy_left.data(), legacy_right.data()};
  prepared.process(prepared_channels, 2, static_cast<int>(prepared_left.size()));
  legacy.process(legacy_channels, 2, static_cast<int>(legacy_left.size()));

  double error = 0.0;
  double reference = 0.0;
  for (size_t i = 0; i < prepared_left.size(); ++i) {
    const double dl = static_cast<double>(prepared_left[i]) - legacy_left[i];
    const double dr = static_cast<double>(prepared_right[i]) - legacy_right[i];
    error += dl * dl + dr * dr;
    reference += static_cast<double>(legacy_left[i]) * legacy_left[i] +
                 static_cast<double>(legacy_right[i]) * legacy_right[i];
  }
  REQUIRE(reference > 1e-8);
  REQUIRE(std::sqrt(error / reference) < 1e-5);
}

TEST_CASE("prepared GS EFX remains dry until its scheduled frame", "[midi][sf2][gsefx][prepared]") {
  REQUIRE(sonare::mastering::api::make_insert("saturation.ampSim", "{}") != nullptr);

  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.bank_rig_binding = false;
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  Sf2Player scheduled(cfg);
  Sf2Player reference(cfg);
  const std::shared_ptr<Sf2File> sf2 = make_sustained_fixture();
  scheduled.set_soundfont(sf2);
  reference.set_soundfont(sf2);

  sonare::engine::RealtimeEngine engine;
  sonare::engine::RealtimeEngine oracle_engine;
  engine.prepare(kOutRate, 256);
  oracle_engine.prepare(kOutRate, 256);
  REQUIRE(engine.set_midi_instrument(0, &scheduled));
  REQUIRE(oracle_engine.set_midi_instrument(0, &reference));

  // Prime the same sustained note directly. The engine is used for the
  // scheduled SysEx boundary; direct note setup keeps this assertion focused
  // on the prepared event rather than the separate live UMP queue.
  for (Sf2Player* player : {&scheduled, &reference}) {
    player->on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
    player->on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  }
  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  REQUIRE(engine.push_command(play));
  REQUIRE(oracle_engine.push_command(play));
  // The operation is accepted and prepared at push time, but its audio state
  // must not change until frame 4096. Assignment precedes type at that frame,
  // matching a real GS file's part setup order.
  REQUIRE(engine.push_midi_sysex(0, kPartOn, sizeof(kPartOn), 4096));
  REQUIRE(engine.push_midi_sysex(0, kOdType, sizeof(kOdType), 4096));

  // The first process call applies kTransportPlay at its head; render one
  // warm-up block before collecting the audible dry span.
  std::array<float, 256> warm_left{};
  std::array<float, 256> warm_right{};
  std::array<float, 256> oracle_warm_left{};
  std::array<float, 256> oracle_warm_right{};
  float* warm_channels[2] = {warm_left.data(), warm_right.data()};
  float* oracle_warm_channels[2] = {oracle_warm_left.data(), oracle_warm_right.data()};
  engine.process(warm_channels, 2, static_cast<int>(warm_left.size()));
  oracle_engine.process(oracle_warm_channels, 2, static_cast<int>(oracle_warm_left.size()));
  INFO("warm reference "
       << (rms(std::vector<float>(oracle_warm_left.begin(), oracle_warm_left.end()), 0,
               oracle_warm_left.size()) +
           rms(std::vector<float>(oracle_warm_right.begin(), oracle_warm_right.end()), 0,
               oracle_warm_right.size())));

  std::vector<float> early_left(3840, 0.0f);
  std::vector<float> early_right(3840, 0.0f);
  std::vector<float> oracle_early_left(3840, 0.0f);
  std::vector<float> oracle_early_right(3840, 0.0f);
  // RealtimeEngine's prepared block contract is 256 frames. Process the
  // pre-event span as real blocks so the early assertion observes the
  // sustained voice instead of silently accepting a short-buffer result.
  for (size_t offset = 0; offset < early_left.size(); offset += 256) {
    float* early_channels[2] = {early_left.data() + offset, early_right.data() + offset};
    float* oracle_early_channels[2] = {oracle_early_left.data() + offset,
                                       oracle_early_right.data() + offset};
    engine.process(early_channels, 2, 256);
    oracle_engine.process(oracle_early_channels, 2, 256);
  }

  double early_error = 0.0;
  double early_reference = 0.0;
  for (size_t i = 0; i < early_left.size(); ++i) {
    const double dl = static_cast<double>(early_left[i]) - oracle_early_left[i];
    const double dr = static_cast<double>(early_right[i]) - oracle_early_right[i];
    early_error += dl * dl + dr * dr;
    early_reference += static_cast<double>(oracle_early_left[i]) * oracle_early_left[i] +
                       static_cast<double>(oracle_early_right[i]) * oracle_early_right[i];
  }
  INFO("early reference " << early_reference << ", error " << early_error);
  REQUIRE(early_reference > 1e-8);
  REQUIRE(std::sqrt(early_error / early_reference) < 1e-6);

  std::vector<float> late_left(256, 0.0f);
  std::vector<float> late_right(256, 0.0f);
  std::vector<float> oracle_late_left(256, 0.0f);
  std::vector<float> oracle_late_right(256, 0.0f);
  float* late_channels[2] = {late_left.data(), late_right.data()};
  float* oracle_late_channels[2] = {oracle_late_left.data(), oracle_late_right.data()};
  engine.process(late_channels, 2, static_cast<int>(late_left.size()));
  oracle_engine.process(oracle_late_channels, 2, static_cast<int>(oracle_late_left.size()));

  double late_error = 0.0;
  double late_reference = 0.0;
  for (size_t i = 0; i < late_left.size(); ++i) {
    const double dl = static_cast<double>(late_left[i]) - oracle_late_left[i];
    const double dr = static_cast<double>(late_right[i]) - oracle_late_right[i];
    late_error += dl * dl + dr * dr;
    late_reference += static_cast<double>(oracle_late_left[i]) * oracle_late_left[i] +
                      static_cast<double>(oracle_late_right[i]) * oracle_late_right[i];
  }
  INFO("late reference " << late_reference << ", error " << late_error);
  REQUIRE(late_reference > 1e-8);
  REQUIRE(std::sqrt(late_error / late_reference) > 1e-3);
}

TEST_CASE("prepared GS EFX accepts split type writes and resolves the current MSB",
          "[midi][sf2][gsefx][prepared]") {
  if (sonare::mastering::api::make_insert("saturation.ampSim", "{}") == nullptr) return;

  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.bank_rig_binding = false;
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  Sf2Player prepared(cfg);
  Sf2Player legacy(cfg);
  const std::shared_ptr<Sf2File> sf2 = make_fixture();
  prepared.set_soundfont(sf2);
  legacy.set_soundfont(sf2);
  prepared.prepare(kOutRate, 256);
  legacy.prepare(kOutRate, 256);

  const auto msb = efx_param_write(0x00, 0x01);
  const auto lsb = efx_param_write(0x01, 0x10);
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> msb_token;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> lsb_token;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> assign_token;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> drive_token;
  REQUIRE(prepared.prepare_sysex(msb.data(), msb.size(), msb_token));
  REQUIRE(prepared.prepare_sysex(lsb.data(), lsb.size(), lsb_token));
  REQUIRE(prepared.prepare_sysex(kPartOn, sizeof(kPartOn), assign_token));
  REQUIRE(prepared.prepare_sysex(kOdDrive, sizeof(kOdDrive), drive_token));

  const auto dispatch = [](Sf2Player& player, const uint8_t* payload, size_t size,
                           const std::shared_ptr<const sonare::midi::PreparedMidiSysEx>& token) {
    MidiEvent event;
    event.ump = sonare::midi::make_sysex_handle(0, 1);
    event.sysex_payload = payload;
    event.sysex_payload_size = size;
    event.prepared_sysex = token.get();
    player.on_event(0, event);
  };
  dispatch(prepared, msb.data(), msb.size(), msb_token);
  dispatch(prepared, lsb.data(), lsb.size(), lsb_token);
  dispatch(prepared, kPartOn, sizeof(kPartOn), assign_token);
  dispatch(prepared, kOdDrive, sizeof(kOdDrive), drive_token);

  legacy.on_control_sysex(msb.data(), msb.size());
  legacy.on_control_sysex(lsb.data(), lsb.size());
  legacy.on_control_sysex(kPartOn, sizeof(kPartOn));
  legacy.on_control_sysex(kOdDrive, sizeof(kOdDrive));
  for (Sf2Player* player : {&prepared, &legacy}) {
    player->on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
    player->on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  }
  std::vector<float> prepared_left(2048, 0.0f);
  std::vector<float> prepared_right(2048, 0.0f);
  std::vector<float> legacy_left(2048, 0.0f);
  std::vector<float> legacy_right(2048, 0.0f);
  float* prepared_channels[2] = {prepared_left.data(), prepared_right.data()};
  float* legacy_channels[2] = {legacy_left.data(), legacy_right.data()};
  prepared.process(prepared_channels, 2, static_cast<int>(prepared_left.size()));
  legacy.process(legacy_channels, 2, static_cast<int>(legacy_left.size()));

  double error = 0.0;
  double reference = 0.0;
  for (size_t i = 0; i < prepared_left.size(); ++i) {
    const double dl = static_cast<double>(prepared_left[i]) - legacy_left[i];
    const double dr = static_cast<double>(prepared_right[i]) - legacy_right[i];
    error += dl * dl + dr * dr;
    reference += static_cast<double>(legacy_left[i]) * legacy_left[i] +
                 static_cast<double>(legacy_right[i]) * legacy_right[i];
  }
  REQUIRE(reference > 1e-8);
  REQUIRE(std::sqrt(error / reference) < 1e-5);
}

TEST_CASE("prepared GS EFX caches bounded LSB candidates", "[midi][sf2][gsefx][prepared]") {
  int factory_calls = 0;
  Sf2PlayerConfig cfg;
  cfg.insert_factory = [&factory_calls](std::string_view, std::string_view) {
    ++factory_calls;
    // Null is an intentional no-DSP stage. It still exercises preparation of
    // every known type sharing this LSB without requiring a mock processor.
    return std::unique_ptr<sonare::rt::ProcessorBase>{};
  };
  Sf2Player player(cfg);
  player.prepare(kOutRate, 256);
  const auto lsb_zero = efx_param_write(0x01, 0x00);
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> first;
  REQUIRE(player.prepare_sysex(lsb_zero.data(), lsb_zero.size(), first));
  const int calls_after_first = factory_calls;
  REQUIRE(calls_after_first > 0);
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> second;
  REQUIRE(player.prepare_sysex(lsb_zero.data(), lsb_zero.size(), second));
  REQUIRE(factory_calls == calls_after_first);
}

TEST_CASE("prepared GS EFX candidate scan stays inside its unit block",
          "[midi][sf2][gsefx][prepared]") {
  int factory_calls = 0;
  Sf2PlayerConfig cfg;
  cfg.insert_factory = [&factory_calls](std::string_view, std::string_view) {
    ++factory_calls;
    return std::unique_ptr<sonare::rt::ProcessorBase>{};
  };
  Sf2Player player(cfg);
  player.prepare(kOutRate, 256);

  // The start is in unit 0's reserved tail. The following two bytes roll over
  // to a valid Overdrive TYPE at unit 1's 40 31 00/01, but belong to neither
  // the addressed block nor its prepared candidate set.
  const std::vector<uint8_t> spilled = efx_extension_write(0x40307F, {0x00, 0x01, 0x10});
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> token;
  REQUIRE(player.prepare_sysex(spilled.data(), spilled.size(), token));
  REQUIRE(token != nullptr);
  REQUIRE(factory_calls == 0);
}

TEST_CASE("unsupported direct EFX keeps its legacy DSP for parameter edits",
          "[midi][sf2][gsefx][prepared]") {
  REQUIRE(sonare::mastering::api::make_insert("utility.gain", R"({"levelDb":6})") != nullptr);

  const auto make_player = [] {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.bank_rig_binding = false;
    cfg.insert_factory = [](std::string_view name, std::string_view) {
      // Deliberately expose a real DSP with metadata that does not describe the
      // amp-sim rows. Preparation must reject this custom graph, while the
      // direct compatibility path still renders its gain stage.
      if (name == "saturation.ampSim") {
        return sonare::mastering::api::make_insert("utility.gain", R"({"levelDb":6})");
      }
      return std::unique_ptr<sonare::rt::ProcessorBase>{};
    };
    auto player = std::make_unique<Sf2Player>(cfg);
    player->set_soundfont(make_sustained_fixture());
    player->prepare(kOutRate, 256);
    player->on_control_sysex(kPartOn, sizeof(kPartOn));
    player->on_control_sysex(kOdType, sizeof(kOdType));
    return player;
  };

  auto player = make_player();
  auto reference = make_player();
  Sf2PlayerConfig dry_config;
  dry_config.gain = 1.0f;
  dry_config.bank_rig_binding = false;
  auto dry = std::make_unique<Sf2Player>(dry_config);
  dry->set_soundfont(make_sustained_fixture());
  dry->prepare(kOutRate, 256);

  // The public preparation API remains structural: a parameter token can be
  // prepared before the unsupported type's scheduled token, regardless of a
  // prior direct fallback in the control mirror. The direct hook below is the
  // compatibility boundary that must route this token back to the legacy graph.
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> parameter_token;
  REQUIRE(player->prepare_sysex(kOdDrive, sizeof(kOdDrive), parameter_token));
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> unsupported_type_token;
  REQUIRE_FALSE(player->prepare_sysex(kOdType, sizeof(kOdType), unsupported_type_token));

  for (Sf2Player* current : {player.get(), reference.get()}) {
    current->on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
    current->on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  }
  dry->on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
  dry->on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  std::array<float, 256> first_left{};
  std::array<float, 256> first_right{};
  float* first_channels[2] = {first_left.data(), first_right.data()};
  player->process(first_channels, 2, 256);
  reference->process(first_channels, 2, 256);

  const auto dispatch_prepared =
      [](Sf2Player& target, const uint8_t* payload, size_t size,
         const std::shared_ptr<const sonare::midi::PreparedMidiSysEx>& token) {
        MidiEvent event;
        event.ump = sonare::midi::make_sysex_handle(0, 1);
        event.sysex_payload = payload;
        event.sysex_payload_size = size;
        event.prepared_sysex = token.get();
        target.on_event(0, event);
      };
  // Exercise the scheduled candidate-less path after a direct custom fallback:
  // it must retain the already-built legacy DSP while applying the raw byte.
  dispatch_prepared(*player, kOdDrive, sizeof(kOdDrive), parameter_token);
  std::array<float, 256> player_left{};
  std::array<float, 256> player_right{};
  std::array<float, 256> reference_left{};
  std::array<float, 256> reference_right{};
  std::array<float, 256> dry_first_left{};
  std::array<float, 256> dry_first_right{};
  std::array<float, 256> dry_left{};
  std::array<float, 256> dry_right{};
  float* player_channels[2] = {player_left.data(), player_right.data()};
  float* reference_channels[2] = {reference_left.data(), reference_right.data()};
  float* dry_first_channels[2] = {dry_first_left.data(), dry_first_right.data()};
  float* dry_channels[2] = {dry_left.data(), dry_right.data()};
  player->process(player_channels, 2, 256);
  reference->process(reference_channels, 2, 256);
  dry->process(dry_first_channels, 2, 256);
  dry->process(dry_channels, 2, 256);

  double error = 0.0;
  double reference_power = 0.0;
  double dry_power = 0.0;
  for (size_t i = 0; i < player_left.size(); ++i) {
    const double dl = static_cast<double>(player_left[i]) - reference_left[i];
    const double dr = static_cast<double>(player_right[i]) - reference_right[i];
    error += dl * dl + dr * dr;
    reference_power += static_cast<double>(reference_left[i]) * reference_left[i] +
                       static_cast<double>(reference_right[i]) * reference_right[i];
    dry_power += static_cast<double>(dry_left[i]) * dry_left[i] +
                 static_cast<double>(dry_right[i]) * dry_right[i];
  }
  REQUIRE(reference_power > 1e-8);
  REQUIRE(reference_power > dry_power * 1.5);
  REQUIRE(std::sqrt(error / reference_power) < 1e-5);
}

TEST_CASE("custom direct EFX can switch to stock before a parameter token",
          "[midi][sf2][gsefx][prepared]") {
  REQUIRE(sonare::mastering::api::make_insert("saturation.ampSim", "{}") != nullptr);
  const auto unsupported = std::make_shared<bool>(true);
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.bank_rig_binding = false;
  cfg.insert_factory = [unsupported](std::string_view name, std::string_view json) {
    if (*unsupported && name == "saturation.ampSim") {
      return sonare::mastering::api::make_insert("utility.gain", R"({"levelDb":6})");
    }
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  Sf2Player player(cfg);
  player.set_soundfont(make_sustained_fixture());
  player.prepare(kOutRate, 256);
  player.on_control_sysex(kPartOn, sizeof(kPartOn));
  // Build the unsupported legacy graph first. The actual assertion below
  // switches to a different stock type, so this cannot pass by accidentally
  // reusing the same type's legacy processor.
  player.on_control_sysex(kOdType, sizeof(kOdType));

  // Replace the unsupported custom fallback with a different stock processor,
  // then dispatch a candidate-less parameter token. The stock chorus graph and
  // its parameter must survive both transitions.
  *unsupported = false;
  player.on_control_sysex(kChorusType, sizeof(kChorusType));
  const auto chorus_parameter = efx_param_write(0x03, 0x7F);
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> parameter_token;
  REQUIRE(player.prepare_sysex(chorus_parameter.data(), chorus_parameter.size(), parameter_token));
  const MidiEvent parameter_event = [&] {
    MidiEvent event;
    event.ump = sonare::midi::make_sysex_handle(0, 1);
    event.sysex_payload = chorus_parameter.data();
    event.sysex_payload_size = chorus_parameter.size();
    event.prepared_sysex = parameter_token.get();
    return event;
  }();
  player.on_event(0, parameter_event);

  std::array<float, 256> actual_left{};
  std::array<float, 256> actual_right{};
  float* actual_channels[2] = {actual_left.data(), actual_right.data()};
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  player.process(actual_channels, 2, 256);

  // A fresh stock-chorus player is the audio oracle for the final state. It
  // uses the same direct payloads, so only the custom->stock transition and
  // scheduled parameter dispatch differ.
  Sf2Player reference(cfg);
  reference.set_soundfont(make_sustained_fixture());
  reference.prepare(kOutRate, 256);
  reference.on_control_sysex(kPartOn, sizeof(kPartOn));
  reference.on_control_sysex(kChorusType, sizeof(kChorusType));
  reference.on_control_sysex(chorus_parameter.data(), chorus_parameter.size());
  reference.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
  reference.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  std::array<float, 256> reference_left{};
  std::array<float, 256> reference_right{};
  float* reference_channels[2] = {reference_left.data(), reference_right.data()};
  reference.process(reference_channels, 2, 256);

  double error = 0.0;
  double reference_power = 0.0;
  for (size_t i = 0; i < actual_left.size(); ++i) {
    const double dl = static_cast<double>(actual_left[i]) - reference_left[i];
    const double dr = static_cast<double>(actual_right[i]) - reference_right[i];
    error += dl * dl + dr * dr;
    reference_power += static_cast<double>(reference_left[i]) * reference_left[i] +
                       static_cast<double>(reference_right[i]) * reference_right[i];
  }
  REQUIRE(reference_power > 1e-8);
  REQUIRE(std::sqrt(error / reference_power) < 1e-5);
}

TEST_CASE("custom direct EFX bulk parameter and send writes rebuild together",
          "[midi][sf2][gsefx][prepared]") {
  REQUIRE(sonare::mastering::api::make_insert("utility.gain", R"({"levelDb":6})") != nullptr);
  const auto make_player = [] {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.bank_rig_binding = false;
    cfg.insert_factory = [](std::string_view name, std::string_view) {
      if (name == "saturation.ampSim") {
        return sonare::mastering::api::make_insert("utility.gain", R"({"levelDb":6})");
      }
      return std::unique_ptr<sonare::rt::ProcessorBase>{};
    };
    auto player = std::make_unique<Sf2Player>(cfg);
    player->set_soundfont(make_sustained_fixture());
    player->prepare(kOutRate, 256);
    player->on_control_sysex(kPartOn, sizeof(kPartOn));
    player->on_control_sysex(kOdType, sizeof(kOdType));
    return player;
  };

  auto bulk = make_player();
  auto split = make_player();
  const auto bulk_write = efx_bulk_write(0x16, {0x7F, 0x7F, 0x00, 0x00});
  bulk->on_control_sysex(bulk_write.data(), bulk_write.size());
  const auto level = efx_param_write(0x16, 0x7F);
  const auto reverb = efx_param_write(0x17, 0x7F);
  split->on_control_sysex(level.data(), level.size());
  split->on_control_sysex(reverb.data(), reverb.size());
  for (Sf2Player* player : {bulk.get(), split.get()}) {
    player->on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
    player->on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  }

  std::vector<float> bulk_left(8 * 256, 0.0f);
  std::vector<float> bulk_right(8 * 256, 0.0f);
  std::vector<float> split_left(8 * 256, 0.0f);
  std::vector<float> split_right(8 * 256, 0.0f);
  for (size_t block = 0; block < 8; ++block) {
    float* bulk_channels[2] = {bulk_left.data() + block * 256, bulk_right.data() + block * 256};
    float* split_channels[2] = {split_left.data() + block * 256, split_right.data() + block * 256};
    bulk->process(bulk_channels, 2, 256);
    split->process(split_channels, 2, 256);
  }
  double error = 0.0;
  double split_power = 0.0;
  for (size_t i = 0; i < bulk_left.size(); ++i) {
    const double dl = static_cast<double>(bulk_left[i]) - split_left[i];
    const double dr = static_cast<double>(bulk_right[i]) - split_right[i];
    error += dl * dl + dr * dr;
    split_power += static_cast<double>(split_left[i]) * split_left[i] +
                   static_cast<double>(split_right[i]) * split_right[i];
  }
  REQUIRE(split_power > 1e-8);
  REQUIRE(std::sqrt(error / split_power) < 1e-5);
}

TEST_CASE("failed EFX parameter translation publishes no partial prefix",
          "[midi][sf2][gsefx][prepared]") {
  const auto throw_factory = std::make_shared<bool>(false);
  const auto set_count = std::make_shared<int>(0);
  const auto make_player = [&] {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.bank_rig_binding = false;
    cfg.insert_factory = [throw_factory, set_count](std::string_view name, std::string_view) {
      if (*throw_factory) throw std::runtime_error("partial rebuild failure");
      if (name == "saturation.overdrive" || name == "saturation.ampSim") {
        return std::unique_ptr<sonare::rt::ProcessorBase>(new MixedSafetyInsert(set_count));
      }
      return std::unique_ptr<sonare::rt::ProcessorBase>{};
    };
    auto player = std::make_unique<Sf2Player>(cfg);
    player->set_soundfont(make_sustained_fixture());
    player->prepare(kOutRate, 256);
    player->on_control_sysex(kPartOn, sizeof(kPartOn));
    player->on_control_sysex(kOdType, sizeof(kOdType));
    player->on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
    player->on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
    return player;
  };

  auto subject = make_player();
  auto reference = make_player();
  std::array<float, 256> warm_subject_left{};
  std::array<float, 256> warm_subject_right{};
  std::array<float, 256> warm_reference_left{};
  std::array<float, 256> warm_reference_right{};
  float* warm_subject[2] = {warm_subject_left.data(), warm_subject_right.data()};
  float* warm_reference[2] = {warm_reference_left.data(), warm_reference_right.data()};
  subject->process(warm_subject, 2, 256);
  reference->process(warm_reference, 2, 256);

  // The Overdrive translation has the pedal's gainDb first and the amps' cab
  // after it. The first is realtime-safe, the second is deliberately not; the
  // throwing rebuild must therefore happen before any queue record is published.
  *throw_factory = true;
  subject->on_control_sysex(kOdDrive, sizeof(kOdDrive));
  *throw_factory = false;
  std::array<float, 256> actual_left{};
  std::array<float, 256> actual_right{};
  std::array<float, 256> expected_left{};
  std::array<float, 256> expected_right{};
  float* actual[2] = {actual_left.data(), actual_right.data()};
  float* expected[2] = {expected_left.data(), expected_right.data()};
  subject->process(actual, 2, 256);
  reference->process(expected, 2, 256);

  double error = 0.0;
  double expected_power = 0.0;
  for (size_t i = 0; i < actual_left.size(); ++i) {
    const double dl = static_cast<double>(actual_left[i]) - expected_left[i];
    const double dr = static_cast<double>(actual_right[i]) - expected_right[i];
    error += dl * dl + dr * dr;
    expected_power += static_cast<double>(expected_left[i]) * expected_left[i] +
                      static_cast<double>(expected_right[i]) * expected_right[i];
  }
  REQUIRE(expected_power > 1e-8);
  REQUIRE(std::sqrt(error / expected_power) < 1e-5);
  CHECK(*set_count == 0);
}

TEST_CASE("direct EFX rebuild failure preserves the prior snapshot",
          "[midi][sf2][gsefx][prepared]") {
  const auto throw_factory = std::make_shared<bool>(false);
  const auto make_config = [throw_factory] {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.bank_rig_binding = false;
    cfg.insert_factory = [throw_factory](std::string_view name, std::string_view) {
      if (*throw_factory) throw std::runtime_error("test insert failure");
      if (name == "saturation.ampSim") {
        return sonare::mastering::api::make_insert("utility.gain", R"({"levelDb":6})");
      }
      return std::unique_ptr<sonare::rt::ProcessorBase>{};
    };
    return cfg;
  };
  auto player = std::make_unique<Sf2Player>(make_config());
  Sf2PlayerConfig reference_config = make_config();
  reference_config.insert_factory = [](std::string_view name, std::string_view) {
    if (name == "saturation.ampSim") {
      return sonare::mastering::api::make_insert("utility.gain", R"({"levelDb":6})");
    }
    return std::unique_ptr<sonare::rt::ProcessorBase>{};
  };
  auto reference = std::make_unique<Sf2Player>(std::move(reference_config));
  const auto sf2 = make_sustained_fixture();
  player->set_soundfont(sf2);
  reference->set_soundfont(sf2);
  player->prepare(kOutRate, 256);
  reference->prepare(kOutRate, 256);
  for (Sf2Player* current : {player.get(), reference.get()}) {
    current->on_control_sysex(kPartOn, sizeof(kPartOn));
    current->on_control_sysex(kOdType, sizeof(kOdType));
  }
  const uint32_t generation = player->gs_efx_generation();
  REQUIRE(player->gs_efx().type == 0x0110);
  REQUIRE(generation > 0);

  // The throwing factory exercises the direct compatibility rebuild from the
  // noexcept control hook. It must leave the old custom graph and mirror alive.
  *throw_factory = true;
  player->on_control_sysex(kChorusType, sizeof(kChorusType));
  REQUIRE(player->gs_efx().type == 0x0110);
  REQUIRE(player->gs_efx_generation() == generation);

  const auto note_on = event(sonare::midi::make_midi1_note_on(0, 0, 60, 127));
  player->on_event(0, note_on);
  reference->on_event(0, note_on);
  std::array<float, 256> player_left{};
  std::array<float, 256> player_right{};
  std::array<float, 256> reference_left{};
  std::array<float, 256> reference_right{};
  float* player_channels[2] = {player_left.data(), player_right.data()};
  float* reference_channels[2] = {reference_left.data(), reference_right.data()};
  player->process(player_channels, 2, 256);
  reference->process(reference_channels, 2, 256);
  double error = 0.0;
  double reference_power = 0.0;
  for (size_t i = 0; i < player_left.size(); ++i) {
    const double dl = static_cast<double>(player_left[i]) - reference_left[i];
    const double dr = static_cast<double>(player_right[i]) - reference_right[i];
    error += dl * dl + dr * dr;
    reference_power += static_cast<double>(reference_left[i]) * reference_left[i] +
                       static_cast<double>(reference_right[i]) * reference_right[i];
  }
  REQUIRE(reference_power > 1e-8);
  REQUIRE(std::sqrt(error / reference_power) < 1e-6);
}

TEST_CASE("prepared source edits activate a legacy control fanout",
          "[midi][sf2][gsefx][prepared]") {
  const auto counters = std::make_shared<EfxCounters>();
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.bank_rig_binding = false;
  cfg.insert_factory = [counters](std::string_view name, std::string_view) {
    if (name == "saturation.overdrive") {
      // Keep the CONTROL 1 gainDb destination while omitting every other
      // stage, forcing the strict prepared plan to reject this custom graph
      // and exercise the snapshot-local partial plan instead.
      return std::unique_ptr<sonare::rt::ProcessorBase>(
          std::make_unique<CountingInsert>(counters, std::vector<std::string>{"gainDb"}));
    }
    return std::unique_ptr<sonare::rt::ProcessorBase>{};
  };
  Sf2Player player(cfg);
  player.set_soundfont(make_sustained_fixture());
  player.prepare(kOutRate, 256);
  player.on_control_sysex(kPartOn, sizeof(kPartOn));
  player.on_control_sysex(kOdType, sizeof(kOdType));
  const int before = counters->set_params;
  const auto depth = efx_param_write(0x1C, 0x7F);   // CONTROL 1 full positive depth
  const auto source = efx_param_write(0x1B, 0x01);  // CONTROL 1 <- CC1
  const auto dispatch = [&](const auto& payload) {
    std::shared_ptr<const sonare::midi::PreparedMidiSysEx> token;
    REQUIRE(player.prepare_sysex(payload.data(), payload.size(), token));
    MidiEvent event;
    event.ump = sonare::midi::make_sysex_handle(0, 1);
    event.sysex_payload = payload.data();
    event.sysex_payload_size = payload.size();
    event.prepared_sysex = token.get();
    player.on_event(0, event);
  };
  dispatch(depth);
  dispatch(source);
  REQUIRE(counters->has_value_by_id[0]);
  const float at_rest = counters->last_value_by_id[0];
  player.on_event(0, sonare::test::event(sonare::midi::make_midi1_control_change(0, 0, 1, 40)));
  std::array<float, 256> left{};
  std::array<float, 256> right{};
  float* channels[2] = {left.data(), right.data()};
  player.process(channels, 2, 256);
  REQUIRE(counters->set_params > before);
  REQUIRE(counters->has_value_by_id[0]);
  CHECK(counters->last_value_by_id[0] != at_rest);
}

TEST_CASE("prepared EFX controls use the lowest routed part", "[midi][sf2][gsefx][prepared]") {
  REQUIRE(sonare::mastering::api::make_insert("saturation.ampSim", "{}") != nullptr);

  const auto render_power = [](uint8_t channel_zero_cc, uint8_t channel_one_cc) {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.bank_rig_binding = false;
    cfg.effects.enable_reverb = false;
    cfg.effects.enable_chorus = false;
    cfg.effects.enable_delay = false;
    cfg.insert_factory = [](std::string_view name, std::string_view json) {
      return sonare::mastering::api::make_insert(std::string(name), std::string(json));
    };
    Sf2Player player(cfg);
    player.set_soundfont(make_sustained_fixture());
    player.prepare(kOutRate, 256);

    const auto dispatch = [&](const uint8_t* data, size_t size) {
      std::shared_ptr<const sonare::midi::PreparedMidiSysEx> token;
      REQUIRE(player.prepare_sysex(data, size, token));
      MidiEvent event;
      event.ump = sonare::midi::make_sysex_handle(0, 1);
      event.sysex_payload = data;
      event.sysex_payload_size = size;
      event.prepared_sysex = token.get();
      player.on_event(0, event);
    };
    const auto depth = efx_param_write(0x1C, 0x7F);   // CONTROL 1 full positive depth.
    const auto source = efx_param_write(0x1B, 0x01);  // CONTROL 1 <- CC1.
    dispatch(kPartOnChannel1, sizeof(kPartOnChannel1));
    dispatch(kOdType, sizeof(kOdType));
    dispatch(depth.data(), depth.size());
    dispatch(source.data(), source.size());

    // The routed part is channel 1. A faulty node-local part default of zero
    // reads the channel-0 CC instead and makes these two renders identical.
    player.on_event(
        0, sonare::test::event(sonare::midi::make_midi1_control_change(0, 0, 1, channel_zero_cc)));
    player.on_event(
        0, sonare::test::event(sonare::midi::make_midi1_control_change(0, 1, 1, channel_one_cc)));
    player.on_event(0, sonare::test::event(sonare::midi::make_midi1_program_change(0, 1, 1)));
    player.on_event(0, sonare::test::event(sonare::midi::make_midi1_note_on(0, 1, 60, 127)));

    std::array<float, 256> left{};
    std::array<float, 256> right{};
    float* channels[2] = {left.data(), right.data()};
    player.process(channels, 2, 256);
    double power = 0.0;
    for (size_t i = 0; i < left.size(); ++i) {
      power += static_cast<double>(left[i]) * left[i] + static_cast<double>(right[i]) * right[i];
    }
    return power;
  };

  const double channel_one_rest = render_power(0, 0);
  const double channel_one_high = render_power(0, 127);
  const double channel_zero_high = render_power(127, 0);
  REQUIRE(channel_one_rest > 1e-8);
  REQUIRE(channel_one_high > channel_one_rest * 1.01);
  CHECK(std::abs(channel_zero_high - channel_one_rest) < channel_one_rest * 1e-5);
}

TEST_CASE("prepared EFX refreshes host rig routing after a program change",
          "[midi][sf2][gsefx][prepared]") {
  const auto make_player = [](bool bank_rig_binding) {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.dc_block = false;
    cfg.bank_rig_binding = bank_rig_binding;
    cfg.effects.enable_reverb = false;
    cfg.effects.enable_chorus = false;
    cfg.effects.enable_delay = false;
    cfg.insert_factory = [](std::string_view, std::string_view) {
      return sonare::mastering::api::make_insert("utility.gain", R"({"levelDb":6})");
    };
    auto player = std::make_unique<Sf2Player>(cfg);
    player->prepare(kOutRate, 256);
    return player;
  };
  auto prepared = make_player(true);
  auto reference = make_player(true);
  auto dry = make_player(false);

  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> token;
  REQUIRE(prepared->prepare_sysex(kOdDrive, sizeof(kOdDrive), token));
  MidiEvent prepared_event;
  prepared_event.ump = sonare::midi::make_sysex_handle(0, 1);
  prepared_event.sysex_payload = kOdDrive;
  prepared_event.sysex_payload_size = sizeof(kOdDrive);
  prepared_event.prepared_sysex = token.get();
  prepared->on_event(0, prepared_event);

  const auto program_and_note = [](Sf2Player& player) {
    player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 30)));
    player.realize_gs_efx();
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  };
  program_and_note(*prepared);
  program_and_note(*reference);
  program_and_note(*dry);

  std::array<float, 256> prepared_left{};
  std::array<float, 256> prepared_right{};
  std::array<float, 256> reference_left{};
  std::array<float, 256> reference_right{};
  std::array<float, 256> dry_left{};
  std::array<float, 256> dry_right{};
  float* prepared_channels[2] = {prepared_left.data(), prepared_right.data()};
  float* reference_channels[2] = {reference_left.data(), reference_right.data()};
  float* dry_channels[2] = {dry_left.data(), dry_right.data()};
  prepared->process(prepared_channels, 2, 256);
  reference->process(reference_channels, 2, 256);
  dry->process(dry_channels, 2, 256);

  double prepared_power = 0.0;
  double reference_power = 0.0;
  double dry_power = 0.0;
  double error = 0.0;
  for (size_t i = 0; i < prepared_left.size(); ++i) {
    prepared_power += static_cast<double>(prepared_left[i]) * prepared_left[i] +
                      static_cast<double>(prepared_right[i]) * prepared_right[i];
    reference_power += static_cast<double>(reference_left[i]) * reference_left[i] +
                       static_cast<double>(reference_right[i]) * reference_right[i];
    dry_power += static_cast<double>(dry_left[i]) * dry_left[i] +
                 static_cast<double>(dry_right[i]) * dry_right[i];
    const double dl = static_cast<double>(prepared_left[i]) - reference_left[i];
    const double dr = static_cast<double>(prepared_right[i]) - reference_right[i];
    error += dl * dl + dr * dr;
  }
  REQUIRE(reference_power > dry_power * 1.5);
  REQUIRE(prepared_power > dry_power * 1.5);
  REQUIRE(std::sqrt(error / reference_power) < 1e-6);
}

TEST_CASE("failed EFX row replacement preserves the prepared graph",
          "[midi][sf2][gsefx][prepared]") {
  REQUIRE(sonare::mastering::api::make_insert("saturation.ampSim", "{}") != nullptr);
  const auto throw_factory = std::make_shared<bool>(false);
  const auto make_config = [throw_factory] {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.bank_rig_binding = false;
    cfg.insert_factory = [throw_factory](std::string_view name, std::string_view json) {
      if (*throw_factory) throw std::runtime_error("row replacement failure");
      return sonare::mastering::api::make_insert(std::string(name), std::string(json));
    };
    return cfg;
  };

  auto player = std::make_unique<Sf2Player>(make_config());
  auto reference = std::make_unique<Sf2Player>(make_config());
  for (Sf2Player* current : {player.get(), reference.get()}) {
    current->set_soundfont(make_sustained_fixture());
    current->prepare(kOutRate, 256);
    current->on_control_sysex(kPartOn, sizeof(kPartOn));
    current->on_control_sysex(kOdType, sizeof(kOdType));
    current->on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
    current->on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  }
  std::array<float, 256> warm_left{};
  std::array<float, 256> warm_right{};
  std::array<float, 256> reference_warm_left{};
  std::array<float, 256> reference_warm_right{};
  float* warm_channels[2] = {warm_left.data(), warm_right.data()};
  float* reference_warm_channels[2] = {reference_warm_left.data(), reference_warm_right.data()};
  player->process(warm_channels, 2, 256);
  reference->process(reference_warm_channels, 2, 256);

  const sonare::midi::synth::GsEfxRowView rows{
      sonare::midi::synth::kGsEfxBindingRows.data(), sonare::midi::synth::kGsEfxBindingRows.size(),
      sonare::midi::synth::kGsEfxEnables.data(), sonare::midi::synth::kGsEfxEnables.size()};
  *throw_factory = true;
  REQUIRE_THROWS(player->set_gs_efx_rows(&rows));
  *throw_factory = false;

  std::array<float, 256> actual_left{};
  std::array<float, 256> actual_right{};
  std::array<float, 256> expected_left{};
  std::array<float, 256> expected_right{};
  float* actual_channels[2] = {actual_left.data(), actual_right.data()};
  float* expected_channels[2] = {expected_left.data(), expected_right.data()};
  player->process(actual_channels, 2, 256);
  reference->process(expected_channels, 2, 256);
  double error = 0.0;
  double expected_power = 0.0;
  for (size_t i = 0; i < actual_left.size(); ++i) {
    const double dl = static_cast<double>(actual_left[i]) - expected_left[i];
    const double dr = static_cast<double>(actual_right[i]) - expected_right[i];
    error += dl * dl + dr * dr;
    expected_power += static_cast<double>(expected_left[i]) * expected_left[i] +
                      static_cast<double>(expected_right[i]) * expected_right[i];
  }
  REQUIRE(expected_power > 1e-8);
  REQUIRE(std::sqrt(error / expected_power) < 1e-5);
}

TEST_CASE("prepared EFX tokens follow a moved player and reject foreign state",
          "[midi][sf2][gsefx][prepared]") {
  REQUIRE(sonare::mastering::api::make_insert("saturation.ampSim", "{}") != nullptr);
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.bank_rig_binding = false;
  cfg.insert_factory = [](std::string_view name, std::string_view json) {
    return sonare::mastering::api::make_insert(std::string(name), std::string(json));
  };
  const auto dispatch = [](Sf2Player& player, const uint8_t* payload, size_t size,
                           const std::shared_ptr<const sonare::midi::PreparedMidiSysEx>& token) {
    MidiEvent event;
    event.ump = sonare::midi::make_sysex_handle(0, 1);
    event.sysex_payload = payload;
    event.sysex_payload_size = size;
    event.prepared_sysex = token.get();
    player.on_event(0, event);
  };
  using Stereo = std::array<float, 2048>;
  const auto render = [](Sf2Player& player) {
    Stereo left{};
    Stereo right{};
    float* channels[2] = {left.data(), right.data()};
    player.process(channels, 2, static_cast<int>(left.size()));
    return std::pair<Stereo, Stereo>{left, right};
  };
  const auto relative_error = [](const std::pair<Stereo, Stereo>& actual,
                                 const std::pair<Stereo, Stereo>& expected) {
    double error = 0.0;
    double power = 0.0;
    for (size_t i = 0; i < actual.first.size(); ++i) {
      const double dl = static_cast<double>(actual.first[i]) - expected.first[i];
      const double dr = static_cast<double>(actual.second[i]) - expected.second[i];
      error += dl * dl + dr * dr;
      power += static_cast<double>(expected.first[i]) * expected.first[i] +
               static_cast<double>(expected.second[i]) * expected.second[i];
    }
    return std::sqrt(error / std::max(power, 1e-30));
  };
  const auto prepare_tokens = [&](Sf2Player& player,
                                  std::shared_ptr<const sonare::midi::PreparedMidiSysEx>& assign,
                                  std::shared_ptr<const sonare::midi::PreparedMidiSysEx>& type,
                                  std::shared_ptr<const sonare::midi::PreparedMidiSysEx>& param) {
    player.set_soundfont(make_fixture());
    player.prepare(kOutRate, 256);
    REQUIRE(player.prepare_sysex(kPartOn, sizeof(kPartOn), assign));
    REQUIRE(player.prepare_sysex(kOdType, sizeof(kOdType), type));
    REQUIRE(player.prepare_sysex(kOdDrive, sizeof(kOdDrive), param));
  };
  const auto note_on = [](Sf2Player& player) {
    player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  };
  const auto direct_reference = [&] {
    Sf2Player reference(cfg);
    reference.set_soundfont(make_fixture());
    reference.prepare(kOutRate, 256);
    reference.on_control_sysex(kPartOn, sizeof(kPartOn));
    reference.on_control_sysex(kOdType, sizeof(kOdType));
    reference.on_control_sysex(kOdDrive, sizeof(kOdDrive));
    note_on(reference);
    return render(reference);
  };

  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> assign_token;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> type_token;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> parameter_token;
  Sf2Player source(cfg);
  prepare_tokens(source, assign_token, type_token, parameter_token);
  Sf2Player moved(std::move(source));
  dispatch(moved, kPartOn, sizeof(kPartOn), assign_token);
  dispatch(moved, kOdType, sizeof(kOdType), type_token);
  dispatch(moved, kOdDrive, sizeof(kOdDrive), parameter_token);
  note_on(moved);
  const auto moved_audio = render(moved);
  const auto reference_audio = direct_reference();
  REQUIRE(relative_error(moved_audio, reference_audio) < 1e-5);

  // A token must not be accepted by an unrelated player, even when both are
  // prepared with identical configuration and happen to share an address later.
  Sf2Player foreign(cfg);
  foreign.set_soundfont(make_fixture());
  foreign.prepare(kOutRate, 256);
  dispatch(foreign, kPartOn, sizeof(kPartOn), assign_token);
  dispatch(foreign, kOdType, sizeof(kOdType), type_token);
  dispatch(foreign, kOdDrive, sizeof(kOdDrive), parameter_token);
  note_on(foreign);
  Sf2PlayerConfig dry_config;
  dry_config.gain = 1.0f;
  dry_config.bank_rig_binding = false;
  Sf2Player dry(dry_config);
  dry.set_soundfont(make_fixture());
  dry.prepare(kOutRate, 256);
  note_on(dry);
  REQUIRE(relative_error(render(foreign), render(dry)) < 1e-6);

  // Re-preparation changes the domain, so an old token must fall back to the
  // ordinary SysEx path and leave the newly prepared player dry.
  Sf2Player stale(cfg);
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> stale_assign;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> stale_type;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> stale_param;
  prepare_tokens(stale, stale_assign, stale_type, stale_param);
  stale.prepare(kOutRate, 256);
  dispatch(stale, kPartOn, sizeof(kPartOn), stale_assign);
  dispatch(stale, kOdType, sizeof(kOdType), stale_type);
  dispatch(stale, kOdDrive, sizeof(kOdDrive), stale_param);
  note_on(stale);
  Sf2PlayerConfig stale_dry_config;
  stale_dry_config.gain = 1.0f;
  stale_dry_config.bank_rig_binding = false;
  Sf2Player stale_dry(stale_dry_config);
  stale_dry.set_soundfont(make_fixture());
  stale_dry.prepare(kOutRate, 256);
  note_on(stale_dry);
  REQUIRE(relative_error(render(stale), render(stale_dry)) < 1e-6);

  // Move assignment transfers the source identity and rejects a token created
  // by the overwritten destination.
  Sf2Player assign_source(cfg);
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> source_assign;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> source_type;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> source_param;
  prepare_tokens(assign_source, source_assign, source_type, source_param);
  Sf2Player assign_target(cfg);
  assign_target.set_soundfont(make_fixture());
  assign_target.prepare(kOutRate, 256);
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> old_target_token;
  REQUIRE(assign_target.prepare_sysex(kChorusType, sizeof(kChorusType), old_target_token));
  assign_target = std::move(assign_source);
  dispatch(assign_target, kChorusType, sizeof(kChorusType), old_target_token);
  dispatch(assign_target, kPartOn, sizeof(kPartOn), source_assign);
  dispatch(assign_target, kOdType, sizeof(kOdType), source_type);
  dispatch(assign_target, kOdDrive, sizeof(kOdDrive), source_param);
  note_on(assign_target);
  REQUIRE(relative_error(render(assign_target), reference_audio) < 1e-5);
}

TEST_CASE("prepared GS EFX reclaims an unused current-domain key", "[midi][sf2][gsefx][prepared]") {
  int factory_calls = 0;
  Sf2PlayerConfig cfg;
  cfg.insert_factory = [&factory_calls](std::string_view, std::string_view) {
    ++factory_calls;
    return std::unique_ptr<sonare::rt::ProcessorBase>{};
  };
  Sf2Player player(cfg);
  player.prepare(kOutRate, 256);

  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> first;
  REQUIRE(player.prepare_sysex(kOdType, sizeof(kOdType), first));
  const int calls_after_first = factory_calls;
  REQUIRE(calls_after_first > 0);

  // Retiring the clip/token leaves the node with only the registry owner and
  // no audio pin. The next preparation sweep must reclaim it even though the
  // preparation domain is unchanged, then rebuild the key on demand.
  first.reset();
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> second;
  REQUIRE(player.prepare_sysex(kOdType, sizeof(kOdType), second));
  REQUIRE(factory_calls > calls_after_first);
}

TEST_CASE("prepared GS EFX switches cached nodes without retaining a prior type",
          "[midi][sf2][gsefx][prepared]") {
  auto counters = std::make_shared<EfxCounters>();
  const std::vector<std::string> keys = all_efx_binding_keys();
  Sf2PlayerConfig cfg;
  cfg.insert_factory = [counters, keys](std::string_view, std::string_view) {
    return std::unique_ptr<sonare::rt::ProcessorBase>(new CountingInsert(counters, keys));
  };
  Sf2Player player(cfg);
  player.prepare(kOutRate, 256);
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> a_token;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> b_token;
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> a_again_token;
  REQUIRE(player.prepare_sysex(kOdType, sizeof(kOdType), a_token));
  REQUIRE(player.prepare_sysex(kChorusType, sizeof(kChorusType), b_token));
  REQUIRE(player.prepare_sysex(kOdType, sizeof(kOdType), a_again_token));

  const auto dispatch = [&](const uint8_t* payload, size_t size,
                            const std::shared_ptr<const sonare::midi::PreparedMidiSysEx>& token) {
    MidiEvent event;
    event.ump = sonare::midi::make_sysex_handle(0, 1);
    event.sysex_payload = payload;
    event.sysex_payload_size = size;
    event.prepared_sysex = token.get();
    player.on_event(0, event);
  };
  std::array<float, 256> left{};
  std::array<float, 256> right{};
  float* channels[2] = {left.data(), right.data()};
  dispatch(kOdType, sizeof(kOdType), a_token);
  player.process(channels, 2, 256);
  const int after_a = counters->resets;
  dispatch(kChorusType, sizeof(kChorusType), b_token);
  player.process(channels, 2, 256);
  const int after_b = counters->resets;
  dispatch(kOdType, sizeof(kOdType), a_again_token);
  player.process(channels, 2, 256);
  REQUIRE(after_a > 0);
  REQUIRE(after_b > after_a);
  // A->B->A selects the cached A node again and resets it after the new raw
  // plan was written. A same-node selection is covered by the split-write case
  // above and does not increment this count.
  REQUIRE(counters->resets > after_b);
}

TEST_CASE("failed prepared GS EFX preparation leaves existing token usable",
          "[midi][sf2][gsefx][prepared]") {
  const std::vector<std::string> keys = all_efx_binding_keys();
  auto counters = std::make_shared<EfxCounters>();
  Sf2PlayerConfig cfg;
  cfg.insert_factory = [counters, keys](std::string_view name, std::string_view) {
    if (name.find("chorus") != std::string_view::npos) {
      return std::unique_ptr<sonare::rt::ProcessorBase>(new CountingInsert(counters, {}));
    }
    return std::unique_ptr<sonare::rt::ProcessorBase>(new CountingInsert(counters, keys));
  };
  Sf2Player player(cfg);
  player.prepare(kOutRate, 256);
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> good;
  REQUIRE(player.prepare_sysex(kOdType, sizeof(kOdType), good));
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> rejected;
  REQUIRE_FALSE(player.prepare_sysex(kChorusType, sizeof(kChorusType), rejected));
  REQUIRE(rejected == nullptr);

  MidiEvent event;
  event.ump = sonare::midi::make_sysex_handle(0, 1);
  event.sysex_payload = kOdType;
  event.sysex_payload_size = sizeof(kOdType);
  event.prepared_sysex = good.get();
  player.on_event(0, event);
  std::array<float, 256> left{};
  std::array<float, 256> right{};
  float* channels[2] = {left.data(), right.data()};
  player.process(channels, 2, 256);
  REQUIRE(counters->resets > 0);
}

TEST_CASE("direct EFX deltas retain the scheduled audio raw state",
          "[midi][sf2][gsefx][prepared]") {
  const std::vector<std::string> keys = all_efx_binding_keys();
  auto counters = std::make_shared<EfxCounters>();
  Sf2PlayerConfig cfg;
  cfg.insert_factory = [counters, keys](std::string_view, std::string_view) {
    return std::unique_ptr<sonare::rt::ProcessorBase>(new CountingInsert(counters, keys));
  };
  Sf2Player player(cfg);
  player.prepare(kOutRate, 256);

  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> type_token;
  REQUIRE(player.prepare_sysex(kOdType, sizeof(kOdType), type_token));
  MidiEvent type_event;
  type_event.ump = sonare::midi::make_sysex_handle(0, 1);
  type_event.sysex_payload = kOdType;
  type_event.sysex_payload_size = sizeof(kOdType);
  type_event.prepared_sysex = type_token.get();
  player.on_event(0, type_event);
  std::array<float, 256> left{};
  std::array<float, 256> right{};
  float* channels[2] = {left.data(), right.data()};
  player.process(channels, 2, 256);
  const int resets_after_type = counters->resets;
  const int sets_after_type = counters->set_params;
  REQUIRE(resets_after_type > 0);

  // The live control mirror still has Thru because the Overdrive type was
  // scheduled. A direct parameter must therefore be a relative audio delta;
  // publishing that mirror as an absolute state would discard the scheduled
  // type and leave the selected node dry.
  player.on_control_sysex(kOdDrive, sizeof(kOdDrive));
  player.process(channels, 2, 256);
  REQUIRE(counters->resets == resets_after_type);
  REQUIRE(counters->set_params > sets_after_type);

  // A scheduled reset followed by the same direct parameter must not resurrect
  // the pre-reset node. The parameter is retained in raw Thru state, but there
  // is no structural candidate to activate.
  constexpr uint8_t kGmReset[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> reset_token;
  REQUIRE(player.prepare_sysex(kGmReset, sizeof(kGmReset), reset_token));
  MidiEvent reset_event;
  reset_event.ump = sonare::midi::make_sysex_handle(0, 1);
  reset_event.sysex_payload = kGmReset;
  reset_event.sysex_payload_size = sizeof(kGmReset);
  reset_event.prepared_sysex = reset_token.get();
  player.on_event(0, reset_event);
  player.process(channels, 2, 256);
  const int resets_after_reset = counters->resets;
  player.on_control_sysex(kOdDrive, sizeof(kOdDrive));
  player.process(channels, 2, 256);
  REQUIRE(counters->resets == resets_after_reset);
}

TEST_CASE("prepared GS EFX ignores a rejected boundary token at dispatch",
          "[midi][sf2][gsefx][prepared]") {
  const std::vector<std::string> keys = all_efx_binding_keys();
  auto counters = std::make_shared<EfxCounters>();
  Sf2PlayerConfig cfg;
  cfg.insert_factory = [counters, keys](std::string_view, std::string_view) {
    return std::unique_ptr<sonare::rt::ProcessorBase>(new CountingInsert(counters, keys));
  };
  Sf2Player player(cfg);
  player.prepare(kOutRate, 256);

  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> type_token;
  REQUIRE(player.prepare_sysex(kOdType, sizeof(kOdType), type_token));
  MidiEvent type_event;
  type_event.ump = sonare::midi::make_sysex_handle(0, 1);
  type_event.sysex_payload = kOdType;
  type_event.sysex_payload_size = sizeof(kOdType);
  type_event.prepared_sysex = type_token.get();
  player.on_event(0, type_event);
  std::array<float, 256> left{};
  std::array<float, 256> right{};
  float* channels[2] = {left.data(), right.data()};
  player.process(channels, 2, 256);
  const int resets_after_type = counters->resets;
  const int sets_after_type = counters->set_params;
  REQUIRE(resets_after_type > 0);

  // This token starts in unit 0's reserved tail and rolls into unit 1's TYPE,
  // so prepare_sysex accepts the frame but has no EFX write to dispatch.
  const std::vector<uint8_t> rejected = efx_extension_write(0x40307F, {0x00, 0x01, 0x10});
  std::shared_ptr<const sonare::midi::PreparedMidiSysEx> rejected_token;
  REQUIRE(player.prepare_sysex(rejected.data(), rejected.size(), rejected_token));
  REQUIRE(rejected_token != nullptr);
  MidiEvent rejected_event;
  rejected_event.ump = sonare::midi::make_sysex_handle(0, 1);
  rejected_event.sysex_payload = rejected.data();
  rejected_event.sysex_payload_size = rejected.size();
  rejected_event.prepared_sysex = rejected_token.get();
  player.on_event(0, rejected_event);
  player.process(channels, 2, 256);

  // A rejected token must not reapply the active node's plan. That would make
  // a no-op boundary message observable as a parameter publication.
  REQUIRE(counters->resets == resets_after_type);
  REQUIRE(counters->set_params == sets_after_type);
}
#endif  // SONARE_MIDI_WITH_FX && SONARE_WITH_MASTERING

TEST_CASE("a GS EFX parameter-only change updates the insert in place", "[midi][sf2][gsefx]") {
  auto counters = std::make_shared<EfxCounters>();
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.insert_factory = [counters](std::string_view, std::string_view) {
    return std::unique_ptr<sonare::rt::ProcessorBase>(
        new CountingInsert(counters, all_efx_binding_keys()));
  };
  Sf2Player player(cfg);
  player.prepare(kOutRate, 256);

  // Route part 1 (channel 0) through an EFX and select Overdrive: the chain is
  // built and every stage of it prepared. The count is the chain's length --
  // the drive block plus the output stage the unit puts after every effect --
  // and what matters below is that it does not move again.
  player.on_control_sysex(kPartOn, sizeof(kPartOn));
  const int prepares_before_type = counters->prepares;
  player.on_control_sysex(kOdType, sizeof(kOdType));
  // The immutable prepared node is built before the first audio boundary;
  // direct control publishes the same payload-only plan used by a scheduled
  // event without duplicating a legacy full snapshot.
  REQUIRE(counters->prepares - prepares_before_type ==
          static_cast<int>(gs_efx_insert_chain(efx_holding(0x0110)).size()));
  const int prepares_after_build = counters->prepares;
  std::vector<float> left(256, 0.0f);
  std::vector<float> right(256, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  player.process(chans, 2, 256);  // select the prepared node before the live edit
  const int resets_after_type = counters->resets;
  const int set_params_before = counters->set_params;

  // A parameter-only edit (40 03 03, the drive) must NOT rebuild
  // the chain: it is resolved on the control thread and applied to the live
  // processor on the audio thread at the next block. on_control_sysex enqueues
  // but does not touch the processor; the set_parameter lands during the
  // following process() call.
  player.on_control_sysex(kOdDrive, sizeof(kOdDrive));
  REQUIRE(counters->prepares == prepares_after_build);  // not rebuilt
  REQUIRE(counters->set_params == set_params_before);   // not applied synchronously

  player.process(chans, 2, 256);  // audio thread drains the queue and applies it

  REQUIRE(counters->prepares == prepares_after_build);  // still no rebuild
  REQUIRE(counters->resets == resets_after_type);       // DSP state preserved
  REQUIRE(counters->set_params > set_params_before);    // applied on the audio thread

  // A genuine EFX TYPE change still triggers a full rebuild (a fresh processor,
  // hence another prepare()): exact behaviour is preserved when it is needed.
  player.on_control_sysex(kChorusType, sizeof(kChorusType));
  REQUIRE(counters->prepares > prepares_after_build);
}

TEST_CASE("a direct EFX burst retains its final delta", "[midi][sf2][gsefx]") {
  const auto make_player = [](const std::shared_ptr<EfxCounters>& counters) {
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.insert_factory = [counters](std::string_view, std::string_view) {
      return std::unique_ptr<sonare::rt::ProcessorBase>(
          new CountingInsert(counters, all_efx_binding_keys()));
    };
    auto player = std::make_unique<Sf2Player>(cfg);
    player->prepare(kOutRate, 256);
    player->on_control_sysex(kPartOn, sizeof(kPartOn));
    player->on_control_sysex(kOdType, sizeof(kOdType));
    return player;
  };

  auto burst_counters = std::make_shared<EfxCounters>();
  auto reference_counters = std::make_shared<EfxCounters>();
  auto burst = make_player(burst_counters);
  auto reference = make_player(reference_counters);

  // A burst of nine edits must reach the processor in publication order. The
  // final write is deliberately distinct from every preceding value so a
  // dropped tail is observable at the processor.
  for (uint8_t value = 10; value <= 18; ++value) {
    const auto edit = efx_param_write(0x03, value);
    burst->on_control_sysex(edit.data(), edit.size());
  }
  const auto final_edit = efx_param_write(0x03, 18);
  reference->on_control_sysex(final_edit.data(), final_edit.size());

  std::vector<float> burst_left(256, 0.0f);
  std::vector<float> burst_right(256, 0.0f);
  std::vector<float> reference_left(256, 0.0f);
  std::vector<float> reference_right(256, 0.0f);
  float* burst_channels[2] = {burst_left.data(), burst_right.data()};
  float* reference_channels[2] = {reference_left.data(), reference_right.data()};
  burst->process(burst_channels, 2, 256);
  reference->process(reference_channels, 2, 256);

  REQUIRE(burst_counters->has_value);
  REQUIRE(reference_counters->has_value);
  CHECK(std::abs(burst_counters->last_value - reference_counters->last_value) < 1e-6f);

  const sonare::midi::synth::GsEfxBindingRow* drive_row = nullptr;
  for (const sonare::midi::synth::GsEfxBindingRow& row : sonare::midi::synth::kGsEfxBindingRows) {
    if (row.type == 0x0110 && row.slot == 0 && row.printed_mark == '+') {
      drive_row = &row;
      break;
    }
  }
  REQUIRE(drive_row != nullptr);
  REQUIRE(burst_counters->has_value_by_id[drive_row->key]);
  REQUIRE(reference_counters->has_value_by_id[drive_row->key]);
  CHECK(std::abs(burst_counters->last_value_by_id[drive_row->key] -
                 reference_counters->last_value_by_id[drive_row->key]) < 1e-6f);
  // Distinguish the ninth write from the stale eighth value; the scalar
  // last-write comparison above must observe the key whose GS byte changed.
  CHECK(std::abs(burst_counters->last_value_by_id[drive_row->key] -
                 sonare::midi::synth::gs_efx_binding_value(*drive_row, 17)) > 1e-6f);
}

TEST_CASE("the measured EFX translations reach automatable insert parameters",
          "[midi][sf2][gsefx]") {
  // The failure this is the net for: a field added to an insert for a measured
  // conversion, wired into the translation, and never given a descriptor. The
  // edit is not lost — enqueue_efx_param_updates falls back to a rebuild — but
  // every parameter edit then zeroes the delay and reverb tails it was written
  // to preserve, and nothing else in the tree can see that happen.
  auto run = [](const std::vector<std::string>& keys, const std::array<uint8_t, 12>& type,
                const std::array<uint8_t, 11>& edit) {
    auto counters = std::make_shared<EfxCounters>();
    Sf2PlayerConfig cfg;
    cfg.gain = 1.0f;
    cfg.insert_factory = [counters, keys](std::string_view, std::string_view) {
      return std::unique_ptr<sonare::rt::ProcessorBase>(new CountingInsert(counters, keys));
    };
    Sf2Player player(cfg);
    player.prepare(kOutRate, 256);
    player.on_control_sysex(kPartOn, sizeof(kPartOn));
    player.on_control_sysex(type.data(), type.size());
    REQUIRE(counters->prepares >= 1);
    const int prepares_after_build = counters->prepares;
    player.on_control_sysex(edit.data(), edit.size());
    std::vector<float> left(256, 0.0f);
    std::vector<float> right(256, 0.0f);
    float* chans[2] = {left.data(), right.data()};
    player.process(chans, 2, 256);
    return std::pair<int, int>{counters->prepares - prepares_after_build, counters->set_params};
  };

  SECTION("a delay time edit is applied in place") {
    // Stereo Delay (01 50), EFX PARAMETER 1 at 40 03 03: the left tap's time.
    const auto result =
        run(kStereoDelayKeys, efx_type_write(0x01, 0x50), efx_param_write(0x03, 40));
    REQUIRE(result.first == 0);  // no rebuild
    REQUIRE(result.second > 0);  // applied through set_parameter
  }

  SECTION("a damping edit is applied in place") {
    // Same type, EFX PARAMETER 8 at 40 03 0A: the corner of the pole inside the
    // feedback loop, which is the field the damping measurement added.
    const auto result =
        run(kStereoDelayKeys, efx_type_write(0x01, 0x50), efx_param_write(0x0A, 40));
    REQUIRE(result.first == 0);
    REQUIRE(result.second > 0);
  }

  SECTION("a rotary acceleration edit rebuilds, because the glide is structure") {
    // Rotary (01 22), EFX PARAMETER 7 at 40 03 09: the horn rotor's acceleration
    // byte. It reaches accelTauS / undershootHz, neither of which the rotary
    // publishes as an automatable parameter, so the edit costs a rebuild. Pinned
    // rather than fixed: a time constant read on the audio thread mid-glide has
    // no defined arrival, and the insert says so by not offering the id.
    const auto result = run(kRotaryKeys, efx_type_write(0x01, 0x22), efx_param_write(0x09, 40));
    REQUIRE(result.first > 0);
    REQUIRE(result.second == 0);
  }
}

TEST_CASE("a partial EFX parameter publication rebuilds the live unit", "[midi][sf2][gsefx]") {
  // Stereo Delay translates one GS byte into several live processor keys. Fill
  // the 128-slot automation ring to 125 records, then make one more edit. Its
  // records must land together or not at all; a prefix alone would leave the
  // unit on a mix of two parameter generations.
  auto counters = std::make_shared<EfxCounters>();
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.insert_factory = [counters](std::string_view, std::string_view) {
    return std::unique_ptr<sonare::rt::ProcessorBase>(
        new CountingInsert(counters, kStereoDelayKeys));
  };
  Sf2Player player(cfg);
  player.prepare(kOutRate, 256);
  player.on_control_sysex(kPartOn, sizeof(kPartOn));
  const auto type = efx_type_write(0x01, 0x50);
  player.on_control_sysex(type.data(), type.size());
  const int prepares_after_build = counters->prepares;
  REQUIRE(prepares_after_build >= 1);

  // The translated stereo-delay JSON has five descriptors in this fixture;
  // 25 edits therefore occupy 125 of the 128 records exactly.
  for (uint8_t value = 40; value < 65; ++value) {
    const auto edit = efx_param_write(0x03, value);
    player.on_control_sysex(edit.data(), edit.size());
  }
  REQUIRE(counters->prepares == prepares_after_build);

  const auto partial = efx_param_write(0x03, 65);
  player.on_control_sysex(partial.data(), partial.size());
  REQUIRE(counters->prepares > prepares_after_build);
}

#if defined(SONARE_WITH_FX) && defined(SONARE_WITH_MASTERING)
namespace {

namespace json = sonare::util::json;
namespace s = sonare::midi::synth;

}  // namespace

TEST_CASE("every EFX binding drives a control its insert can automate", "[midi][sf2][gsefx]") {
  // Every generated binding row must reach a realtime-safe descriptor in the
  // factory's actual processor. Missing descriptors would force a direct GS
  // edit to rebuild the chain and cut its DSP tail.
  std::map<std::string, std::set<std::string>> automatable;
  std::map<std::string, std::set<std::string>> accepted;
  for (std::string_view stage : s::kGsEfxRowStages) {
    const std::string name(stage);
    const json::Value parsed =
        json::parse_strict(sonare::mastering::api::insert_param_info_json(name));
    INFO("insert " << name);
    REQUIRE(parsed.is_array());
    std::set<std::string> ids;
    for (const json::Value& parameter : parsed.as_array()) {
      const json::Value* key = parameter.find("name");
      const json::Value* id = parameter.find("id");
      REQUIRE(key != nullptr);
      REQUIRE(id != nullptr);
      // A construction-only key is listed too, with no id to automate it by.
      if (!id->is_null()) ids.insert(key->as_string());
    }
    // An insert name the factory does not know and one whose build feature is
    // off both answer with an empty array, which would excuse every key on it.
    REQUIRE_FALSE(ids.empty());
    automatable.emplace(name, std::move(ids));

    const std::vector<std::string> keys = sonare::mastering::api::insert_param_names(name);
    REQUIRE_FALSE(keys.empty());  // same two ways of answering nothing
    accepted.emplace(name, std::set<std::string>(keys.begin(), keys.end()));
  }

  std::set<std::pair<std::string, std::string>> seen;
  int checked = 0;
  for (const s::GsEfxBindingRow& row : s::kGsEfxBindingRows) {
    const std::string stage(s::kGsEfxRowStages[row.stage]);
    const std::string key(s::kGsEfxRowKeys[row.key]);
    if (!seen.emplace(stage, key).second) continue;  // one verdict per control
    ++checked;

    // A key the insert does not read is dropped in silence at construction, so
    // there is no excused list for this half: the byte reaches nothing at all
    // rather than reaching something that cannot be ridden.
    {
      INFO(stage << "." << key << " is driven by a binding row and is not a key that insert reads");
      CHECK(accepted[stage].count(key) != 0);
    }

    INFO(stage << "." << key << " is driven by a binding row but is not realtime-safe");
    CHECK(automatable[stage].count(key) != 0);
  }

  // A floor rather than an equality: binding one more control is not a
  // regression, and a ceiling would read it as one.
  WARN("distinct (insert, control) pairs checked: " << checked);
  REQUIRE(checked >= 42);
}
#endif  // SONARE_WITH_FX && SONARE_WITH_MASTERING
