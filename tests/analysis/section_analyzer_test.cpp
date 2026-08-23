/// @file section_analyzer_test.cpp
/// @brief Tests for section analyzer.

#include "analysis/section_analyzer.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "support/section_form.h"
#include "util/constants.h"

using namespace sonare;
using Catch::Matchers::WithinAbs;

namespace {

/// @brief Creates 20 seconds of audio as five sections of four seconds each.
/// @param sr Sample rate in Hz.
/// @details Sections differ in pitch and harmonic richness as well as level:
///
///   0-4s    F3, 1 partial,  level 0.2   quiet opening
///   4-8s    C4, 3 partials, level 0.5
///   8-12s   G4, 4 partials, level 0.9   loudest and brightest
///   12-16s  C4, 3 partials, level 0.5   repeats 4-8s
///   16-20s  F3, 1 partial,  level 0.2   repeats 0-4s
///
/// Level alone would not be enough, and the pitch contrast is not decoration. An
/// earlier version of this fixture stepped only its amplitude, and once the
/// boundary detector gained an absolute novelty floor it produced no boundaries at
/// all, leaving every test here iterating one whole-track span. That is correct
/// behaviour, not a regression: the detector reads MFCC and chroma, where a change
/// of level turns the feature vector about five times less than a comparable
/// change of pitch, which leaves it quieter than steady noise. **It cannot be
/// recovered by lowering the threshold** — anything low enough to admit it admits
/// noise first. So the sections have to differ in what the detector actually
/// reads. The level plateaus are kept on top of that so cases relying on energy
/// ordering still hold.
///
/// Section *labels* are deliberately not asserted anywhere: the classifier is a
/// fixed-threshold heuristic and this signal is not shaped like a pop song.
Audio create_sectioned_audio(int sr = 22050) {
  struct SectionTone {
    float amplitude;
    float frequency;
    int partials;
  };
  constexpr float kDuration = 20.0f;
  constexpr float kSectionSeconds = 4.0f;
  // Keeps the richest section inside [-1, 1] once its partials are summed.
  constexpr float kHeadroom = 0.45f;
  constexpr float kPartialDecay = 0.6f;
  const std::array<SectionTone, 5> tones = {{{0.2f, 174.61f, 1},
                                             {0.5f, 261.63f, 3},
                                             {0.9f, 392.00f, 4},
                                             {0.5f, 261.63f, 3},
                                             {0.2f, 174.61f, 1}}};

  const int n_samples = static_cast<int>(static_cast<float>(sr) * kDuration);
  std::vector<float> samples(static_cast<size_t>(n_samples));

  for (int i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    const size_t index = static_cast<size_t>(
        std::min<int>(static_cast<int>(tones.size()) - 1, static_cast<int>(t / kSectionSeconds)));
    const SectionTone& tone = tones[index];

    float value = 0.0f;
    float weight = 1.0f;
    for (int harmonic = 1; harmonic <= tone.partials; ++harmonic) {
      value += weight * std::sin(2.0f * sonare::constants::kPiD * tone.frequency *
                                 static_cast<float>(harmonic) * t);
      weight *= kPartialDecay;
    }
    samples[static_cast<size_t>(i)] = tone.amplitude * kHeadroom * value;
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Creates simple sine wave.
Audio create_sine(float freq, int sr = 22050, float duration = 10.0f) {
  int n_samples = static_cast<int>(sr * duration);
  std::vector<float> samples(n_samples);

  for (int i = 0; i < n_samples; ++i) {
    float t = static_cast<float>(i) / sr;
    samples[i] = 0.5f * std::sin(2.0f * sonare::constants::kPiD * freq * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

}  // namespace

TEST_CASE("SectionAnalyzer basic", "[section_analyzer]") {
  Audio audio = create_sine(440.0f);

  SectionConfig config;
  SectionAnalyzer analyzer(audio, config);

  // A steady tone changes in nothing the detector reads, so the analyzer returns
  // its single whole-track fallback span. One is the answer here, not a lower
  // bound -- any other count would mean the detector cut a tone that never
  // changed.
  REQUIRE(analyzer.count() == 1);
}

TEST_CASE("SectionAnalyzer sections", "[section_analyzer]") {
  Audio audio = create_sectioned_audio();

  SectionConfig config;
  config.min_section_sec = 2.0f;
  config.boundary_threshold = 0.2f;

  SectionAnalyzer analyzer(audio, config);

  const auto& sections = analyzer.sections();

  // The fixture is built as five sections and this configuration resolves all
  // five, so pin the count: a fixture the detector cannot separate would leave
  // the loop below iterating one whole-track span and asserting nothing.
  REQUIRE(sections.size() == 5);

  // Each section should have valid timing
  for (const auto& section : sections) {
    REQUIRE(section.start >= 0.0f);
    REQUIRE(section.end > section.start);
    REQUIRE(section.duration() > 0.0f);
    REQUIRE(section.energy_level >= 0.0f);
    REQUIRE(section.energy_level <= 1.0f);
    REQUIRE(section.confidence >= 0.0f);
    REQUIRE(section.confidence <= 1.0f);
  }

  // Sections should cover the audio
  if (!sections.empty()) {
    REQUIRE_THAT(sections.front().start, WithinAbs(0.0f, 0.1f));
  }
}

TEST_CASE("SectionAnalyzer form", "[section_analyzer]") {
  Audio audio = create_sectioned_audio();

  SectionConfig config;
  SectionAnalyzer analyzer(audio, config);

  std::string form = analyzer.form();

  // Form should be a string of section characters
  REQUIRE(!form.empty());

  // Each character must name some section type. The valid set is derived from
  // section_type_to_char() rather than spelled out here: the same alphabet was
  // hand-written in two test files, and adding Unknown to the enum's reachable
  // set left one of them asserting a stale list that passed only because no
  // fixture happened to produce a '?'.
  INFO("form " << form);
  REQUIRE(sonare::test::is_section_form(form));
}

TEST_CASE("every section type has a distinct form character", "[section_analyzer]") {
  // form() is a string of one character per section, so two types sharing a
  // character would silently merge them in every form comparison -- and would
  // also make is_section_form() accept a character no type actually renders.
  std::vector<char> chars;
  for (SectionType type : sonare::test::kAllSectionTypes) {
    chars.push_back(section_type_to_char(type));
  }
  std::sort(chars.begin(), chars.end());
  REQUIRE(std::adjacent_find(chars.begin(), chars.end()) == chars.end());
  REQUIRE(section_type_to_char(SectionType::Unknown) == '?');
}

TEST_CASE("SectionAnalyzer section_at", "[section_analyzer]") {
  Audio audio = create_sectioned_audio();

  SectionAnalyzer analyzer(audio);

  // Get section at middle of audio
  Section section = analyzer.section_at(10.0f);

  REQUIRE(section.start <= 10.0f);
  REQUIRE(section.end >= 10.0f);
}

TEST_CASE("SectionAnalyzer duration", "[section_analyzer]") {
  Audio audio = create_sine(440.0f, 22050, 10.0f);

  SectionAnalyzer analyzer(audio);

  // Duration should match audio duration
  REQUIRE_THAT(analyzer.duration(), WithinAbs(10.0f, 1.0f));
}

TEST_CASE("SectionAnalyzer boundary_times", "[section_analyzer]") {
  Audio audio = create_sectioned_audio();

  SectionConfig config;
  config.boundary_threshold = 0.2f;

  SectionAnalyzer analyzer(audio, config);

  auto boundaries = analyzer.boundary_times();

  // One boundary per section change. Pinned because the ordering check below is
  // vacuous on an empty vector, which is what an undetectable fixture produces.
  REQUIRE(boundaries.size() == 4);

  // Boundaries should be sorted
  for (size_t i = 1; i < boundaries.size(); ++i) {
    REQUIRE(boundaries[i] > boundaries[i - 1]);
  }
}

TEST_CASE("SectionAnalyzer Section type_string", "[section_analyzer]") {
  Section section;
  section.start = 0.0f;
  section.end = 1.0f;
  section.energy_level = 0.5f;
  section.confidence = 0.8f;

  section.type = SectionType::Intro;
  REQUIRE(section.type_string() == "Intro");

  section.type = SectionType::Verse;
  REQUIRE(section.type_string() == "Verse");

  section.type = SectionType::Chorus;
  REQUIRE(section.type_string() == "Chorus");

  section.type = SectionType::Bridge;
  REQUIRE(section.type_string() == "Bridge");

  section.type = SectionType::Outro;
  REQUIRE(section.type_string() == "Outro");
}

TEST_CASE("section_type_to_char", "[section_analyzer]") {
  REQUIRE(section_type_to_char(SectionType::Intro) == 'I');
  REQUIRE(section_type_to_char(SectionType::Verse) == 'A');
  REQUIRE(section_type_to_char(SectionType::Chorus) == 'B');
  REQUIRE(section_type_to_char(SectionType::Bridge) == 'C');
  REQUIRE(section_type_to_char(SectionType::Outro) == 'O');
}

TEST_CASE("section_type_to_string", "[section_analyzer]") {
  REQUIRE(section_type_to_string(SectionType::Intro) == "Intro");
  REQUIRE(section_type_to_string(SectionType::Verse) == "Verse");
  REQUIRE(section_type_to_string(SectionType::PreChorus) == "Pre-Chorus");
  REQUIRE(section_type_to_string(SectionType::Chorus) == "Chorus");
  REQUIRE(section_type_to_string(SectionType::Bridge) == "Bridge");
  REQUIRE(section_type_to_string(SectionType::Instrumental) == "Instrumental");
  REQUIRE(section_type_to_string(SectionType::Outro) == "Outro");
}

TEST_CASE("SectionAnalyzer config options", "[section_analyzer]") {
  Audio audio = create_sectioned_audio();

  SectionConfig config;
  config.min_section_sec = 1.0f;
  config.boundary_threshold = 0.1f;

  const SectionAnalyzer analyzer(audio, config);

  // The point of the case is that the options reach the analysis, so assert the
  // outcome they change rather than that some sections came back. A permissive
  // minimum keeps all five detected sections; the default 4 s minimum is longer
  // than the first detected span, which merges into its neighbour and cascades.
  // Asserting only non-emptiness passed even when the fixture collapsed to a
  // single whole-track span, which is the failure this pins.
  REQUIRE(analyzer.count() == 5);
  REQUIRE(SectionAnalyzer(audio, SectionConfig{}).count() == 2);
}

TEST_CASE("SectionAnalyzer short audio", "[section_analyzer]") {
  Audio audio = create_sine(440.0f, 22050, 3.0f);

  SectionConfig config;
  config.min_section_sec = 1.0f;

  SectionAnalyzer analyzer(audio, config);

  // Three seconds is only twice the checkerboard kernel's span, and the tone does
  // not change anyway, so the whole clip comes back as one span. What this pins is
  // that a clip that short still analyzes at all rather than throwing or coming
  // back empty -- and that it returns exactly the one span, not a stray cut made
  // out of the little room the kernel has.
  REQUIRE(analyzer.count() == 1);
}

TEST_CASE("SectionAnalyzer section_at out of range", "[section_analyzer]") {
  Audio audio = create_sine(440.0f, 22050, 5.0f);

  SectionAnalyzer analyzer(audio);

  // Time beyond audio
  Section section = analyzer.section_at(100.0f);

  REQUIRE(section.duration() == 0.0f);
  REQUIRE(section.confidence == 0.0f);
}

TEST_CASE("SectionAnalyzer enforces min_section_sec at its configured value",
          "[section_analyzer]") {
  // The 4s..6s span is half of min_section_sec: long enough to survive a
  // half-value floor, short enough that the documented floor must merge it.
  const std::vector<float> boundaries{4.0f, 6.0f, 10.0f};
  SectionConfig config;
  config.min_section_sec = 4.0f;

  const SectionAnalyzer analyzer(create_sectioned_audio(), boundaries, config);
  const auto& sections = analyzer.sections();

  // Four spans in, the 4s..6s one merged, three out. Exact rather than a lower
  // bound: the boundaries are supplied, so nothing here is estimated, and a merge
  // that ate one span too many would land on two -- the very failure this case
  // was written to catch.
  REQUIRE(sections.size() == 3);
  for (const auto& section : sections) {
    REQUIRE(section.duration() >= config.min_section_sec);
  }
}

TEST_CASE("SectionAnalyzer is stable across common source rates", "[section_analyzer]") {
  const std::vector<float> boundaries{4.0f, 8.0f, 12.0f, 16.0f};
  SectionConfig config;
  config.min_section_sec = 2.0f;

  const SectionAnalyzer at_44100(create_sectioned_audio(44100), boundaries, config);
  const SectionAnalyzer at_48000(create_sectioned_audio(48000), boundaries, config);

  // Pin the absolute count before comparing the two rates. An equality between
  // them holds just as well when both collapse, and the comparison below would
  // then be one whole-track span against another -- agreement about nothing.
  REQUIRE(at_44100.sections().size() == 5);
  REQUIRE(at_44100.form() == at_48000.form());
  REQUIRE(at_44100.sections().size() == at_48000.sections().size());
  for (size_t i = 0; i < at_44100.sections().size(); ++i) {
    REQUIRE_THAT(at_44100.sections()[i].energy_level,
                 WithinAbs(at_48000.sections()[i].energy_level, 0.01f));
  }
}

TEST_CASE("a track with no detected structure is reported as unidentified", "[section_analyzer]") {
  // A steady tone gives the boundary detector nothing to cut on, so the whole
  // track comes back as one span. That span is the segmenter reporting failure,
  // not a finding that the track is one long verse: a caller filtering on Verse
  // would otherwise collect every unanalysable track alongside the sections the
  // classifier actually identified, with no way to tell them apart.
  const int sr = 22050;
  std::vector<float> samples(static_cast<size_t>(sr) * 10);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = 0.3f * std::sin(sonare::constants::kTwoPi * 440.0f * static_cast<float>(i) /
                                 static_cast<float>(sr));
  }
  SectionAnalyzer analyzer(Audio::from_vector(samples, sr));

  REQUIRE(analyzer.count() == 1);
  const Section section = analyzer.sections()[0];
  REQUIRE(section.type == SectionType::Unknown);
  REQUIRE(section.confidence == 0.0f);
}

TEST_CASE("an unclassified edge segment is not labelled as a verse", "[section_analyzer]") {
  // Every positive branch of the classifier claims something specific, and
  // together they cover every repeat and every interior segment. What is left is
  // one shape only: a loud, non-repeating first or last segment -- a cold open
  // or a loud final section. Labelling that Verse asserts a reading nothing
  // measured, and merges it with the repeated sections the repeat branch does
  // identify, leaving a caller no way to separate the two.
  //
  // Both fixtures below were chosen by sweeping until they reached that branch;
  // the ordinary low-intro/loud-chorus fixture in this file never does, so it
  // cannot stand in for them.
  //
  // Each pins its section count. That is not the claim being made, it is what
  // keeps the claim meaningful: front() and back() are only different segments
  // while there is more than one, and a collapse to a single whole-track span
  // would satisfy Unknown-with-zero-confidence for free, leaving both branches
  // green while checking nothing.
  const auto segmented = [](const std::array<std::pair<float, float>, 4>& segments) {
    constexpr int sr = 22050;
    std::vector<float> samples(static_cast<size_t>(sr) * 20);
    for (size_t i = 0; i < samples.size(); ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(sr);
      const size_t seg = std::min<size_t>(3, static_cast<size_t>(t / 5.0f));
      samples[i] =
          segments[seg].first * std::sin(sonare::constants::kTwoPi * segments[seg].second * t);
    }
    return Audio::from_vector(samples, sr);
  };

  SECTION("a loud non-repeating opening") {
    SectionAnalyzer analyzer(
        segmented({{{0.95f, 660.0f}, {0.30f, 330.0f}, {0.35f, 392.0f}, {0.28f, 294.0f}}}));
    REQUIRE(analyzer.count() == 2);
    const Section first = analyzer.sections().front();
    REQUIRE(first.type == SectionType::Unknown);
    REQUIRE(first.confidence == 0.0f);
  }

  SECTION("a loud non-repeating ending") {
    SectionAnalyzer analyzer(
        segmented({{{0.30f, 330.0f}, {0.32f, 330.0f}, {0.30f, 330.0f}, {0.98f, 740.0f}}}));
    REQUIRE(analyzer.count() == 2);
    const Section last = analyzer.sections().back();
    REQUIRE(last.type == SectionType::Unknown);
    REQUIRE(last.confidence == 0.0f);
  }
}

TEST_CASE("SectionAnalyzer does not invent structure in uniform material", "[section_analyzer]") {
  // Twenty seconds of one unchanging chord, cut at four-second boundaries the
  // caller supplies. Nothing in the audio changes at those cuts, so nothing in
  // the audio is a section: the segments are indistinguishable from each other,
  // which also makes every one of them a "repetition" of every other. Before
  // the indistinct-merge and the uniform-repetition guard, that pair of facts
  // was enough to produce a full verse/chorus alternation out of material that
  // never moved.
  constexpr int sr = 22050;
  constexpr float kDuration = 20.0f;
  const int n_samples = static_cast<int>(static_cast<float>(sr) * kDuration);
  std::vector<float> samples(static_cast<size_t>(n_samples), 0.0f);
  for (int i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    for (float frequency : {261.63f, 329.63f, 392.00f}) {
      samples[static_cast<size_t>(i)] +=
          0.28f * std::sin(sonare::constants::kTwoPi * frequency * t);
    }
  }
  const Audio audio = Audio::from_vector(std::move(samples), sr);

  const std::vector<float> boundaries = {4.0f, 8.0f, 12.0f, 16.0f};
  SectionAnalyzer analyzer(audio, boundaries);

  // The supplied boundaries separate nothing, so they are not kept.
  REQUIRE(analyzer.count() == 1);
  REQUIRE(analyzer.sections().front().start == 0.0f);
  REQUIRE_THAT(analyzer.sections().front().end, WithinAbs(kDuration, 0.05f));

  // And the one span left is not named. Uniform material supports no reading of
  // musical function, so asserting one would be a claim the audio cannot back.
  const std::string form = analyzer.form();
  REQUIRE(form.find('A') == std::string::npos);
  REQUIRE(form.find('B') == std::string::npos);
}
