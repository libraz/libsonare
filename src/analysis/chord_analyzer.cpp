#include "analysis/chord_analyzer.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

#include "core/convert.h"
#include "util/exception.h"

namespace sonare {

std::string Chord::to_string() const {
  std::string name = pitch_class_to_string(root);
  switch (quality) {
    case ChordQuality::Major:
      break;  // Major is implied
    case ChordQuality::Minor:
      name += "m";
      break;
    case ChordQuality::Diminished:
      name += "dim";
      break;
    case ChordQuality::Augmented:
      name += "aug";
      break;
    case ChordQuality::Dominant7:
      name += "7";
      break;
    case ChordQuality::Major7:
      name += "maj7";
      break;
    case ChordQuality::Minor7:
      name += "m7";
      break;
    case ChordQuality::Sus2:
      name += "sus2";
      break;
    case ChordQuality::Sus4:
      name += "sus4";
      break;
    case ChordQuality::Unknown:
      name += "?";
      break;
  }
  return name;
}

ChordAnalyzer::ChordAnalyzer(const Audio& audio, const ChordConfig& config) : config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  // Compute chroma
  ChromaConfig chroma_config;
  chroma_config.n_fft = config.n_fft;
  chroma_config.hop_length = config.hop_length;

  chroma_ = Chroma::compute(audio, chroma_config);

  // Generate templates
  if (config.use_triads_only) {
    templates_ = generate_triad_templates();
  } else {
    templates_ = generate_all_chord_templates();
  }

  analyze_chords();
}

ChordAnalyzer::ChordAnalyzer(const Chroma& chroma, const ChordConfig& config)
    : chroma_(chroma), config_(config) {
  // Generate templates
  if (config.use_triads_only) {
    templates_ = generate_triad_templates();
  } else {
    templates_ = generate_all_chord_templates();
  }

  analyze_chords();
}

int ChordAnalyzer::find_best_chord(const float* chroma) const {
  float best_correlation = -1.0f;
  int best_idx = 0;

  for (size_t i = 0; i < templates_.size(); ++i) {
    float corr = templates_[i].correlate(chroma);
    if (corr > best_correlation) {
      best_correlation = corr;
      best_idx = static_cast<int>(i);
    }
  }

  return best_idx;
}

void ChordAnalyzer::analyze_chords() {
  if (chroma_.empty()) {
    return;
  }

  int n_frames = chroma_.n_frames();
  int n_chroma = chroma_.n_chroma();

  // Smoothing: apply running average to chroma
  int smooth_frames =
      static_cast<int>(config_.smoothing_window * chroma_.sample_rate() / chroma_.hop_length());
  smooth_frames = std::max(1, smooth_frames);

  // Detect chord for each frame
  frame_chords_.resize(n_frames);
  std::vector<float> confidences(n_frames);

  for (int f = 0; f < n_frames; ++f) {
    // Compute smoothed chroma
    std::array<float, 12> smoothed = {};
    int start = std::max(0, f - smooth_frames / 2);
    int end = std::min(n_frames, f + smooth_frames / 2 + 1);
    int count = end - start;

    for (int sf = start; sf < end; ++sf) {
      for (int c = 0; c < n_chroma; ++c) {
        smoothed[c] += chroma_.at(c, sf);
      }
    }

    for (int c = 0; c < n_chroma; ++c) {
      smoothed[c] /= static_cast<float>(count);
    }

    // Find best chord
    float best_corr = -1.0f;
    int best_idx = 0;

    for (size_t i = 0; i < templates_.size(); ++i) {
      float corr = templates_[i].correlate(smoothed.data());
      if (corr > best_corr) {
        best_corr = corr;
        best_idx = static_cast<int>(i);
      }
    }

    frame_chords_[f] = best_idx;
    confidences[f] = best_corr;
  }

  // Convert frame-level to segment-level chords
  if (n_frames == 0) return;

  float hop_duration = static_cast<float>(chroma_.hop_length()) / chroma_.sample_rate();

  int current_chord = frame_chords_[0];
  int segment_start = 0;
  float segment_confidence = confidences[0];
  int confidence_count = 1;

  for (int f = 1; f <= n_frames; ++f) {
    bool is_last = (f == n_frames);
    bool chord_changed = !is_last && (frame_chords_[f] != current_chord);

    if (chord_changed || is_last) {
      // End current segment
      Chord chord;
      chord.root = templates_[current_chord].root;
      chord.quality = templates_[current_chord].quality;
      chord.start = static_cast<float>(segment_start) * hop_duration;
      chord.end = static_cast<float>(f) * hop_duration;
      chord.confidence = segment_confidence / static_cast<float>(confidence_count);

      chords_.push_back(chord);

      if (!is_last) {
        current_chord = frame_chords_[f];
        segment_start = f;
        segment_confidence = confidences[f];
        confidence_count = 1;
      }
    } else if (!is_last) {
      segment_confidence += confidences[f];
      confidence_count++;
    }
  }

  // Merge short segments
  merge_short_segments();
}

void ChordAnalyzer::merge_short_segments() {
  if (chords_.size() < 2) return;

  std::vector<Chord> merged;
  merged.reserve(chords_.size());

  for (size_t i = 0; i < chords_.size(); ++i) {
    const Chord& chord = chords_[i];

    if (chord.duration() < config_.min_duration && !merged.empty()) {
      // Merge with previous chord
      merged.back().end = chord.end;
    } else if (chord.duration() < config_.min_duration && i + 1 < chords_.size()) {
      // Very short first chord: merge with next
      // Skip for now, will be handled by next iteration
      continue;
    } else if (!merged.empty() && merged.back().root == chord.root &&
               merged.back().quality == chord.quality) {
      // Same chord as previous: extend
      merged.back().end = chord.end;
      merged.back().confidence = (merged.back().confidence + chord.confidence) / 2.0f;
    } else {
      merged.push_back(chord);
    }
  }

  chords_ = std::move(merged);
}

std::string ChordAnalyzer::progression_pattern() const {
  if (chords_.empty()) return "";

  std::ostringstream oss;
  bool first = true;

  for (const auto& chord : chords_) {
    if (!first) {
      oss << " - ";
    }
    oss << chord.to_string();
    first = false;
  }

  return oss.str();
}

std::string ChordAnalyzer::chord_to_roman_numeral(const Chord& chord, PitchClass key_root,
                                                  Mode /* mode */) const {
  // Calculate interval from key root
  int interval = (static_cast<int>(chord.root) - static_cast<int>(key_root) + 12) % 12;

  // Scale degree names (major scale intervals)
  static const std::vector<std::pair<int, std::string>> major_degrees = {
      {0, "I"}, {2, "II"}, {4, "III"}, {5, "IV"}, {7, "V"}, {9, "VI"}, {11, "VII"}};

  // Find closest scale degree
  std::string numeral;
  bool is_chromatic = true;

  for (const auto& deg : major_degrees) {
    if (deg.first == interval) {
      numeral = deg.second;
      is_chromatic = false;
      break;
    }
  }

  // Handle chromatic chords (flat/sharp)
  if (is_chromatic) {
    for (const auto& deg : major_degrees) {
      if ((deg.first + 1) % 12 == interval) {
        numeral = "b" + deg.second;
        break;
      }
      if ((deg.first - 1 + 12) % 12 == interval) {
        numeral = "#" + std::string(deg.second.begin(), deg.second.end() - 1) +
                  std::string(1, deg.second.back());
        break;
      }
    }
  }

  if (numeral.empty()) {
    numeral = "?";
  }

  // Adjust case based on chord quality
  bool is_minor = (chord.quality == ChordQuality::Minor || chord.quality == ChordQuality::Minor7 ||
                   chord.quality == ChordQuality::Diminished);

  if (is_minor) {
    // Convert to lowercase
    for (char& c : numeral) {
      if (c >= 'A' && c <= 'Z') {
        c = c - 'A' + 'a';
      }
    }
  }

  // Add quality suffix
  switch (chord.quality) {
    case ChordQuality::Diminished:
      numeral += "°";
      break;
    case ChordQuality::Augmented:
      numeral += "+";
      break;
    case ChordQuality::Dominant7:
      numeral += "7";
      break;
    case ChordQuality::Major7:
      numeral += "maj7";
      break;
    case ChordQuality::Minor7:
      numeral += "7";
      break;
    default:
      break;
  }

  return numeral;
}

std::vector<std::string> ChordAnalyzer::functional_analysis(PitchClass key_root, Mode mode) const {
  std::vector<std::string> result;
  result.reserve(chords_.size());

  for (const auto& chord : chords_) {
    result.push_back(chord_to_roman_numeral(chord, key_root, mode));
  }

  return result;
}

Chord ChordAnalyzer::chord_at(float time) const {
  for (const auto& chord : chords_) {
    if (time >= chord.start && time < chord.end) {
      return chord;
    }
  }

  // Return empty chord
  Chord empty;
  empty.root = PitchClass::C;
  empty.quality = ChordQuality::Major;
  empty.start = 0.0f;
  empty.end = 0.0f;
  empty.confidence = 0.0f;
  return empty;
}

Chord ChordAnalyzer::most_common_chord() const {
  if (chords_.empty()) {
    Chord empty;
    empty.root = PitchClass::C;
    empty.quality = ChordQuality::Major;
    empty.start = 0.0f;
    empty.end = 0.0f;
    empty.confidence = 0.0f;
    return empty;
  }

  // Count duration for each chord type
  std::map<std::pair<PitchClass, ChordQuality>, float> durations;

  for (const auto& chord : chords_) {
    auto key = std::make_pair(chord.root, chord.quality);
    durations[key] += chord.duration();
  }

  // Find chord with maximum duration
  auto max_it = std::max_element(durations.begin(), durations.end(),
                                 [](const auto& a, const auto& b) { return a.second < b.second; });

  // Find the first occurrence of this chord type
  for (const auto& chord : chords_) {
    if (chord.root == max_it->first.first && chord.quality == max_it->first.second) {
      return chord;
    }
  }

  return chords_[0];
}

std::vector<Chord> detect_chords(const Audio& audio, const ChordConfig& config) {
  ChordAnalyzer analyzer(audio, config);
  return analyzer.chords();
}

}  // namespace sonare
