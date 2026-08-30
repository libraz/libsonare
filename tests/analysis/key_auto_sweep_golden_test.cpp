/// @file key_auto_sweep_golden_test.cpp
/// @brief Golden decisions for the automatic key-detection candidate sweep.
///
/// The corpus is rendered in code rather than loaded from disk, so this guards
/// the `genre_hint = "auto"` path without any audio in the repository. Each row
/// pins the detected key and mode plus the confidence and the ranked
/// candidates, which is what catches the case a decision-only check waves
/// through: the numbers move while the key columns do not.
///
/// The floats are compared to kTolerance rather than hashed, because the
/// pipeline does not reproduce bit-exactly across optimization levels — Debug
/// and Release agree on every decision and every candidate ordering and differ
/// on the values by up to 3.6e-7. A quantized hash makes that a coin toss: at a
/// 1e-6 step 15 of these 96 rows flip, and coarsening the step is not
/// monotonic, since a value can sit on a 1e-4 boundary while resting safely
/// inside a 1e-5 one.
///
/// This is a stability golden, **not** an accuracy benchmark. The corpus is
/// saturated — every row currently detects its own tonic — so the pass rate
/// says nothing about accuracy on real material; that is the optional-fixture
/// harness's job (see tests/fixtures/music_eval/README.md). The modal profiles
/// are outside it too: the auto path scores major and minor unless a caller
/// names `KeyConfig::modes`.
///
/// Runs for roughly 70 s, so it sits in the hidden `[golden]` tier rather than
/// the default ctest run — 96 tracks each costing one harmonic separation and
/// two chroma passes, which is coverage rather than harness overhead.
///
/// Regenerate with `SONARE_UPDATE_KEY_GOLDEN=1`.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "analysis/key_analyzer.h"
#include "core/audio.h"
#include "util/constants.h"

namespace {

using Catch::Matchers::WithinAbs;
using sonare::Audio;
using sonare::Key;
using sonare::KeyAnalyzer;
using sonare::KeyConfig;
using sonare::Mode;
using sonare::PitchClass;
using sonare::constants::kTwoPiD;

/// Scale degrees in semitones, major and natural minor.
constexpr std::array<int, 7> kMajorSteps = {0, 2, 4, 5, 7, 9, 11};
constexpr std::array<int, 7> kMinorSteps = {0, 2, 3, 5, 7, 8, 10};

/// Chord progressions as scale degrees, so every voicing stays diatonic.
constexpr std::array<std::array<int, 4>, 3> kProgressions = {
    {{0, 5, 3, 4}, {0, 3, 4, 0}, {5, 3, 0, 4}}};

double midi_hz(int midi) { return 440.0 * std::pow(2.0, (midi - 69) / 12.0); }

/// @brief Renders a chord progression in a known key.
/// @param root Tonic as a pitch class index
/// @param major True for major, false for natural minor
/// @param variant Selects the progression and the voicing octave
/// @param sr Sample rate
/// @details Decaying harmonic triads over a bass line, plus a percussive layer
///          on a fixed grid so the harmonic separation the sweep runs has
///          something to remove.
Audio render_track(int root, bool major, int variant, int sr) {
  // Two full turns of the four-chord progression, at one chord per second. The
  // progression rather than the duration is what the detector needs, so this is
  // the shortest signal that still presents the whole cadence twice.
  constexpr double kSeconds = 8.0;
  const int n = static_cast<int>(sr * kSeconds);
  std::vector<float> out(static_cast<size_t>(n), 0.0f);
  const auto& steps = major ? kMajorSteps : kMinorSteps;
  const auto& degrees = kProgressions[static_cast<size_t>(variant) % kProgressions.size()];
  const int chord_len = sr;
  const int beat = sr / 2;
  const int click_len = sr / 200;

  for (int i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / sr;
    const int degree = degrees[static_cast<size_t>((i / chord_len) % 4)];
    const double env = std::exp(-2.2 * (static_cast<double>(i % chord_len) / chord_len));

    double v = 0.0;
    for (int voice = 0; voice < 3; ++voice) {
      const int scale_index = degree + voice * 2;
      const int semitone = steps[static_cast<size_t>(scale_index % 7)] + 12 * (scale_index / 7);
      const double f = midi_hz(60 + root + semitone + (variant % 2) * 12);
      for (int harm = 1; harm <= 4; ++harm) {
        v += (0.30 / harm) * env * std::sin(kTwoPiD * f * harm * t);
      }
    }
    v += 0.45 * env *
         std::sin(kTwoPiD * midi_hz(36 + root + steps[static_cast<size_t>(degree % 7)]) * t);

    const int into_beat = i % beat;
    if (into_beat < click_len) {
      const double decay = 1.0 - static_cast<double>(into_beat) / click_len;
      const double noise = static_cast<double>((i * 1103515245 + 12345) & 0xFFFF) / 32768.0 - 1.0;
      v += 0.5 * decay * noise;
    }
    out[static_cast<size_t>(i)] = static_cast<float>(0.25 * v);
  }
  return Audio::from_vector(std::move(out), sr);
}

const char* pitch_class_short(PitchClass pc) {
  static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return kNames[static_cast<int>(pc) % 12];
}

const char* mode_short(Mode mode) {
  if (mode == Mode::Major) return "maj";
  if (mode == Mode::Minor) return "min";
  return "other";
}

/// Two hundred times the 3.6e-7 an optimization level moves these numbers by,
/// and still tight enough to see a real one: moving the Krumhansl-Schmuckler
/// major tonic weight by 0.9% reddens thirty of these assertions.
constexpr float kTolerance = 1.0e-4f;

/// One ranked key candidate as the golden records it.
struct Candidate {
  int root = 0;              ///< Pitch class index, compared exactly
  int mode = 0;              ///< Mode enumerator, compared exactly
  float correlation = 0.0f;  ///< Compared to kTolerance
};

struct Row {
  std::string track;        ///< Corpus coordinates, also the golden key
  std::string decision;     ///< Detected key and mode, e.g. "Cmaj"
  float confidence = 0.0f;  ///< The winning candidate's posterior share
  std::vector<Candidate> candidates;
};

std::vector<Row> compute_rows() {
  std::vector<Row> rows;
  rows.reserve(96);
  for (int root = 0; root < 12; ++root) {
    for (int major = 1; major >= 0; --major) {
      for (int variant = 0; variant < 3; ++variant) {
        // Every tonality is covered at the analysis rate; one variant also runs
        // at 44.1 kHz so the internal downsample to 22.05 kHz stays covered
        // without paying for it on every row.
        std::vector<int> rates = {22050};
        if (variant == 0) rates.push_back(44100);
        for (int sr : rates) {
          const Audio audio = render_track(root, major != 0, variant, sr);
          KeyConfig config;
          config.genre_hint = "auto";
          const KeyAnalyzer analyzer(audio, config);
          const Key key = analyzer.key();

          std::vector<Candidate> candidates;
          for (const auto& candidate : analyzer.candidates()) {
            candidates.push_back({static_cast<int>(candidate.key.root),
                                  static_cast<int>(candidate.key.mode), candidate.correlation});
          }

          std::ostringstream track;
          track << pitch_class_short(static_cast<PitchClass>(root)) << (major ? "maj" : "min")
                << "/v" << variant << "/sr" << sr;
          rows.push_back({track.str(),
                          std::string(pitch_class_short(key.root)) + mode_short(key.mode),
                          key.confidence, std::move(candidates)});
        }
      }
    }
  }
  return rows;
}

std::map<std::string, Row> load_manifest(const std::filesystem::path& path) {
  std::ifstream file(path);
  REQUIRE(file.is_open());
  std::map<std::string, Row> out;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::stringstream stream(line);
    Row row;
    std::string field;
    std::getline(stream, row.track, '\t');
    std::getline(stream, row.decision, '\t');
    std::getline(stream, field, '\t');
    row.confidence = std::stof(field);
    // The remaining fields are whole candidate triples; a truncated one is a
    // corrupt manifest rather than a shorter candidate list.
    while (std::getline(stream, field, '\t')) {
      Candidate candidate;
      candidate.root = std::stoi(field);
      REQUIRE(std::getline(stream, field, '\t'));
      candidate.mode = std::stoi(field);
      REQUIRE(std::getline(stream, field, '\t'));
      candidate.correlation = std::stof(field);
      row.candidates.push_back(candidate);
    }
    out[row.track] = std::move(row);
  }
  return out;
}

void write_manifest(const std::filesystem::path& path, const std::vector<Row>& rows) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  file << "# track\tdetected_key\tconfidence\t(candidate root\tmode\tcorrelation)...\n";
  // Six decimals: four more than the tolerance compares to, so the written form
  // never decides a comparison, and short enough for a golden move to be read.
  file << std::fixed << std::setprecision(6);
  for (const auto& row : rows) {
    file << row.track << '\t' << row.decision << '\t' << row.confidence;
    for (const auto& candidate : row.candidates) {
      file << '\t' << candidate.root << '\t' << candidate.mode << '\t' << candidate.correlation;
    }
    file << '\n';
  }
}

}  // namespace

TEST_CASE("automatic key sweep decisions stay stable", "[.][key][golden]") {
  const std::filesystem::path manifest = "tests/analysis/golden/key_auto_sweep.tsv";
  // The sweep runs a harmonic separation per track, so the corpus is computed
  // once and reused for both the regeneration and the comparison.
  const auto rows = compute_rows();
  if (std::getenv("SONARE_UPDATE_KEY_GOLDEN") != nullptr) {
    write_manifest(manifest, rows);
  }

  const auto expected = load_manifest(manifest);
  REQUIRE(rows.size() == 96);
  REQUIRE(expected.size() == rows.size());

  // CHECK, not REQUIRE: one drifted row must not hide the rest. A behaviour
  // change usually moves a whole family of tracks together, and that shape is
  // what separates a stale golden from a single genuine regression.
  for (const auto& row : rows) {
    CAPTURE(row.track);
    const Row& want = expected.at(row.track);
    CHECK(want.decision == row.decision);
    CHECK_THAT(row.confidence, WithinAbs(want.confidence, kTolerance));
    CHECK(want.candidates.size() == row.candidates.size());
    for (size_t i = 0; i < std::min(want.candidates.size(), row.candidates.size()); ++i) {
      CAPTURE(i);
      CHECK(want.candidates[i].root == row.candidates[i].root);
      CHECK(want.candidates[i].mode == row.candidates[i].mode);
      CHECK_THAT(row.candidates[i].correlation,
                 WithinAbs(want.candidates[i].correlation, kTolerance));
    }
  }
}
