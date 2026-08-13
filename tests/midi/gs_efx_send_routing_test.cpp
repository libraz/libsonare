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
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mastering/api/insert_factory.h"
#include "midi/midi_event.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "rt/processor_base.h"
#include "support/sf2_builder.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;

// GS SysEx (Roland DT1, framed) shared with tests/midi/sf2_effects_test.cpp:
// enable EFX on part 1 (channel 0), select Overdrive (01 10), set OD Drive
// (EFX PARAMETER 2 = 40 03 04) to max. Checksums per the DT1 rule.
constexpr uint8_t kPartOn[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x41, 0x22, 0x01, 0x5C, 0xF7};
constexpr uint8_t kOdType[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                               0x03, 0x00, 0x01, 0x10, 0x2C, 0xF7};
constexpr uint8_t kOdDrive[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, 0x04, 0x7F, 0x3A, 0xF7};
// A genuine TYPE change: select Stereo Chorus (01 42).
constexpr uint8_t kChorusType[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
                                   0x03, 0x00, 0x01, 0x42, 0x7A, 0xF7};

// Only used by the post-effect reverb-routing test below, which is itself
// gated on SONARE_MIDI_WITH_FX and SONARE_WITH_MASTERING (it needs the EFX
// insertion chain, built through mastering::api::make_insert, to build).
#if defined(SONARE_MIDI_WITH_FX) && defined(SONARE_WITH_MASTERING)
MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

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
/// apart from a full rebuild (a fresh processor, prepare()). Its named
/// parameters match the keys the Overdrive EFX translation emits.
struct EfxCounters {
  int prepares = 0;
  int resets = 0;
  int set_params = 0;
};

class CountingInsert final : public sonare::rt::ProcessorBase {
 public:
  explicit CountingInsert(std::shared_ptr<EfxCounters> counters) : counters_(std::move(counters)) {}
  void prepare(double, int) override { ++counters_->prepares; }
  void process(float* const*, int, int) override {}
  void reset() override { ++counters_->resets; }
  bool set_parameter(unsigned int, float) override {
    ++counters_->set_params;
    return true;
  }
  std::vector<sonare::rt::ParamDescriptor> parameter_descriptors() const override {
    return {{"drive", 1}, {"ampModel", 2}, {"levelDb", 3}};
  }

 private:
  std::shared_ptr<EfxCounters> counters_;
};

}  // namespace

#if defined(SONARE_MIDI_WITH_FX) && defined(SONARE_WITH_MASTERING)
TEST_CASE("a GS EFX part sends its post-effect signal to reverb", "[midi][sf2][gsefx]") {
  // The insertion-effect (Overdrive) stage must actually build for the part to
  // be bussed; skip if the amp-sim insert is unavailable in this build.
  if (sonare::mastering::api::make_insert("saturation.ampSim", "{}") == nullptr) return;

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
#endif  // SONARE_MIDI_WITH_FX && SONARE_WITH_MASTERING

TEST_CASE("a GS EFX parameter-only change updates the insert in place", "[midi][sf2][gsefx]") {
  auto counters = std::make_shared<EfxCounters>();
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.insert_factory = [counters](std::string_view, std::string_view) {
    return std::unique_ptr<sonare::rt::ProcessorBase>(new CountingInsert(counters));
  };
  Sf2Player player(cfg);
  player.prepare(kOutRate, 256);

  // Route part 1 (channel 0) through an EFX and select Overdrive: one insert is
  // built and prepared.
  player.on_control_sysex(kPartOn, sizeof(kPartOn));
  player.on_control_sysex(kOdType, sizeof(kOdType));
  REQUIRE(counters->prepares == 1);
  const int prepares_after_build = counters->prepares;
  const int set_params_before = counters->set_params;

  // A parameter-only edit (OD Drive, 40 03 04) must NOT rebuild the chain: it is
  // resolved on the control thread and applied to the live processor on the
  // audio thread at the next block. on_control_sysex enqueues but does not touch
  // the processor; the set_parameter lands during the following process() call.
  player.on_control_sysex(kOdDrive, sizeof(kOdDrive));
  REQUIRE(counters->prepares == prepares_after_build);  // not rebuilt
  REQUIRE(counters->set_params == set_params_before);   // not applied synchronously

  std::vector<float> left(256, 0.0f);
  std::vector<float> right(256, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  player.process(chans, 2, 256);  // audio thread drains the queue and applies it

  REQUIRE(counters->prepares == prepares_after_build);  // still no rebuild
  REQUIRE(counters->resets == 0);                       // DSP state preserved
  REQUIRE(counters->set_params > set_params_before);    // applied on the audio thread

  // A genuine EFX TYPE change still triggers a full rebuild (a fresh processor,
  // hence another prepare()): exact behaviour is preserved when it is needed.
  player.on_control_sysex(kChorusType, sizeof(kChorusType));
  REQUIRE(counters->prepares > prepares_after_build);
}
