/// @file section_structure_test.cpp
/// @brief Section layout recovery on the synthetic song of support/structure_song.h.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "analysis/section_analyzer.h"
#include "support/structure_song.h"
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

void check_layout(const Audio& audio, float tolerance_sec) {
  const SectionAnalyzer analyzer(audio, SectionConfig());
  INFO(describe(analyzer));
  CHECK(boundaries_match(internal_boundaries(analyzer), song::true_boundaries(), tolerance_sec));
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
