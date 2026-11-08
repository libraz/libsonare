/// @file chord_beat_span_test.cpp
/// @brief Tests for beat-span chroma pooling and the default constant-Q chord front end.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <vector>

#include "analysis/chord_analyzer.h"
#include "analysis/chord_templates.h"
#include "analysis/key_analyzer.h"
#include "util/constants.h"

using namespace sonare;

namespace {

/// @brief Adds a note with four 1/k partials under @p envelope, ramped in and out over 5 ms.
template <typename Envelope>
void add_note(std::vector<float>& samples, int sr, float midi, double start, double end,
              Envelope envelope) {
  const double f0 =
      sonare::constants::kA4Hz *
      std::pow(2.0, (midi - sonare::constants::kMidiA4) / sonare::constants::kSemitonesPerOctave);
  constexpr double kRamp = 0.005;
  const size_t first = static_cast<size_t>(std::max(0.0, start) * sr);
  const size_t last = std::min(samples.size(), static_cast<size_t>(end * sr));
  for (size_t i = first; i < last; ++i) {
    const double t = static_cast<double>(i) / sr;
    const double gate = std::min({1.0, (t - start) / kRamp, (end - t) / kRamp});
    double v = 0.0;
    for (int k = 1; k <= 4; ++k) v += std::sin(sonare::constants::kTwoPiD * f0 * k * t) / k;
    samples[i] += static_cast<float>(0.08 * std::max(0.0, gate) * envelope(t - start) * v);
  }
}

const std::vector<float> kCMajor = {36.0f, 60.0f, 64.0f, 67.0f};  // C2 | C4 E4 G4
const std::vector<float> kGMajor = {43.0f, 55.0f, 59.0f, 62.0f};  // G2 | G3 B3 D4
const std::vector<float> kAMinor = {45.0f, 57.0f, 60.0f, 64.0f};  // A2 | A3 C4 E4
const std::vector<float> kFMajor = {41.0f, 53.0f, 57.0f, 60.0f};  // F2 | F3 A3 C4

/// @brief I-V-vi-IV in C, 2 s per chord, each chord struck on every 0.5 s beat.
Audio pop_progression(int sr) {
  const std::vector<std::vector<float>> voicings = {kCMajor, kGMajor, kAMinor, kFMajor};
  std::vector<float> samples(static_cast<size_t>(8.0 * sr), 0.0f);
  const auto decay = [](double t) { return std::exp(-2.0 * t); };
  for (size_t c = 0; c < voicings.size(); ++c) {
    for (int strike = 0; strike < 4; ++strike) {
      const double on = 2.0 * static_cast<double>(c) + 0.5 * strike;
      for (float midi : voicings[c]) add_note(samples, sr, midi, on, on + 0.5, decay);
    }
  }
  return Audio::from_vector(std::move(samples), sr);
}

std::vector<std::string> names_of(const std::vector<Chord>& chords) {
  std::vector<std::string> names;
  for (const Chord& chord : chords) names.push_back(chord.to_string());
  return names;
}

}  // namespace

TEST_CASE("beat-synchronised chords pool chroma over the whole beat span", "[chord][beat_span]") {
  constexpr int kSr = 22050;
  constexpr double kDuration = 12.0;
  constexpr float kPeriod = 2.0f;
  constexpr double kBefore = 0.15;
  constexpr double kAfter = 0.30;
  std::vector<float> beats;
  for (float t = 1.0f; t < kDuration; t += kPeriod) beats.push_back(t);

  // G from 0.15 s before to 0.30 s after each beat, C elsewhere: G fills 22.5 % of each span.
  std::vector<float> samples(static_cast<size_t>(kDuration * kSr), 0.0f);
  const auto sustain = [](double) { return 1.0; };
  double c_start = 0.0;
  for (float beat : beats) {
    for (float midi : kCMajor) add_note(samples, kSr, midi, c_start, beat - kBefore, sustain);
    for (float midi : kGMajor) add_note(samples, kSr, midi, beat - kBefore, beat + kAfter, sustain);
    c_start = beat + kAfter;
  }
  for (float midi : kCMajor) add_note(samples, kSr, midi, c_start, kDuration, sustain);
  const Audio audio = Audio::from_vector(std::move(samples), kSr);

  const Chroma chroma = chord_chroma(audio, ChordChromaConfig{});
  BassChromaConfig bass_config;
  bass_config.cqt.hop_length = ChordConfig{}.hop_length;
  const Chroma bass = bass_chroma(audio, bass_config);

  for (const float shift : {0.0f, 0.046f}) {
    CAPTURE(shift);
    std::vector<float> shifted = beats;
    for (float& beat : shifted) beat += shift;
    const std::vector<Chord> chords = ChordAnalyzer(chroma, shifted, bass, ChordConfig{}).chords();
    CAPTURE(names_of(chords));
    REQUIRE_FALSE(chords.empty());
    for (const Chord& chord : chords) {
      CHECK(chord.root == PitchClass::C);
      CHECK(chord.quality == ChordQuality::Major);
    }
  }
}

TEST_CASE("default chord detection reads a clean I-V-vi-IV at every input rate",
          "[chord][cqt_default]") {
  const std::vector<std::string> expected = {"C", "G", "Am", "F"};
  const std::vector<float> starts = {0.0f, 2.0f, 4.0f, 6.0f};
  constexpr float kBoundaryTolerance = 0.4f;

  for (int sr : {22050, 44100, 48000}) {
    const Audio audio = pop_progression(sr);
    for (const bool beat_sync : {true, false}) {
      CAPTURE(sr, beat_sync);
      ChordConfig config;
      config.use_beat_sync = beat_sync;
      const std::vector<Chord> chords = detect_chords(audio, config);
      CAPTURE(names_of(chords));
      REQUIRE(names_of(chords) == expected);
      for (size_t i = 0; i < chords.size(); ++i) {
        CAPTURE(i, chords[i].start);
        CHECK(std::abs(chords[i].start - starts[i]) <= kBoundaryTolerance);
      }
    }
  }
}

TEST_CASE("default chord detection accepts an input shorter than a chroma frame",
          "[chord][cqt_default]") {
  constexpr int kSr = 22050;
  const Audio full = pop_progression(kSr);
  const Audio audio = Audio::from_vector(std::vector<float>(full.data(), full.data() + 4096), kSr);
  CHECK_NOTHROW(chord_chroma(audio, ChordChromaConfig{}));
  CHECK_NOTHROW(detect_chords(audio));
  ChordConfig frame_path;
  frame_path.use_beat_sync = false;
  CHECK_NOTHROW(detect_chords(audio, frame_path));
}

namespace confidence_duration_core {

std::array<float, 12> feature(int tonic, bool minor, float noise) {
  std::array<float, 12> v{};
  v.fill(noise);
  v[tonic] = 1.0f;
  v[(tonic + (minor ? 3 : 4)) % 12] = 1.0f;
  v[(tonic + 7) % 12] = 1.0f;
  return v;
}
float score(int root, bool minor, const std::array<float, 12>& v) {
  for (const auto& t : generate_triad_templates()) {
    if (static_cast<int>(t.root) == root &&
        t.quality == (minor ? ChordQuality::Minor : ChordQuality::Major))
      return std::clamp(t.correlate(v.data()), 0.0f, 1.0f);
  }
  return -1;
}
Chroma block_chroma(int root, bool minor, float low_noise, float high_noise) {
  constexpr int frames = 800;
  auto lo = feature(root, minor, low_noise);
  auto hi = feature(root, minor, high_noise);
  std::vector<float> values(12 * frames);
  for (int c = 0; c < 12; ++c)
    for (int f = 0; f < frames; ++f) values[c * frames + f] = f < 600 ? lo[c] : hi[c];
  return Chroma(std::move(values), 12, frames, 10000, 100);
}
TEST_CASE("beat segment confidence integrates fixed evidence over elapsed time",
          "[chord][beat_confidence]") {
  std::vector<std::pair<std::string, std::vector<float>>> grids;
  std::vector<float> uniform, subdivided;
  for (int i = 0; i < 8; ++i) uniform.push_back(i);
  subdivided = {0, 2, 4};
  for (int i = 24; i < 32; ++i) subdivided.push_back(i * 0.25f);
  grids.push_back({"uniform", uniform});
  grids.push_back({"subdivide_strong", subdivided});
  grids.push_back({"unequal", {0, 6, 7}});
  grids.push_back({"single_span", {0, 8}});
  for (int root = 0; root < 12; ++root)
    for (bool minor : {false, true})
      for (bool hmm : {false, true}) {
        for (const auto& grid : grids) {
          CAPTURE(root, minor, hmm, grid.first);
          const float lo_noise = grid.first == "single_span" ? 0.35f : 0.8f;
          const float hi_noise = grid.first == "single_span" ? 0.35f : 0.0f;
          const Chroma chroma = block_chroma(root, minor, lo_noise, hi_noise);
          ChordConfig cfg;
          cfg.use_hmm = hmm;
          cfg.min_duration = 0;
          cfg.smoothing_window = 0;
          cfg.threshold = 0;
          const auto chords = ChordAnalyzer(chroma, grid.second, cfg).chords();
          const float oracle = .75f * score(root, minor, feature(root, minor, lo_noise)) +
                               .25f * score(root, minor, feature(root, minor, hi_noise));
          const bool setup =
              chords.size() == 1 && static_cast<int>(chords[0].root) == root &&
              chords[0].quality == (minor ? ChordQuality::Minor : ChordQuality::Major) &&
              std::abs(chords[0].duration() - 8.f) < 1e-5f;
          const float got = chords.empty() ? -1 : chords[0].confidence;
          REQUIRE(setup);
          CHECK(std::abs(chords[0].start) < 1e-5f);
          CHECK(std::abs(chords[0].end - 8.0f) < 1e-5f);
          const bool pass = setup && std::abs(got - oracle) < 2e-5f;
          REQUIRE(setup);
          CHECK(pass);
        }
      }
}
}  // namespace confidence_duration_core

namespace confidence_duration_edges {

std::array<float, 12> feat(int root, bool minor, float noise) {
  std::array<float, 12> v;
  v.fill(noise);
  v[root] = 1;
  v[(root + (minor ? 3 : 4)) % 12] = 1;
  v[(root + 7) % 12] = 1;
  return v;
}
float corr(int root, bool minor, float noise) {
  auto v = feat(root, minor, noise);
  for (const auto& t : generate_triad_templates())
    if (static_cast<int>(t.root) == root &&
        t.quality == (minor ? ChordQuality::Minor : ChordQuality::Major))
      return std::clamp(t.correlate(v.data()), 0.f, 1.f);
  return -1;
}
Chroma blocks(int root, bool minor, int frames, int sr, int hop, float weak = .8f,
              float strong = 0) {
  auto lo = feat(root, minor, weak), hi = feat(root, minor, strong);
  std::vector<float> data(12 * frames);
  for (int c = 0; c < 12; ++c)
    for (int f = 0; f < frames; ++f) data[c * frames + f] = (f < 600 ? lo : hi)[c];
  return Chroma(std::move(data), 12, frames, sr, hop);
}
struct Case {
  std::string name;
  std::vector<float> beats;
  int frames = 800, sr = 10000, hop = 100;
  double weak_seconds = 6;
  bool frame_path = false;
  float threshold = 0;
  float weak_noise = .8f, strong_noise = 0;
};
TEST_CASE("beat confidence duration weighting preserves coverage and consumers",
          "[chord][beat_confidence]") {
  std::vector<Case> specs = {{"nonzero_prefix", {.25f, 2, 4, 6, 6.5f, 7, 7.5f}},
                             {"partial_tail", {0, 2, 4, 6, 7.5f}, 775},
                             {"frame_path", {0, 6, 7}, 800, 10000, 100, 6, true},
                             {"no_chord",
                              {0, 2, 4, 6, 6.25f, 6.5f, 6.75f, 7, 7.25f, 7.5f, 7.75f},
                              800,
                              10000,
                              100,
                              6,
                              false,
                              1.0f}};
  specs.back().strong_noise = .6f;
  specs.push_back({"zero_terminal", {0, 6, 7, 8}});
  specs.push_back({"constant_score", {0, 8}, 800, 10000, 100, 6, false, 0, .8f, .8f});
  Case fractional{"fractional_uniform", {}};
  for (int n = 0; n < 32; ++n) fractional.beats.push_back(n * .25f);
  specs.push_back(fractional);
  Case unequal{"fractional_unequal", {}};
  for (int n = 0; n < 16; ++n) unequal.beats.push_back(n * .375f);
  for (int n = 48; n < 64; ++n) unequal.beats.push_back(n * .125f);
  specs.push_back(unequal);
  Case quantized{"quantized_hop", {}, 800, 44100, 4096};
  const float hopsec = static_cast<float>(quantized.hop) / quantized.sr;
  for (int f : {0, 200, 400, 600, 650, 700, 750})
    quantized.beats.push_back(f == 0 ? 0.f : (f + .25f) * hopsec);
  quantized.weak_seconds = quantized.beats[3];
  specs.push_back(quantized);
  for (int root = 0; root < 12; ++root)
    for (bool minor : {false, true})
      for (bool hmm : {false, true})
        for (const auto& s : specs) {
          CAPTURE(root, minor, hmm, s.name);
          auto chroma = blocks(root, minor, s.frames, s.sr, s.hop, s.weak_noise, s.strong_noise);
          ChordConfig cfg;
          cfg.use_hmm = hmm;
          cfg.min_duration = 0;
          cfg.smoothing_window = 0;
          cfg.threshold = s.threshold;
          cfg.use_beat_sync = !s.frame_path;
          auto result = ChordAnalyzer(chroma, s.beats, cfg).chords();
          const double duration = chroma.duration();
          const double oracle = (s.weak_seconds * corr(root, minor, s.weak_noise) +
                                 (duration - s.weak_seconds) * corr(root, minor, s.strong_noise)) /
                                duration;
          // N.C. carries the final rejected template correlations over its emitted duration.
          bool setup = result.size() == 1 && static_cast<int>(result[0].root) == root &&
                       result[0].quality == (minor ? ChordQuality::Minor : ChordQuality::Major);
          bool pass = setup && std::abs(result[0].confidence - oracle) < 2e-5;
          if (s.name == "no_chord") {
            setup = result.size() == 1 && result[0].quality == ChordQuality::Unknown;
            pass = setup && std::abs(result[0].confidence - oracle) < 2e-5;
          }
          REQUIRE(setup);
          CHECK(std::abs(result[0].start) < 1e-5f);
          CHECK(std::abs(result[0].end - static_cast<float>(duration)) < 1e-5f);
          CHECK(pass);
        }

  // Two unequal-confidence segments must reset the duration and mass independently.
  for (int root = 0; root < 12; ++root)
    for (bool hmm : {false, true}) {
      CAPTURE(root, hmm);
      const int frames = 1600;
      std::vector<float> data(12 * frames);
      for (int f = 0; f < frames; ++f) {
        const int r = f < 800 ? root : (root + 5) % 12;
        const bool weak = f < 800 ? f < 600 : f < 1000;
        const auto v = feat(r, false, weak ? .8f : 0.f);
        for (int c = 0; c < 12; ++c) data[c * frames + f] = v[c];
      }
      Chroma chroma(std::move(data), 12, frames, 10000, 100);
      const std::vector<float> beats = {0, 2, 4, 6, 7, 8, 9, 10, 12, 14};
      ChordConfig cfg;
      cfg.use_hmm = hmm;
      cfg.min_duration = 0;
      cfg.smoothing_window = 0;
      cfg.threshold = 0;
      const auto chords = ChordAnalyzer(chroma, beats, cfg).chords();
      REQUIRE(chords.size() == 2);
      REQUIRE(chords[0].quality == ChordQuality::Major);
      REQUIRE(chords[1].quality == ChordQuality::Major);
      CHECK(std::abs(chords[0].start) < 1e-5f);
      const double first = .75 * corr(root, false, .8f) + .25;
      const double second = .25 * corr((root + 5) % 12, false, .8f) + .75;
      const bool setup = chords.size() == 2 && static_cast<int>(chords[0].root) == root &&
                         static_cast<int>(chords[1].root) == (root + 5) % 12 &&
                         chords[0].end == 8 && chords[1].start == 8 && chords[1].end == 16;
      const bool pass = setup && std::abs(chords[0].confidence - first) < 2e-5 &&
                        std::abs(chords[1].confidence - second) < 2e-5;
      REQUIRE(setup);
      CHECK(pass);
    }
  // A native chord timeline witnesses the consumer of the inflated confidence.
  for (int minor_root = 0; minor_root < 12; ++minor_root) {
    CAPTURE(minor_root);
    const int frames = 3600;
    std::vector<float> data(12 * frames);
    std::vector<float> uniform, subdivide;
    for (int beat = 0; beat < 36; ++beat) uniform.push_back(beat);
    for (int repeat = 0; repeat < 3; ++repeat) {
      const float base = repeat * 12.f;
      for (int sec : {0, 2, 4}) subdivide.push_back(base + sec);
      for (int tick = 24; tick < 32; ++tick) subdivide.push_back(base + tick * .25f);
      for (int sec : {8, 9, 10, 11}) subdivide.push_back(base + sec);
    }
    for (int f = 0; f < frames; ++f) {
      const int local = f % 1200;
      const int root = local < 800 ? minor_root : (minor_root + (local < 1000 ? 8 : 10)) % 12;
      const auto v = feat(root, local < 800, local < 600 ? .8f : 0.f);
      for (int c = 0; c < 12; ++c) data[c * frames + f] = v[c];
    }
    Chroma chroma(std::move(data), 12, frames, 10000, 100);
    ChordConfig cfg;
    cfg.use_hmm = false;
    cfg.min_duration = 0;
    cfg.smoothing_window = 0;
    std::vector<KeyCandidate> candidates = {
        {{static_cast<PitchClass>((minor_root + 3) % 12), Mode::Major, .9f}, 1.f},
        {{static_cast<PitchClass>(minor_root), Mode::Minor, .1f}, .7f}};
    auto a = ChordAnalyzer(chroma, uniform, cfg).chords();
    auto b = ChordAnalyzer(chroma, subdivide, cfg).chords();
    REQUIRE(a.size() == 9);
    REQUIRE(b.size() == a.size());
    for (size_t i = 0; i < a.size(); ++i) {
      REQUIRE(a[i].root == b[i].root);
      REQUIRE(a[i].quality == b[i].quality);
      REQUIRE(a[i].bass == b[i].bass);
      REQUIRE(std::abs(a[i].start - b[i].start) < 1e-5f);
      REQUIRE(std::abs(a[i].end - b[i].end) < 1e-5f);
    }
    const auto ka = refine_key_with_chords(candidates, a),
               kb = refine_key_with_chords(candidates, b);
    const bool setup = a.size() == 9 && b.size() == 9 && ka.root == candidates.front().key.root &&
                       ka.mode == Mode::Major;
    const bool pass = setup && kb.root == ka.root && kb.mode == ka.mode;
    REQUIRE(setup);
    CHECK(pass);
  }
}
}  // namespace confidence_duration_edges

TEST_CASE("zero duration terminal chord retains finite selected confidence",
          "[chord][beat_confidence]") {
  constexpr int frames = 800;
  const auto first = confidence_duration_edges::feat(0, false, 0.0f);
  const auto last = confidence_duration_edges::feat(5, false, 0.8f);
  std::vector<float> data(12 * frames);
  for (int c = 0; c < 12; ++c) {
    for (int f = 0; f < frames; ++f) {
      data[c * frames + f] = f == frames - 1 ? last[c] : first[c];
    }
  }
  const Chroma chroma(std::move(data), 12, frames, 10000, 100);
  ChordConfig config;
  config.use_hmm = false;
  config.min_duration = 0.0f;
  config.smoothing_window = 0.0f;
  config.threshold = 0.0f;
  const auto chords = ChordAnalyzer(chroma, {0.0f, 6.0f, 7.0f, 8.0f}, config).chords();
  REQUIRE(chords.size() == 2);
  REQUIRE(chords[0].root == PitchClass::C);
  REQUIRE(chords[1].root == PitchClass::F);
  REQUIRE(chords[0].quality == ChordQuality::Major);
  REQUIRE(chords[1].quality == ChordQuality::Major);
  CHECK(std::abs(chords[0].start) < 1e-5f);
  CHECK(std::abs(chords[0].end - 8.0f) < 1e-5f);
  REQUIRE(std::abs(chords[1].start - 8.0f) < 1e-5f);
  REQUIRE(std::abs(chords[1].end - chords[1].start) < 1e-5f);
  REQUIRE(chords[0].confidence > 0.9f);
  REQUIRE(confidence_duration_edges::corr(5, false, 0.8f) < 0.8f);
  CHECK(std::isfinite(chords[1].confidence));
  CHECK(std::abs(chords[1].confidence - confidence_duration_edges::corr(5, false, 0.8f)) < 2e-5f);
}
