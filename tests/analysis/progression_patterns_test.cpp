#include "analysis/progression_patterns.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <set>
#include <string>
#include <vector>

using sonare::known_progression_patterns;
using sonare::ProgressionPattern;

namespace {

const ProgressionPattern& pattern_named(const std::string& name) {
  const auto& patterns = known_progression_patterns();
  auto it = std::find_if(patterns.begin(), patterns.end(),
                         [&](const ProgressionPattern& p) { return p.name == name; });
  REQUIRE(it != patterns.end());
  return *it;
}

}  // namespace

TEST_CASE("royalRoad is the subdominant-rooted IV-V-iii-vi cycle", "[analysis][progression]") {
  // The named progression is defined by its chord content, so the table is
  // asserted against the interval spelling rather than against itself. Quality
  // 0 is major and 1 is minor, matching diatonic_triads().
  const std::vector<std::pair<int, int>> expected = {
      {5, 0},  // IV
      {7, 0},  // V
      {4, 1},  // iii
      {9, 1},  // vi
  };
  REQUIRE(pattern_named("royalRoad").chords == expected);
}

TEST_CASE("komuro is the vi-IV-V-I cycle", "[analysis][progression]") {
  const std::vector<std::pair<int, int>> expected = {
      {9, 1},  // vi
      {5, 0},  // IV
      {7, 0},  // V
      {0, 0},  // I
  };
  REQUIRE(pattern_named("komuro").chords == expected);
}

TEST_CASE("no two known progressions have identical chords", "[analysis][progression]") {
  // Matching is positional against bars rather than cyclic, so two entries that
  // are rotations of one another still score differently and both stay
  // reachable -- komuro and fifties are such a pair. An exact duplicate is the
  // case that does not survive: it scores identically at every position, and
  // the scorer keeps the first strictly-better match, so the later entry could
  // never be reported.
  const auto& patterns = known_progression_patterns();
  for (size_t i = 0; i < patterns.size(); ++i) {
    for (size_t j = i + 1; j < patterns.size(); ++j) {
      INFO(patterns[i].name << " and " << patterns[j].name << " have identical chords");
      REQUIRE(patterns[i].chords != patterns[j].chords);
    }
  }
}

TEST_CASE("known progression names are unique and non-empty", "[analysis][progression]") {
  std::set<std::string> seen;
  for (const auto& pattern : known_progression_patterns()) {
    REQUIRE(!pattern.name.empty());
    REQUIRE(!pattern.chords.empty());
    INFO("duplicate progression name " << pattern.name);
    REQUIRE(seen.insert(pattern.name).second);
  }
  // Floor against the table being emptied out from under these assertions.
  REQUIRE(known_progression_patterns().size() >= 8);
}

TEST_CASE("known progression chords are in-range triad spellings", "[analysis][progression]") {
  for (const auto& pattern : known_progression_patterns()) {
    for (const auto& chord : pattern.chords) {
      INFO(pattern.name << " has an out-of-range chord");
      REQUIRE(chord.first >= 0);
      REQUIRE(chord.first < 12);
      REQUIRE(chord.second >= 0);
      REQUIRE(chord.second <= 2);
    }
  }
}
