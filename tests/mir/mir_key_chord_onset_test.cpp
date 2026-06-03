/// @file mir_key_chord_onset_test.cpp
/// @brief MIR key/chord/onset bridge: HarmonicTimeline construction from
///        offline analysis, quality/mode mapping, modulation boundaries, pitch-
///        correction target derivation, and onset -> split-candidate conversion.
///
/// These are control-plane, offline checks. The bridge never mutates a Project
/// and never auto-splits; it only PRODUCES a thin HarmonicTimeline and split
/// CANDIDATES. All functions are deterministic given their inputs.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>

#include "arrangement/harmonic_timeline.h"
#include "mir/key_context.h"
#include "transport/tempo_map.h"

namespace {

using sonare::Chord;
using sonare::ChordQuality;
using sonare::Key;
using sonare::Mode;
using sonare::Onset;
using sonare::PitchClass;
using sonare::arrangement::HarmonicTimeline;
using sonare::mir::HarmonicAnalysisInput;
using sonare::mir::OnsetSplitConfig;
using sonare::transport::TempoMap;
using sonare::transport::TempoSegment;

// Prepares `map` as a 120-BPM map: 1 quarter note (1 PPQ unit) lasts 0.5 s, so
// the PPQ value at t seconds is t / 0.5 = 2 * t (deterministic, no ramp).
// TempoMap is non-copyable (holds an RtSnapshot), so it is filled in place.
void prepare_120bpm_map(TempoMap& map, double sr = 22050.0) {
  map.prepare(sr);
  TempoSegment seg;
  seg.start_ppq = 0.0;
  seg.bpm = 120.0;
  seg.start_sample = 0.0;
  map.set_segments({seg});
}

Chord make_chord(PitchClass root, ChordQuality quality, float start, float end,
                 PitchClass bass = PitchClass::C) {
  Chord c;
  c.root = root;
  c.quality = quality;
  c.start = start;
  c.end = end;
  c.confidence = 1.0f;
  c.bass = bass;
  return c;
}

bool has_pc(const std::vector<uint8_t>& v, uint8_t pc) {
  return std::find(v.begin(), v.end(), pc) != v.end();
}

}  // namespace

TEST_CASE("map_chord_quality folds detector qualities onto symbol granularity", "[mir]") {
  // Major -> kMajor (no extensions).
  auto maj = sonare::mir::map_chord_quality(sonare::ChordQuality::Major);
  CHECK(maj.quality == sonare::arrangement::ChordQuality::kMajor);
  CHECK(maj.extensions.empty());

  // Dominant7 -> kDominant with a 7 extension.
  auto dom7 = sonare::mir::map_chord_quality(sonare::ChordQuality::Dominant7);
  CHECK(dom7.quality == sonare::arrangement::ChordQuality::kDominant);
  CHECK(dom7.extensions == std::vector<uint8_t>{7});

  // Dominant9 -> kDominant with {7, 9}.
  auto dom9 = sonare::mir::map_chord_quality(sonare::ChordQuality::Dominant9);
  CHECK(dom9.quality == sonare::arrangement::ChordQuality::kDominant);
  CHECK(dom9.extensions == std::vector<uint8_t>{7, 9});

  // HalfDim7 -> kHalfDiminished.
  CHECK(sonare::mir::map_chord_quality(sonare::ChordQuality::HalfDim7).quality ==
        sonare::arrangement::ChordQuality::kHalfDiminished);

  // Unknown stays unknown.
  CHECK(sonare::mir::map_chord_quality(sonare::ChordQuality::Unknown).quality ==
        sonare::arrangement::ChordQuality::kUnknown);
}

TEST_CASE("map_key_mode maps every mode", "[mir]") {
  CHECK(sonare::mir::map_key_mode(Mode::Major) == sonare::arrangement::KeyMode::kMajor);
  CHECK(sonare::mir::map_key_mode(Mode::Minor) == sonare::arrangement::KeyMode::kMinor);
  CHECK(sonare::mir::map_key_mode(Mode::Dorian) == sonare::arrangement::KeyMode::kDorian);
  CHECK(sonare::mir::map_key_mode(Mode::Locrian) == sonare::arrangement::KeyMode::kLocrian);
}

TEST_CASE("build_harmonic_timeline emits chords at symbol granularity and PPQ", "[mir]") {
  TempoMap map;
  prepare_120bpm_map(map);

  HarmonicAnalysisInput in;
  // C major (0..1s) then G dominant7 (1..2s). At 120 BPM, 1s -> 2 PPQ.
  in.chords.push_back(make_chord(PitchClass::C, ChordQuality::Major, 0.0f, 1.0f));
  in.chords.push_back(make_chord(PitchClass::G, ChordQuality::Dominant7, 1.0f, 2.0f));
  in.has_global_key = true;
  in.global_key = Key{PitchClass::C, Mode::Major, 1.0f};

  const HarmonicTimeline tl = sonare::mir::build_harmonic_timeline(in, map);

  REQUIRE(tl.chords.size() == 2);
  CHECK(tl.chords[0].root_pc == static_cast<uint8_t>(PitchClass::C));
  CHECK(tl.chords[0].quality == sonare::arrangement::ChordQuality::kMajor);
  CHECK(tl.chords[0].start_ppq == 0.0);
  CHECK(tl.chords[0].end_ppq == 2.0);

  CHECK(tl.chords[1].root_pc == static_cast<uint8_t>(PitchClass::G));
  CHECK(tl.chords[1].quality == sonare::arrangement::ChordQuality::kDominant);
  CHECK(tl.chords[1].extensions == std::vector<uint8_t>{7});
  CHECK(tl.chords[1].start_ppq == 2.0);
  CHECK(tl.chords[1].end_ppq == 4.0);

  // A single global key spans the whole progression.
  REQUIRE(tl.keys.size() == 1);
  CHECK(tl.keys[0].tonic_pc == static_cast<uint8_t>(PitchClass::C));
  CHECK(tl.keys[0].mode == sonare::arrangement::KeyMode::kMajor);
  CHECK(tl.keys[0].start_ppq == 0.0);
  CHECK(tl.keys[0].end_ppq == 4.0);
}

TEST_CASE("build_harmonic_timeline marks modulation boundary on key change", "[mir]") {
  TempoMap map;
  prepare_120bpm_map(map);

  HarmonicAnalysisInput in;
  // Two chords; key changes from C major to D major at the second chord.
  in.chords.push_back(make_chord(PitchClass::C, ChordQuality::Major, 0.0f, 1.0f));
  in.chords.push_back(make_chord(PitchClass::D, ChordQuality::Major, 1.0f, 2.0f));
  in.keys = {Key{PitchClass::C, Mode::Major, 1.0f}, Key{PitchClass::D, Mode::Major, 1.0f}};
  in.key_start_times = {0.0f, 1.0f};

  const HarmonicTimeline tl = sonare::mir::build_harmonic_timeline(in, map);

  REQUIRE(tl.keys.size() == 2);
  REQUIRE(tl.chords.size() == 2);
  CHECK_FALSE(tl.chords[0].modulation_boundary);
  CHECK(tl.chords[1].modulation_boundary);
}

TEST_CASE("build_harmonic_timeline marks modulation boundary on mode change", "[mir]") {
  TempoMap map;
  prepare_120bpm_map(map);

  HarmonicAnalysisInput in;
  in.chords.push_back(make_chord(PitchClass::C, ChordQuality::Major, 0.0f, 1.0f));
  in.chords.push_back(make_chord(PitchClass::C, ChordQuality::Minor, 1.0f, 2.0f));
  in.keys = {Key{PitchClass::C, Mode::Major, 1.0f}, Key{PitchClass::C, Mode::Minor, 1.0f}};
  in.key_start_times = {0.0f, 1.0f};

  const HarmonicTimeline tl = sonare::mir::build_harmonic_timeline(in, map);

  REQUIRE(tl.keys.size() == 2);
  REQUIRE(tl.chords.size() == 2);
  CHECK_FALSE(tl.chords[0].modulation_boundary);
  CHECK(tl.chords[1].modulation_boundary);
}

TEST_CASE("build_harmonic_timeline is deterministic", "[mir]") {
  TempoMap map;
  prepare_120bpm_map(map);
  HarmonicAnalysisInput in;
  in.chords.push_back(make_chord(PitchClass::A, ChordQuality::Minor, 0.0f, 0.75f));
  in.chords.push_back(make_chord(PitchClass::F, ChordQuality::Major7, 0.75f, 1.5f));
  in.has_global_key = true;
  in.global_key = Key{PitchClass::A, Mode::Minor, 1.0f};

  const HarmonicTimeline a = sonare::mir::build_harmonic_timeline(in, map);
  const HarmonicTimeline b = sonare::mir::build_harmonic_timeline(in, map);

  REQUIRE(a.chords.size() == b.chords.size());
  for (size_t i = 0; i < a.chords.size(); ++i) {
    CHECK(a.chords[i].start_ppq == b.chords[i].start_ppq);
    CHECK(a.chords[i].end_ppq == b.chords[i].end_ppq);
    CHECK(a.chords[i].root_pc == b.chords[i].root_pc);
    CHECK(a.chords[i].quality == b.chords[i].quality);
    CHECK(a.chords[i].extensions == b.chords[i].extensions);
  }
}

TEST_CASE("derive_pitch_correction_target returns chord + scale tones", "[mir]") {
  HarmonicTimeline tl;
  // C major key spanning [0, 8).
  sonare::arrangement::KeySegment key;
  key.start_ppq = 0.0;
  key.end_ppq = 8.0;
  key.tonic_pc = static_cast<uint8_t>(PitchClass::C);
  key.mode = sonare::arrangement::KeyMode::kMajor;
  tl.keys.push_back(key);
  // G dominant chord over [0, 4) -> tones G B D F (7,11,2,5).
  sonare::arrangement::ChordSymbol chord;
  chord.start_ppq = 0.0;
  chord.end_ppq = 4.0;
  chord.root_pc = static_cast<uint8_t>(PitchClass::G);
  chord.quality = sonare::arrangement::ChordQuality::kDominant;
  tl.chords.push_back(chord);

  const auto target = sonare::mir::derive_pitch_correction_target(tl, 1.0);

  // Chord tones for G7: G(7) B(11) D(2) F(5).
  CHECK(has_pc(target.chord_tones, 7));
  CHECK(has_pc(target.chord_tones, 11));
  CHECK(has_pc(target.chord_tones, 2));
  CHECK(has_pc(target.chord_tones, 5));
  // Sorted ascending.
  CHECK(std::is_sorted(target.chord_tones.begin(), target.chord_tones.end()));

  // C major scale: C D E F G A B = {0,2,4,5,7,9,11}.
  CHECK(target.scale_tones == std::vector<uint8_t>{0, 2, 4, 5, 7, 9, 11});
}

TEST_CASE("derive_pitch_correction_target maps major seventh to semitone 11", "[mir]") {
  HarmonicTimeline tl;
  sonare::arrangement::ChordSymbol chord;
  chord.start_ppq = 0.0;
  chord.end_ppq = 4.0;
  chord.root_pc = static_cast<uint8_t>(PitchClass::C);
  chord.quality = sonare::arrangement::ChordQuality::kMajor;
  chord.extensions = {7};
  tl.chords.push_back(chord);

  const auto target = sonare::mir::derive_pitch_correction_target(tl, 1.0);

  CHECK(has_pc(target.chord_tones, 11));
  CHECK_FALSE(has_pc(target.chord_tones, 10));
  CHECK(target.chord_tones == std::vector<uint8_t>{0, 4, 7, 11});
}

TEST_CASE("derive_pitch_correction_target falls back to chord tones with no key", "[mir]") {
  HarmonicTimeline tl;
  sonare::arrangement::ChordSymbol chord;
  chord.start_ppq = 0.0;
  chord.end_ppq = 4.0;
  chord.root_pc = static_cast<uint8_t>(PitchClass::C);
  chord.quality = sonare::arrangement::ChordQuality::kMajor;
  tl.chords.push_back(chord);

  const auto target = sonare::mir::derive_pitch_correction_target(tl, 1.0);
  // C major triad: C E G = {0,4,7}.
  CHECK(target.chord_tones == std::vector<uint8_t>{0, 4, 7});
  // No key -> scale falls back to chord tones.
  CHECK(target.scale_tones == target.chord_tones);
}

TEST_CASE("derive_pitch_correction_target is empty outside all segments", "[mir]") {
  HarmonicTimeline tl;
  const auto target = sonare::mir::derive_pitch_correction_target(tl, 5.0);
  CHECK(target.chord_tones.empty());
  CHECK(target.scale_tones.empty());
}

TEST_CASE("onset_split_candidates keeps onsets strictly inside the clip span", "[mir]") {
  TempoMap map;
  prepare_120bpm_map(map);
  // At 120 BPM, t seconds -> 2t PPQ.
  std::vector<Onset> onsets = {
      Onset{0.0f, 1.0f},  // ppq 0   -> on clip start, rejected
      Onset{0.5f, 1.0f},  // ppq 1   -> inside, kept
      Onset{1.0f, 1.0f},  // ppq 2   -> inside, kept
      Onset{2.0f, 1.0f},  // ppq 4   -> on clip end, rejected
  };

  const auto cands = sonare::mir::onset_split_candidates(onsets, map, /*clip_start*/ 0.0,
                                                         /*clip_end*/ 4.0);
  REQUIRE(cands.size() == 2);
  CHECK(cands[0].ppq == 1.0);
  CHECK(cands[1].ppq == 2.0);
  // Ascending.
  CHECK(cands[0].ppq < cands[1].ppq);
}

TEST_CASE("onset_split_candidates honors strength, spacing and edge guard", "[mir]") {
  TempoMap map;
  prepare_120bpm_map(map);
  std::vector<Onset> onsets = {
      Onset{0.5f, 0.2f},   // ppq 1, weak
      Onset{0.75f, 1.0f},  // ppq 1.5
      Onset{0.8f, 1.0f},   // ppq 1.6, too close to previous
      Onset{1.5f, 1.0f},   // ppq 3.0
  };

  OnsetSplitConfig cfg;
  cfg.min_strength = 0.5f;    // drops the weak onset at ppq 1
  cfg.min_spacing_ppq = 1.0;  // drops ppq 1.6 (only 0.1 after 1.5)
  cfg.edge_guard_ppq = 0.25;  // span becomes (0.25, 3.75)

  const auto cands = sonare::mir::onset_split_candidates(onsets, map, 0.0, 4.0, cfg);
  REQUIRE(cands.size() == 2);
  // PPQ is derived through a seconds->sample->PPQ tempo conversion, so compare
  // within hop-resolution tolerance rather than exact equality (the +-50 ms
  // F-measure window is the real acceptance gate).
  CHECK_THAT(cands[0].ppq, Catch::Matchers::WithinAbs(1.5, 1e-2));
  CHECK_THAT(cands[1].ppq, Catch::Matchers::WithinAbs(3.0, 1e-2));
}

TEST_CASE("onset_split_candidates returns empty for a degenerate clip span", "[mir]") {
  TempoMap map;
  prepare_120bpm_map(map);
  std::vector<Onset> onsets = {Onset{0.5f, 1.0f}};
  const auto cands = sonare::mir::onset_split_candidates(onsets, map, 4.0, 4.0);
  CHECK(cands.empty());
}
