/// @file key_auto_sweep_golden_test.cpp
/// @brief Golden decisions for the automatic key-detection candidate sweep.
///
/// The corpus is rendered in code rather than loaded from disk, so this guards
/// the whole `genre_hint = "auto"` path without any audio in the repository.
/// Each row pins the detected key and mode plus a quantized hash of the
/// confidence and the ranked candidate correlations, so a refactor that is
/// meant to preserve behaviour either matches exactly or has to justify a
/// golden move. A change that shifts the numbers without flipping a decision is
/// the case a decision-only check waves through, and it is the case this
/// catches: the hash columns move while the key columns do not.
///
/// This is a bit-exactness golden, **not** an accuracy benchmark. The corpus is
/// saturated — every row currently detects its own tonic — which is what makes
/// a decision flip obvious in the diff, and which also means the pass rate says
/// nothing about accuracy on real material. Accuracy is the optional-fixture
/// harness's job (see tests/fixtures/music_eval/README.md).
///
/// Runs for roughly 70 s, so it sits in the hidden `[golden]` tier rather than
/// the default ctest run. Around 93% of that is the analysis itself — 96 tracks
/// each costing one harmonic separation and two chroma passes — and about 7% is
/// rendering the corpus, so the cost is the coverage rather than the harness.
///
/// Regenerate with `SONARE_UPDATE_KEY_GOLDEN=1`.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "analysis/key_analyzer.h"
#include "core/audio.h"
#include "support/golden_hash.h"
#include "util/constants.h"

namespace {

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

std::string hex64(std::uint64_t value) {
  std::ostringstream stream;
  stream << std::hex << value;
  return stream.str();
}

struct Row {
  std::string track;     ///< Corpus coordinates, also the golden key
  std::string decision;  ///< Detected key and mode, e.g. "Cmaj"
  std::string hash;      ///< Quantized confidence + candidate correlations
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

          // Quantize through the shared golden helper so the numbers survive a
          // different libm the way every other golden in the tree does.
          std::vector<float> numbers;
          numbers.push_back(key.confidence);
          for (const auto& candidate : analyzer.candidates()) {
            numbers.push_back(static_cast<float>(static_cast<int>(candidate.key.root)));
            numbers.push_back(static_cast<float>(static_cast<int>(candidate.key.mode)));
            numbers.push_back(candidate.correlation);
          }

          std::ostringstream track;
          track << pitch_class_short(static_cast<PitchClass>(root)) << (major ? "maj" : "min")
                << "/v" << variant << "/sr" << sr;
          rows.push_back({track.str(),
                          std::string(pitch_class_short(key.root)) + mode_short(key.mode),
                          hex64(sonare::test::fnv1a_quantized(numbers))});
        }
      }
    }
  }
  return rows;
}

std::map<std::string, std::pair<std::string, std::string>> load_manifest(
    const std::filesystem::path& path) {
  std::ifstream file(path);
  REQUIRE(file.is_open());
  std::map<std::string, std::pair<std::string, std::string>> out;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::stringstream stream(line);
    std::string track;
    std::string decision;
    std::string hash;
    std::getline(stream, track, '\t');
    std::getline(stream, decision, '\t');
    std::getline(stream, hash, '\t');
    out[track] = {decision, hash};
  }
  return out;
}

void write_manifest(const std::filesystem::path& path, const std::vector<Row>& rows) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  file << "# track\tdetected_key\tfnv1a_quantized_hash\n";
  for (const auto& row : rows) {
    file << row.track << '\t' << row.decision << '\t' << row.hash << '\n';
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
    CHECK(expected.at(row.track).first == row.decision);
    CHECK(expected.at(row.track).second == row.hash);
  }
}
