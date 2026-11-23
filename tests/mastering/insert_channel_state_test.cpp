/// @file insert_channel_state_test.cpp
/// @brief Every realtime insert sizes its per-channel state for the channels it is prepared for.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

#include "mastering/api/insert_factory.h"
#include "rt/processor_base.h"
#include "support/alloc_guard.h"

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 512;
constexpr int kWide = 64;
// Repeated builds of one insert vary by up to ~160 bytes; the smallest real per-channel growth
// measured (repair.decrackle, 1 to 64 channels) is 480.
constexpr long long kNoiseBytes = 256;

/// Inserts whose held bytes do not depend on the prepared channel count, each with the reason.
const std::map<std::string, std::string>& channel_free_inserts() {
  static const std::string kLinked = "linked detection: one follower drives every channel";
  static const std::string kStereo = "two channels by construction";
  static const std::map<std::string, std::string> inserts = {
      {"dynamics.expander", kLinked},
      {"dynamics.upwardCompressor", kLinked},
      {"dynamics.upwardExpander", kLinked},
      {"eq.midSide", "two channels by construction; each child filters one mono stream"},
      {"stereo.autoPan", kStereo},
      {"stereo.binaural", kStereo},
      {"stereo.haasEnhancer", kStereo},
      {"stereo.imager", kStereo},
      {"stereo.monoMaker", kStereo},
      {"stereo.phaseAlign", kStereo},
      {"stereo.stereoBalance", kStereo},
      {"utility.gain", "holds no state"},
      {"effects.gsEfx", kStereo},
      {"effects.reverb.plate", kStereo},
      {"effects.reverb.fdn", kStereo},
      {"effects.reverb.velvet", kStereo},
      {"effects.reverb.convolution", kStereo},
      {"effects.reverb.room", kStereo},
      {"effects.acoustic.roomMorph", kStereo},
      {"effects.filter.vowel", kStereo},
      {"effects.modulation.chorus", kStereo},
      {"effects.modulation.ensemble", kStereo},
      {"effects.modulation.flanger", kStereo},
      {"effects.modulation.phaser", kStereo},
      {"effects.modulation.wah", kStereo},
      {"effects.modulation.autoWah", kStereo},
      {"effects.modulation.rotary", kStereo},
      {"effects.modulation.ringModulator", "one carrier drives every channel"},
      {"effects.modulation.pitchShifter", kStereo},
      {"effects.delay.stereo", kStereo},
  };
  return inserts;
}

/// Inserts that refuse a prepare wider than their own ceiling are measured at that ceiling.
int widest_channels(const std::string& name) {
  static const std::map<std::string, int> ceilings = {{"effects.gsEfx", 2}, {"voice.changer", 2}};
  const auto it = ceilings.find(name);
  return it == ceilings.end() ? kWide : it->second;
}

std::unique_ptr<sonare::rt::ProcessorBase> build(const std::string& name) {
  return sonare::mastering::api::make_insert(
      name, "{}", nullptr, sonare::resource::kDefaultProjectImportResourceLimits, kSampleRate);
}

/// Least bytes a fresh insert holds once built and prepared for @p channels, over three builds.
long long held_after_prepare(const std::string& name, int channels) {
  long long least = -1;
  for (int round = 0; round < 3; ++round) {
    sonare::test::PeakLiveBytesGuard guard;
    std::unique_ptr<sonare::rt::ProcessorBase> insert = build(name);
    REQUIRE(insert != nullptr);
    insert->prepare(kSampleRate, kBlock, channels);
    const long long held = sonare::test::g_live_bytes.load();
    least = least < 0 ? held : std::min(least, held);
  }
  return least;
}

}  // namespace

TEST_CASE("every insert holds more state prepared for more channels", "[mastering][insert]") {
  const auto& names = sonare::mastering::api::insert_factory_names();
  for (const auto& [name, reason] : channel_free_inserts()) {
    INFO(name);
    CHECK(std::find(names.begin(), names.end(), name) != names.end());
  }
  for (const std::string& name : names) {
    // A first build settles one-time statics, so they land on neither measured instance.
    held_after_prepare(name, 1);
    const int wide_channels = widest_channels(name);
    const long long narrow = held_after_prepare(name, 1);
    const long long wide = held_after_prepare(name, wide_channels);
    INFO(name << ": " << narrow << " bytes at 1 channel, " << wide << " at " << wide_channels);
    const auto exception = channel_free_inserts().find(name);
    if (exception == channel_free_inserts().end()) {
      CHECK(wide - narrow > kNoiseBytes);
    } else {
      INFO("listed as channel-free: " << exception->second);
      CHECK(std::abs(wide - narrow) <= kNoiseBytes);
    }
  }
}
