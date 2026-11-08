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
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
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
  double start = 0.0;
  double end = 0.0;
  PitchClass root = PitchClass::C;
  ChordQuality quality = ChordQuality::Major;
  PitchClass bass = PitchClass::C;
  bool has_bass = false;
  enum class State { Known, NoChord, Excluded } state = State::Known;
};

std::vector<ReferenceChord> read_chord_annotation(const std::filesystem::path& path) {
  std::vector<ReferenceChord> chords;
  std::ifstream input(path);
  if (!input) throw std::runtime_error("Cannot open chord annotation: " + path.string());
  std::string line;
  size_t line_number = 0;
  double previous_start = -1.0;
  double previous_end = -1.0;
  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const auto fields = split_tsv(line);
    auto invalid = [&](const std::string& reason) -> void {
      throw std::runtime_error(path.string() + ":" + std::to_string(line_number) + ": " + reason);
    };
    if (fields.size() < 3) invalid("expected start, end, and chord symbol");
    auto parse_time = [&](const std::string& value, const char* name) {
      size_t consumed = 0;
      double result = 0.0;
      try {
        result = std::stod(value, &consumed);
      } catch (const std::exception&) {
        invalid(std::string("invalid ") + name + " time");
      }
      if (consumed != value.size() || !std::isfinite(result)) {
        invalid(std::string("invalid ") + name + " time");
      }
      return result;
    };
    ReferenceChord chord;
    const double start = parse_time(fields[0], "start");
    const double end = parse_time(fields[1], "end");
    if (start < 0.0 || end <= start)
      invalid("interval must have finite nonnegative start and positive duration");
    if (!chords.empty() && (start < previous_start || start < previous_end)) {
      invalid("intervals must be sorted and non-overlapping");
    }
    chord.start = start;
    chord.end = end;
    std::string symbol = fields[2];
    if (symbol == "N") {
      chord.state = ReferenceChord::State::NoChord;
      chord.quality = ChordQuality::Unknown;
    } else if (symbol == "X") {
      chord.state = ReferenceChord::State::Excluded;
      chord.quality = ChordQuality::Unknown;
    } else if (!parse_chord_symbol(symbol, chord.root, chord.quality, chord.bass, chord.has_bass)) {
      invalid("unsupported chord symbol: " + symbol);
    }
    chords.push_back(chord);
    previous_start = start;
    previous_end = end;
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

bool exact_chord_equal(const Chord& detected, const ReferenceChord& reference);

bool maj_min_equal(const Chord& detected, const ReferenceChord& reference) {
  if (reference.state == ReferenceChord::State::NoChord) {
    return detected.quality == ChordQuality::Unknown;
  }
  if (reference.state != ReferenceChord::State::Known ||
      detected.quality == ChordQuality::Unknown) {
    return false;
  }
  ChordQuality detected_collapsed;
  ChordQuality reference_collapsed;
  return detected.root == reference.root &&
         majmin_reduction(detected.quality, detected_collapsed) &&
         majmin_reduction(reference.quality, reference_collapsed) &&
         detected_collapsed == reference_collapsed;
}

enum class ChordMetric { MajMin, Root, Quality, Exact, Bass };

struct ChordMetricMeasurement {
  double correct_seconds = 0.0;
  double scored_seconds = 0.0;
  double annotated_seconds = 0.0;
  double ignored_seconds = 0.0;
  double missing_prediction_seconds = 0.0;
  bool measured() const { return scored_seconds > 0.0; }
  double score() const {
    return measured() ? correct_seconds / scored_seconds : std::numeric_limits<double>::quiet_NaN();
  }
};

/// Integrate the selected label over actual interval intersections. Prediction gaps remain in
/// the scored denominator, while excluded annotation vocabulary contributes only to ignored time.
ChordMetricMeasurement measure_chords(const std::vector<Chord>& detected,
                                      const std::vector<ReferenceChord>& reference,
                                      ChordMetric metric) {
  ChordMetricMeasurement result;
  for (const auto& ref : reference) {
    const double ref_start = ref.start;
    const double ref_end = ref.end;
    const double duration = ref_end - ref_start;
    result.annotated_seconds += duration;
    if (ref.state == ReferenceChord::State::Excluded) {
      result.ignored_seconds += duration;
      continue;
    }
    ChordQuality reduced;
    const bool supported =
        ref.state == ReferenceChord::State::NoChord
            ? metric == ChordMetric::MajMin
            : (metric != ChordMetric::MajMin || majmin_reduction(ref.quality, reduced));
    if (!supported || (metric == ChordMetric::Bass &&
                       (ref.state == ReferenceChord::State::NoChord || !ref.has_bass))) {
      result.ignored_seconds += duration;
      continue;
    }
    result.scored_seconds += duration;

    std::vector<double> boundaries{ref_start, ref_end};
    for (const auto& chord : detected) {
      const double start = std::max(ref_start, static_cast<double>(chord.start));
      const double end = std::min(ref_end, static_cast<double>(chord.end));
      if (end > start) {
        boundaries.push_back(start);
        boundaries.push_back(end);
      }
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    for (size_t index = 1; index < boundaries.size(); ++index) {
      const double start = boundaries[index - 1];
      const double end = boundaries[index];
      if (end <= start) continue;
      const double midpoint = start + (end - start) * 0.5;
      auto found = std::find_if(detected.begin(), detected.end(), [midpoint](const Chord& chord) {
        return static_cast<double>(chord.start) <= midpoint && midpoint < chord.end;
      });
      if (found == detected.end()) {
        result.missing_prediction_seconds += end - start;
        continue;
      }
      bool correct = false;
      if (ref.state == ReferenceChord::State::NoChord) {
        correct = found->quality == ChordQuality::Unknown;
      } else if (found->quality != ChordQuality::Unknown) {
        switch (metric) {
          case ChordMetric::MajMin:
            correct = maj_min_equal(*found, ref);
            break;
          case ChordMetric::Root:
            correct = found->root == ref.root;
            break;
          case ChordMetric::Quality:
            correct = found->quality == ref.quality;
            break;
          case ChordMetric::Exact:
            correct = exact_chord_equal(*found, ref);
            break;
          case ChordMetric::Bass:
            correct = found->bass == ref.bass;
            break;
        }
      }
      if (correct) result.correct_seconds += end - start;
    }
  }
  return result;
}

float chord_metric_score(const std::vector<Chord>& detected,
                         const std::vector<ReferenceChord>& reference, ChordMetric metric) {
  const double score = measure_chords(detected, reference, metric).score();
  return static_cast<float>(score);
}

/// Chord metric wrappers retain their historical private names for fixture callers.
float chord_wcsr(const std::vector<Chord>& detected, const std::vector<ReferenceChord>& reference) {
  return chord_metric_score(detected, reference, ChordMetric::MajMin);
}

bool exact_chord_equal(const Chord& detected, const ReferenceChord& reference) {
  return detected.root == reference.root && detected.quality == reference.quality;
}

float chord_root_accuracy(const std::vector<Chord>& detected,
                          const std::vector<ReferenceChord>& reference) {
  return chord_metric_score(detected, reference, ChordMetric::Root);
}

float chord_quality_accuracy(const std::vector<Chord>& detected,
                             const std::vector<ReferenceChord>& reference) {
  return chord_metric_score(detected, reference, ChordMetric::Quality);
}

float chord_exact_wcsr(const std::vector<Chord>& detected,
                       const std::vector<ReferenceChord>& reference) {
  return chord_metric_score(detected, reference, ChordMetric::Exact);
}

float chord_bass_accuracy(const std::vector<Chord>& detected,
                          const std::vector<ReferenceChord>& reference) {
  return chord_metric_score(detected, reference, ChordMetric::Bass);
}

const char* chord_metric_name(ChordMetric metric) {
  switch (metric) {
    case ChordMetric::MajMin:
      return "chord_wcsr";
    case ChordMetric::Root:
      return "chord_root_accuracy";
    case ChordMetric::Quality:
      return "chord_quality_accuracy";
    case ChordMetric::Exact:
      return "chord_exact_wcsr";
    case ChordMetric::Bass:
      return "chord_bass_acc";
  }
  return "unknown";
}

std::string chord_measurement_warning(const std::string& fixture, ChordMetric metric,
                                      const ChordMetricMeasurement& measurement, double threshold,
                                      const char* scope, const char* metric_override = nullptr) {
  std::ostringstream output;
  output << std::setprecision(17) << "Report-only chord measurement fixture " << fixture
         << " metric " << (metric_override ? metric_override : chord_metric_name(metric))
         << " version chord_overlap_v2"
         << " correct_seconds " << measurement.correct_seconds << " scored_seconds "
         << measurement.scored_seconds << " annotated_seconds " << measurement.annotated_seconds
         << " ignored_seconds " << measurement.ignored_seconds << " missing_prediction_seconds "
         << measurement.missing_prediction_seconds << " score " << measurement.score()
         << " threshold " << threshold << " scope " << scope;
  return output.str();
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
    const auto majmin_measurement = measure_chords(detected, reference, ChordMetric::MajMin);
    const auto root_measurement = measure_chords(detected, reference, ChordMetric::Root);
    const auto quality_measurement = measure_chords(detected, reference, ChordMetric::Quality);
    const auto exact_measurement = measure_chords(detected, reference, ChordMetric::Exact);
    const auto bass_measurement = measure_chords(detected, reference, ChordMetric::Bass);
    const float wcsr = static_cast<float>(majmin_measurement.score());
    const float extended_wcsr = evaluate_extended ? static_cast<float>(exact_measurement.score())
                                                  : std::numeric_limits<float>::quiet_NaN();
    const float bass_accuracy = static_cast<float>(bass_measurement.score());
    const float change_rate = chord_change_rate_per_minute(detected);
    const float baseline_change_rate = chord_change_rate_per_minute(baseline_detected);
    const float change_reduction =
        baseline_change_rate > 0.0f
            ? std::max(0.0f, (baseline_change_rate - change_rate) / baseline_change_rate)
            : (change_rate <= 0.0f ? 1.0f : 0.0f);
    if (is_report_only(row, 4)) {
      const std::string fixture = row[0] + "/" + row[1];
      WARN(chord_measurement_warning(fixture, ChordMetric::MajMin, majmin_measurement,
                                     std::stof(row[3]), "known_plus_no_chord"));
      WARN(chord_measurement_warning(fixture, ChordMetric::Root, root_measurement,
                                     std::numeric_limits<double>::quiet_NaN(), "known_chords"));
      WARN(chord_measurement_warning(fixture, ChordMetric::Quality, quality_measurement,
                                     std::numeric_limits<double>::quiet_NaN(), "known_chords"));
      WARN(chord_measurement_warning(fixture, ChordMetric::Exact, exact_measurement,
                                     std::numeric_limits<double>::quiet_NaN(), "known_chords"));
      if (evaluate_extended) {
        WARN(chord_measurement_warning(fixture, ChordMetric::Exact, exact_measurement,
                                       min_extended_wcsr, "extended_vocabulary",
                                       "chord_extended_wcsr"));
      }
      if (evaluate_bass) {
        WARN(chord_measurement_warning(fixture, ChordMetric::Bass, bass_measurement, min_bass_acc,
                                       "explicit_known_bass"));
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
    } else {
      REQUIRE(std::isfinite(wcsr));
      REQUIRE(wcsr >= std::stof(row[3]));
      if (has_bass_threshold) {
        REQUIRE(std::isfinite(bass_accuracy));
        REQUIRE(bass_accuracy >= min_bass_acc);
      }
      if (has_extended_threshold) {
        REQUIRE(std::isfinite(extended_wcsr));
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

TEST_CASE("Chord metrics integrate interval overlap", "[music_dataset_parsing][chord_overlap]") {
  const std::vector<ReferenceChord> reference = {
      {0.0, 10.0, PitchClass::C, ChordQuality::Major, PitchClass::C, true},
  };
  const std::vector<Chord> midpoint_only = {
      {PitchClass::C, ChordQuality::Major, 4.9f, 5.1f, 1.0f},
  };
  const std::vector<Chord> away_from_midpoint = {
      {PitchClass::C, ChordQuality::Major, 0.5f, 0.7f, 1.0f},
  };
  const auto midpoint_measurement = measure_chords(midpoint_only, reference, ChordMetric::MajMin);
  const auto away_measurement = measure_chords(away_from_midpoint, reference, ChordMetric::MajMin);
  CHECK(midpoint_measurement.score() == Catch::Approx(0.02));
  CHECK(away_measurement.score() == Catch::Approx(0.02));
  CHECK(midpoint_measurement.scored_seconds == Catch::Approx(10.0));

  const std::vector<Chord> one_prediction = {
      {PitchClass::C, ChordQuality::Major, 0.0f, 10.0f, 1.0f},
  };
  const std::vector<Chord> split_prediction = {
      {PitchClass::C, ChordQuality::Major, 0.0f, 4.0f, 1.0f},
      {PitchClass::C, ChordQuality::Major, 4.0f, 10.0f, 1.0f},
  };
  CHECK(measure_chords(one_prediction, reference, ChordMetric::Exact).score() == 1.0);
  CHECK(measure_chords(split_prediction, reference, ChordMetric::Exact).score() == 1.0);
  CHECK(chord_quality_accuracy(one_prediction, reference) == 1.0f);
  CHECK(chord_exact_wcsr(one_prediction, reference) == 1.0f);
  CHECK(chord_bass_accuracy(one_prediction, reference) == 1.0f);

  const std::vector<Chord> with_gap = {
      {PitchClass::C, ChordQuality::Major, 0.0f, 3.0f, 1.0f},
      {PitchClass::C, ChordQuality::Major, 5.0f, 10.0f, 1.0f},
  };
  const auto gap_measurement = measure_chords(with_gap, reference, ChordMetric::Exact);
  CHECK(gap_measurement.score() == Catch::Approx(0.8));
  CHECK(gap_measurement.missing_prediction_seconds == Catch::Approx(2.0));
}

TEST_CASE("Chord metrics handle no-chord and excluded labels",
          "[music_dataset_parsing][chord_overlap]") {
  ReferenceChord no_chord;
  no_chord.start = 0.0;
  no_chord.end = 2.0;
  no_chord.state = ReferenceChord::State::NoChord;
  no_chord.quality = ChordQuality::Unknown;
  ReferenceChord excluded;
  excluded.start = 2.0;
  excluded.end = 3.0;
  excluded.state = ReferenceChord::State::Excluded;
  ReferenceChord known{3.0, 4.0, PitchClass::C, ChordQuality::Major, PitchClass::C, true};
  const std::vector<ReferenceChord> reference{no_chord, excluded, known};
  const std::vector<Chord> detected = {
      {PitchClass::G, ChordQuality::Unknown, 0.0f, 2.0f, 1.0f},
      {PitchClass::C, ChordQuality::Major, 3.0f, 4.0f, 1.0f},
  };
  const auto majmin = measure_chords(detected, reference, ChordMetric::MajMin);
  const auto root = measure_chords(detected, reference, ChordMetric::Root);
  const auto quality = measure_chords(detected, reference, ChordMetric::Quality);
  const auto exact = measure_chords(detected, reference, ChordMetric::Exact);
  CHECK(majmin.score() == 1.0);
  CHECK(root.score() == 1.0);
  CHECK(quality.score() == 1.0);
  CHECK(exact.score() == 1.0);
  CHECK(root.scored_seconds == Catch::Approx(1.0));
  CHECK(quality.ignored_seconds == Catch::Approx(3.0));
  CHECK(majmin.annotated_seconds == Catch::Approx(4.0));
  CHECK(majmin.scored_seconds == Catch::Approx(3.0));
  CHECK(majmin.ignored_seconds == Catch::Approx(1.0));

  const std::vector<Chord> unknown_over_everything = {
      {PitchClass::C, ChordQuality::Unknown, 0.0f, 4.0f, 1.0f},
  };
  const auto known_only_root =
      measure_chords(unknown_over_everything, reference, ChordMetric::Root);
  CHECK(known_only_root.score() == 0.0);
  CHECK(known_only_root.scored_seconds == Catch::Approx(1.0));
  CHECK(known_only_root.ignored_seconds == Catch::Approx(3.0));

  const std::vector<ReferenceChord> only_excluded{excluded};
  const auto unmeasured = measure_chords({}, only_excluded, ChordMetric::MajMin);
  CHECK_FALSE(unmeasured.measured());
  CHECK_FALSE(std::isfinite(chord_wcsr({}, only_excluded)));
}

TEST_CASE("Chord annotation parsing fails closed with line diagnostics",
          "[music_dataset_parsing][chord_overlap]") {
  const auto write_temp = [](const std::string& contents) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("sonare-chord-annotation-" + std::to_string(stamp) + ".tsv");
    std::ofstream output(path);
    output << contents;
    output.close();
    return path;
  };
  const auto valid_path = write_temp("0\t1\tN\n1\t2\tX\n");
  const auto parsed = read_chord_annotation(valid_path);
  std::filesystem::remove(valid_path);
  REQUIRE(parsed.size() == 2);
  CHECK(parsed[0].state == ReferenceChord::State::NoChord);
  CHECK(parsed[1].state == ReferenceChord::State::Excluded);

  const auto expect_invalid = [&](const std::string& contents, const std::string& marker) {
    const auto path = write_temp(contents);
    try {
      (void)read_chord_annotation(path);
      CHECK(false);
    } catch (const std::runtime_error& error) {
      CHECK(std::string(error.what()).find(path.string() + ":2:") != std::string::npos);
      CHECK(std::string(error.what()).find(marker) != std::string::npos);
    }
    std::filesystem::remove(path);
  };
  expect_invalid("0\t1\tC\n1\t2\tH:maj\n", "unsupported chord symbol");
  expect_invalid("0\t1\tC\n0.5\t2\tG\n", "non-overlapping");
  expect_invalid("0\t1\tC\n1\tinf\tG\n", "invalid end time");
  expect_invalid("1\t2\tC\n0\t0.5\tG\n", "sorted and non-overlapping");
}

}  // namespace sonare
