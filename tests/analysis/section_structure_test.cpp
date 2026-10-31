/// @file section_structure_test.cpp
/// @brief Section layout recovery on the synthetic song of support/structure_song.h.

#include "analysis/section_structure.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "analysis/analysis_rate.h"
#include "analysis/beat_analyzer.h"
#include "analysis/section_analyzer.h"
#include "core/spectrum.h"
#include "feature/chroma.h"
#include "support/structure_song.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/types.h"

using namespace sonare;
namespace song = sonare::test::structure_song;

namespace {

/// Detected form and per-section start/end/type, for assertion messages.
std::string describe(const SectionAnalyzer& analyzer) {
  std::ostringstream out;
  out << "form=\"" << analyzer.form() << "\"";
  for (const Section& s : analyzer.sections()) {
    out << " [" << s.start << "-" << s.end << " " << section_type_to_char(s.type) << "]";
  }
  return out.str();
}

/// Whether every true boundary has exactly one detected boundary within @p tolerance_sec and
/// no detected boundary is left over.
bool boundaries_match(const std::vector<float>& detected, const std::vector<float>& truth,
                      float tolerance_sec) {
  if (detected.size() != truth.size()) return false;
  std::vector<float> sorted = detected;
  std::sort(sorted.begin(), sorted.end());
  for (std::size_t i = 0; i < truth.size(); ++i) {
    if (std::abs(sorted[i] - truth[i]) > tolerance_sec) return false;
  }
  return true;
}

std::vector<float> internal_boundaries(const SectionAnalyzer& analyzer) {
  std::vector<float> out;
  const auto& sections = analyzer.sections();
  for (std::size_t i = 1; i < sections.size(); ++i) out.push_back(sections[i].start);
  return out;
}

std::vector<SectionType> true_labels() {
  std::vector<SectionType> out;
  for (const auto& section : song::truth()) out.push_back(section.label);
  return out;
}

std::vector<SectionType> detected_labels(const SectionAnalyzer& analyzer) {
  std::vector<SectionType> out;
  for (const Section& s : analyzer.sections()) out.push_back(s.type);
  return out;
}

/// Whether @p groups partitions the sections exactly as @p truth does (numbering aside).
bool same_partition(const std::vector<int>& groups, const std::vector<int>& truth) {
  if (groups.size() != truth.size()) return false;
  for (std::size_t i = 0; i < truth.size(); ++i) {
    for (std::size_t j = i + 1; j < truth.size(); ++j) {
      if ((groups[i] == groups[j]) != (truth[i] == truth[j])) return false;
    }
  }
  return true;
}

std::vector<int> true_groups() {
  std::vector<int> out;
  for (const auto& section : song::truth()) out.push_back(section.group);
  return out;
}

std::vector<int> detected_groups(const SectionAnalyzer& analyzer) {
  std::vector<int> out;
  for (const Section& s : analyzer.sections()) out.push_back(s.group);
  return out;
}

void check_layout(const Audio& audio, float tolerance_sec) {
  const SectionAnalyzer analyzer(audio, SectionConfig());
  INFO(describe(analyzer));
  CHECK(boundaries_match(internal_boundaries(analyzer), song::true_boundaries(), tolerance_sec));
  CHECK(same_partition(detected_groups(analyzer), true_groups()));
  CHECK(detected_labels(analyzer) == true_labels());
}

}  // namespace

TEST_CASE("Structure song score matches its truth table", "[section_structure_fixture]") {
  const auto& truth = song::truth();
  REQUIRE(truth.size() == 11);

  SECTION("section lengths follow the bar counts") {
    int bars = 0;
    float start = 0.0f;
    for (const auto& section : truth) {
      CHECK(section.start_sec == start);
      CHECK(section.end_sec - section.start_sec == section.bars * song::kBarSec);
      start = section.end_sec;
      bars += section.bars;
    }
    CHECK(bars == song::kNumBars);
    CHECK(start == song::kDurationSec);
  }

  SECTION("true boundaries are the ten section starts") {
    const std::vector<float> expected = {8, 24, 32, 48, 56, 72, 80, 96, 112, 128};
    CHECK(song::true_boundaries() == expected);
  }

  SECTION("every bar of every section carries a bass note on its downbeat") {
    const int rate = song::kRenderRate;
    const auto events = song::build_events(rate);
    std::vector<int> bass_downbeats(truth.size(), 0);
    for (const auto& event : events) {
      if (!event.ump.is_note_on() || event.ump.channel() != song::kChBass) continue;
      if (event.sample % static_cast<int64_t>(song::kBarSec * rate) != 0) continue;
      const double sec = static_cast<double>(event.sample) / rate;
      for (std::size_t i = 0; i < truth.size(); ++i) {
        if (sec >= truth[i].start_sec && sec < truth[i].end_sec) ++bass_downbeats[i];
      }
    }
    for (std::size_t i = 0; i < truth.size(); ++i) {
      INFO(truth[i].name << " at " << truth[i].start_sec);
      CHECK(bass_downbeats[i] == truth[i].bars);
    }
  }

  SECTION("sections of one group repeat the same length") {
    for (const auto& a : truth) {
      for (const auto& b : truth) {
        if (a.group == b.group && a.label == b.label) CHECK(a.bars == b.bars);
      }
    }
  }
}

TEST_CASE("Structure song layout at 44100 Hz", "[section_structure_render][.][slow]") {
  check_layout(song::rendered_audio(), 0.5f);
}

TEST_CASE("Structure song layout resampled to 48000 Hz", "[section_structure_render][.][slow]") {
  REQUIRE(song::resampled_audio().sample_rate() == song::kResampledRate);
  check_layout(song::resampled_audio(), song::kBeatSec);
}

TEST_CASE("Structure song with a modulated last chorus", "[section_structure_render][.][slow]") {
  // Semitone-up last chorus: matching it is out of scope, but it must not read as Outro/Bridge.
  const SectionAnalyzer analyzer(song::modulated_audio(), SectionConfig());
  INFO(describe(analyzer));
  REQUIRE(analyzer.count() >= 2);
  const auto& sections = analyzer.sections();
  const auto last_chorus = std::find_if(sections.begin(), sections.end(), [](const Section& s) {
    return s.start <= 120.0f && 120.0f < s.end;
  });
  REQUIRE(last_chorus != sections.end());
  CHECK(last_chorus->type != SectionType::Outro);
  CHECK(last_chorus->type != SectionType::Bridge);
}

namespace {

namespace ss = sonare::section_structure;

/// The STFT section analysis takes of @p audio at the analysis rate.
Spectrogram analysis_spectrogram(const Audio& audio, const SectionConfig& config) {
  const Audio analysis = analysis_rate_audio(audio);
  return Spectrogram::compute(
      analysis, ss::section_chroma_config(config, analysis.sample_rate()).to_stft_config());
}

enum class Material { kStructured, kUniform, kSilence };
enum class GridKind { kBeatsDownbeats, kBeatsOnly, kFallback, kCentreOnly };

constexpr float kGridSongSec = 20.0f;
constexpr float kGridSectionSec = 4.0f;

/// Five 4 s spans alternating two pitch classes and timbres (A B A B A), one steady chord, or
/// silence.
Audio grid_material(Material material, int sr) {
  std::vector<float> samples(static_cast<std::size_t>(kGridSongSec * static_cast<float>(sr)));
  if (material == Material::kSilence) return Audio::from_vector(std::move(samples), sr);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const double t = static_cast<double>(i) / sr;
    double v = 0.0;
    if (material == Material::kUniform) {
      for (double f : {261.63, 329.63, 392.00}) v += 0.25 * std::sin(constants::kTwoPiD * f * t);
    } else {
      const bool b = static_cast<int>(t / kGridSectionSec) % 2 == 1;
      const double f0 = b ? 330.0 : 220.0;
      const int partials = b ? 8 : 2;
      for (int h = 1; h <= partials; ++h) {
        v += 0.3 / h * std::sin(constants::kTwoPiD * f0 * h * t);
      }
    }
    samples[i] = static_cast<float>(v);
  }
  return Audio::from_vector(std::move(samples), sr);
}

SectionGrid make_grid(GridKind kind, float beat_sec = 0.5f) {
  SectionGrid grid;
  if (kind == GridKind::kFallback) {
    grid.beat_times = {1.0f, 2.0f, 3.0f};  // fewer than kMinGridBeats
    return grid;
  }
  const float first = kind == GridKind::kCentreOnly ? 0.2f * kGridSongSec : 0.0f;
  const float last = kind == GridKind::kCentreOnly ? 0.8f * kGridSongSec : kGridSongSec;
  int k = 0;
  for (float t = first; t < last + 1e-3f; t += beat_sec, ++k) {
    grid.beat_times.push_back(t);
    if (k % 4 == 0) grid.downbeat_indices.push_back(k);
  }
  grid.downbeats_reliable = kind != GridKind::kBeatsOnly;
  return grid;
}

SectionConfig grid_config() {
  SectionConfig config;
  config.min_section_sec = 2.0f;  // a 4 s kernel half-width for 4 s spans
  return config;
}

std::vector<float> starts_after_first(const std::vector<Section>& sections) {
  std::vector<float> out;
  for (std::size_t i = 1; i < sections.size(); ++i) out.push_back(sections[i].start);
  return out;
}

void check_grid_row(GridKind kind, int sr, Material material) {
  const SectionConfig config = grid_config();
  const Audio audio = grid_material(material, sr);
  const SectionAnalyzer analyzer(audio, make_grid(kind), analysis_spectrogram(audio, config),
                                 config);
  const auto& sections = analyzer.sections();
  CAPTURE(static_cast<int>(kind), sr, static_cast<int>(material), analyzer.form());
  switch (material) {
    case Material::kStructured: {
      CHECK(boundaries_match(starts_after_first(sections), {4.0f, 8.0f, 12.0f, 16.0f}, 0.5f));
      std::vector<int> groups;
      for (const Section& section : sections) groups.push_back(section.group);
      CHECK(same_partition(groups, {0, 1, 0, 1, 0}));
      break;
    }
    case Material::kUniform:
      REQUIRE(sections.size() == 1);
      CHECK(sections[0].type == SectionType::Unknown);
      CHECK(sections[0].confidence == 0.0f);
      CHECK(sections[0].group == 0);
      break;
    case Material::kSilence:
      REQUIRE(sections.size() == 1);
      CHECK(sections[0].type == SectionType::Unknown);
      CHECK(sections[0].confidence == 0.0f);
      CHECK(sections[0].energy_level == 0.0f);
      break;
  }
}

}  // namespace

// Grid rows: grid kind x rate x material, pairwise-covering, plus the filled-edges row.
TEST_CASE("section grid row: beats and downbeats, 48 kHz, silence", "[section_structure]") {
  check_grid_row(GridKind::kBeatsDownbeats, 48000, Material::kSilence);
}

TEST_CASE("section grid row: fallback grid, 44.1 kHz, structured", "[section_structure]") {
  check_grid_row(GridKind::kFallback, 44100, Material::kStructured);
}

TEST_CASE("section grid row: beats only, 48 kHz, structured", "[section_structure]") {
  check_grid_row(GridKind::kBeatsOnly, 48000, Material::kStructured);
}

TEST_CASE("section grid row: fallback grid, 48 kHz, uniform", "[section_structure]") {
  check_grid_row(GridKind::kFallback, 48000, Material::kUniform);
}

TEST_CASE("section grid row: beats and downbeats, 44.1 kHz, uniform", "[section_structure]") {
  check_grid_row(GridKind::kBeatsDownbeats, 44100, Material::kUniform);
}

TEST_CASE("section grid row: fallback grid, 22.05 kHz, silence", "[section_structure]") {
  check_grid_row(GridKind::kFallback, 22050, Material::kSilence);
}

TEST_CASE("section grid row: beats only, 44.1 kHz, silence", "[section_structure]") {
  check_grid_row(GridKind::kBeatsOnly, 44100, Material::kSilence);
}

TEST_CASE("section grid row: beats and downbeats, 22.05 kHz, structured", "[section_structure]") {
  check_grid_row(GridKind::kBeatsDownbeats, 22050, Material::kStructured);
}

TEST_CASE("section grid row: beats only, 22.05 kHz, uniform", "[section_structure]") {
  check_grid_row(GridKind::kBeatsOnly, 22050, Material::kUniform);
}

TEST_CASE("section grid row: beats over the central 60% only, 22.05 kHz, structured",
          "[section_structure]") {
  check_grid_row(GridKind::kCentreOnly, 22050, Material::kStructured);
}

TEST_CASE("a doubled or halved beat tempo only changes the grid's resolution",
          "[section_structure]") {
  const SectionConfig config = grid_config();
  const Audio audio = grid_material(Material::kStructured, 22050);
  const Spectrogram spec = analysis_spectrogram(audio, config);
  for (float beat_sec : {0.25f, 0.5f, 1.0f}) {
    CAPTURE(beat_sec);
    const SectionAnalyzer analyzer(audio, make_grid(GridKind::kBeatsDownbeats, beat_sec), spec,
                                   config);
    CHECK(boundaries_match(starts_after_first(analyzer.sections()), {4.0f, 8.0f, 12.0f, 16.0f},
                           0.5f));
  }
}

TEST_CASE("column grid fills, absorbs, falls back and halves", "[section_structure]") {
  constexpr float kFrame = 512.0f / 22050.0f;
  SECTION("columns before, between and after the beats at the median spacing") {
    SectionGrid grid;
    for (float t = 2.0f; t < 6.0f - 1e-3f; t += 0.5f) grid.beat_times.push_back(t);
    for (float t = 8.0f; t < 10.0f - 1e-3f; t += 0.5f) grid.beat_times.push_back(t);
    grid.downbeat_indices = {0, 8};
    grid.downbeats_reliable = true;
    const ss::ColumnGrid columns = ss::column_grid(grid, 12.0f, kFrame);
    REQUIRE(columns.times.front() == 0.0f);
    for (int c = 1; c < columns.size(); ++c) {
      CHECK(columns.times[static_cast<std::size_t>(c)] -
                columns.times[static_cast<std::size_t>(c - 1)] ==
            0.5f);
    }
    CHECK(columns.times.back() == 11.5f);
    REQUIRE(columns.downbeats.size() == 2);
    CHECK(columns.time(columns.downbeats[0]) == 2.0f);
    CHECK(columns.time(columns.downbeats[1]) == 8.0f);
    CHECK(columns.column_sec == 0.5f);
    CHECK(columns.to_columns(4.0f) == 8);
  }
  SECTION("a beat closer than one frame is absorbed with its downbeat") {
    SectionGrid grid;
    for (int k = 0; k < 10; ++k) grid.beat_times.push_back(0.5f * static_cast<float>(k));
    grid.beat_times.insert(grid.beat_times.begin() + 6, 2.5f + 0.5f * kFrame);
    grid.downbeat_indices = {6};
    const ss::ColumnGrid columns = ss::column_grid(grid, 5.0f, kFrame);
    CHECK(columns.size() == 10);
    REQUIRE(columns.downbeats.size() == 1);
    CHECK(columns.time(columns.downbeats[0]) == 2.5f);
  }
  SECTION("fewer than eight beats use the fixed grid") {
    SectionGrid grid;
    grid.beat_times = {1.0f, 1.5f, 2.0f};
    grid.downbeat_indices = {0};
    const ss::ColumnGrid columns = ss::column_grid(grid, 3.0f, kFrame);
    CHECK(columns.times == std::vector<float>{0.0f, 0.5f, 1.0f, 1.5f, 2.0f, 2.5f});
    CHECK(columns.downbeats.empty());
  }
  SECTION("past the column budget adjacent columns merge pairwise") {
    SectionGrid grid;
    for (int k = 0; k < 3000; ++k) {
      grid.beat_times.push_back(0.5f * static_cast<float>(k));
      if (k % 4 == 0) grid.downbeat_indices.push_back(k);
    }
    const ss::ColumnGrid columns = ss::column_grid(grid, 1500.0f, kFrame);
    CHECK(columns.size() == 1500);
    CHECK(columns.size() <= ss::kMaxColumns);
    CHECK(columns.column_sec == 1.0f);
    CHECK(columns.downbeats.size() == 750);
    CHECK(columns.time(columns.downbeats[1]) == 2.0f);
  }
}

namespace {

/// Hand-built frame features, one frame per 0.5 s column, MFCC in dimension @p mfcc_dim and
/// chroma in pitch class @p pitch_class per frame; @p level 0 marks a silent frame.
ss::FrameFeatures hand_frames(const std::vector<int>& mfcc_dim, const std::vector<int>& pitch_class,
                              const std::vector<float>& level) {
  ss::FrameFeatures frames;
  const int n = static_cast<int>(level.size());
  frames.n_frames = n;
  frames.frame_sec = 0.5f;
  frames.duration_sec = 0.5f * static_cast<float>(n);
  frames.mfcc.assign(static_cast<std::size_t>(ss::kFrameMfcc) * n, 1.0f);
  frames.chroma.assign(static_cast<std::size_t>(ss::kFrameChroma) * n, 0.0f);
  frames.rms = level;
  for (int f = 0; f < n; ++f) {
    frames.frame_times.push_back(0.5f * static_cast<float>(f));
    frames.mfcc[static_cast<std::size_t>(mfcc_dim[static_cast<std::size_t>(f)]) * n + f] += 10.0f;
    frames.chroma[static_cast<std::size_t>(pitch_class[static_cast<std::size_t>(f)]) * n + f] =
        1.0f;
  }
  return frames;
}

ss::ColumnGrid half_second_columns(int n) {
  ss::ColumnGrid grid;
  for (int c = 0; c < n; ++c) grid.times.push_back(0.5f * static_cast<float>(c));
  grid.duration_sec = 0.5f * static_cast<float>(n);
  grid.column_sec = 0.5f;
  return grid;
}

float dot(const ss::ColumnFeatures& f, int a, int b) {
  float sum = 0.0f;
  for (int d = 0; d < f.n_dims; ++d) {
    sum += f.values[static_cast<std::size_t>(a * f.n_dims + d)] *
           f.values[static_cast<std::size_t>(b * f.n_dims + d)];
  }
  return sum;
}

}  // namespace

TEST_CASE("column features zero silence, centre MFCC and weight the blocks",
          "[section_structure]") {
  // Columns 0-1 one timbre and pitch class, 2-3 another, 4 silent.
  const ss::FrameFeatures frames =
      hand_frames({0, 0, 1, 1, 2}, {0, 0, 7, 7, 3}, {0.5f, 0.5f, 0.5f, 0.5f, 0.0f});
  const ss::ColumnMeans means = ss::column_means(frames, half_second_columns(5));
  CHECK(means.audible == std::vector<uint8_t>{1, 1, 1, 1, 0});

  const ss::ColumnFeatures both = ss::column_features(means);
  REQUIRE(both.n_dims == ss::kFrameMfcc + ss::kFrameChroma);
  for (int c = 0; c < 4; ++c) CHECK(std::abs(dot(both, c, c) - 1.0f) < 1e-5f);
  CHECK(dot(both, 4, 4) == 0.0f);
  CHECK(dot(both, 0, 4) == 0.0f);
  // Centred, the two timbres point in opposite directions: -1/2 from MFCC, 0 from chroma.
  CHECK(std::abs(dot(both, 0, 2) + 0.5f) < 1e-5f);
  CHECK(std::abs(dot(both, 0, 1) - 1.0f) < 1e-5f);

  const ss::ColumnFeatures timbre = ss::column_features(means, ss::FeatureSet::kTimbreOnly);
  const ss::ColumnFeatures harmony = ss::column_features(means, ss::FeatureSet::kHarmonyOnly);
  REQUIRE(timbre.n_dims == ss::kFrameMfcc);
  REQUIRE(harmony.n_dims == ss::kFrameChroma);
  CHECK(std::abs(dot(timbre, 0, 2) + 1.0f) < 1e-5f);
  CHECK(std::abs(dot(harmony, 0, 2)) < 1e-6f);
  CHECK(std::abs(dot(harmony, 0, 1) - 1.0f) < 1e-5f);
}

TEST_CASE("column contrast reads the raw change either side", "[section_structure]") {
  // 8 columns of one pitch class and timbre, 8 of another; then a silent tail.
  std::vector<int> dims(16, 0);
  std::vector<int> pcs(16, 0);
  std::vector<float> level(16, 0.5f);
  for (int c = 8; c < 16; ++c) {
    dims[static_cast<std::size_t>(c)] = 1;
    pcs[static_cast<std::size_t>(c)] = 7;
  }
  const ss::ColumnMeans means =
      ss::column_means(hand_frames(dims, pcs, level), half_second_columns(16));
  const ss::ColumnContrast contrast = ss::column_contrast(means, 8);
  CHECK(std::abs(contrast.timbre[8] - std::sqrt(200.0f)) < 1e-3f);
  CHECK(std::abs(contrast.harmony[8] - 1.0f) < 1e-6f);
  CHECK(contrast.harmony[12] == 0.0f);
  CHECK(contrast.timbre[1] == 0.0f);  // inside half a kernel of the start: not reached

  std::vector<float> gap = level;
  for (int c = 8; c < 16; ++c) gap[static_cast<std::size_t>(c)] = 0.0f;
  const ss::ColumnContrast entry = ss::column_contrast(
      ss::column_means(hand_frames(dims, pcs, gap), half_second_columns(16)), 8);
  CHECK(entry.harmony[8] == std::numeric_limits<float>::infinity());
}

TEST_CASE("diagonal smoothing averages only the terms inside the matrix", "[section_structure]") {
  const int n = 3;
  const std::vector<float> ssm = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  const std::vector<float> smoothed = ss::smooth_diagonal(ssm, n, 2);
  CHECK(smoothed[0] == (1.0f + 5.0f) / 2.0f);
  CHECK(smoothed[1] == (2.0f + 6.0f) / 2.0f);
  CHECK(smoothed[2] == 3.0f);
  CHECK(smoothed[8] == 9.0f);
  CHECK(smoothed[3] == (4.0f + 8.0f) / 2.0f);
  CHECK(ss::smooth_diagonal(ssm, n, 1) == ssm);
}

namespace {

/// Unit column vectors: letter m at position k is basis vector m * 64 + k, so one letter's
/// columns repeat exactly wherever it recurs and two letters are orthogonal.
struct Sketch {
  ss::ColumnFeatures features;
  std::vector<ss::Span> spans;
  ss::ColumnGrid grid;
};

/// @p parts: (material, first position inside it, columns).
Sketch sketch(const std::vector<std::tuple<int, int, int>>& parts) {
  Sketch out;
  out.features.n_dims = 8 * 64;
  int begin = 0;
  for (const auto& [material, offset, length] : parts) {
    for (int k = 0; k < length; ++k) {
      std::vector<float> column(static_cast<std::size_t>(out.features.n_dims), 0.0f);
      column[static_cast<std::size_t>(material * 64 + offset + k)] = 1.0f;
      out.features.values.insert(out.features.values.end(), column.begin(), column.end());
    }
    out.spans.push_back({begin, begin + length});
    begin += length;
  }
  out.features.n_columns = begin;
  out.grid = half_second_columns(begin);
  return out;
}

std::vector<float> smoothed_of(const Sketch& s, int length = 8) {
  return ss::smooth_diagonal(ss::column_ssm(s.features), s.features.n_columns, length);
}

}  // namespace

TEST_CASE("pair scores align the shorter section inside the longer", "[section_structure]") {
  // A (16), B (16), A's columns 4-11 (8), C (2).
  const Sketch s = sketch({{0, 0, 16}, {1, 0, 16}, {0, 4, 8}, {2, 0, 2}});
  const std::vector<float> smoothed = smoothed_of(s, 1);
  const ss::PairScores scores = ss::pair_scores(s.spans, smoothed, s.features.n_columns, 2);
  CHECK(scores.score(0, 0) == 1.0f);
  CHECK(scores.score(2, 0) == 1.0f);
  CHECK(scores.offset(2, 0) == 4);
  CHECK(scores.offset(0, 2) == -4);
  CHECK(scores.score(0, 1) == 0.0f);
  CHECK(scores.score(1, 2) == 0.0f);
  // 2 of 16 columns is under the length ratio.
  CHECK(scores.score(3, 0) == 0.0f);
  CHECK(scores.score(0, 2) == scores.score(2, 0));
  // An offset that would leave the matrix is not a candidate.
  const Sketch tail = sketch({{0, 0, 8}, {0, 0, 4}});
  const ss::PairScores edge =
      ss::pair_scores(tail.spans, smoothed_of(tail, 1), tail.features.n_columns, 6);
  CHECK(edge.score(1, 0) == 1.0f);
  CHECK(edge.offset(1, 0) == 0);
}

TEST_CASE("a section split in two is joined when it lines up with a whole occurrence",
          "[section_structure]") {
  // V C1 V C2a C2b C3, C2a and C2b the two halves of the chorus.
  const Sketch s =
      sketch({{0, 0, 32}, {1, 0, 32}, {0, 0, 32}, {1, 0, 16}, {1, 16, 16}, {1, 0, 32}});
  const auto joined = ss::join_split_sections(s.spans, smoothed_of(s), s.grid, ss::kRepeatScore);
  REQUIRE(joined.size() == 5);
  CHECK(joined[3].begin == 96);
  CHECK(joined[3].end == 128);
  // Halves in the wrong order do not line up with any occurrence.
  const Sketch swapped =
      sketch({{0, 0, 32}, {1, 0, 32}, {0, 0, 32}, {1, 16, 16}, {1, 0, 16}, {1, 0, 32}});
  CHECK(ss::join_split_sections(swapped.spans, smoothed_of(swapped), swapped.grid, ss::kRepeatScore)
            .size() == 6);
}

TEST_CASE("groups are complete-linkage with ties broken by section order", "[section_structure]") {
  ss::PairScores scores;
  scores.count = 4;
  scores.offsets.assign(16, 0);
  // 0~1 and 1~2 match, 0 and 2 do not; 3 matches 2 exactly as well as 1 matches 0.
  scores.scores = {1.0f, 0.9f, 0.1f, 0.1f,  //
                   0.9f, 1.0f, 0.9f, 0.1f,  //
                   0.1f, 0.9f, 1.0f, 0.9f,  //
                   0.1f, 0.1f, 0.9f, 1.0f};
  // The (0, 1) merge wins the three-way tie, so 2 cannot join (its distance to 0 is 0.9).
  CHECK(ss::group_sections(scores, 0.6f) == std::vector<int>{0, 0, 1, 1});
  // 0 and 2 now close enough; 3 no longer matches 2, so {0, 1} takes 2 and 3 stays alone.
  scores.scores[2] = scores.scores[8] = 0.7f;
  scores.scores[11] = scores.scores[14] = 0.5f;
  CHECK(ss::group_sections(scores, 0.6f) == std::vector<int>{0, 0, 0, 1});
  CHECK(ss::group_sections(scores, 0.95f) == std::vector<int>{0, 1, 2, 3});
}

TEST_CASE("adjacent sections of a group merge only up to one occurrence", "[section_structure]") {
  const ss::ColumnGrid grid = half_second_columns(64);
  SECTION("two halves of a 16-column occurrence merge") {
    std::vector<ss::Span> spans = {{0, 16}, {16, 32}, {32, 40}, {40, 48}, {48, 64}};
    std::vector<int> groups = {0, 1, 0, 0, 2};
    ss::merge_adjacent(spans, groups, grid);
    REQUIRE(spans.size() == 4);
    CHECK(spans[2].begin == 32);
    CHECK(spans[2].end == 48);
    CHECK(groups == std::vector<int>{0, 1, 0, 2});
  }
  SECTION("two whole occurrences stay two") {
    std::vector<ss::Span> spans = {{0, 16}, {16, 32}, {32, 48}, {48, 64}};
    std::vector<int> groups = {0, 1, 0, 0};
    ss::merge_adjacent(spans, groups, grid);
    CHECK(spans.size() == 4);
  }
  SECTION("a group with no other occurrence is not merged") {
    std::vector<ss::Span> spans = {{0, 16}, {16, 24}, {24, 32}};
    std::vector<int> groups = {0, 1, 1};
    ss::merge_adjacent(spans, groups, grid);
    CHECK(spans.size() == 3);
  }
}

namespace {

ss::LabelInput section(int group, float length, float energy, float vocal = 0.9f) {
  ss::LabelInput in;
  in.group = group;
  in.length_sec = length;
  in.energy = energy;
  in.vocal_likelihood = vocal;
  return in;
}

/// Pair scores of 0.9 inside a group and 0.1 across.
ss::PairScores group_scores(const std::vector<ss::LabelInput>& sections) {
  ss::PairScores scores;
  scores.count = static_cast<int>(sections.size());
  scores.offsets.assign(static_cast<std::size_t>(scores.count * scores.count), 0);
  for (const auto& a : sections) {
    for (const auto& b : sections) scores.scores.push_back(a.group == b.group ? 0.9f : 0.1f);
  }
  for (int i = 0; i < scores.count; ++i) {
    scores.scores[static_cast<std::size_t>(i * scores.count + i)] = 1.0f;
  }
  return scores;
}

std::vector<SectionType> types_of(const std::vector<ss::Label>& labels) {
  std::vector<SectionType> out;
  for (const auto& label : labels) {
    // Every label carries a confidence in its documented range.
    if (label.type == SectionType::Unknown) {
      CHECK(label.confidence == 0.0f);
    } else {
      CHECK(label.confidence >= 0.5f);
      CHECK(label.confidence <= 1.0f);
    }
    out.push_back(label.type);
  }
  return out;
}

std::vector<SectionType> label(const std::vector<ss::LabelInput>& sections) {
  return types_of(ss::label_sections(sections, group_scores(sections)));
}

using T = SectionType;

}  // namespace

TEST_CASE("labelling rules each fire on their input and not on its near miss",
          "[section_structure]") {
  SECTION("rule 1: the most repeated loud group is the Chorus") {
    CHECK(label({section(0, 8, 1), section(0, 8, 1), section(1, 8, 1)}) ==
          std::vector<T>{T::Chorus, T::Chorus, T::Unknown});
    CHECK(label({section(0, 8, 1), section(1, 8, 1), section(2, 8, 1)}) ==
          std::vector<T>{T::Unknown, T::Unknown, T::Unknown});
    // Equal fitness: more coverage wins, then the earlier group.
    CHECK(label({section(0, 8, 1), section(1, 12, 1), section(0, 8, 1), section(1, 12, 1)})[1] ==
          T::Chorus);
    CHECK(label({section(0, 8, 1), section(1, 8, 1), section(0, 8, 1), section(1, 8, 1)})[0] ==
          T::Chorus);
  }
  SECTION("rule 2: the other repeating group covering most time is the Verse") {
    const auto in = std::vector<ss::LabelInput>{section(0, 16, 0.5f), section(1, 16, 1),
                                                section(0, 16, 0.5f), section(1, 16, 1)};
    CHECK(label(in) == std::vector<T>{T::Verse, T::Chorus, T::Verse, T::Chorus});
  }
  SECTION("rule 3: a shorter group always leading into the Chorus is the Pre-Chorus") {
    const auto in = std::vector<ss::LabelInput>{
        section(0, 16, 0.5f), section(1, 8, 0.5f), section(2, 16, 1),  //
        section(0, 16, 0.5f), section(1, 8, 0.5f), section(2, 16, 1)};
    CHECK(label(in)[1] == T::PreChorus);
    auto not_leading = in;
    std::swap(not_leading[4], not_leading[3]);  // V P C P V C: the second P leads into V
    CHECK(label(not_leading)[1] != T::PreChorus);
    auto not_shorter = in;
    not_shorter[1].length_sec = not_shorter[4].length_sec = 16;
    CHECK(label(not_shorter)[1] != T::PreChorus);
  }
  SECTION("rule 4: a quiet, short, or instrumental first section is the Intro") {
    const auto body = [](ss::LabelInput first) {
      return std::vector<ss::LabelInput>{first, section(1, 16, 1), section(2, 16, 0.5f),
                                         section(1, 16, 1), section(2, 16, 0.5f)};
    };
    CHECK(label(body(section(0, 16, 0.1f)))[0] == T::Intro);
    CHECK(label(body(section(0, 8, 0.8f)))[0] == T::Intro);
    CHECK(label(body(section(0, 16, 0.8f)))[0] == T::Unknown);
    // (ii) a member of a repeating group played without vocals
    CHECK(label(body(section(2, 16, 0.5f, 0.1f)))[0] == T::Intro);
    CHECK(label(body(section(2, 16, 0.5f, 0.9f)))[0] == T::Verse);
  }
  SECTION("rule 5: the last section mirrors the Intro") {
    const auto body = [](ss::LabelInput last) {
      return std::vector<ss::LabelInput>{section(1, 16, 1), section(2, 16, 0.8f), section(1, 16, 1),
                                         section(2, 16, 0.8f), last};
    };
    CHECK(label(body(section(0, 16, 0.1f))).back() == T::Outro);
    CHECK(label(body(section(0, 8, 0.8f))).back() == T::Outro);
    CHECK(label(body(section(0, 16, 0.8f))).back() == T::Unknown);
    CHECK(label(body(section(1, 16, 1, 0.1f))).back() == T::Outro);
  }
  SECTION("rule 6: Instrumental by the edge material or by missing vocals beside a repeat") {
    CHECK(label({section(0, 8, 0.1f), section(2, 16, 0.5f), section(1, 16, 1), section(0, 8, 0.1f),
                 section(2, 16, 0.5f), section(1, 16, 1)})[3] == T::Instrumental);
    CHECK(label({section(1, 16, 1), section(2, 16, 0.8f, 0.1f), section(1, 16, 1),
                 section(3, 16, 0.8f)})[1] == T::Instrumental);
    CHECK(label({section(1, 16, 1), section(1, 16, 1), section(3, 16, 0.8f),
                 section(2, 16, 0.8f, 0.1f), section(4, 16, 0.8f)})[3] == T::Unknown);
  }
  SECTION("rule 7: a section between two Chorus occurrences is the Bridge") {
    CHECK(label({section(1, 16, 1), section(2, 16, 0.8f), section(1, 16, 1)})[1] == T::Bridge);
    CHECK(label({section(1, 16, 1), section(1, 16, 1), section(2, 16, 0.8f),
                 section(3, 16, 0.8f)})[2] == T::Unknown);
  }
}

namespace {

/// One part of a song-form row: material letter index, first position, columns, energy, vocal.
struct Part {
  int material;
  int columns;
  float energy;
  float vocal = 0.9f;
  int offset = 0;
};

/// Runs the repetition stage and the rules on a sketched form; returns labels, and the final
/// groups through @p groups.
std::vector<T> form_labels(const std::vector<Part>& parts, std::vector<int>* groups = nullptr) {
  std::vector<std::tuple<int, int, int>> layout;
  for (const Part& p : parts) layout.emplace_back(p.material, p.offset, p.columns);
  const Sketch s = sketch(layout);
  const ss::Repetition rep = ss::resolve_repetition(s.spans, smoothed_of(s), s.grid);
  std::vector<ss::LabelInput> inputs;
  for (std::size_t i = 0; i < rep.spans.size(); ++i) {
    // Each final span takes the energy and vocals of the part it starts with.
    std::size_t part = 0;
    for (std::size_t k = 0; k < parts.size(); ++k) {
      if (s.spans[k].begin == rep.spans[i].begin) part = k;
    }
    inputs.push_back(section(rep.groups[i], 0.5f * static_cast<float>(rep.spans[i].length()),
                             parts[part].energy, parts[part].vocal));
  }
  if (groups) *groups = rep.groups;
  return types_of(ss::label_sections(inputs, rep.scores));
}

}  // namespace

TEST_CASE("song forms read as the rule table states", "[section_structure]") {
  constexpr int I = 0, V = 1, P = 2, C = 3, B = 4, O = 5, X = 6, Y = 7;
  SECTION("I V P C X V P C B C O, X the intro material") {
    std::vector<int> groups;
    const auto labels = form_labels({{I, 16, 0.2f},
                                     {V, 32, 0.5f},
                                     {P, 16, 0.4f},
                                     {C, 32, 1},
                                     {I, 16, 0.2f},
                                     {V, 32, 0.5f},
                                     {P, 16, 0.4f},
                                     {C, 32, 1},
                                     {B, 32, 0.6f},
                                     {C, 32, 1},
                                     {I, 8, 0.15f}},
                                    &groups);
    CHECK(labels == std::vector<T>{T::Intro, T::Verse, T::PreChorus, T::Chorus, T::Instrumental,
                                   T::Verse, T::PreChorus, T::Chorus, T::Bridge, T::Chorus,
                                   T::Outro});
    CHECK(same_partition(groups, {0, 1, 2, 3, 0, 1, 2, 3, 4, 3, 0}));
  }
  SECTION("V P C B V P C O, O quiet") {
    CHECK(form_labels({{V, 32, 0.5f},
                       {P, 16, 0.4f},
                       {C, 32, 1},
                       {B, 32, 0.6f},
                       {V, 32, 0.5f},
                       {P, 16, 0.4f},
                       {C, 32, 1},
                       {O, 16, 0.05f}}) == std::vector<T>{T::Verse, T::PreChorus, T::Chorus,
                                                          T::Bridge, T::Verse, T::PreChorus,
                                                          T::Chorus, T::Outro});
  }
  SECTION("C V C V C, chorus first") {
    CHECK(
        form_labels({{C, 32, 0.8f}, {V, 32, 0.8f}, {C, 32, 0.8f}, {V, 32, 0.8f}, {C, 32, 0.8f}}) ==
        std::vector<T>{T::Chorus, T::Verse, T::Chorus, T::Verse, T::Chorus});
  }
  SECTION("I V C V C C', a longer modulated last chorus") {
    CHECK(form_labels(
              {{I, 16, 0.1f}, {V, 32, 0.6f}, {C, 32, 1}, {V, 32, 0.6f}, {C, 32, 1}, {X, 48, 1}}) ==
          std::vector<T>{T::Intro, T::Verse, T::Chorus, T::Verse, T::Chorus, T::Unknown});
  }
  SECTION("A A B A") {
    CHECK(form_labels({{C, 32, 1}, {C, 32, 1}, {B, 32, 0.8f}, {C, 32, 1}}) ==
          std::vector<T>{T::Chorus, T::Chorus, T::Bridge, T::Chorus});
  }
  SECTION("V V C O, one chorus") {
    CHECK(form_labels({{V, 32, 0.6f}, {V, 32, 0.6f}, {C, 32, 1}, {O, 16, 0.05f}}) ==
          std::vector<T>{T::Chorus, T::Chorus, T::Unknown, T::Outro});
  }
  SECTION("R V R V R R, a riff outweighing a louder verse") {
    CHECK(form_labels(
              {{C, 32, 1}, {V, 32, 1.5f}, {C, 32, 1}, {V, 32, 1.5f}, {C, 32, 1}, {C, 32, 1}}) ==
          std::vector<T>{T::Chorus, T::Verse, T::Chorus, T::Verse, T::Chorus, T::Chorus});
  }
  SECTION("D E F G, through-composed") {
    CHECK(form_labels({{I, 32, 0.1f}, {V, 32, 0.8f}, {P, 32, 0.9f}, {C, 32, 1}}) ==
          std::vector<T>{T::Intro, T::Unknown, T::Unknown, T::Unknown});
  }
  SECTION("B D B D, an unvoiced build and drop") {
    // D is the Chorus by energy and B the Verse; with no vocals the edges read as Intro/Outro.
    const auto labels =
        form_labels({{B, 32, 0.5f, 0.1f}, {C, 32, 1, 0.1f}, {B, 32, 0.5f, 0.1f}, {C, 32, 1, 0.1f}});
    CHECK(labels == std::vector<T>{T::Intro, T::Chorus, T::Verse, T::Outro});
  }
  SECTION("C C C, one material only") {
    std::vector<int> groups;
    CHECK(form_labels({{C, 32, 1}, {C, 32, 1}, {C, 32, 1}}, &groups) == std::vector<T>{T::Unknown});
    CHECK(groups == std::vector<int>{0});
  }
  SECTION("I C C C") {
    CHECK(form_labels({{I, 16, 0.1f}, {C, 32, 1}, {C, 32, 1}, {C, 32, 1}}) ==
          std::vector<T>{T::Intro, T::Chorus, T::Chorus, T::Chorus});
  }
  SECTION("V C1 V C2a C2b C3, the second chorus split") {
    CHECK(form_labels({{V, 32, 0.6f},
                       {C, 32, 1},
                       {V, 32, 0.6f},
                       {C, 16, 1, 0.9f, 0},
                       {C, 16, 1, 0.9f, 16},
                       {C, 32, 1}}) ==
          std::vector<T>{T::Verse, T::Chorus, T::Verse, T::Chorus, T::Chorus});
  }
  SECTION("V C V C C, the last chorus doubled") {
    CHECK(form_labels({{V, 32, 0.6f}, {C, 32, 1}, {V, 32, 0.6f}, {C, 32, 1}, {C, 32, 1}}) ==
          std::vector<T>{T::Verse, T::Chorus, T::Verse, T::Chorus, T::Chorus});
  }
  SECTION("X Y Y, a pair with no other occurrence") {
    CHECK(form_labels({{X, 16, 0.1f}, {Y, 32, 1}, {Y, 32, 1}}) ==
          std::vector<T>{T::Intro, T::Chorus, T::Chorus});
  }
}

TEST_CASE("the column kernel is truncated at the ends down to half its width",
          "[section_structure]") {
  // A change at column c in [h/2, h) of a 16-column kernel (h = 8).
  const auto detect = [](int change, std::vector<uint8_t> eligible) {
    const int n = 40;
    std::vector<float> features;
    std::vector<float> times;
    for (int c = 0; c < n; ++c) {
      features.push_back(c < change ? 1.0f : 0.0f);
      features.push_back(c < change ? 0.0f : 1.0f);
      times.push_back(0.5f * static_cast<float>(c));
    }
    if (eligible.empty()) eligible.assign(static_cast<std::size_t>(n), 1);
    BoundaryConfig config;
    config.kernel_size = 16;
    config.peak_distance = 1.0f;
    return BoundaryDetector(features, 2, times, std::move(eligible), config);
  };
  const BoundaryDetector at_six = detect(6, {});
  // Half-width 6: + blocks sum 2 * 36, cross blocks 0, over (2 * 6)^2.
  CHECK(std::abs(at_six.novelty_curve()[6] * at_six.novelty_peak() - 0.5f) < 1e-6f);
  CHECK(at_six.novelty_curve()[3] == 0.0f);  // below half the width: not computed
  REQUIRE(at_six.boundaries().size() == 1);
  CHECK(at_six.boundaries()[0].frame == 6);
  CHECK(at_six.boundaries()[0].time == 3.0f);
  CHECK(at_six.sample_rate() == 0);
  CHECK(at_six.hop_length() == 0);
  // A masked column is no candidate.
  std::vector<uint8_t> mask(40, 1);
  mask[6] = 0;
  for (const Boundary& b : detect(6, mask).boundaries()) CHECK(b.frame != 6);
  // A change closer to the start than half the kernel is not computed where it is.
  for (const Boundary& b : detect(3, {}).boundaries()) CHECK(b.frame != 3);
}

TEST_CASE("analysis confidences stay in range", "[section_structure]") {
  const SectionConfig config = grid_config();
  const Audio audio = grid_material(Material::kStructured, 22050);
  const SectionAnalyzer analyzer(audio, make_grid(GridKind::kBeatsDownbeats),
                                 analysis_spectrogram(audio, config), config);
  for (const Section& section : analyzer.sections()) {
    CHECK(section.confidence >= 0.0f);
    CHECK(section.confidence <= 1.0f);
    if (section.type != SectionType::Unknown) CHECK(section.confidence >= 0.5f);
    CHECK(section.energy_level >= 0.0f);
    CHECK(section.energy_level <= 1.0f);
  }
  for (float score : analyzer.section_self_similarity()) {
    CHECK(score >= 0.0f);
    CHECK(score <= 1.0f);
  }
}

TEST_CASE("a steady tone with four onsets: its grid and meter confidence", "[section_structure]") {
  // Recorded, not judged: whether it gets the beat or the fallback grid, and how sure the meter
  // is, are what decide the downbeat-confidence gate.
  const Audio audio = song::steady_tone_four_onsets(22050);
  const BeatAnalyzer beats(audio, BeatConfig());
  const SectionGrid grid = ss::make_section_grid(beats);
  REQUIRE_NOTHROW(validate_section_grid(grid));
  const float confidence = beats.time_signature().confidence;
  CAPTURE(grid.beat_times.size(), confidence);
  CHECK(confidence >= 0.0f);
  CHECK(confidence <= 1.0f);
  CHECK(grid.downbeats_reliable == (confidence >= ss::kDownbeatConfidence));
}
