/// @file section_analyzer_test.cpp
/// @brief Tests for section analyzer.

#include "analysis/section_analyzer.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/spectrum.h"
#include "feature/chroma.h"
#include "support/section_form.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"

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

/// @brief Two 1.5 s tones a fifth apart, so a boundary at 1.5 s separates two
///        stretches the chroma descriptors can tell apart.
Audio create_two_part_audio(int sr = 22050) {
  constexpr float kDuration = 3.0f;
  const int n_samples = static_cast<int>(static_cast<float>(sr) * kDuration);
  std::vector<float> samples(static_cast<size_t>(n_samples));

  for (int i = 0; i < n_samples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    const float frequency = t < 0.5f * kDuration ? 261.63f : 392.00f;
    samples[static_cast<size_t>(i)] =
        0.5f * std::sin(2.0f * sonare::constants::kPiD * frequency * t);
  }

  return Audio::from_vector(std::move(samples), sr);
}

/// @brief The STFT geometry @ref SectionAnalyzer analyzes @p config with.
StftConfig section_stft_config(const SectionConfig& config) {
  ChromaConfig chroma_config;
  chroma_config.n_fft = config.n_fft;
  chroma_config.hop_length = config.hop_length;
  return chroma_config.to_stft_config();
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

TEST_CASE("A section analysis handed its STFT matches one that built its own",
          "[section_analyzer]") {
  const Audio audio = create_two_part_audio();

  // A framing none of the defaults produce: a spectrogram that had quietly
  // reverted to one would be read here as frames of a hop this call never asked
  // for, and the section spans would land on the wrong ones.
  SectionConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.min_section_sec = 1.0f;
  const std::vector<float> boundaries = {1.5f};

  const Spectrogram spec = Spectrogram::compute(audio, section_stft_config(config));
  const SectionAnalyzer own(audio, boundaries, config);
  const SectionAnalyzer shared(audio, boundaries, spec, config);

  REQUIRE(own.count() == 2);
  REQUIRE(shared.count() == own.count());
  REQUIRE(shared.form() == own.form());
  for (size_t index = 0; index < own.sections().size(); ++index) {
    const Section& expected = own.sections()[index];
    const Section& measured = shared.sections()[index];
    REQUIRE(measured.start == expected.start);
    REQUIRE(measured.end == expected.end);
    REQUIRE(measured.energy_level == expected.energy_level);
    REQUIRE(measured.confidence == expected.confidence);
    REQUIRE(measured.type == expected.type);
  }
  REQUIRE(shared.section_self_similarity() == own.section_self_similarity());
}

TEST_CASE("A section analysis rejects an STFT that is not the one it asked for",
          "[section_analyzer]") {
  const Audio audio = create_two_part_audio();
  SectionConfig config;
  config.n_fft = 1024;
  config.hop_length = 256;
  config.min_section_sec = 1.0f;
  const std::vector<float> boundaries = {1.5f};

  SECTION("a different hop") {
    SectionConfig other = config;
    other.hop_length = config.hop_length * 2;
    const Spectrogram spec = Spectrogram::compute(audio, section_stft_config(other));
    REQUIRE_THROWS_AS(SectionAnalyzer(audio, boundaries, spec, config), SonareException);
  }

  SECTION("an STFT of the native-rate signal") {
    // Above 22.05 kHz the analyzer measures a downsampled copy, so a spectrogram
    // of the input as handed in describes frames of a different hop duration.
    const Audio high_rate = create_two_part_audio(44100);
    const Spectrogram spec = Spectrogram::compute(high_rate, section_stft_config(config));
    REQUIRE_THROWS_AS(SectionAnalyzer(high_rate, boundaries, spec, config), SonareException);
  }
}

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
  // minimum keeps all five detected sections. The detected spans sit just
  // either side of 4 s, so the default 4 s floor drops the boundaries that close
  // the short ones and keeps the rest: three sections. Asserting only
  // non-emptiness passed even when the fixture collapsed to a single whole-track
  // span, which is the failure this pins.
  REQUIRE(analyzer.count() == 5);
  REQUIRE(SectionAnalyzer(audio, SectionConfig{}).count() == 3);
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

TEST_CASE("a bridge reaches the caller instead of being demoted to Unknown", "[section_analyzer]") {
  // The Bridge branch assigned a flat confidence that sat under the same
  // function's demotion floor, so every Bridge it identified was rewritten to
  // Unknown before leaving the analyzer and the enum value was unreachable in
  // output. Its four sibling branches all score themselves from the evidence
  // that selected them; this pins that Bridge does too.
  //
  // A search for the label alone would pass on a build that never enters the
  // branch, so the confidence is asserted as well: a Bridge has to arrive with
  // a score that came from its own vocal likelihood rather than with the
  // constant that used to be there.
  constexpr int sr = 22050;
  // Interior, non-repeating, and inside the vocal band, which is the exact
  // shape the branch claims. The repeated tone around it is what leaves this
  // one segment without a repeat to match.
  const std::array<float, 6> hz = {220.0f, 330.0f, 330.0f, 880.0f, 330.0f, 330.0f};
  std::vector<float> samples(static_cast<size_t>(sr) * 30);
  for (size_t i = 0; i < samples.size(); ++i) {
    const float time = static_cast<float>(i) / static_cast<float>(sr);
    const size_t seg = std::min<size_t>(hz.size() - 1, static_cast<size_t>(time / 5.0f));
    samples[i] = 0.4f * std::sin(sonare::constants::kTwoPi * hz[seg] * time);
  }
  SectionAnalyzer analyzer(Audio::from_vector(samples, sr));

  const auto& sections = analyzer.sections();
  const auto bridge = std::find_if(sections.begin(), sections.end(),
                                   [](const Section& s) { return s.type == SectionType::Bridge; });
  REQUIRE(bridge != sections.end());
  REQUIRE(bridge->confidence > 0.5f);
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

TEST_CASE("the self-similarity matrix is the one the labeller used", "[analysis][section][reuse]") {
  // The analyzer keeps the descriptors classification built rather than running the chroma
  // analysis again when this accessor is read, and it derives that chromagram from the STFT it
  // already computed for the rest of the pass. Both are only correct if the result is the same
  // floats a standalone chromagram over the reported section spans produces, so the oracle
  // rebuilds exactly that and compares without a tolerance.
  const int sr = 22050;
  const Audio audio = create_sectioned_audio(sr);
  const SectionAnalyzer analyzer(audio);

  const size_t n = analyzer.count();
  REQUIRE(n > 1);

  const SectionConfig section_config;
  ChromaConfig chroma_config;
  chroma_config.n_fft = section_config.n_fft;
  chroma_config.hop_length = section_config.hop_length;
  const Chroma chroma = Chroma::compute(audio, chroma_config);

  // The analysis rate is the input rate here: section analysis only resamples above 22.05 kHz.
  const float hop_duration = static_cast<float>(section_config.hop_length) / static_cast<float>(sr);
  std::vector<std::array<float, 12>> expected(n);
  for (size_t s = 0; s < n; ++s) {
    const Section& section = analyzer.sections()[s];
    const int start = std::clamp(static_cast<int>(section.start / hop_duration), 0,
                                 std::max(0, chroma.n_frames()));
    const int end =
        std::clamp(static_cast<int>(section.end / hop_duration), start, chroma.n_frames());
    int count = 0;
    for (int f = start; f < end; ++f) {
      for (int c = 0; c < chroma.n_chroma() && c < 12; ++c) {
        expected[s][static_cast<size_t>(c)] += chroma.at(c, f);
      }
      ++count;
    }
    if (count > 0) {
      for (float& value : expected[s]) value /= static_cast<float>(count);
    }
    normalize_l2(expected[s].data(), expected[s].size());
  }

  const std::vector<float> similarity = analyzer.section_self_similarity();
  REQUIRE(similarity.size() == n * n);
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j < n; ++j) {
      float dot = 0.0f;
      for (size_t c = 0; c < 12; ++c) {
        dot += expected[i][c] * expected[j][c];
      }
      CAPTURE(i, j);
      REQUIRE(similarity[i * n + j] == std::clamp(dot, 0.0f, 1.0f));
    }
  }

  // Reading it twice must not depend on having read it once: the descriptors are state now.
  REQUIRE(analyzer.section_self_similarity() == similarity);
}

namespace {

/// @brief Consecutive single-partial tones, one per span, at pitch classes far
///        enough apart that no two neighbours read as indistinct chroma.
Audio create_tone_spans(const std::vector<std::pair<float, float>>& spans, int sr = 22050) {
  const float duration = spans.back().first;
  std::vector<float> samples(static_cast<size_t>(static_cast<float>(sr) * duration));
  for (size_t i = 0; i < samples.size(); ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(sr);
    float frequency = spans.back().second;
    for (const auto& span : spans) {
      if (t < span.first) {
        frequency = span.second;
        break;
      }
    }
    samples[i] = 0.5f * std::sin(2.0f * sonare::constants::kPiD * frequency * t);
  }
  return Audio::from_vector(std::move(samples), sr);
}

/// @brief Section boundaries of @p analyzer, i.e. every section start but the first.
std::vector<float> section_starts(const SectionAnalyzer& analyzer) {
  std::vector<float> starts;
  for (size_t i = 1; i < analyzer.sections().size(); ++i) {
    starts.push_back(analyzer.sections()[i].start);
  }
  return starts;
}

}  // namespace

TEST_CASE("the section floor keeps the stronger boundary next to a short section",
          "[section_analyzer]") {
  // 0-5 s C4, 5-6 s E4, 6-12 s G#4. The 1 s span must go, and which of its two
  // boundaries goes with it is decided by novelty strength, not by position.
  const Audio audio = create_tone_spans({{5.0f, 261.63f}, {6.0f, 329.63f}, {12.0f, 415.30f}});
  SectionConfig config;
  config.min_section_sec = 4.0f;
  const Spectrogram spec = Spectrogram::compute(audio, section_stft_config(config));

  const auto boundary = [](float time, float strength) { return Boundary{time, 0, strength}; };
  {
    const SectionAnalyzer analyzer(audio, {boundary(5.0f, 0.9f), boundary(6.0f, 0.2f)}, spec,
                                   config);
    REQUIRE(section_starts(analyzer) == std::vector<float>{5.0f});
  }
  {
    const SectionAnalyzer analyzer(audio, {boundary(5.0f, 0.2f), boundary(6.0f, 0.9f)}, spec,
                                   config);
    REQUIRE(section_starts(analyzer) == std::vector<float>{6.0f});
  }
}

TEST_CASE("the section floor keeps boundaries between sections longer than it",
          "[section_analyzer]") {
  // Spans of 10, 1, 3 and 16 s under a 4 s floor, all boundaries equally strong.
  // Dropping only 11 s joins the two short spans into one 4 s section; absorbing
  // the short spans leftward would also remove the boundary at 10 s.
  const Audio audio =
      create_tone_spans({{10.0f, 261.63f}, {11.0f, 329.63f}, {14.0f, 415.30f}, {30.0f, 293.66f}});
  SectionConfig config;
  config.min_section_sec = 4.0f;

  const SectionAnalyzer analyzer(audio, std::vector<float>{10.0f, 11.0f, 14.0f}, config);
  REQUIRE(section_starts(analyzer) == std::vector<float>{10.0f, 14.0f});
}

namespace {

/// @brief Adds a decaying note whose partials fall as 1 / n^tilt.
void add_note(std::vector<float>& out, int sr, double start, double length, int midi,
              double amplitude, int partials, double tilt) {
  const double f0 = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
  const size_t first = static_cast<size_t>(start * sr);
  const size_t count = static_cast<size_t>(length * sr);
  for (size_t i = 0; i < count && first + i < out.size(); ++i) {
    const double t = static_cast<double>(i) / sr;
    const double envelope = std::min(1.0, t / 0.01) * std::exp(-1.5 * t);
    double value = 0.0;
    for (int n = 1; n <= partials && f0 * n < 0.45 * sr; ++n) {
      value += std::sin(2.0 * sonare::constants::kPiD * f0 * n * t) / std::pow(n, tilt);
    }
    out[first + i] += static_cast<float>(amplitude * envelope * value);
  }
}

/// @brief Intro, verse, chorus, verse, chorus, outro at 0 / 4 / 4+L / ... / 4+4L s.
/// @details Every interior change moves timbre, register, density and chord set
///          at once: a low two-note bass figure; a mellow mid-register C Am F G
///          on the half note; a bright high-register Ab Eb Bb F on the eighth
///          with an octave doubling and noise hats.
Audio create_arrangement(double verse_seconds, int sr = 22050) {
  const double s = verse_seconds;
  const std::array<double, 7> edges{0.0,         4.0,         4.0 + s,    4.0 + 2 * s,
                                    4.0 + 3 * s, 4.0 + 4 * s, 9.0 + 4 * s};
  const std::array<int, 6> kind{0, 1, 2, 1, 2, 0};
  const int verse[4][3] = {{48, 52, 55}, {45, 48, 52}, {41, 45, 48}, {43, 47, 50}};
  const int chorus[4][3] = {{68, 72, 75}, {63, 67, 70}, {70, 74, 77}, {65, 69, 72}};
  std::vector<float> out(static_cast<size_t>(edges[6] * sr), 0.0f);
  uint32_t seed = 12345u;
  for (size_t k = 0; k < kind.size(); ++k) {
    int bar = 0;
    for (double b = edges[k]; b < edges[k + 1] - 1e-6; b += 2.0, ++bar) {
      const double end = std::min(b + 2.0, edges[k + 1]);
      if (kind[k] == 0) {
        add_note(out, sr, b, std::min(1.0, end - b), 36, 0.10, 4, 2.0);
        if (b + 1.0 < end) add_note(out, sr, b + 1.0, end - b - 1.0, 43, 0.10, 4, 2.0);
      } else if (kind[k] == 1) {
        for (double q = b; q < end - 1e-6; q += 1.0) {
          for (int n : verse[bar % 4])
            add_note(out, sr, q, std::min(1.0, end - q), n, 0.08, 6, 2.0);
        }
      } else {
        for (double q = b; q < end - 1e-6; q += 0.5) {
          for (int n : chorus[bar % 4])
            add_note(out, sr, q, std::min(0.5, end - q), n, 0.07, 16, 1.0);
          add_note(out, sr, q, std::min(0.5, end - q), chorus[bar % 4][0] + 12, 0.07, 16, 1.0);
        }
        for (double h = b; h < end - 1e-6; h += 0.25) {
          const size_t first = static_cast<size_t>(h * sr);
          float previous = 0.0f;
          for (size_t i = 0; i < static_cast<size_t>(0.05 * sr) && first + i < out.size(); ++i) {
            seed = seed * 1664525u + 1013904223u;
            const float white = static_cast<float>(seed >> 8) / 16777216.0f * 2.0f - 1.0f;
            const double t = static_cast<double>(i) / sr;
            out[first + i] += static_cast<float>(0.25 * std::exp(-60.0 * t) * (white - previous));
            previous = white;
          }
        }
      }
    }
  }
  return Audio::from_vector(std::move(out), sr);
}

/// @brief True when some entry of @p times lies within @p tolerance of @p target.
bool has_time_near(const std::vector<float>& times, float target, float tolerance) {
  return std::any_of(times.begin(), times.end(),
                     [&](float t) { return std::fabs(t - target) <= tolerance; });
}

}  // namespace

TEST_CASE("a section floor below the section length keeps the arrangement's boundaries",
          "[section_analyzer][.][slow]") {
  constexpr float kTolerance = 0.25f;
  SECTION("the detector finds every interior arrangement change") {
    BoundaryConfig config;
    config.peak_distance = 4.0f;
    const std::vector<float> times = detect_boundaries(create_arrangement(8.0), config);
    INFO("boundaries: " << ::Catch::Detail::stringify(times));
    for (float change : {12.0f, 20.0f, 28.0f, 36.0f})
      REQUIRE(has_time_near(times, change, kTolerance));
  }
  SECTION("10 s sections under the default 4 s floor") {
    // A weak in-chorus novelty peak 4 s into each chorus leaves a short span
    // there; the floor must dissolve that weak peak, not the section change.
    SectionConfig config;
    config.min_section_sec = 4.0f;
    const std::vector<float> starts =
        section_starts(SectionAnalyzer(create_arrangement(10.0), config));
    INFO("section starts: " << ::Catch::Detail::stringify(starts));
    for (float change : {14.0f, 24.0f, 34.0f}) REQUIRE(has_time_near(starts, change, kTolerance));
  }
  SECTION("8 s sections under a 6 s floor") {
    SectionConfig config;
    config.min_section_sec = 6.0f;
    const std::vector<float> starts =
        section_starts(SectionAnalyzer(create_arrangement(8.0), config));
    INFO("section starts: " << ::Catch::Detail::stringify(starts));
    for (float change : {12.0f, 20.0f, 28.0f}) REQUIRE(has_time_near(starts, change, kTolerance));
  }
}
