/// @file music_dataset_test.cpp
/// @brief Optional external BPM/key/meter/beat/downbeat/chord evaluation fixture checks.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "analysis/beat_analyzer.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/chord_analyzer.h"
#include "analysis/key_analyzer.h"
#include "core/audio.h"
#include "core/resample.h"
#include "quick.h"

namespace sonare {
namespace {

std::filesystem::path fixture_root() {
  if (const char* root = std::getenv("SONARE_MUSIC_FIXTURE_ROOT")) {
    if (root[0] != '\0') {
      return std::filesystem::path(root);
    }
  }
  return "tests/fixtures/music_eval";
}

std::vector<std::string> split_tsv(const std::string& line) {
  std::istringstream stream(line);
  std::vector<std::string> fields;
  std::string field;
  while (std::getline(stream, field, '\t')) {
    fields.push_back(field);
  }
  return fields;
}

std::vector<std::vector<std::string>> read_rows(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::vector<std::vector<std::string>> rows;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    rows.push_back(split_tsv(line));
  }
  return rows;
}

bool parse_pitch_class(const std::string& text, PitchClass& out) {
  static const std::pair<const char*, PitchClass> names[] = {
      {"C", PitchClass::C},   {"C#", PitchClass::Cs}, {"D", PitchClass::D},
      {"D#", PitchClass::Ds}, {"E", PitchClass::E},   {"F", PitchClass::F},
      {"F#", PitchClass::Fs}, {"G", PitchClass::G},   {"G#", PitchClass::Gs},
      {"A", PitchClass::A},   {"A#", PitchClass::As}, {"B", PitchClass::B},
  };
  for (const auto& [name, pitch] : names) {
    if (text == name) {
      out = pitch;
      return true;
    }
  }
  return false;
}

bool parse_mode(const std::string& text, Mode& out) {
  std::string value;
  value.reserve(text.size());
  for (char ch : text) {
    value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }

  if (value == "major" || value == "maj") {
    out = Mode::Major;
  } else if (value == "minor" || value == "min" || value == "m") {
    out = Mode::Minor;
  } else if (value == "dorian") {
    out = Mode::Dorian;
  } else if (value == "phrygian") {
    out = Mode::Phrygian;
  } else if (value == "lydian") {
    out = Mode::Lydian;
  } else if (value == "mixolydian") {
    out = Mode::Mixolydian;
  } else if (value == "locrian") {
    out = Mode::Locrian;
  } else {
    return false;
  }
  return true;
}

std::vector<Mode> parse_mode_list(const std::string& text) {
  std::string value;
  value.reserve(text.size());
  for (char ch : text) {
    value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  if (value == "all" || value == "modal") {
    return {Mode::Major,  Mode::Minor,      Mode::Dorian, Mode::Phrygian,
            Mode::Lydian, Mode::Mixolydian, Mode::Locrian};
  }
  if (value == "major-minor" || value == "majmin" || value == "diatonic") {
    return {Mode::Major, Mode::Minor};
  }

  std::replace(value.begin(), value.end(), '|', ',');
  std::istringstream stream(value);
  std::string item;
  std::vector<Mode> modes;
  while (std::getline(stream, item, ',')) {
    if (item.empty()) continue;
    Mode mode;
    if (parse_mode(item, mode)) {
      modes.push_back(mode);
    }
  }
  return modes;
}

bool parse_key_profile(const std::string& text, KeyProfileType& out) {
  std::string value;
  value.reserve(text.size());
  for (char ch : text) {
    value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }

  if (value == "ks" || value == "krumhansl" || value == "krumhansl-schmuckler") {
    out = KeyProfileType::KrumhanslSchmuckler;
  } else if (value == "temperley") {
    out = KeyProfileType::Temperley;
  } else if (value == "shaath" || value == "keyfinder") {
    out = KeyProfileType::Shaath;
  } else if (value == "faraldo-edmt" || value == "edmt") {
    out = KeyProfileType::FaraldoEDMT;
  } else if (value == "faraldo-edma" || value == "edma") {
    out = KeyProfileType::FaraldoEDMA;
  } else if (value == "faraldo-edmm" || value == "edmm") {
    out = KeyProfileType::FaraldoEDMM;
  } else if (value == "bellman-budge" || value == "bellman") {
    out = KeyProfileType::BellmanBudge;
  } else {
    return false;
  }
  return true;
}

bool is_report_only(const std::vector<std::string>& row, size_t first_optional_column) {
  if (row.size() <= first_optional_column) {
    return false;
  }
  return std::find(row.begin() + static_cast<std::ptrdiff_t>(first_optional_column), row.end(),
                   "report_only") != row.end();
}

bool parse_optional_float_token(const std::vector<std::string>& row, size_t first_optional_column,
                                const std::string& prefix, float& out) {
  if (row.size() <= first_optional_column) {
    return false;
  }
  for (auto it = row.begin() + static_cast<std::ptrdiff_t>(first_optional_column); it != row.end();
       ++it) {
    if (it->rfind(prefix, 0) == 0) {
      out = std::stof(it->substr(prefix.size()));
      return true;
    }
  }
  return false;
}

bool parse_optional_string_token(const std::vector<std::string>& row, size_t first_optional_column,
                                 const std::string& prefix, std::string& out) {
  if (row.size() <= first_optional_column) {
    return false;
  }
  for (auto it = row.begin() + static_cast<std::ptrdiff_t>(first_optional_column); it != row.end();
       ++it) {
    if (it->rfind(prefix, 0) == 0) {
      out = it->substr(prefix.size());
      return true;
    }
  }
  return false;
}

struct ExpectedKey {
  PitchClass root;
  Mode mode;
};

bool parse_key_reference(const std::string& text, ExpectedKey& out) {
  const size_t separator = text.find(':');
  if (separator == std::string::npos) {
    return false;
  }
  PitchClass root;
  Mode mode;
  if (!parse_pitch_class(text.substr(0, separator), root) ||
      !parse_mode(text.substr(separator + 1), mode)) {
    return false;
  }
  out = ExpectedKey{root, mode};
  return true;
}

std::vector<ExpectedKey> key_targets(const std::vector<std::string>& row) {
  std::vector<ExpectedKey> targets;
  PitchClass root;
  Mode mode;
  REQUIRE(parse_pitch_class(row[2], root));
  REQUIRE(parse_mode(row[3], mode));
  targets.push_back(ExpectedKey{root, mode});

  std::string alt_keys;
  if (!parse_optional_string_token(row, 4, "alt_key=", alt_keys)) {
    return targets;
  }
  std::replace(alt_keys.begin(), alt_keys.end(), '|', ',');
  std::istringstream stream(alt_keys);
  std::string item;
  while (std::getline(stream, item, ',')) {
    if (item.empty()) continue;
    ExpectedKey target;
    REQUIRE(parse_key_reference(item, target));
    targets.push_back(target);
  }
  return targets;
}

Audio prepare_quick_key_audio(const Audio& audio) {
  constexpr int kAnalysisSampleRate = 22050;
  if (audio.sample_rate() > kAnalysisSampleRate) {
    return resample(audio, kAnalysisSampleRate);
  }
  return audio;
}

std::vector<float> bpm_targets(const std::vector<std::string>& row) {
  std::vector<float> targets;
  targets.push_back(std::stof(row[2]));
  constexpr const char* kAltPrefix = "alt_bpm=";
  for (size_t i = 4; i < row.size(); ++i) {
    if (row[i].rfind(kAltPrefix, 0) != 0) continue;
    std::string values = row[i].substr(std::char_traits<char>::length(kAltPrefix));
    std::replace(values.begin(), values.end(), '|', ',');
    std::istringstream stream(values);
    std::string value;
    while (std::getline(stream, value, ',')) {
      if (!value.empty()) {
        targets.push_back(std::stof(value));
      }
    }
  }
  return targets;
}

std::pair<float, float> best_bpm_error(float measured_bpm, const std::vector<float>& targets) {
  float best_target = targets.empty() ? 0.0f : targets.front();
  float best_error = std::numeric_limits<float>::infinity();
  for (float target : targets) {
    if (target <= 0.0f) continue;
    const float error = std::abs(measured_bpm - target) / target;
    if (error < best_error) {
      best_error = error;
      best_target = target;
    }
  }
  return {best_target, best_error};
}

bool parse_chord_symbol(const std::string& symbol, PitchClass& root, ChordQuality& quality,
                        PitchClass& bass, bool& has_bass) {
  const size_t slash = symbol.find('/');
  const std::string chord_text = slash == std::string::npos ? symbol : symbol.substr(0, slash);
  const size_t separator = chord_text.find(':');
  const std::string root_text =
      separator == std::string::npos ? chord_text : chord_text.substr(0, separator);
  const std::string quality_text =
      separator == std::string::npos ? "maj" : chord_text.substr(separator + 1);
  if (!parse_pitch_class(root_text, root)) {
    return false;
  }
  std::string normalized_quality;
  normalized_quality.reserve(quality_text.size());
  for (char ch : quality_text) {
    if (ch != ' ' && ch != '_' && ch != '-') {
      normalized_quality.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
  }
  if (normalized_quality == "maj" || normalized_quality == "major") {
    quality = ChordQuality::Major;
  } else if (normalized_quality == "min" || normalized_quality == "minor" ||
             normalized_quality == "m") {
    quality = ChordQuality::Minor;
  } else if (normalized_quality == "dim" || normalized_quality == "diminished") {
    quality = ChordQuality::Diminished;
  } else if (normalized_quality == "aug" || normalized_quality == "augmented") {
    quality = ChordQuality::Augmented;
  } else if (normalized_quality == "7" || normalized_quality == "dom7" ||
             normalized_quality == "dominant7") {
    quality = ChordQuality::Dominant7;
  } else if (normalized_quality == "maj7" || normalized_quality == "major7") {
    quality = ChordQuality::Major7;
  } else if (normalized_quality == "min7" || normalized_quality == "minor7" ||
             normalized_quality == "m7") {
    quality = ChordQuality::Minor7;
  } else if (normalized_quality == "sus2") {
    quality = ChordQuality::Sus2;
  } else if (normalized_quality == "sus4" || normalized_quality == "sus") {
    quality = ChordQuality::Sus4;
  } else if (normalized_quality == "add9" || normalized_quality == "majadd9") {
    quality = ChordQuality::Add9;
  } else if (normalized_quality == "minadd9" || normalized_quality == "minoradd9" ||
             normalized_quality == "madd9") {
    quality = ChordQuality::MinorAdd9;
  } else if (normalized_quality == "dim7" || normalized_quality == "diminished7") {
    quality = ChordQuality::Dim7;
  } else if (normalized_quality == "m7b5" || normalized_quality == "min7b5" ||
             normalized_quality == "minor7b5" || normalized_quality == "hdim7" ||
             normalized_quality == "halfdim7") {
    quality = ChordQuality::HalfDim7;
  } else if (normalized_quality == "maj9" || normalized_quality == "major9") {
    quality = ChordQuality::Major9;
  } else if (normalized_quality == "9" || normalized_quality == "dom9" ||
             normalized_quality == "dominant9") {
    quality = ChordQuality::Dominant9;
  } else if (normalized_quality == "sus2add4" || normalized_quality == "sus4add2") {
    quality = ChordQuality::Sus2Add4;
  } else {
    return false;
  }
  bass = root;
  has_bass = false;
  if (slash != std::string::npos) {
    has_bass = parse_pitch_class(symbol.substr(slash + 1), bass);
    if (!has_bass) {
      return false;
    }
  }
  return true;
}

struct ReferenceChord {
  float start = 0.0f;
  float end = 0.0f;
  PitchClass root = PitchClass::C;
  ChordQuality quality = ChordQuality::Major;
  PitchClass bass = PitchClass::C;
  bool has_bass = false;
};

std::vector<ReferenceChord> read_chord_annotation(const std::filesystem::path& path) {
  std::vector<ReferenceChord> chords;
  for (const auto& fields : read_rows(path)) {
    if (fields.size() < 3) continue;
    ReferenceChord chord;
    chord.start = std::stof(fields[0]);
    chord.end = std::stof(fields[1]);
    if (chord.end <= chord.start ||
        !parse_chord_symbol(fields[2], chord.root, chord.quality, chord.bass, chord.has_bass)) {
      continue;
    }
    chords.push_back(chord);
  }
  return chords;
}

bool maj_min_equal(const Chord& detected, const ReferenceChord& reference) {
  auto collapse_quality = [](ChordQuality quality, ChordQuality& collapsed) {
    switch (quality) {
      case ChordQuality::Major:
      case ChordQuality::Dominant7:
      case ChordQuality::Major7:
      case ChordQuality::Add9:
      case ChordQuality::Major9:
      case ChordQuality::Dominant9:
        collapsed = ChordQuality::Major;
        return true;
      case ChordQuality::Minor:
      case ChordQuality::Minor7:
      case ChordQuality::MinorAdd9:
        collapsed = ChordQuality::Minor;
        return true;
      default:
        return false;
    }
  };

  ChordQuality detected_collapsed;
  ChordQuality reference_collapsed;
  return detected.root == reference.root &&
         collapse_quality(detected.quality, detected_collapsed) &&
         collapse_quality(reference.quality, reference_collapsed) &&
         detected_collapsed == reference_collapsed;
}

float chord_wcsr(const std::vector<Chord>& detected, const std::vector<ReferenceChord>& reference) {
  float total = 0.0f;
  float correct = 0.0f;
  for (const auto& ref : reference) {
    const float duration = ref.end - ref.start;
    if (duration <= 0.0f) continue;
    total += duration;
    const float midpoint = 0.5f * (ref.start + ref.end);
    auto found = std::find_if(detected.begin(), detected.end(), [midpoint](const Chord& chord) {
      return chord.start <= midpoint && midpoint < chord.end;
    });
    if (found != detected.end() && maj_min_equal(*found, ref)) {
      correct += duration;
    }
  }
  return total > 0.0f ? correct / total : 0.0f;
}

bool exact_chord_equal(const Chord& detected, const ReferenceChord& reference) {
  return detected.root == reference.root && detected.quality == reference.quality;
}

float chord_root_accuracy(const std::vector<Chord>& detected,
                          const std::vector<ReferenceChord>& reference) {
  float total = 0.0f;
  float correct = 0.0f;
  for (const auto& ref : reference) {
    const float duration = ref.end - ref.start;
    if (duration <= 0.0f) continue;
    total += duration;
    const float midpoint = 0.5f * (ref.start + ref.end);
    auto found = std::find_if(detected.begin(), detected.end(), [midpoint](const Chord& chord) {
      return chord.start <= midpoint && midpoint < chord.end;
    });
    if (found != detected.end() && found->root == ref.root) {
      correct += duration;
    }
  }
  return total > 0.0f ? correct / total : 0.0f;
}

float chord_quality_accuracy(const std::vector<Chord>& detected,
                             const std::vector<ReferenceChord>& reference) {
  float total = 0.0f;
  float correct = 0.0f;
  for (const auto& ref : reference) {
    const float duration = ref.end - ref.start;
    if (duration <= 0.0f) continue;
    total += duration;
    const float midpoint = 0.5f * (ref.start + ref.end);
    auto found = std::find_if(detected.begin(), detected.end(), [midpoint](const Chord& chord) {
      return chord.start <= midpoint && midpoint < chord.end;
    });
    if (found != detected.end() && found->quality == ref.quality) {
      correct += duration;
    }
  }
  return total > 0.0f ? correct / total : 0.0f;
}

float chord_exact_wcsr(const std::vector<Chord>& detected,
                       const std::vector<ReferenceChord>& reference) {
  float total = 0.0f;
  float correct = 0.0f;
  for (const auto& ref : reference) {
    const float duration = ref.end - ref.start;
    if (duration <= 0.0f) continue;
    total += duration;
    const float midpoint = 0.5f * (ref.start + ref.end);
    auto found = std::find_if(detected.begin(), detected.end(), [midpoint](const Chord& chord) {
      return chord.start <= midpoint && midpoint < chord.end;
    });
    if (found != detected.end() && exact_chord_equal(*found, ref)) {
      correct += duration;
    }
  }
  return total > 0.0f ? correct / total : 0.0f;
}

bool reference_has_extended_chords(const std::vector<ReferenceChord>& reference) {
  return std::any_of(reference.begin(), reference.end(), [](const ReferenceChord& chord) {
    return chord.quality != ChordQuality::Major && chord.quality != ChordQuality::Minor;
  });
}

bool reference_has_bass_labels(const std::vector<ReferenceChord>& reference) {
  return std::any_of(reference.begin(), reference.end(),
                     [](const ReferenceChord& chord) { return chord.has_bass; });
}

float chord_bass_accuracy(const std::vector<Chord>& detected,
                          const std::vector<ReferenceChord>& reference) {
  float total = 0.0f;
  float correct = 0.0f;
  for (const auto& ref : reference) {
    if (!ref.has_bass) continue;
    const float duration = ref.end - ref.start;
    if (duration <= 0.0f) continue;
    total += duration;
    const float midpoint = 0.5f * (ref.start + ref.end);
    auto found = std::find_if(detected.begin(), detected.end(), [midpoint](const Chord& chord) {
      return chord.start <= midpoint && midpoint < chord.end;
    });
    if (found != detected.end() && found->bass == ref.bass) {
      correct += duration;
    }
  }
  return total > 0.0f ? correct / total : 0.0f;
}

float chord_change_rate_per_minute(const std::vector<Chord>& chords) {
  if (chords.size() < 2) {
    return 0.0f;
  }
  float start = std::numeric_limits<float>::infinity();
  float end = 0.0f;
  size_t changes = 0;
  for (const auto& chord : chords) {
    start = std::min(start, chord.start);
    end = std::max(end, chord.end);
  }
  for (size_t i = 1; i < chords.size(); ++i) {
    if (chords[i].root != chords[i - 1].root || chords[i].quality != chords[i - 1].quality ||
        chords[i].bass != chords[i - 1].bass) {
      ++changes;
    }
  }
  const float duration_min = (end - start) / 60.0f;
  return duration_min > 0.0f ? static_cast<float>(changes) / duration_min : 0.0f;
}

std::vector<float> read_time_annotation(const std::filesystem::path& path) {
  std::vector<float> times;
  for (const auto& fields : read_rows(path)) {
    if (fields.empty()) continue;
    const float time = std::stof(fields[0]);
    if (std::isfinite(time) && time >= 0.0f) {
      times.push_back(time);
    }
  }
  std::sort(times.begin(), times.end());
  return times;
}

std::vector<float> beat_times_from_beats(const std::vector<Beat>& beats) {
  std::vector<float> times;
  times.reserve(beats.size());
  for (const auto& beat : beats) {
    times.push_back(beat.time);
  }
  return times;
}

float event_f_measure(const std::vector<float>& detected, const std::vector<float>& reference,
                      float tolerance_seconds) {
  if (detected.empty() || reference.empty() || tolerance_seconds < 0.0f) {
    return 0.0f;
  }

  std::vector<bool> used(detected.size(), false);
  size_t true_positive = 0;
  for (float ref_time : reference) {
    size_t best_index = detected.size();
    float best_distance = tolerance_seconds;
    for (size_t i = 0; i < detected.size(); ++i) {
      if (used[i]) continue;
      const float distance = std::abs(detected[i] - ref_time);
      if (distance <= best_distance) {
        best_distance = distance;
        best_index = i;
      }
    }
    if (best_index < detected.size()) {
      used[best_index] = true;
      ++true_positive;
    }
  }

  const float precision = static_cast<float>(true_positive) / static_cast<float>(detected.size());
  const float recall = static_cast<float>(true_positive) / static_cast<float>(reference.size());
  return (precision + recall) > 0.0f ? 2.0f * precision * recall / (precision + recall) : 0.0f;
}

int key_candidate_rank(const std::vector<KeyCandidate>& candidates, PitchClass expected_root,
                       Mode expected_mode) {
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (candidates[i].key.root == expected_root && candidates[i].key.mode == expected_mode) {
      return static_cast<int>(i) + 1;
    }
  }
  return 0;
}

bool key_matches_any(const Key& key, const std::vector<ExpectedKey>& targets) {
  return std::any_of(targets.begin(), targets.end(), [&key](const ExpectedKey& target) {
    return key.root == target.root && key.mode == target.mode;
  });
}

float key_candidate_correlation(const std::vector<KeyCandidate>& candidates, PitchClass root,
                                Mode mode) {
  for (const auto& candidate : candidates) {
    if (candidate.key.root == root && candidate.key.mode == mode) {
      return candidate.correlation;
    }
  }
  return 0.0f;
}

std::pair<const char*, float> key_mirex_category(const Key& measured, PitchClass expected_root,
                                                 Mode expected_mode) {
  if (measured.root == expected_root && measured.mode == expected_mode) {
    return {"exact", 1.0f};
  }

  const int measured_root = static_cast<int>(measured.root);
  const int expected = static_cast<int>(expected_root);
  const int interval = (measured_root - expected + 12) % 12;
  if (measured.mode == expected_mode && (interval == 7 || interval == 5)) {
    return {"fifth", 0.5f};
  }
  if (measured.mode != expected_mode) {
    const bool relative = (expected_mode == Mode::Major && measured.mode == Mode::Minor &&
                           measured_root == (expected + 9) % 12) ||
                          (expected_mode == Mode::Minor && measured.mode == Mode::Major &&
                           measured_root == (expected + 3) % 12);
    if (relative) {
      return {"relative", 0.3f};
    }
    if (measured.root == expected_root) {
      return {"parallel", 0.2f};
    }
  }
  return {"other", 0.0f};
}

struct ExpectedKeyMatch {
  ExpectedKey target;
  int rank = 0;
  float correlation = 0.0f;
  const char* mirex_category = "other";
  float mirex_score = 0.0f;
};

ExpectedKeyMatch best_expected_key_match(const Key& measured,
                                         const std::vector<KeyCandidate>& candidates,
                                         const std::vector<ExpectedKey>& targets) {
  ExpectedKeyMatch best;
  bool has_best = false;
  for (const auto& target : targets) {
    ExpectedKeyMatch match;
    match.target = target;
    match.rank = key_candidate_rank(candidates, target.root, target.mode);
    match.correlation = key_candidate_correlation(candidates, target.root, target.mode);
    const auto [category, score] = key_mirex_category(measured, target.root, target.mode);
    match.mirex_category = category;
    match.mirex_score = score;
    if (!has_best || match.mirex_score > best.mirex_score ||
        (match.mirex_score == best.mirex_score &&
         ((match.rank > 0 && (best.rank == 0 || match.rank < best.rank)) ||
          (match.rank == best.rank && match.correlation > best.correlation)))) {
      best = match;
      has_best = true;
    }
  }
  return best;
}

}  // namespace

TEST_CASE("Music optional BPM fixtures", "[music_dataset][bpm]") {
  const std::filesystem::path root = fixture_root();
  const auto rows = read_rows(root / "bpm_manifest.tsv");
  if (rows.empty()) SKIP("No BPM fixture rows are configured");

  size_t checked = 0;
  for (const auto& row : rows) {
    if (row.size() < 4) continue;
    const std::filesystem::path audio_path = root / row[1];
    if (!std::filesystem::exists(audio_path)) continue;

    INFO("BPM fixture: " << row[0] << " / " << row[1]);
    const std::vector<float> expected_bpms = bpm_targets(row);
    const float tolerance_rel = std::stof(row[3]);
    const Audio audio = Audio::from_file(audio_path.string());
    const float measured_bpm = quick::detect_bpm(audio.data(), audio.size(), audio.sample_rate());
    const auto [matched_bpm, error_rel] = best_bpm_error(measured_bpm, expected_bpms);
    if (is_report_only(row, 4)) {
      WARN("Report-only BPM fixture " << row[0] << "/" << row[1] << " measured " << measured_bpm
                                      << " BPM; expected " << matched_bpm << " +/- "
                                      << tolerance_rel * 100.0f << "%; relative error "
                                      << error_rel * 100.0f << "%");
    } else {
      REQUIRE(error_rel <= tolerance_rel);
    }
    ++checked;
  }
  if (checked == 0) SKIP("No BPM audio fixtures are present");
}

TEST_CASE("Music optional key fixtures", "[music_dataset][key]") {
  const std::filesystem::path root = fixture_root();
  const auto rows = read_rows(root / "key_manifest.tsv");
  if (rows.empty()) SKIP("No key fixture rows are configured");

  size_t checked = 0;
  for (const auto& row : rows) {
    if (row.size() < 4) continue;
    const std::filesystem::path audio_path = root / row[1];
    if (!std::filesystem::exists(audio_path)) continue;

    const std::vector<ExpectedKey> expected_keys = key_targets(row);

    INFO("Key fixture: " << row[0] << " / " << row[1]);
    const Audio audio = Audio::from_file(audio_path.string());
    const Audio analysis_audio = prepare_quick_key_audio(audio);
    KeyConfig key_config;
    std::string mode_list;
    if (parse_optional_string_token(row, 4, "modes=", mode_list)) {
      key_config.modes = parse_mode_list(mode_list);
      REQUIRE(!key_config.modes.empty());
    }
    std::string profile_text;
    if (parse_optional_string_token(row, 4, "profile=", profile_text)) {
      REQUIRE(parse_key_profile(profile_text, key_config.profile_type));
    }
    std::string genre_hint;
    if (parse_optional_string_token(row, 4, "genre_hint=", genre_hint)) {
      key_config.genre_hint = genre_hint;
    }
    const KeyAnalyzer analyzer(analysis_audio, key_config);
    const Key key = analyzer.key();
    const auto& candidates = analyzer.all_candidates();
    const ExpectedKeyMatch expected = best_expected_key_match(key, candidates, expected_keys);
    const float best_corr = candidates.empty() ? 0.0f : candidates.front().correlation;
    if (is_report_only(row, 4)) {
      WARN("Report-only key fixture "
           << row[0] << "/" << row[1] << " measured root=" << static_cast<int>(key.root)
           << " mode=" << static_cast<int>(key.mode)
           << "; expected root=" << static_cast<int>(expected.target.root) << " mode="
           << static_cast<int>(expected.target.mode) << "; expected rank=" << expected.rank
           << "; best_corr=" << best_corr << " expected_corr=" << expected.correlation
           << " corr_gap=" << best_corr - expected.correlation << "; mirex_category="
           << expected.mirex_category << " mirex_score=" << expected.mirex_score);
    } else {
      REQUIRE(key_matches_any(key, expected_keys));
    }
    ++checked;
  }
  if (checked == 0) SKIP("No key audio fixtures are present");
}

TEST_CASE("Music optional meter fixtures", "[music_dataset][meter]") {
  const std::filesystem::path root = fixture_root();
  const auto rows = read_rows(root / "meter_manifest.tsv");
  if (rows.empty()) SKIP("No meter fixture rows are configured");

  size_t checked = 0;
  for (const auto& row : rows) {
    if (row.size() < 5) continue;
    const std::filesystem::path audio_path = root / row[1];
    if (!std::filesystem::exists(audio_path)) continue;

    INFO("Meter fixture: " << row[0] << " / " << row[1]);
    const int expected_numerator = std::stoi(row[2]);
    const int expected_denominator = std::stoi(row[3]);
    const float min_confidence = std::stof(row[4]);
    const Audio audio = Audio::from_file(audio_path.string());
    const BeatAnalyzer analyzer(audio);
    const TimeSignature ts = analyzer.time_signature();
    if (is_report_only(row, 5)) {
      const bool correct =
          ts.numerator == expected_numerator && ts.denominator == expected_denominator;
      WARN("Report-only meter fixture "
           << row[0] << "/" << row[1] << " measured " << ts.numerator << "/" << ts.denominator
           << " confidence " << ts.confidence << "; expected " << expected_numerator << "/"
           << expected_denominator << " min confidence " << min_confidence << "; correct "
           << (correct ? 1 : 0));
    } else {
      REQUIRE(ts.numerator == expected_numerator);
      REQUIRE(ts.denominator == expected_denominator);
      REQUIRE(ts.confidence >= min_confidence);
    }
    ++checked;
  }
  if (checked == 0) SKIP("No meter audio fixtures are present");
}

TEST_CASE("Music optional beat F-measure fixtures", "[music_dataset][beat]") {
  const std::filesystem::path root = fixture_root();
  const auto rows = read_rows(root / "beat_manifest.tsv");
  if (rows.empty()) SKIP("No beat fixture rows are configured");

  size_t checked = 0;
  for (const auto& row : rows) {
    if (row.size() < 5) continue;
    const std::filesystem::path audio_path = root / row[1];
    const std::filesystem::path annotation_path = root / row[2];
    if (!std::filesystem::exists(audio_path) || !std::filesystem::exists(annotation_path)) {
      continue;
    }

    INFO("Beat fixture: " << row[0] << " / " << row[1]);
    const Audio audio = Audio::from_file(audio_path.string());
    BeatConfig config;
    config.adaptive_tempo = row.size() >= 6 && row[5] == "adaptive";
    float min_improvement = 0.0f;
    const bool has_improvement_threshold =
        parse_optional_float_token(row, 6, "min_improvement=", min_improvement);
    const BeatAnalyzer analyzer(audio, config);
    const auto reference = read_time_annotation(annotation_path);
    REQUIRE_FALSE(reference.empty());
    const float f_measure =
        event_f_measure(beat_times_from_beats(analyzer.beats()), reference, std::stof(row[4]));

    float baseline_f_measure = std::numeric_limits<float>::quiet_NaN();
    float improvement = std::numeric_limits<float>::quiet_NaN();
    if (config.adaptive_tempo || has_improvement_threshold) {
      BeatConfig baseline_config;
      baseline_config.adaptive_tempo = false;
      const BeatAnalyzer baseline_analyzer(audio, baseline_config);
      baseline_f_measure = event_f_measure(beat_times_from_beats(baseline_analyzer.beats()),
                                           reference, std::stof(row[4]));
      improvement = f_measure - baseline_f_measure;
    }

    if (is_report_only(row, 6)) {
      WARN("Report-only beat fixture " << row[0] << "/" << row[1] << " F-measure " << f_measure
                                       << "; threshold " << row[3]);
      if (config.adaptive_tempo || has_improvement_threshold) {
        WARN("Report-only beat improvement fixture "
             << row[0] << "/" << row[1] << " improvement " << improvement << "; baseline_f_measure "
             << baseline_f_measure << "; adaptive_f_measure " << f_measure << "; threshold "
             << (has_improvement_threshold ? min_improvement
                                           : std::numeric_limits<float>::quiet_NaN()));
      }
    } else {
      REQUIRE(f_measure >= std::stof(row[3]));
      if (has_improvement_threshold) {
        REQUIRE(improvement >= min_improvement);
      }
    }
    ++checked;
  }
  if (checked == 0) SKIP("No beat audio/annotation fixtures are present");
}

TEST_CASE("Music optional downbeat F-measure fixtures", "[music_dataset][downbeat]") {
  const std::filesystem::path root = fixture_root();
  const auto rows = read_rows(root / "downbeat_manifest.tsv");
  if (rows.empty()) SKIP("No downbeat fixture rows are configured");

  size_t checked = 0;
  for (const auto& row : rows) {
    if (row.size() < 5) continue;
    const std::filesystem::path audio_path = root / row[1];
    const std::filesystem::path annotation_path = root / row[2];
    if (!std::filesystem::exists(audio_path) || !std::filesystem::exists(annotation_path)) {
      continue;
    }

    INFO("Downbeat fixture: " << row[0] << " / " << row[1]);
    const Audio audio = Audio::from_file(audio_path.string());
    BeatConfig config;
    config.adaptive_tempo = row.size() >= 6 && row[5] == "adaptive";
    std::string time_signature;
    parse_optional_string_token(row, 6, "time_sig=", time_signature);
    const BeatAnalyzer analyzer(audio, config);
    const auto reference = read_time_annotation(annotation_path);
    REQUIRE_FALSE(reference.empty());
    const float f_measure =
        event_f_measure(beat_times_from_beats(analyzer.downbeats()), reference, std::stof(row[4]));
    if (is_report_only(row, 6)) {
      WARN("Report-only downbeat fixture "
           << row[0] << "/" << row[1] << " F-measure " << f_measure << "; threshold " << row[3]
           << "; time_signature " << (time_signature.empty() ? "unknown" : time_signature));
    } else {
      REQUIRE(f_measure >= std::stof(row[3]));
    }
    ++checked;
  }
  if (checked == 0) SKIP("No downbeat audio/annotation fixtures are present");
}

TEST_CASE("Music optional chord WCSR fixtures", "[music_dataset][chord]") {
  const std::filesystem::path root = fixture_root();
  const auto rows = read_rows(root / "chord_manifest.tsv");
  if (rows.empty()) SKIP("No chord fixture rows are configured");

  size_t checked = 0;
  for (const auto& row : rows) {
    if (row.size() < 4) continue;
    const std::filesystem::path audio_path = root / row[1];
    const std::filesystem::path annotation_path = root / row[2];
    if (!std::filesystem::exists(audio_path) || !std::filesystem::exists(annotation_path)) {
      continue;
    }

    INFO("Chord fixture: " << row[0] << " / " << row[1]);
    const Audio audio = Audio::from_file(audio_path.string());
    const auto reference = read_chord_annotation(annotation_path);
    REQUIRE_FALSE(reference.empty());
    float min_bass_acc = 0.0f;
    const bool has_bass_threshold = parse_optional_float_token(row, 4, "bass_acc=", min_bass_acc);
    float min_extended_wcsr = 0.0f;
    const bool has_extended_threshold =
        parse_optional_float_token(row, 4, "extended_wcsr=", min_extended_wcsr);
    float max_change_rate = 0.0f;
    const bool has_change_rate_threshold =
        parse_optional_float_token(row, 4, "max_change_rate=", max_change_rate);
    float min_change_reduction = 0.0f;
    const bool has_change_reduction_threshold =
        parse_optional_float_token(row, 4, "min_change_reduction=", min_change_reduction);
    const bool evaluate_bass = has_bass_threshold || reference_has_bass_labels(reference);
    const bool evaluate_extended =
        has_extended_threshold || reference_has_extended_chords(reference);
    ChordConfig config;
    config.use_triads_only = !evaluate_extended;
    if (evaluate_extended || evaluate_bass) {
      config.chroma_method = ChromaMethod::NNLS;
    }
    config.use_hmm = true;
    config.detect_inversions = evaluate_bass;
    const ChordAnalyzer analyzer(audio, config);
    const auto detected = analyzer.chords();
    ChordConfig baseline_config = config;
    baseline_config.use_hmm = false;
    baseline_config.use_key_context = false;
    const ChordAnalyzer baseline_analyzer(audio, baseline_config);
    const auto baseline_detected = baseline_analyzer.chords();
    const float wcsr = chord_wcsr(detected, reference);
    const float extended_wcsr = evaluate_extended ? chord_exact_wcsr(detected, reference) : 0.0f;
    const float exact_wcsr = chord_exact_wcsr(detected, reference);
    const float root_accuracy = chord_root_accuracy(detected, reference);
    const float quality_accuracy = chord_quality_accuracy(detected, reference);
    const float bass_accuracy = evaluate_bass ? chord_bass_accuracy(detected, reference) : 0.0f;
    const float change_rate = chord_change_rate_per_minute(detected);
    const float baseline_change_rate = chord_change_rate_per_minute(baseline_detected);
    const float change_reduction =
        baseline_change_rate > 0.0f
            ? std::max(0.0f, (baseline_change_rate - change_rate) / baseline_change_rate)
            : (change_rate <= 0.0f ? 1.0f : 0.0f);
    if (is_report_only(row, 4)) {
      WARN("Report-only chord fixture " << row[0] << "/" << row[1] << " WCSR " << wcsr
                                        << "; threshold " << row[3]);
      WARN("Report-only chord detail fixture "
           << row[0] << "/" << row[1] << " root_accuracy " << root_accuracy << "; quality_accuracy "
           << quality_accuracy << "; exact_wcsr " << exact_wcsr);
      if (evaluate_extended) {
        WARN("Report-only chord extended fixture "
             << row[0] << "/" << row[1] << " WCSR " << extended_wcsr << "; threshold "
             << (has_extended_threshold ? min_extended_wcsr
                                        : std::numeric_limits<float>::quiet_NaN()));
      }
      WARN("Report-only chord change-rate fixture "
           << row[0] << "/" << row[1] << " changes_per_minute " << change_rate << "; threshold "
           << (has_change_rate_threshold ? max_change_rate
                                         : std::numeric_limits<float>::quiet_NaN()));
      WARN("Report-only chord change-reduction fixture "
           << row[0] << "/" << row[1] << " reduction " << change_reduction
           << "; baseline_changes_per_minute " << baseline_change_rate
           << "; smoothed_changes_per_minute " << change_rate << "; threshold "
           << (has_change_reduction_threshold ? min_change_reduction
                                              : std::numeric_limits<float>::quiet_NaN()));
      if (evaluate_bass) {
        WARN("Report-only chord bass fixture " << row[0] << "/" << row[1] << " accuracy "
                                               << bass_accuracy << "; threshold " << min_bass_acc);
      }
    } else {
      REQUIRE(wcsr >= std::stof(row[3]));
      if (has_bass_threshold) {
        REQUIRE(bass_accuracy >= min_bass_acc);
      }
      if (has_extended_threshold) {
        REQUIRE(extended_wcsr >= min_extended_wcsr);
      }
      if (has_change_rate_threshold) {
        REQUIRE(change_rate <= max_change_rate);
      }
      if (has_change_reduction_threshold) {
        REQUIRE(change_reduction >= min_change_reduction);
      }
    }
    ++checked;
  }
  if (checked == 0) SKIP("No chord audio/annotation fixtures are present");
}

}  // namespace sonare
