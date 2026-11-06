/// @file key_synthetic_matrix_test.cpp
/// @brief Synthetic matrix tests for key detection.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <vector>

#include "analysis/chord_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/key_profiles.h"

using namespace sonare;

namespace {

std::array<float, 12> boosted_major_chroma(PitchClass root, KeyProfileType profile_type) {
  KeyProfileBoosts boosts;
  boosts.tonic = key_constants::kMajorTonicBoost;
  boosts.third = key_constants::kMajorThirdBoost;
  boosts.fifth = key_constants::kMajorFifthBoost;
  boosts.seventh = 1.0f;
  return normalize_profile(get_boosted_major_profile(root, boosts, profile_type));
}

std::array<float, 12> boosted_minor_chroma(PitchClass root, KeyProfileType profile_type) {
  KeyProfileBoosts boosts;
  boosts.tonic = key_constants::kMinorTonicBoost;
  boosts.third = key_constants::kMinorThirdBoost;
  boosts.fifth = key_constants::kMinorFifthBoost;
  boosts.seventh = key_constants::kMinorSeventhBoost;
  return normalize_profile(get_boosted_minor_profile(root, boosts, profile_type));
}

std::array<float, 12> boosted_mode_chroma(PitchClass root, Mode mode, KeyProfileType profile_type) {
  KeyProfileBoosts boosts;
  boosts.tonic = key_constants::kMinorTonicBoost;
  boosts.third = key_constants::kMinorThirdBoost;
  boosts.fifth = key_constants::kMinorFifthBoost;
  boosts.seventh = key_constants::kMinorSeventhBoost;
  return normalize_profile(get_boosted_mode_profile(root, mode, boosts, profile_type));
}

Chord chord(PitchClass root, ChordQuality quality, float start, float end) {
  return Chord{root, quality, start, end, 1.0f};
}

PitchClass transpose(PitchClass root, int semitones) {
  return static_cast<PitchClass>((static_cast<int>(root) + semitones) % 12);
}

/// Chroma candidates over @p modes: @p given keys carry their posteriors, the rest split the rest.
std::vector<KeyCandidate> chroma_candidates(const std::vector<Key>& given,
                                            const std::vector<Mode>& modes = {Mode::Major,
                                                                              Mode::Minor}) {
  float given_mass = 0.0f;
  for (const Key& key : given) given_mass += key.confidence;
  const size_t others = modes.size() * 12 - given.size();
  std::vector<KeyCandidate> out;
  for (Mode mode : modes) {
    for (int root = 0; root < 12; ++root) {
      Key key{static_cast<PitchClass>(root), mode, (1.0f - given_mass) / others};
      for (const Key& g : given) {
        if (g.root == key.root && g.mode == key.mode) key.confidence = g.confidence;
      }
      out.push_back({key, 0.0f});
    }
  }
  return out;
}

}  // namespace

TEST_CASE("KeyAnalyzer synthetic chroma matrix detects all major and minor keys",
          "[key_analyzer][synthetic_matrix]") {
  for (KeyProfileType profile_type :
       {KeyProfileType::KrumhanslSchmuckler, KeyProfileType::Temperley, KeyProfileType::Shaath,
        KeyProfileType::FaraldoEDMT, KeyProfileType::FaraldoEDMA, KeyProfileType::FaraldoEDMM,
        KeyProfileType::BellmanBudge}) {
    KeyConfig config;
    config.profile_type = profile_type;

    for (int root_idx = 0; root_idx < 12; ++root_idx) {
      auto root = static_cast<PitchClass>(root_idx);

      CAPTURE(root_idx);
      CAPTURE(static_cast<int>(profile_type));

      KeyAnalyzer major_analyzer(boosted_major_chroma(root, profile_type), config);
      REQUIRE(major_analyzer.key().root == root);
      REQUIRE(major_analyzer.key().mode == Mode::Major);
      REQUIRE(major_analyzer.key().confidence > 0.5f);

      KeyAnalyzer minor_analyzer(boosted_minor_chroma(root, profile_type), config);
      REQUIRE(minor_analyzer.key().root == root);
      REQUIRE(minor_analyzer.key().mode == Mode::Minor);
      REQUIRE(minor_analyzer.key().confidence > 0.5f);
    }
  }
}

TEST_CASE("estimate_key_from_chords synthetic cadence matrix resolves all tonics",
          "[key_analyzer][synthetic_matrix]") {
  for (int root_idx = 0; root_idx < 12; ++root_idx) {
    auto tonic = static_cast<PitchClass>(root_idx);

    CAPTURE(root_idx);

    std::vector<Chord> major_cadence = {
        chord(tonic, ChordQuality::Major, 0.0f, 1.0f),
        chord(transpose(tonic, 5), ChordQuality::Major, 1.0f, 2.0f),
        chord(transpose(tonic, 7), ChordQuality::Major, 2.0f, 3.0f),
        chord(tonic, ChordQuality::Major, 3.0f, 4.0f),
    };
    Key major_key = estimate_key_from_chords(major_cadence);
    REQUIRE(major_key.root == tonic);
    REQUIRE(major_key.mode == Mode::Major);
    REQUIRE(major_key.confidence > 0.5f);

    std::vector<Chord> minor_cadence = {
        chord(tonic, ChordQuality::Minor, 0.0f, 1.0f),
        chord(transpose(tonic, 5), ChordQuality::Minor, 1.0f, 2.0f),
        chord(transpose(tonic, 7), ChordQuality::Major, 2.0f, 3.0f),
        chord(tonic, ChordQuality::Minor, 3.0f, 4.0f),
    };
    Key minor_key = estimate_key_from_chords(minor_cadence);
    REQUIRE(minor_key.root == tonic);
    REQUIRE(minor_key.mode == Mode::Minor);
    REQUIRE(minor_key.confidence > 0.5f);
  }
}

TEST_CASE("estimate_key_from_chords scores minor cadence/bookend bonuses symmetrically",
          "[key_analyzer][synthetic_matrix]") {
  // Regression for the major-bias defect: the minor branch used to receive no
  // cadence/bookend/first-chord bonus, so a progression that ends on an authentic
  // minor cadence (V major -> i minor) but contains prominent relative-major
  // chords was mis-detected as the relative major. Here C/G/C give C major a
  // strong tonic+dominant+first-chord lead, but the piece resolves E -> Am (an
  // A-minor V-i). With the symmetric minor cadence bonus the correct A-minor
  // reading wins; without it the relative major (C) would.
  std::vector<Chord> minor_cadence_in_major_context = {
      chord(PitchClass::C, ChordQuality::Major, 0.0f, 1.0f),
      chord(PitchClass::G, ChordQuality::Major, 1.0f, 2.0f),
      chord(PitchClass::C, ChordQuality::Major, 2.0f, 3.0f),
      chord(PitchClass::E, ChordQuality::Major, 3.0f, 4.0f),
      chord(PitchClass::A, ChordQuality::Minor, 4.0f, 5.0f),
  };
  Key key = estimate_key_from_chords(minor_cadence_in_major_context);
  REQUIRE(key.root == PitchClass::A);
  REQUIRE(key.mode == Mode::Minor);

  // The analogous all-minor bookended vamp (i ... i with a minor tonic at both
  // ends) must also resolve to minor rather than its relative major.
  std::vector<Chord> minor_bookended = {
      chord(PitchClass::A, ChordQuality::Minor, 0.0f, 1.0f),
      chord(PitchClass::D, ChordQuality::Minor, 1.0f, 2.0f),
      chord(PitchClass::E, ChordQuality::Major, 2.0f, 3.0f),
      chord(PitchClass::A, ChordQuality::Minor, 3.0f, 4.0f),
  };
  Key bookend_key = estimate_key_from_chords(minor_bookended);
  REQUIRE(bookend_key.root == PitchClass::A);
  REQUIRE(bookend_key.mode == Mode::Minor);
}

TEST_CASE("estimate_key_from_chords does not bias zero-score progressions to C major",
          "[key_analyzer][synthetic_matrix]") {
  std::vector<Chord> zero_duration = {
      chord(PitchClass::Fs, ChordQuality::Minor, 0.0f, 0.0f),
      chord(PitchClass::B, ChordQuality::Minor, 0.0f, 0.0f),
  };
  Key key = estimate_key_from_chords(zero_duration);
  REQUIRE(key.root == PitchClass::Fs);
  REQUIRE(key.mode == Mode::Minor);
  REQUIRE(key.confidence == 0.0f);
}

TEST_CASE("Modal profiles identify a mode from a scale histogram they were not built from",
          "[key_analyzer][key_profiles][synthetic_matrix]") {
  // The matrix case below feeds each profile back to itself, which proves the
  // profiles are distinguishable but says nothing about whether they describe
  // the mode they claim to. This one builds the chroma from the *scale* instead:
  // flat weight on every scale degree, a tonic and fifth emphasis that says
  // which note the scale is heard from, and nothing outside the scale. The
  // weights are chosen here and share no value with the profile construction,
  // so a profile that merely encodes its own shape cannot pass.
  //
  // The emphasis is what the case turns on. Every mode shares its pitch-class
  // set with six others, so a histogram with no tonic emphasis identifies the
  // set and not the mode; the profiles have to read the emphasis.
  struct ModeScale {
    Mode mode;
    std::array<int, 7> degrees;
  };
  const ModeScale scales[] = {
      {Mode::Dorian, {0, 2, 3, 5, 7, 9, 10}},  {Mode::Phrygian, {0, 1, 3, 5, 7, 8, 10}},
      {Mode::Lydian, {0, 2, 4, 6, 7, 9, 11}},  {Mode::Mixolydian, {0, 2, 4, 5, 7, 9, 10}},
      {Mode::Locrian, {0, 1, 3, 5, 6, 8, 10}},
  };

  KeyConfig config;
  config.modes = {Mode::Major,  Mode::Minor,      Mode::Dorian, Mode::Phrygian,
                  Mode::Lydian, Mode::Mixolydian, Mode::Locrian};

  for (const ModeScale& entry : scales) {
    for (int root_idx = 0; root_idx < 12; ++root_idx) {
      const auto root = static_cast<PitchClass>(root_idx);
      CAPTURE(root_idx, static_cast<int>(entry.mode));

      std::array<float, 12> chroma{};
      chroma.fill(0.05f);
      for (int degree : entry.degrees) {
        chroma[(root_idx + degree) % 12] = 1.0f;
      }
      chroma[root_idx] = 2.0f;
      // Locrian is the one mode whose fifth is diminished; emphasising the
      // perfect fifth there would describe a scale it does not have.
      const int fifth = entry.mode == Mode::Locrian ? 6 : 7;
      chroma[(root_idx + fifth) % 12] = 1.4f;

      KeyAnalyzer analyzer(chroma, config);
      CAPTURE(analyzer.key().to_string());
      REQUIRE(analyzer.key().root == root);
      REQUIRE(analyzer.key().mode == entry.mode);
    }
  }
}

TEST_CASE("KeyAnalyzer modal synthetic matrix is opt-in", "[key_analyzer][synthetic_matrix]") {
  const Mode modes[] = {Mode::Dorian, Mode::Phrygian, Mode::Lydian, Mode::Mixolydian,
                        Mode::Locrian};

  KeyConfig config;
  config.modes = {Mode::Major,  Mode::Minor,      Mode::Dorian, Mode::Phrygian,
                  Mode::Lydian, Mode::Mixolydian, Mode::Locrian};

  for (Mode mode : modes) {
    for (int root_idx = 0; root_idx < 12; ++root_idx) {
      auto root = static_cast<PitchClass>(root_idx);

      CAPTURE(root_idx);
      CAPTURE(static_cast<int>(mode));

      KeyAnalyzer analyzer(boosted_mode_chroma(root, mode, *config.profile_type), config);
      REQUIRE(analyzer.key().root == root);
      REQUIRE(analyzer.key().mode == mode);
      REQUIRE(analyzer.key().confidence > 0.5f);
    }
  }
}

TEST_CASE("N.C. segments carry no key evidence in the chord-based estimate",
          "[key_analyzer][synthetic_matrix]") {
  // N.C. is ChordQuality::Unknown with a placeholder root of C.
  const auto nc = [](float start, float end) {
    return chord(PitchClass::C, ChordQuality::Unknown, start, end);
  };
  const PitchClass g = PitchClass::G;
  const std::vector<Chord> progression = {
      chord(g, ChordQuality::Major, 1.0f, 2.0f),
      chord(transpose(g, 5), ChordQuality::Major, 2.0f, 3.0f),
      chord(transpose(g, 7), ChordQuality::Major, 3.0f, 4.0f),
      chord(g, ChordQuality::Major, 4.0f, 5.0f),
  };
  std::vector<Chord> bracketed = {nc(0.0f, 1.0f)};
  bracketed.insert(bracketed.end(), progression.begin(), progression.end());
  bracketed.push_back(nc(5.0f, 9.0f));

  const Key plain = estimate_key_from_chords(progression);
  const Key with_nc = estimate_key_from_chords(bracketed);
  REQUIRE(plain.root == g);
  CHECK(with_nc.root == plain.root);
  CHECK(with_nc.mode == plain.mode);
  CHECK(with_nc.confidence == plain.confidence);

  // A timeline of nothing but N.C. leaves the chroma estimate as it was.
  const Key chroma{PitchClass::F, Mode::Major, 0.5f};
  const Key refined =
      refine_key_with_chords(chroma_candidates({chroma}), {nc(0.0f, 4.0f), nc(4.0f, 8.0f)});
  CHECK(refined.root == chroma.root);
  CHECK(refined.mode == chroma.mode);
  CHECK(refined.confidence == chroma.confidence);
}

TEST_CASE("refine_key_with_chords lets the chords choose only among keys the chroma leaves open",
          "[key_analyzer][synthetic_matrix]") {
  const PitchClass c = PitchClass::C;
  const std::vector<Chord> c_cadence = {
      chord(c, ChordQuality::Major, 0.0f, 2.0f),
      chord(transpose(c, 5), ChordQuality::Major, 2.0f, 4.0f),
      chord(transpose(c, 7), ChordQuality::Dominant7, 4.0f, 6.0f),
      chord(c, ChordQuality::Major, 6.0f, 8.0f),
  };
  REQUIRE(estimate_key_from_chords(c_cadence).root == c);

  SECTION("an ambiguous chroma defers to the progression") {
    const Key refined = refine_key_with_chords(
        chroma_candidates({{PitchClass::F, Mode::Major, 0.5f}, {c, Mode::Major, 0.4f}}), c_cadence);
    CHECK(refined.root == c);
    CHECK(refined.mode == Mode::Major);
    CHECK(refined.confidence == 0.4f);
  }

  SECTION("a key the chroma rules out is never published") {
    const Key refined = refine_key_with_chords(
        chroma_candidates({{PitchClass::F, Mode::Major, 0.95f}, {c, Mode::Major, 0.004f}}),
        c_cadence);
    CHECK(refined.root == PitchClass::F);
    CHECK(refined.mode == Mode::Major);
    CHECK(refined.confidence == 0.95f);
  }

  SECTION("a key the chroma never scored is never published") {
    const PitchClass a = PitchClass::A;
    const std::vector<Chord> a_minor = {
        chord(a, ChordQuality::Minor, 0.0f, 2.0f),
        chord(transpose(a, 5), ChordQuality::Minor, 2.0f, 4.0f),
        chord(transpose(a, 7), ChordQuality::Major, 4.0f, 6.0f),
        chord(a, ChordQuality::Minor, 6.0f, 8.0f),
    };
    REQUIRE(estimate_key_from_chords(a_minor).mode == Mode::Minor);
    const Key refined =
        refine_key_with_chords(chroma_candidates({{c, Mode::Major, 0.6f}}, {Mode::Major}), a_minor);
    CHECK(refined.mode == Mode::Major);
    CHECK(refined.root == c);
  }
}

TEST_CASE("refine_key_with_chords uses recurrent relative tonics when chroma is decisive",
          "[key_analyzer][relative_tonic]") {
  const auto relative_major = [](PitchClass minor) { return transpose(minor, 3); };
  const auto minor_loop = [](PitchClass tonic, int repeats, float first_start) {
    const float d = 2.0f;
    std::vector<Chord> out;
    float t = first_start;
    for (int repeat = 0; repeat < repeats; ++repeat) {
      out.push_back(chord(tonic, ChordQuality::Minor, t, t + d));
      t += d;
      out.push_back(chord(transpose(tonic, 8), ChordQuality::Major, t, t + d));
      t += d;
      out.push_back(chord(transpose(tonic, 3), ChordQuality::Major, t, t + d));
      t += d;
      out.push_back(chord(transpose(tonic, 10), ChordQuality::Major, t, t + d));
      t += d;
    }
    return out;
  };

  SECTION("minor reading is transposition invariant and keeps the chroma posterior") {
    for (int root = 0; root < 12; ++root) {
      const auto tonic = static_cast<PitchClass>(root);
      const auto major = relative_major(tonic);
      const auto progression = minor_loop(tonic, 3, 0.0f);
      const Key progression_key = estimate_key_from_chords(progression);
      REQUIRE(progression_key.root == tonic);
      REQUIRE(progression_key.mode == Mode::Minor);
      const Key refined = refine_key_with_chords(
          chroma_candidates({{Key{major, Mode::Major, 0.97f}, Key{tonic, Mode::Minor, 0.004f}}}),
          progression);
      CAPTURE(root, refined.to_string(), refined.confidence);
      CHECK(refined.root == tonic);
      CHECK(refined.mode == Mode::Minor);
      CHECK(refined.confidence == 0.004f);
    }
  }

  SECTION("N.C. does not change a valid recurrent relative-key reading") {
    const PitchClass tonic = PitchClass::A;
    const PitchClass major = relative_major(tonic);
    std::vector<Chord> progression;
    float t = 0.0f;
    for (int repeat = 0; repeat < 3; ++repeat) {
      progression.push_back(chord(tonic, ChordQuality::Minor, t, t + 2.0f));
      t += 2.0f;
      progression.push_back(chord(transpose(tonic, 8), ChordQuality::Major, t, t + 2.0f));
      t += 2.0f;
      if (repeat < 2) {
        progression.push_back(chord(PitchClass::C, ChordQuality::Unknown, t, t + 1.0f));
        t += 1.0f;
      }
      progression.push_back(chord(transpose(tonic, 3), ChordQuality::Major, t, t + 2.0f));
      t += 2.0f;
      progression.push_back(chord(transpose(tonic, 10), ChordQuality::Major, t, t + 2.0f));
      t += 2.0f;
    }
    const Key refined = refine_key_with_chords(
        chroma_candidates({{Key{major, Mode::Major, 0.97f}, Key{tonic, Mode::Minor, 0.004f}}}),
        progression);
    CHECK(refined.root == tonic);
    CHECK(refined.mode == Mode::Minor);
    CHECK(refined.confidence == 0.004f);
  }

  SECTION("the mirrored major reading can win from repeated tonic resolution") {
    const PitchClass tonic = PitchClass::C;
    std::vector<Chord> progression;
    float t = 0.0f;
    for (int repeat = 0; repeat < 3; ++repeat) {
      progression.push_back(chord(tonic, ChordQuality::Major, t, t + 2.0f));
      t += 2.0f;
      progression.push_back(chord(transpose(tonic, 7), ChordQuality::Major, t, t + 2.0f));
      t += 2.0f;
      progression.push_back(chord(transpose(tonic, 9), ChordQuality::Minor, t, t + 2.0f));
      t += 2.0f;
      progression.push_back(chord(transpose(tonic, 5), ChordQuality::Major, t, t + 2.0f));
      t += 2.0f;
    }
    const Key progression_key = estimate_key_from_chords(progression);
    REQUIRE(progression_key.root == tonic);
    REQUIRE(progression_key.mode == Mode::Major);
    const Key refined = refine_key_with_chords(
        chroma_candidates(
            {{Key{PitchClass::A, Mode::Minor, 0.97f}, Key{tonic, Mode::Major, 0.004f}}}),
        progression);
    CHECK(refined.root == tonic);
    CHECK(refined.mode == Mode::Major);
    CHECK(refined.confidence == 0.004f);
  }

  SECTION("rotating the chord order does not invent an opening tonic") {
    const PitchClass tonic = PitchClass::A;
    std::vector<Chord> progression;
    float t = 0.0f;
    for (int repeat = 0; repeat < 3; ++repeat) {
      progression.push_back(chord(transpose(tonic, 8), ChordQuality::Major, t, t + 2.0f));
      t += 2.0f;
      progression.push_back(chord(transpose(tonic, 3), ChordQuality::Major, t, t + 2.0f));
      t += 2.0f;
      progression.push_back(chord(transpose(tonic, 10), ChordQuality::Major, t, t + 2.0f));
      t += 2.0f;
      progression.push_back(chord(tonic, ChordQuality::Minor, t, t + 2.0f));
      t += 2.0f;
    }
    const PitchClass major = relative_major(tonic);
    const Key progression_key = estimate_key_from_chords(progression);
    REQUIRE(progression_key.root == major);
    REQUIRE(progression_key.mode == Mode::Major);
    const Key refined = refine_key_with_chords(
        chroma_candidates({{Key{tonic, Mode::Minor, 0.97f}, Key{major, Mode::Major, 0.004f}}}),
        progression);
    CHECK(refined.root == tonic);
    CHECK(refined.mode == Mode::Minor);
  }

  SECTION("equal consecutive runs do not count as separate tonic returns") {
    const PitchClass tonic = PitchClass::A;
    const PitchClass major = relative_major(tonic);
    std::vector<Chord> progression;
    float t = 0.0f;
    for (int repeat = 0; repeat < 2; ++repeat) {
      for (int part = 0; part < 5; ++part) {
        progression.push_back(chord(tonic, ChordQuality::Minor, t, t + 1.0f));
        t += 1.0f;
      }
      progression.push_back(chord(transpose(tonic, 8), ChordQuality::Major, t, t + 1.0f));
      t += 1.0f;
      progression.push_back(chord(transpose(tonic, 3), ChordQuality::Major, t, t + 1.0f));
      t += 1.0f;
      progression.push_back(chord(transpose(tonic, 10), ChordQuality::Major, t, t + 1.0f));
      t += 1.0f;
    }
    const Key progression_key = estimate_key_from_chords(progression);
    REQUIRE(progression_key.root == tonic);
    REQUIRE(progression_key.mode == Mode::Minor);
    const Key refined = refine_key_with_chords(
        chroma_candidates({{Key{major, Mode::Major, 0.97f}, Key{tonic, Mode::Minor, 0.004f}}}),
        progression);
    CHECK(refined.root == major);
    CHECK(refined.mode == Mode::Major);
  }

  SECTION("a short pickup is not the opening tonic evidence") {
    const PitchClass tonic = PitchClass::A;
    const PitchClass major = relative_major(tonic);
    auto progression = minor_loop(tonic, 3, 0.1f);
    progression.front().start = 0.0f;
    progression.front().end = 0.1f;
    const Key refined = refine_key_with_chords(
        chroma_candidates({{Key{major, Mode::Major, 0.97f}, Key{tonic, Mode::Minor, 0.004f}}}),
        progression);
    CHECK(refined.root == major);
    CHECK(refined.mode == Mode::Major);
  }

  SECTION("one cycle or weak template confidence is insufficient") {
    const PitchClass tonic = PitchClass::A;
    const PitchClass major = relative_major(tonic);
    std::vector<Chord> one_cycle = {
        chord(tonic, ChordQuality::Minor, 0.0f, 2.0f),
        chord(transpose(tonic, 8), ChordQuality::Major, 2.0f, 4.0f),
        chord(transpose(tonic, 3), ChordQuality::Major, 4.0f, 6.0f),
        chord(transpose(tonic, 10), ChordQuality::Major, 6.0f, 8.0f),
        chord(tonic, ChordQuality::Minor, 8.0f, 10.0f),
    };
    REQUIRE(estimate_key_from_chords(one_cycle).root == tonic);
    REQUIRE(estimate_key_from_chords(one_cycle).mode == Mode::Minor);
    const auto candidates =
        chroma_candidates({{Key{major, Mode::Major, 0.97f}, Key{tonic, Mode::Minor, 0.004f}}});
    Key refined = refine_key_with_chords(candidates, one_cycle);
    CHECK(refined.root == major);
    CHECK(refined.mode == Mode::Major);

    auto weak = minor_loop(tonic, 3, 0.0f);
    for (Chord& item : weak) item.confidence = 0.60f;
    refined = refine_key_with_chords(candidates, weak);
    CHECK(refined.root == major);
    CHECK(refined.mode == Mode::Major);

    auto mixed_strength = minor_loop(tonic, 3, 0.0f);
    mixed_strength[8].confidence = 0.60f;
    float confidence_mass = 0.0f;
    float total_duration = 0.0f;
    for (const Chord& item : mixed_strength) {
      confidence_mass += item.duration() * item.confidence;
      total_duration += item.duration();
    }
    REQUIRE(confidence_mass / total_duration > 0.8f);
    REQUIRE(estimate_key_from_chords(mixed_strength).root == tonic);
    REQUIRE(estimate_key_from_chords(mixed_strength).mode == Mode::Minor);
    refined = refine_key_with_chords(candidates, mixed_strength);
    CHECK(refined.root == major);
    CHECK(refined.mode == Mode::Major);

    auto nonfinite_confidence = minor_loop(tonic, 3, 0.0f);
    nonfinite_confidence[1].confidence = std::numeric_limits<float>::quiet_NaN();
    refined = refine_key_with_chords(candidates, nonfinite_confidence);
    CHECK(refined.root == major);
    CHECK(refined.mode == Mode::Major);
  }

  SECTION("unscored keys and nonpositive or N.C. segments do not create tonic returns") {
    const PitchClass tonic = PitchClass::A;
    const PitchClass major = relative_major(tonic);
    auto progression = minor_loop(tonic, 3, 0.0f);
    const Key only_major = refine_key_with_chords(
        chroma_candidates({Key{major, Mode::Major, 0.97f}}, {Mode::Major}), progression);
    CHECK(only_major.root == major);
    CHECK(only_major.mode == Mode::Major);

    std::vector<Chord> two_returns = {
        chord(tonic, ChordQuality::Minor, 0.0f, 2.0f),
        chord(transpose(tonic, 5), ChordQuality::Minor, 2.0f, 4.0f),
        chord(tonic, ChordQuality::Minor, 4.0f, 4.0f),
        chord(tonic, ChordQuality::Unknown, 4.0f, 5.0f),
        chord(transpose(tonic, 7), ChordQuality::Major, 5.0f, 7.0f),
        chord(transpose(tonic, 5), ChordQuality::Minor, 7.0f, 9.0f),
        chord(transpose(tonic, 7), ChordQuality::Major, 9.0f, 11.0f),
        chord(tonic, ChordQuality::Minor, 11.0f, 13.0f),
    };
    const Key chord_estimate = estimate_key_from_chords(two_returns);
    REQUIRE(chord_estimate.root == tonic);
    REQUIRE(chord_estimate.mode == Mode::Minor);
    const Key zero_duration = refine_key_with_chords(
        chroma_candidates({{Key{major, Mode::Major, 0.97f}, Key{tonic, Mode::Minor, 0.004f}}}),
        two_returns);
    CHECK(zero_duration.root == major);
    CHECK(zero_duration.mode == Mode::Major);
  }
}

TEST_CASE("estimate_key_from_chords weights only the terminal cadence runs",
          "[key_analyzer][cadence]") {
  SECTION("a long minor body resists a short cadence to its relative major") {
    for (int root = 0; root < 12; ++root) {
      const auto tonic = static_cast<PitchClass>(root);
      std::vector<Chord> progression;
      float t = 0.0f;
      for (int repeat = 0; repeat < 16; ++repeat) {
        progression.push_back(chord(tonic, ChordQuality::Minor, t, t + 1.0f));
        t += 1.0f;
        progression.push_back(chord(transpose(tonic, 8), ChordQuality::Major, t, t + 1.0f));
        t += 1.0f;
        progression.push_back(chord(transpose(tonic, 3), ChordQuality::Major, t, t + 1.0f));
        t += 1.0f;
        progression.push_back(chord(transpose(tonic, 10), ChordQuality::Major, t, t + 1.0f));
        t += 1.0f;
      }
      progression.push_back(chord(transpose(tonic, 3), ChordQuality::Major, t, t + 1.0f));

      const Key key = estimate_key_from_chords(progression);
      CAPTURE(root, key.to_string());
      CHECK(key.root == tonic);
      CHECK(key.mode == Mode::Minor);

      const PitchClass relative_major = transpose(tonic, 3);
      const Key refined = refine_key_with_chords(
          chroma_candidates(
              {{Key{tonic, Mode::Minor, 0.75f}, Key{relative_major, Mode::Major, 0.25f}}}),
          progression);
      CHECK(refined.root == tonic);
      CHECK(refined.mode == Mode::Minor);
      CHECK(refined.confidence == 0.75f);
    }
  }

  SECTION("extending an unrelated suspended prefix does not amplify a fixed minor cadence") {
    const auto c_centered_body = [](float prefix_duration) {
      return std::vector<Chord>{
          chord(PitchClass::Fs, ChordQuality::Sus4, 0.0f, prefix_duration),
          chord(PitchClass::C, ChordQuality::Major, prefix_duration, prefix_duration + 64.0f),
          chord(PitchClass::E, ChordQuality::Major, prefix_duration + 64.0f,
                prefix_duration + 65.0f),
          chord(PitchClass::A, ChordQuality::Minor, prefix_duration + 65.0f,
                prefix_duration + 66.0f),
      };
    };

    const Key short_prefix = estimate_key_from_chords(c_centered_body(1.0f));
    const Key long_prefix = estimate_key_from_chords(c_centered_body(100.0f));
    CHECK(short_prefix.root == PitchClass::C);
    CHECK(short_prefix.mode == Mode::Major);
    CHECK(long_prefix.root == PitchClass::C);
    CHECK(long_prefix.mode == Mode::Major);
  }

  SECTION("terminal dominant and tonic fragments with extensions remain one cadence") {
    std::vector<Chord> progression = {
        chord(PitchClass::C, ChordQuality::Major, 0.0f, 1.0f),
        chord(PitchClass::G, ChordQuality::Major, 1.0f, 2.0f),
        chord(PitchClass::C, ChordQuality::Major, 2.0f, 3.0f),
        chord(PitchClass::E, ChordQuality::Major, 3.0f, 3.5f),
        chord(PitchClass::E, ChordQuality::Dominant7, 3.5f, 4.0f),
        chord(PitchClass::A, ChordQuality::Minor, 4.0f, 4.25f),
        chord(PitchClass::A, ChordQuality::Minor7, 4.25f, 4.5f),
        chord(PitchClass::C, ChordQuality::Unknown, 4.5f, 5.0f),
        chord(PitchClass::E, ChordQuality::Major, 5.0f, 5.0f),
    };

    const Key key = estimate_key_from_chords(progression);
    CHECK(key.root == PitchClass::A);
    CHECK(key.mode == Mode::Minor);
  }

  SECTION("a later positive harmony prevents an earlier cadence from being terminal") {
    const std::vector<Chord> progression = {
        chord(PitchClass::C, ChordQuality::Major, 0.0f, 1.0f),
        chord(PitchClass::G, ChordQuality::Major, 1.0f, 2.0f),
        chord(PitchClass::C, ChordQuality::Major, 2.0f, 3.0f),
        chord(PitchClass::E, ChordQuality::Major, 3.0f, 4.0f),
        chord(PitchClass::A, ChordQuality::Minor, 4.0f, 5.0f),
        chord(PitchClass::Fs, ChordQuality::Sus4, 5.0f, 6.0f),
    };

    const Key key = estimate_key_from_chords(progression);
    CHECK(key.root == PitchClass::C);
    CHECK(key.mode == Mode::Major);
  }

  SECTION("short major and minor cadences still resolve after non-harmonic gaps") {
    const std::vector<Chord> major_cadence = {
        chord(PitchClass::C, ChordQuality::Major, 0.0f, 1.0f),
        chord(PitchClass::G, ChordQuality::Dominant7, 1.0f, 1.5f),
        chord(PitchClass::C, ChordQuality::Major7, 1.5f, 1.75f),
        chord(PitchClass::C, ChordQuality::Unknown, 1.75f, 2.0f),
        chord(PitchClass::F, ChordQuality::Major, 2.0f, 2.0f),
    };
    const std::vector<Chord> minor_cadence = {
        chord(PitchClass::A, ChordQuality::Minor, 0.0f, 1.0f),
        chord(PitchClass::E, ChordQuality::Dominant7, 1.0f, 1.5f),
        chord(PitchClass::A, ChordQuality::Minor7, 1.5f, 1.75f),
        chord(PitchClass::C, ChordQuality::Unknown, 1.75f, 2.0f),
        chord(PitchClass::D, ChordQuality::Major, 2.0f, 2.0f),
    };

    const Key major_key = estimate_key_from_chords(major_cadence);
    CHECK(major_key.root == PitchClass::C);
    CHECK(major_key.mode == Mode::Major);
    const Key minor_key = estimate_key_from_chords(minor_cadence);
    CHECK(minor_key.root == PitchClass::A);
    CHECK(minor_key.mode == Mode::Minor);
  }
}

TEST_CASE("refine_key_with_chords recognizes repeated natural-minor v-i motion",
          "[key_analyzer][relative_tonic]") {
  struct LoopChord {
    int interval;
    ChordQuality quality;
    float duration;
  };
  const std::vector<LoopChord> natural_minor = {
      {7, ChordQuality::Minor, 2.0f},  {0, ChordQuality::Minor, 2.0f},
      {8, ChordQuality::Major, 2.0f},  {3, ChordQuality::Major, 2.0f},
      {10, ChordQuality::Major, 1.0f},
  };
  const std::vector<LoopChord> reordered = {
      {7, ChordQuality::Minor, 2.0f},  {8, ChordQuality::Major, 2.0f},
      {0, ChordQuality::Minor, 2.0f},  {3, ChordQuality::Major, 2.0f},
      {10, ChordQuality::Major, 1.0f},
  };
  const std::vector<LoopChord> major_centered = {
      {7, ChordQuality::Minor, 2.0f}, {0, ChordQuality::Minor, 2.0f},
      {8, ChordQuality::Major, 2.0f}, {10, ChordQuality::Major, 2.0f},
      {3, ChordQuality::Major, 4.0f},
  };
  const auto progression = [](PitchClass tonic, int repeats, const std::vector<LoopChord>& pattern,
                              bool fragment_v_i = false) {
    std::vector<Chord> chords;
    float t = 0.0f;
    for (int repeat = 0; repeat < repeats; ++repeat) {
      for (const LoopChord& item : pattern) {
        const int pieces = fragment_v_i && (item.interval == 7 || item.interval == 0) ? 4 : 1;
        const float piece_duration = item.duration / static_cast<float>(pieces);
        for (int piece = 0; piece < pieces; ++piece) {
          chords.push_back(
              chord(transpose(tonic, item.interval), item.quality, t, t + piece_duration));
          t += piece_duration;
        }
      }
    }
    return chords;
  };
  const auto candidates = [](PitchClass tonic) {
    return chroma_candidates(
        {{Key{transpose(tonic, 3), Mode::Major, 0.97f}, Key{tonic, Mode::Minor, 0.004f}}});
  };

  SECTION("all twelve transpositions keep the original chroma posterior") {
    for (int root = 0; root < 12; ++root) {
      const PitchClass tonic = static_cast<PitchClass>(root);
      const auto chords = progression(tonic, 3, natural_minor);
      const Key chord_key = estimate_key_from_chords(chords);
      CAPTURE(root, chord_key.to_string());
      CHECK(chord_key.root == tonic);
      CHECK(chord_key.mode == Mode::Minor);
      const Key refined = refine_key_with_chords(candidates(tonic), chords);
      CHECK(refined.root == tonic);
      CHECK(refined.mode == Mode::Minor);
      CHECK(refined.confidence == 0.004f);
    }
  }

  SECTION("reordering the same chord bag supplies no v-i evidence") {
    const PitchClass tonic = PitchClass::A;
    const auto chords = progression(tonic, 3, reordered);
    const Key chord_key = estimate_key_from_chords(chords);
    CHECK(chord_key.root == transpose(tonic, 3));
    CHECK(chord_key.mode == Mode::Major);
    const Key refined = refine_key_with_chords(candidates(tonic), chords);
    CHECK(refined.root == transpose(tonic, 3));
    CHECK(refined.mode == Mode::Major);
    CHECK(refined.confidence == 0.97f);
  }

  SECTION("major-centered order keeps its relative-major reading") {
    const PitchClass tonic = PitchClass::A;
    const auto chords = progression(tonic, 3, major_centered);
    const Key refined = refine_key_with_chords(candidates(tonic), chords);
    CHECK(refined.root == transpose(tonic, 3));
    CHECK(refined.mode == Mode::Major);
    CHECK(refined.confidence == 0.97f);
  }

  SECTION("a short opening v pickup is not a strong recurrent v-i pattern") {
    const PitchClass tonic = PitchClass::A;
    auto chords = progression(tonic, 3, natural_minor);
    constexpr float kPickupDuration = 0.1f;
    const float shift = chords.front().duration() - kPickupDuration;
    chords.front().end = chords.front().start + kPickupDuration;
    for (size_t index = 1; index < chords.size(); ++index) {
      chords[index].start -= shift;
      chords[index].end -= shift;
    }
    const Key refined = refine_key_with_chords(candidates(tonic), chords);
    CHECK(refined.root == transpose(tonic, 3));
    CHECK(refined.mode == Mode::Major);
  }

  SECTION("two v-i cycles are insufficient") {
    const PitchClass tonic = PitchClass::A;
    const auto chords = progression(tonic, 2, natural_minor);
    const Key refined = refine_key_with_chords(candidates(tonic), chords);
    CHECK(refined.root == transpose(tonic, 3));
    CHECK(refined.mode == Mode::Major);
  }

  SECTION("weak v-i pairs do not pass by global average confidence") {
    const PitchClass tonic = PitchClass::A;
    auto chords = progression(tonic, 3, natural_minor);
    float confidence_mass = 0.0f;
    float total_duration = 0.0f;
    for (int repeat = 0; repeat < 3; ++repeat) {
      chords[static_cast<size_t>(repeat * 5)].confidence = 0.7f;
      chords[static_cast<size_t>(repeat * 5 + 1)].confidence = 0.7f;
    }
    for (const Chord& item : chords) {
      confidence_mass += item.duration() * item.confidence;
      total_duration += item.duration();
    }
    REQUIRE(confidence_mass / total_duration > 0.8f);
    const Key refined = refine_key_with_chords(candidates(tonic), chords);
    CHECK(refined.root == transpose(tonic, 3));
    CHECK(refined.mode == Mode::Major);
  }

  SECTION("each direct pair must be strong even when the opening and global average are strong") {
    const PitchClass tonic = PitchClass::A;
    for (const size_t weak_chord : {size_t{10}, size_t{11}}) {
      auto chords = progression(tonic, 3, natural_minor);
      chords[weak_chord].confidence = 0.7f;
      float confidence_mass = 0.0f;
      float total_duration = 0.0f;
      for (const Chord& item : chords) {
        confidence_mass += item.duration() * item.confidence;
        total_duration += item.duration();
      }
      REQUIRE(chords.front().confidence == 1.0f);
      REQUIRE(confidence_mass / total_duration > 0.8f);
      const Key chord_key = estimate_key_from_chords(chords);
      REQUIRE(chord_key.root == tonic);
      REQUIRE(chord_key.mode == Mode::Minor);
      const Key refined = refine_key_with_chords(candidates(tonic), chords);
      CHECK(refined.root == transpose(tonic, 3));
      CHECK(refined.mode == Mode::Major);
    }
  }

  SECTION("fragmented v and i runs still provide three strong resolutions") {
    const PitchClass tonic = PitchClass::A;
    const auto chords = progression(tonic, 3, natural_minor, true);
    const Key refined = refine_key_with_chords(candidates(tonic), chords);
    CHECK(refined.root == tonic);
    CHECK(refined.mode == Mode::Minor);
    CHECK(refined.confidence == 0.004f);
  }
}
