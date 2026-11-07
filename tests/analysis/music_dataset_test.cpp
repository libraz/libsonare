/// @file music_dataset_test.cpp
/// @brief Optional external BPM/key/meter/beat/downbeat/chord/structure evaluation fixture checks.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "analysis/analysis_rate.h"
#include "analysis/beat_analyzer.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/chord_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/section_analyzer.h"
#include "analysis/section_structure.h"
#include "core/audio.h"
#include "core/spectrum.h"
#include "quick.h"
#ifdef SONARE_WITH_PITCH_EDITOR
#include "core/audio_io.h"
#include "editing/note_model/note_transcriber.h"
#include "support/note_match.h"
#endif

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

/// Letter name plus any run of `#` / `b` accidentals, as written in Harte labels.
bool parse_pitch_class(const std::string& text, PitchClass& out) {
  static constexpr int kNaturals[] = {9, 11, 0, 2, 4, 5, 7};  // A..G
  if (text.empty() || text[0] < 'A' || text[0] > 'G') return false;
  int semitone = kNaturals[text[0] - 'A'];
  for (size_t i = 1; i < text.size(); ++i) {
    if (text[i] == '#') {
      ++semitone;
    } else if (text[i] == 'b') {
      --semitone;
    } else {
      return false;
    }
  }
  out = static_cast<PitchClass>(((semitone % 12) + 12) % 12);
  return true;
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

Audio prepare_quick_key_audio(const Audio& audio) { return analysis_rate_audio(audio); }

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

/// MIREX majmin reduction: a quality with a major or minor triad reduces to it;
/// anything else (dim, aug, sus, dim7, hdim7, 7sus4, unknown) reduces to nothing.
bool majmin_reduction(ChordQuality quality, ChordQuality& reduced) {
  switch (quality) {
    case ChordQuality::Major:
    case ChordQuality::Dominant7:
    case ChordQuality::Major7:
    case ChordQuality::Add9:
    case ChordQuality::Major9:
    case ChordQuality::Dominant9:
    case ChordQuality::Major6:
    case ChordQuality::Dominant11:
    case ChordQuality::Dominant13:
    case ChordQuality::Dominant7b9:
    case ChordQuality::Dominant7s9:
      reduced = ChordQuality::Major;
      return true;
    case ChordQuality::Minor:
    case ChordQuality::Minor7:
    case ChordQuality::MinorAdd9:
    case ChordQuality::Minor6:
    case ChordQuality::MinorMajor7:
      reduced = ChordQuality::Minor;
      return true;
    default:
      return false;
  }
}

bool maj_min_equal(const Chord& detected, const ReferenceChord& reference) {
  ChordQuality detected_collapsed;
  ChordQuality reference_collapsed;
  return detected.root == reference.root &&
         majmin_reduction(detected.quality, detected_collapsed) &&
         majmin_reduction(reference.quality, reference_collapsed) &&
         detected_collapsed == reference_collapsed;
}

/// Majmin WCSR; a reference chord outside the maj/min vocabulary is not scored.
float chord_wcsr(const std::vector<Chord>& detected, const std::vector<ReferenceChord>& reference) {
  float total = 0.0f;
  float correct = 0.0f;
  for (const auto& ref : reference) {
    const float duration = ref.end - ref.start;
    ChordQuality reduced;
    if (duration <= 0.0f || !majmin_reduction(ref.quality, reduced)) continue;
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

/// Median of `detected - nearest reference` (milliseconds, positive = late) over detected beats
/// within a quarter of the median reference inter-beat interval; NaN when none qualify.
float median_beat_offset_ms(const std::vector<float>& detected,
                            const std::vector<float>& reference) {
  const float none = std::numeric_limits<float>::quiet_NaN();
  if (detected.empty() || reference.size() < 2) return none;
  std::vector<float> intervals;
  for (size_t i = 1; i < reference.size(); ++i) {
    intervals.push_back(reference[i] - reference[i - 1]);
  }
  std::sort(intervals.begin(), intervals.end());
  const size_t mid = intervals.size() / 2;
  const float interval =
      intervals.size() % 2 == 1 ? intervals[mid] : 0.5f * (intervals[mid - 1] + intervals[mid]);
  const float window = 0.25f * interval;

  std::vector<float> offsets;
  for (float time : detected) {
    const auto next = std::lower_bound(reference.begin(), reference.end(), time);
    float nearest = *next;
    if (next == reference.end()) {
      nearest = reference.back();
    } else if (next != reference.begin() && time - *(next - 1) < *next - time) {
      nearest = *(next - 1);
    }
    if (std::abs(time - nearest) <= window) offsets.push_back((time - nearest) * 1000.0f);
  }
  if (offsets.empty()) return none;
  std::sort(offsets.begin(), offsets.end());
  const size_t half = offsets.size() / 2;
  return offsets.size() % 2 == 1 ? offsets[half] : 0.5f * (offsets[half - 1] + offsets[half]);
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

/// One annotated or estimated section: a time span and the group it belongs to.
struct StructureSpan {
  float start = 0.0f;
  float end = 0.0f;
  int group = 0;
};

/// Reads `start_sec<TAB>label` lines; the last line is `end_sec<TAB>end`. Equal labels share a
/// group.
std::vector<StructureSpan> read_structure_annotation(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::vector<float> starts;
  std::vector<std::string> labels;
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream fields(line);
    float time = 0.0f;
    std::string label;
    if (fields >> time >> label) {
      starts.push_back(time);
      labels.push_back(label);
    }
  }
  std::vector<StructureSpan> spans;
  if (starts.size() < 2) return spans;
  std::map<std::string, int> group_of;
  for (size_t i = 0; i + 1 < starts.size(); ++i) {
    const int group = group_of.emplace(labels[i], static_cast<int>(group_of.size())).first->second;
    spans.push_back({starts[i], starts[i + 1], group});
  }
  return spans;
}

/// The repetition group the section analyzer reports for a section.
int estimated_group(const Section& section) { return section.group; }

std::vector<StructureSpan> structure_spans(const SectionAnalyzer& analyzer) {
  std::vector<StructureSpan> spans;
  for (const Section& section : analyzer.sections()) {
    spans.push_back({section.start, section.end, estimated_group(section)});
  }
  return spans;
}

/// Interior boundaries: every span start but the first (the track start and end are excluded).
std::vector<float> interior_boundaries(const std::vector<StructureSpan>& spans) {
  std::vector<float> boundaries;
  for (size_t i = 1; i < spans.size(); ++i) boundaries.push_back(spans[i].start);
  return boundaries;
}

bool augment_match(size_t reference, const std::vector<std::vector<size_t>>& edges,
                   std::vector<bool>& visited, std::vector<size_t>& match_of_estimate) {
  for (size_t estimate : edges[reference]) {
    if (visited[estimate]) continue;
    visited[estimate] = true;
    if (match_of_estimate[estimate] == edges.size() ||
        augment_match(match_of_estimate[estimate], edges, visited, match_of_estimate)) {
      match_of_estimate[estimate] = reference;
      return true;
    }
  }
  return false;
}

/// Boundary hit-rate F-measure: a maximum one-to-one matching (augmenting paths) between
/// estimated and reference boundaries that lie within @p window_sec of each other.
float boundary_hit_rate_f(const std::vector<float>& estimated, const std::vector<float>& reference,
                          float window_sec) {
  if (estimated.empty() && reference.empty()) return 1.0f;
  if (estimated.empty() || reference.empty()) return 0.0f;
  std::vector<std::vector<size_t>> edges(reference.size());
  for (size_t r = 0; r < reference.size(); ++r) {
    for (size_t e = 0; e < estimated.size(); ++e) {
      if (std::abs(estimated[e] - reference[r]) <= window_sec) edges[r].push_back(e);
    }
  }
  std::vector<size_t> match_of_estimate(estimated.size(), reference.size());
  size_t matched = 0;
  for (size_t r = 0; r < reference.size(); ++r) {
    std::vector<bool> visited(estimated.size(), false);
    if (augment_match(r, edges, visited, match_of_estimate)) ++matched;
  }
  const float precision = static_cast<float>(matched) / static_cast<float>(estimated.size());
  const float recall = static_cast<float>(matched) / static_cast<float>(reference.size());
  return (precision + recall) > 0.0f ? 2.0f * precision * recall / (precision + recall) : 0.0f;
}

int group_at(const std::vector<StructureSpan>& spans, float time) {
  for (const StructureSpan& span : spans) {
    if (time < span.end) return span.group;
  }
  return spans.back().group;
}

double pair_count(long long n) { return 0.5 * static_cast<double>(n) * static_cast<double>(n - 1); }

/// Pairwise F-measure on 0.1 s frames: over pairs of frames in the same group, precision against
/// the reference pairs and recall of them.
float structure_pairwise_f(const std::vector<StructureSpan>& estimated,
                           const std::vector<StructureSpan>& reference) {
  constexpr float kFrameSec = 0.1f;
  const long long frames = static_cast<long long>(std::ceil(reference.back().end / kFrameSec));
  std::map<int, long long> estimated_counts;
  std::map<int, long long> reference_counts;
  std::map<std::pair<int, int>, long long> joint_counts;
  for (long long k = 0; k < frames; ++k) {
    const float time = (static_cast<float>(k) + 0.5f) * kFrameSec;
    const int e = group_at(estimated, time);
    const int r = group_at(reference, time);
    ++estimated_counts[e];
    ++reference_counts[r];
    ++joint_counts[{e, r}];
  }
  double estimated_pairs = 0.0;
  double reference_pairs = 0.0;
  double joint_pairs = 0.0;
  for (const auto& [group, n] : estimated_counts) estimated_pairs += pair_count(n);
  for (const auto& [group, n] : reference_counts) reference_pairs += pair_count(n);
  for (const auto& [groups, n] : joint_counts) joint_pairs += pair_count(n);
  const double precision = estimated_pairs > 0.0 ? joint_pairs / estimated_pairs : 0.0;
  const double recall = reference_pairs > 0.0 ? joint_pairs / reference_pairs : 0.0;
  return (precision + recall) > 0.0
             ? static_cast<float>(2.0 * precision * recall / (precision + recall))
             : 0.0f;
}

/// Reports the single-block feature sets on the same grid and STFT the analyzer uses, so the
/// timbre and harmony blocks can be compared against the combined result.
void report_structure_feature_sets(const Audio& audio, const std::vector<StructureSpan>& reference,
                                   const std::vector<std::string>& row) {
  const SectionConfig config;
  const Audio analysis_audio = analysis_rate_audio(audio);
  BeatConfig beat_config;
  beat_config.n_fft = config.n_fft;
  beat_config.hop_length = config.hop_length;
  const BeatAnalyzer beats(analysis_audio, beat_config);
  const Spectrogram spec = Spectrogram::compute(
      analysis_audio, section_structure::section_chroma_config(config, analysis_audio.sample_rate())
                          .to_stft_config());
  const auto frames = section_structure::frame_features(analysis_audio, spec, config);
  const SectionGrid grid = section_structure::make_section_grid(beats);
  const std::pair<section_structure::FeatureSet, const char*> sets[] = {
      {section_structure::FeatureSet::kTimbreOnly, "timbre"},
      {section_structure::FeatureSet::kHarmonyOnly, "harmony"}};
  for (const auto& [set, name] : sets) {
    const auto result = section_structure::analyze_structure(frames, grid, config, set);
    std::vector<StructureSpan> estimated;
    for (const Section& section : result.sections) {
      estimated.push_back({section.start, section.end, section.group});
    }
    const auto estimated_boundaries = interior_boundaries(estimated);
    const auto reference_boundaries = interior_boundaries(reference);
    WARN("Report-only structure fixture "
         << row[0] << "/" << row[1] << " features " << name << " HR.5F "
         << boundary_hit_rate_f(estimated_boundaries, reference_boundaries, 0.5f) << "; HR3F "
         << boundary_hit_rate_f(estimated_boundaries, reference_boundaries, 3.0f) << "; pairwise_f "
         << structure_pairwise_f(estimated, reference) << "; min_hr3f " << row[3]
         << "; min_pairwise_f " << row[4]);
  }
}

#ifdef SONARE_WITH_PITCH_EDITOR

using editing::note_model::transcribe_notes;
using editing::note_model::TranscribeConfig;
using editing::note_model::TranscribedNote;
using editing::note_model::TranscribeSource;

/// @brief Reads a mir_eval transcription annotation: "onset_sec offset_sec pitch_hz" per line.
std::vector<test::EvalNote> read_notes_annotation(const std::filesystem::path& path) {
  std::ifstream in(path);
  REQUIRE(in.is_open());
  std::vector<test::EvalNote> notes;
  std::string line;
  size_t line_number = 0;
  while (std::getline(in, line)) {
    ++line_number;
    const size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos || line[first] == '#') continue;
    std::istringstream fields(line);
    double onset = 0.0;
    double offset = 0.0;
    double hz = 0.0;
    INFO("Notes annotation " << path.string() << ":" << line_number);
    REQUIRE((fields >> onset >> offset >> hz));
    REQUIRE(std::isfinite(onset));
    REQUIRE(std::isfinite(offset));
    REQUIRE(onset < offset);
    REQUIRE(std::isfinite(hz));
    REQUIRE(hz > 0.0);
    notes.push_back({onset, offset, test::midi_from_hz(hz)});
  }
  return notes;
}

struct NotesRowResult {
  double f_note = 0.0;
  double f_note_offset = 0.0;
  double f_onset = 0.0;
  size_t truth_count = 0;
  size_t predicted_count = 0;
};

/// @brief Transcribes a manifest row's audio (monophonic) and scores it against its annotation.
NotesRowResult evaluate_notes_row(const std::filesystem::path& root,
                                  const std::vector<std::string>& row) {
  REQUIRE(row.size() >= 5);
  const Audio audio = Audio::from_file((root / row[1]).string());
  const std::vector<test::EvalNote> truth = read_notes_annotation(root / row[2]);
  REQUIRE_FALSE(truth.empty());

  TranscribeConfig config;
  config.source = TranscribeSource::kMonophonic;
  float fmin = 0.0f;
  float fmax = 0.0f;
  if (parse_optional_float_token(row, 5, "fmin=", fmin)) config.fmin = fmin;
  if (parse_optional_float_token(row, 5, "fmax=", fmax)) config.fmax = fmax;
  const std::vector<TranscribedNote> transcribed = transcribe_notes(audio, config);

  const double sample_rate = static_cast<double>(audio.sample_rate());
  std::vector<test::EvalNote> estimates;
  estimates.reserve(transcribed.size());
  for (const TranscribedNote& note : transcribed) {
    REQUIRE(note.median_hz > 0.0f);
    estimates.push_back({static_cast<double>(note.onset_sample) / sample_rate,
                         static_cast<double>(note.offset_sample) / sample_rate,
                         test::midi_from_hz(note.median_hz)});
  }

  test::NoteMatchCriteria onset_only;
  onset_only.pitch_tolerance_cents = std::numeric_limits<double>::infinity();
  test::NoteMatchCriteria with_pitch;
  test::NoteMatchCriteria with_offset;
  with_offset.match_offset = true;

  NotesRowResult result;
  result.f_onset = test::match_notes(truth, estimates, onset_only).f_measure();
  result.f_note = test::match_notes(truth, estimates, with_pitch).f_measure();
  result.f_note_offset = test::match_notes(truth, estimates, with_offset).f_measure();
  result.truth_count = truth.size();
  result.predicted_count = estimates.size();
  return result;
}

std::string notes_report_line(const std::vector<std::string>& row, const NotesRowResult& result) {
  std::ostringstream line;
  line << "Report-only notes fixture " << row[0] << "/" << row[1] << " F-measure_note "
       << result.f_note << "; F-measure_note_offset " << result.f_note_offset
       << "; F-measure_onset " << result.f_onset << "; truth " << result.truth_count
       << "; predicted " << result.predicted_count << "; min_f_note " << row[3]
       << "; min_f_note_offset " << row[4];
  return line.str();
}

#endif  // SONARE_WITH_PITCH_EDITOR

}  // namespace

#ifdef SONARE_WITH_PITCH_EDITOR

TEST_CASE("Music optional note transcription fixtures", "[music_dataset][notes]") {
  const std::filesystem::path root = fixture_root();
  const auto rows = read_rows(root / "notes_manifest.tsv");
  if (rows.empty()) SKIP("No notes fixture rows are configured");

  size_t checked = 0;
  for (const auto& row : rows) {
    if (row.size() < 5) continue;
    if (!std::filesystem::exists(root / row[1]) || !std::filesystem::exists(root / row[2])) {
      continue;
    }

    INFO("Notes fixture: " << row[0] << " / " << row[1]);
    const NotesRowResult result = evaluate_notes_row(root, row);
    if (is_report_only(row, 5)) {
      WARN(notes_report_line(row, result));
    } else {
      REQUIRE(result.f_note >= std::stod(row[3]));
      REQUIRE(result.f_note_offset >= std::stod(row[4]));
    }
    ++checked;
  }
  if (checked == 0) SKIP("No notes audio/annotation fixtures are present");
}

TEST_CASE("Notes fixture row evaluation end to end", "[music_dataset_parsing][notes]") {
  constexpr int kSampleRate = 22050;
  constexpr double kLeadSeconds = 0.2;
  constexpr double kNoteSeconds = 0.4;
  constexpr double kGapSeconds = 0.1;
  constexpr double kFadeSeconds = 0.01;
  const double frequencies[] = {220.0, 261.6255653005986, 329.6275569128699};

  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("sonare_notes_fixture_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);

  const double total_seconds = kLeadSeconds * 2 + kNoteSeconds * 3 + kGapSeconds * 2;
  std::vector<float> samples(static_cast<size_t>(total_seconds * kSampleRate), 0.0f);
  std::ostringstream annotation;
  annotation << "# onset_sec offset_sec pitch_hz\n\n";
  for (size_t i = 0; i < 3; ++i) {
    const double onset = kLeadSeconds + static_cast<double>(i) * (kNoteSeconds + kGapSeconds);
    const size_t first = static_cast<size_t>(std::llround(onset * kSampleRate));
    const size_t count = static_cast<size_t>(std::llround(kNoteSeconds * kSampleRate));
    const double fade_samples = kFadeSeconds * kSampleRate;
    for (size_t n = 0; n < count; ++n) {
      const double envelope = std::min({1.0, static_cast<double>(n) / fade_samples,
                                        static_cast<double>(count - n) / fade_samples});
      samples[first + n] = static_cast<float>(
          0.5 * envelope * std::sin(constants::kTwoPiD * frequencies[i] * n / kSampleRate));
    }
    annotation << onset << '\t' << onset + kNoteSeconds << '\t' << frequencies[i] << '\n';
  }
  save_wav((dir / "notes.wav").string(), samples, kSampleRate);
  {
    std::ofstream out(dir / "notes.txt");
    out << annotation.str();
  }
  {
    std::ofstream out(dir / "notes_manifest.tsv");
    out << "# dataset\tfile\tannotation\tmin_f_note\tmin_f_note_offset\n"
        << "synthetic\tnotes.wav\tnotes.txt\t0.9\t0.5\treport_only\n";
  }

  const auto rows = read_rows(dir / "notes_manifest.tsv");
  REQUIRE(rows.size() == 1);
  const NotesRowResult result = evaluate_notes_row(dir, rows[0]);
  const std::string line = notes_report_line(rows[0], result);
  std::filesystem::remove_all(dir);

  INFO(line);
  CHECK(result.truth_count == 3);
  CHECK(result.predicted_count == 3);
  CHECK(result.f_note == 1.0);
  CHECK(line.find("; truth 3; predicted 3; min_f_note 0.9; min_f_note_offset 0.5") !=
        std::string::npos);
}

TEST_CASE("Note matching", "[note_match]") {
  using test::EvalNote;
  using test::match_notes;
  using test::NoteMatchCriteria;
  const NoteMatchCriteria defaults;

  SECTION("one-to-one: two estimates on one reference give one match") {
    const auto counts =
        match_notes({{1.0, 1.5, 60.0}}, {{1.0, 1.5, 60.0}, {1.01, 1.5, 60.0}}, defaults);
    CHECK(counts.true_positive == 1);
    CHECK(counts.false_positive == 1);
    CHECK(counts.false_negative == 0);
  }

  SECTION("augmenting paths reassign an estimate that a greedy pass would take") {
    const auto counts = match_notes({{0.0, 0.5, 60.0}, {0.04, 0.5, 60.0}},
                                    {{0.02, 0.5, 60.0}, {-0.02, 0.5, 60.0}}, defaults);
    CHECK(counts.true_positive == 2);
  }

  SECTION("onset tolerance") {
    CHECK(match_notes({{1.0, 1.5, 60.0}}, {{1.045, 1.5, 60.0}}, defaults).true_positive == 1);
    CHECK(match_notes({{1.0, 1.5, 60.0}}, {{1.055, 1.5, 60.0}}, defaults).true_positive == 0);
    CHECK(match_notes({{1.0, 1.5, 60.0}}, {{0.945, 1.5, 60.0}}, defaults).true_positive == 0);
  }

  SECTION("pitch tolerance") {
    CHECK(match_notes({{1.0, 1.5, 60.0}}, {{1.0, 1.5, 60.49}}, defaults).true_positive == 1);
    CHECK(match_notes({{1.0, 1.5, 60.0}}, {{1.0, 1.5, 60.51}}, defaults).true_positive == 0);
    CHECK(match_notes({{1.0, 1.5, 60.0}}, {{1.0, 1.5, 59.51}}, defaults).true_positive == 1);
  }

  SECTION("offset tolerance scales with reference length") {
    NoteMatchCriteria with_offset;
    with_offset.match_offset = true;
    // 1 s reference: tolerance 0.2 s.
    CHECK(match_notes({{0.0, 1.0, 60.0}}, {{0.0, 1.19, 60.0}}, with_offset).true_positive == 1);
    CHECK(match_notes({{0.0, 1.0, 60.0}}, {{0.0, 1.21, 60.0}}, with_offset).true_positive == 0);
    // 0.1 s reference: tolerance floors at 50 ms.
    CHECK(match_notes({{0.0, 0.1, 60.0}}, {{0.0, 0.145, 60.0}}, with_offset).true_positive == 1);
    CHECK(match_notes({{0.0, 0.1, 60.0}}, {{0.0, 0.155, 60.0}}, with_offset).true_positive == 0);
    // Offset is ignored unless asked for.
    CHECK(match_notes({{0.0, 1.0, 60.0}}, {{0.0, 3.0, 60.0}}, defaults).true_positive == 1);
  }

  SECTION("infinite pitch tolerance ignores pitch") {
    NoteMatchCriteria onset_only;
    onset_only.pitch_tolerance_cents = std::numeric_limits<double>::infinity();
    CHECK(match_notes({{1.0, 1.5, 60.0}}, {{1.0, 1.5, 72.0}}, onset_only).true_positive == 1);
    CHECK(match_notes({{1.0, 1.5, 60.0}}, {{1.0, 1.5, 72.0}}, defaults).true_positive == 0);
  }

  SECTION("empty input scores zero") {
    const auto none = match_notes({}, {}, defaults);
    CHECK(none.precision() == 0.0);
    CHECK(none.recall() == 0.0);
    CHECK(none.f_measure() == 0.0);
    CHECK(match_notes({{0.0, 1.0, 60.0}}, {}, defaults).f_measure() == 0.0);
    CHECK(match_notes({}, {{0.0, 1.0, 60.0}}, defaults).f_measure() == 0.0);
  }

  SECTION("integer pitches with offset off score as a semitone-exact onset matcher") {
    const std::vector<EvalNote> truth = {{0.0, 0.4, 60.0}, {0.5, 0.9, 62.0}, {1.0, 1.4, 64.0}};
    const std::vector<EvalNote> estimates = {
        {0.01, 0.3, 60.0}, {0.52, 0.9, 63.0}, {1.04, 2.0, 64.0}, {1.5, 1.9, 60.0}};
    const auto counts = match_notes(truth, estimates, defaults);
    CHECK(counts.true_positive == 2);
    CHECK(counts.false_positive == 2);
    CHECK(counts.false_negative == 1);
    CHECK(counts.precision() == Catch::Approx(0.5));
    CHECK(counts.recall() == Catch::Approx(2.0 / 3.0));
  }

  SECTION("midi_from_hz") {
    CHECK(test::midi_from_hz(440.0) == Catch::Approx(69.0));
    CHECK(test::midi_from_hz(880.0) == Catch::Approx(81.0));
  }
}

#endif  // SONARE_WITH_PITCH_EDITOR

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
      KeyProfileType profile_type = KeyProfileType::KrumhanslSchmuckler;
      REQUIRE(parse_key_profile(profile_text, profile_type));
      key_config.profile_type = profile_type;
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
      const float offset_ms =
          median_beat_offset_ms(beat_times_from_beats(analyzer.beats()), reference);
      std::ostringstream offset_field;
      if (std::isfinite(offset_ms)) offset_field << "; median_offset_ms " << offset_ms;
      WARN("Report-only beat fixture " << row[0] << "/" << row[1] << " F-measure " << f_measure
                                       << "; threshold " << row[3] << offset_field.str());
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
    // The majmin row measures the library default; the extended vocabulary, NNLS and inversions
    // run only when the manifest row asks for them.
    const bool evaluate_bass = has_bass_threshold;
    const bool evaluate_extended = has_extended_threshold;
    ChordConfig config;
    if (evaluate_extended) {
      config.use_triads_only = false;
    }
    if (evaluate_extended || evaluate_bass) {
      config.chroma_method = ChromaMethod::NNLS;
      config.detect_inversions = evaluate_bass;
    }
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

TEST_CASE("Music optional structure fixtures", "[music_dataset][structure]") {
  const std::filesystem::path root = fixture_root();
  const auto rows = read_rows(root / "structure_manifest.tsv");
  if (rows.empty()) SKIP("No structure fixture rows are configured");

  size_t checked = 0;
  for (const auto& row : rows) {
    if (row.size() < 5) continue;
    const std::filesystem::path audio_path = root / row[1];
    const std::filesystem::path annotation_path = root / row[2];
    if (!std::filesystem::exists(audio_path) || !std::filesystem::exists(annotation_path)) {
      continue;
    }

    INFO("Structure fixture: " << row[0] << " / " << row[1]);
    const Audio audio = Audio::from_file(audio_path.string());
    const auto reference = read_structure_annotation(annotation_path);
    REQUIRE_FALSE(reference.empty());
    const SectionAnalyzer analyzer(audio, SectionConfig());
    const auto estimated = structure_spans(analyzer);
    REQUIRE_FALSE(estimated.empty());

    const auto estimated_boundaries = interior_boundaries(estimated);
    const auto reference_boundaries = interior_boundaries(reference);
    const float hr05f = boundary_hit_rate_f(estimated_boundaries, reference_boundaries, 0.5f);
    const float hr3f = boundary_hit_rate_f(estimated_boundaries, reference_boundaries, 3.0f);
    const float pairwise_f = structure_pairwise_f(estimated, reference);
    if (is_report_only(row, 5)) {
      WARN("Report-only structure fixture "
           << row[0] << "/" << row[1] << " features both HR.5F " << hr05f << "; HR3F " << hr3f
           << "; pairwise_f " << pairwise_f << "; min_hr3f " << row[3] << "; min_pairwise_f "
           << row[4]);
      report_structure_feature_sets(audio, reference, row);
    } else {
      REQUIRE(hr3f >= std::stof(row[3]));
      REQUIRE(pairwise_f >= std::stof(row[4]));
    }
    ++checked;
  }
  if (checked == 0) SKIP("No structure audio/annotation fixtures are present");
}

TEST_CASE("Music dataset annotation parsing", "[music_dataset_parsing]") {
  PitchClass pitch = PitchClass::C;
  REQUIRE(parse_pitch_class("Eb", pitch));
  CHECK(pitch == PitchClass::Ds);
  REQUIRE(parse_pitch_class("Cb", pitch));
  CHECK(pitch == PitchClass::B);
  CHECK_FALSE(parse_pitch_class("H", pitch));
  CHECK_FALSE(parse_pitch_class("Ex", pitch));

  PitchClass root = PitchClass::C;
  PitchClass bass = PitchClass::C;
  ChordQuality quality = ChordQuality::Major;
  bool has_bass = false;
  REQUIRE(parse_chord_symbol("Bb:min", root, quality, bass, has_bass));
  CHECK(root == PitchClass::As);
  CHECK(quality == ChordQuality::Minor);
  REQUIRE(parse_chord_symbol("C:maj/Ab", root, quality, bass, has_bass));
  CHECK(has_bass);
  CHECK(bass == PitchClass::Gs);

  const std::vector<ReferenceChord> reference = {
      {0.0f, 2.0f, PitchClass::A, ChordQuality::Major, PitchClass::A, false},
      {2.0f, 4.0f, PitchClass::C, ChordQuality::Diminished, PitchClass::C, false},
  };
  const std::vector<Chord> detected = {
      {PitchClass::A, ChordQuality::Dominant7s9, 0.0f, 2.0f, 1.0f},
      {PitchClass::C, ChordQuality::Major, 2.0f, 4.0f, 1.0f},
  };
  // The 7#9 reduces to A major; the dim reference is outside majmin and not scored.
  CHECK(chord_wcsr(detected, reference) == 1.0f);
  // Root accuracy still scores the dim span.
  CHECK(chord_root_accuracy(detected, reference) == 1.0f);
}

}  // namespace sonare
