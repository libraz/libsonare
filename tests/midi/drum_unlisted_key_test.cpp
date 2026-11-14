/// @file drum_unlisted_key_test.cpp
/// @brief A GS kit is realised with its own listing or falls back to Standard whole.
///
/// The expected listing is `gs_drum_unlisted_keys.tsv`, read from the owner's
/// manual drum set tables: a key printed "---" or not printed is silent on that
/// kit under that map. The drift case walks every (kit, map, key) against it; the
/// note-on cases pin the boundary from both sides at both voice banks.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/gs_layer.h"
#include "midi/synth/native_synth.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/midi_render.h"
#include "support/sf2_builder.h"

namespace {

using sonare::midi::synth::gm_fallback_drum_key_is_silent;
using sonare::midi::synth::GsToneMap;
using sonare::midi::synth::kGsDrumKits;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::event;

constexpr double kRate = 48000.0;
constexpr uint8_t kDrumChannel = 9;
constexpr uint8_t kUnlistedStandardKey = 90;  ///< "---" on Standard under the SC-8850 map.
constexpr uint8_t kRhythmFxProgram = 57;      ///< Not voiced apart: falls back whole.
constexpr uint8_t kAcousticBassDrum = 35;     ///< "---" on Rhythm FX's own listing.

using Listing = std::map<std::pair<int, GsToneMap>, std::vector<bool>>;

GsToneMap map_named(const std::string& name) {
  if (name == "sc55") return GsToneMap::kSc55;
  if (name == "sc88") return GsToneMap::kSc88;
  if (name == "sc88pro") return GsToneMap::kSc88Pro;
  REQUIRE(name == "sc8850");
  return GsToneMap::kSc8850;
}

Listing load_listing() {
  std::ifstream in("tests/midi/gs_drum_unlisted_keys.tsv");
  REQUIRE(in.good());
  Listing out;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream row(line);
    int kit = 0;
    std::string map_name;
    std::string ranges;
    row >> kit >> map_name >> ranges;
    std::vector<bool> silent(128, false);
    if (ranges != "-") {
      std::istringstream rs(ranges);
      std::string range;
      while (std::getline(rs, range, ',')) {
        const size_t dash = range.find('-');
        const int first = std::stoi(range.substr(0, dash));
        const int last = dash == std::string::npos ? first : std::stoi(range.substr(dash + 1));
        for (int k = first; k <= last; ++k) silent[static_cast<size_t>(k)] = true;
      }
    }
    out[{kit, map_named(map_name)}] = silent;
  }
  return out;
}

int voices_after(uint8_t program, GsToneMap map, uint8_t note) {
  NativeSynthConfig cfg;
  cfg.use_gm_programs = true;
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, kDrumChannel, 0, 0)));
  synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, kDrumChannel, 32,
                                                                  static_cast<uint8_t>(map))));
  synth.on_event(0, event(sonare::midi::make_midi1_program_change(0, kDrumChannel, program)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, kDrumChannel, note, 100)));
  sonare::test::render_stereo(synth, 256);
  return synth.active_voice_count();
}

/// Bank 128 with one zone on key 44 only, so every other key reaches the model floor.
std::shared_ptr<Sf2File> one_key_kit() {
  sonare::test::Sf2Builder b;
  const int sample = b.add_sample("s", std::vector<float>(128, 0.25f), 32000, 60, 0, 128);
  sonare::test::Sf2Builder::ZoneSpec zone;
  zone.key_lo = 44;
  zone.key_hi = 44;
  zone.target = sample;
  sonare::test::Sf2Builder::ZoneSpec pz;
  pz.target = b.add_instrument("i", {zone});
  b.add_preset("Kit", 128, 0, {pz});
  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

int floor_voices_after(uint8_t program, uint8_t note) {
  Sf2PlayerConfig cfg;
  cfg.synth_fallback = true;
  Sf2Player player(cfg);
  player.set_soundfont(one_key_kit());
  player.prepare(kRate, 256);
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, kDrumChannel, program)));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, kDrumChannel, note, 100)));
  std::vector<float> l(256), r(256);
  float* chans[2] = {l.data(), r.data()};
  player.process(chans, 2, 256);
  return player.active_voice_count();
}

}  // namespace

TEST_CASE("A key a realised kit's listing prints as no sound starts no voice", "[midi][drums]") {
  CHECK(voices_after(0, GsToneMap::kSc8850, kUnlistedStandardKey) == 0);
  CHECK(floor_voices_after(0, kUnlistedStandardKey) == 0);
}

TEST_CASE("A kit not yet voiced falls back to Standard whole", "[midi][drums]") {
  // Rhythm FX prints no sound on 35, but it is not realised, so 35 plays Standard's piece.
  REQUIRE_FALSE(sonare::midi::synth::gs_drum_kit_is_voiced_apart(
      sonare::midi::synth::gm_fallback_drum_kit(kRhythmFxProgram, GsToneMap::kSc8850)));
  CHECK(voices_after(kRhythmFxProgram, GsToneMap::kSc8850, kAcousticBassDrum) == 1);
  CHECK(floor_voices_after(kRhythmFxProgram, kAcousticBassDrum) == 1);
}

TEST_CASE("The silent keys of every kit and map are the manual's listing", "[midi][drums]") {
  const Listing listing = load_listing();
  size_t reachable = 0;
  for (const auto& kit : kGsDrumKits) {
    for (const GsToneMap map :
         {GsToneMap::kSc55, GsToneMap::kSc88, GsToneMap::kSc88Pro, GsToneMap::kSc8850}) {
      if (!sonare::midi::synth::gs_map_reaches(map, kit.since)) continue;
      ++reachable;
      const auto it = listing.find({kit.index, map});
      REQUIRE(it != listing.end());
      for (int key = 0; key < 128; ++key) {
        CAPTURE(kit.name, static_cast<int>(map), key);
        const auto note = static_cast<uint8_t>(key);
        CHECK(gm_fallback_drum_key_is_silent(kit.index, map, note) ==
              it->second[static_cast<size_t>(key)]);
        if (map == GsToneMap::kSc8850) {
          CHECK(gm_fallback_drum_key_is_silent(kit.index, GsToneMap::kModuleDefault, note) ==
                it->second[static_cast<size_t>(key)]);
        }
      }
    }
  }
  CHECK(listing.size() == reachable);
}

TEST_CASE("Every kit still sounds a key its listing prints", "[midi][drums]") {
  const Listing listing = load_listing();
  for (const auto& kit : kGsDrumKits) {
    const std::vector<bool>& silent = listing.at({kit.index, kit.since});
    int key = 35;
    while (key < 82 && silent[static_cast<size_t>(key)]) ++key;
    CAPTURE(kit.name, key);
    REQUIRE(key < 82);
    CHECK(voices_after(kit.program, kit.since, static_cast<uint8_t>(key)) == 1);
  }
}
