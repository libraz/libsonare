/// @file gs_mod_tvf_lfo_test.cpp
/// @brief GS LFO1 TVF DEPTH (40 2x 05): that a controller's filter swing is a
///        wobble and not a shift.
///
/// This destination and TVF CUTOFF CONTROL two addresses below it both end at
/// the same cutoff, and a byte wired to the wrong one of them would still change
/// the render, still move in a direction, and still scale with the controller.
/// What separates them is time: an offset moves the filter and leaves it there,
/// a depth keeps moving it. So the measurement is not brightness but the
/// VARIATION of brightness over a block track — high for a swing, flat for an
/// offset of the same size.
///
/// Both voice banks are exercised. The LFO is the shared one, so the swing rides
/// whatever 40 2x 03 set the rate to; the rate is left alone here and the depth
/// is the only thing varied.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/sf2_builder.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;

constexpr uint8_t kChannel = 0;
/// Part 1 = channel 0 = block nibble 1, in the controller-destination block.
constexpr uint8_t kDestBlock = 0x21;
constexpr uint8_t kTvfCutoff = 0x01;
constexpr uint8_t kLfo1TvfDepth = 0x05;
constexpr uint8_t kModWheel = 1;

enum class Bank : uint8_t { kSoundFont, kModel };

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

std::vector<uint8_t> dt1(uint8_t lo, uint8_t value) {
  std::vector<uint8_t> msg{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, kDestBlock, lo, value};
  const int sum = 0x40 + kDestBlock + lo + value;
  msg.push_back(static_cast<uint8_t>((128 - (sum % 128)) & 0x7F));
  msg.push_back(0xF7);
  return msg;
}

/// A sawtooth, so a moving low-pass has partials to take away and give back.
std::shared_ptr<Sf2File> make_fixture() {
  Sf2Builder b;
  std::vector<float> saw(128);
  for (size_t i = 0; i < saw.size(); ++i) {
    const double phase = static_cast<double>(i % 32) / 32.0;
    saw[i] = static_cast<float>(0.5 * (2.0 * phase - 1.0));
  }
  const int saw_id = b.add_sample("saw", saw, 32000, 60, 32, 128);

  Sf2Builder::ZoneSpec zone;
  zone.gens.push_back({54 /*sampleModes*/, 1});
  zone.target = saw_id;
  const int inst = b.add_instrument("sawinst", {zone});

  Sf2Builder::ZoneSpec pz;
  pz.target = inst;
  b.add_preset("Saw", 0, 0, {pz});

  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

using Render = std::vector<float>;
using Writes = std::function<void(Sf2Player&)>;

constexpr int kFrames = 48000;
/// 5 ms: a twenty-fourth of the SoundFont LFO's period and a fortieth of the
/// model bank's, so a block sits on the swing rather than averaging it away.
constexpr size_t kBlock = 240;

Render render(Bank bank, const Writes& before) {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  cfg.synth_fallback = bank == Bank::kModel;
#if defined(SONARE_MIDI_WITH_FX)
  cfg.effects.enable_reverb = false;
  cfg.effects.enable_chorus = false;
  cfg.effects.enable_delay = false;
#endif
  Sf2Player player(cfg);
  if (bank == Bank::kSoundFont) player.set_soundfont(make_fixture());
  player.prepare(kOutRate, 256);
  if (before) before(player);

  std::vector<float> left(static_cast<size_t>(kFrames), 0.0f);
  std::vector<float> right(left.size(), 0.0f);
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, kChannel, 60, 100)));
  float* out[2] = {left.data(), right.data()};
  player.process(out, 2, kFrames);
  return left;
}

/// Per-block spectral tilt: the first difference's energy over the signal's own.
/// A block the filter has closed on reads lower than one it has opened on.
std::vector<double> tilt_track(const Render& r) {
  std::vector<double> track;
  for (size_t w = kBlock; w + kBlock <= r.size(); w += kBlock) {
    double num = 0.0;
    double den = 0.0;
    for (size_t i = w; i < w + kBlock; ++i) {
      const double d = static_cast<double>(r[i]) - static_cast<double>(r[i - 1]);
      num += d * d;
      den += static_cast<double>(r[i]) * static_cast<double>(r[i]);
    }
    if (den > 0.0) track.push_back(num / den);
  }
  return track;
}

/// How much the tilt moves, relative to where it sits: a swing's signature, and
/// the one number an offset of the same size cannot produce. Relative rather
/// than absolute because a darker render has a smaller tilt to vary.
double tilt_variation(const Render& r) {
  const std::vector<double> track = tilt_track(r);
  if (track.size() < 4) return 0.0;
  double mean = 0.0;
  for (const double v : track) mean += v;
  mean /= static_cast<double>(track.size());
  if (mean <= 0.0) return 0.0;
  double var = 0.0;
  for (const double v : track) var += (v - mean) * (v - mean);
  return std::sqrt(var / static_cast<double>(track.size())) / mean;
}

double mean_tilt(const Render& r) {
  const std::vector<double> track = tilt_track(r);
  if (track.empty()) return 0.0;
  double mean = 0.0;
  for (const double v : track) mean += v;
  return mean / static_cast<double>(track.size());
}

void write_dest(Sf2Player& p, uint8_t dest, uint8_t value) {
  const std::vector<uint8_t> m = dt1(dest, value);
  p.handle_sysex(m.data(), m.size());
}

void wheel(Sf2Player& p, uint8_t value) {
  p.on_event(0, event(sonare::midi::make_midi1_control_change(0, kChannel, kModWheel, value)));
}

}  // namespace

TEST_CASE("40 2x 05 swings the filter where 40 2x 01 moves it", "[midi][synth][gs]") {
  // The two are compared at comparable magnitudes — 32 steps of offset is 4800
  // cents, the depth's own full scale is 2400 either way — so what separates
  // them is the shape of the result and not its size.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Render shifted = render(bank, [](Sf2Player& p) {
      write_dest(p, kTvfCutoff, 0x20);
      wheel(p, 127);
    });
    const Render swung = render(bank, [](Sf2Player& p) {
      write_dest(p, kLfo1TvfDepth, 0x7F);
      wheel(p, 127);
    });
    INFO("variation shifted " << tilt_variation(shifted) << " vs swung " << tilt_variation(swung));
    INFO("mean tilt shifted " << mean_tilt(shifted) << " vs swung " << mean_tilt(swung));
    // The offset darkens and stays there; the depth keeps moving. The factor is
    // asked for on the SoundFont bank only, where a held sawtooth's tilt is
    // constant unless something moves it: the model bank's piano decays through
    // its own dynamics, which puts a floor under the variation that has nothing
    // to do with the filter, so what it can say is the ordering. The depth's own
    // scale is checked on both banks by the case below.
    if (bank == Bank::kSoundFont) {
      CHECK(tilt_variation(swung) > 4.0 * tilt_variation(shifted));
    } else {
      CHECK(tilt_variation(swung) > tilt_variation(shifted));
    }
    // Non-vacuous in the other direction: the offset did reach the filter, or
    // the comparison is against a render nothing happened to.
    CHECK(mean_tilt(shifted) < mean_tilt(swung));
  }
}

TEST_CASE("40 2x 05 swings further the further its byte goes", "[midi][synth][gs]") {
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    auto at = [bank](uint8_t depth) {
      return tilt_variation(render(bank, [depth](Sf2Player& p) {
        write_dest(p, kLfo1TvfDepth, depth);
        wheel(p, 127);
      }));
    };
    // From 40 rather than from 00: the depth's zero is the one value that leaves
    // the part's filter disengaged, so a track taken there is measuring the
    // engagement rather than the depth.
    const double small = at(0x40);
    const double full = at(0x7F);
    INFO("variation 40 " << small << " / 7F " << full);
    CHECK(full > small);
  }
}

TEST_CASE("40 2x 05 is worth nothing while the mod wheel is down", "[midi][synth][gs]") {
  // A depth: it says what a raised controller is worth. Compared between two
  // engaged values rather than against an untouched part, because writing the
  // address at all engages the part's filter and that is audible on its own.
  for (const Bank bank : {Bank::kSoundFont, Bank::kModel}) {
    const std::string tag = bank == Bank::kSoundFont ? "soundfont" : "model";
    INFO("bank: " << tag);

    const Render small = render(bank, [](Sf2Player& p) { write_dest(p, kLfo1TvfDepth, 0x40); });
    const Render full = render(bank, [](Sf2Player& p) { write_dest(p, kLfo1TvfDepth, 0x7F); });
    CHECK(small == full);
  }
}
