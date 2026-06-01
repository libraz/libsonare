#include "analysis/chord_analyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <sstream>

#include "core/convert.h"
#include "feature/nnls_chroma.h"
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
    case ChordQuality::Add9:
      name += "add9";
      break;
    case ChordQuality::MinorAdd9:
      name += "madd9";
      break;
    case ChordQuality::Dim7:
      name += "dim7";
      break;
    case ChordQuality::HalfDim7:
      name += "m7b5";
      break;
    case ChordQuality::Major9:
      name += "maj9";
      break;
    case ChordQuality::Dominant9:
      name += "9";
      break;
    case ChordQuality::Sus2Add4:
      name += "sus2add4";
      break;
  }
  if (bass != root) {
    name += "/";
    name += pitch_class_to_string(bass);
  }
  return name;
}

ChordAnalyzer::ChordAnalyzer(const Audio& audio, const ChordConfig& config) : config_(config) {
  SONARE_CHECK(!audio.empty(), ErrorCode::InvalidParameter);

  if (config.chroma_method == ChromaMethod::NNLS) {
    NnlsChromaConfig nnls_config;
    // Chord recognition does not need the full seven-octave, 100-iteration
    // analysis profile used by the standalone NNLS feature. A lighter front-end
    // keeps the audio constructor practical for tests and interactive use while
    // preserving the semitone salience needed by the template matcher.
    nnls_config.cqt.bins_per_octave = 12;
    nnls_config.cqt.n_bins = 84;
    nnls_config.cqt.hop_length = config.hop_length;
    nnls_config.midi_min = 24;
    nnls_config.n_pitches = 60;
    nnls_config.n_harmonics = 4;
    nnls_config.max_iter = 25;
    nnls_config.tolerance = 1.0e-3f;
    chroma_ = nnls_chroma(audio, nnls_config);
  } else {
    ChromaConfig chroma_config;
    chroma_config.n_fft = config.n_fft;
    chroma_config.hop_length = config.hop_length;
    chroma_ = Chroma::compute(audio, chroma_config);
  }

  if (config.detect_inversions) {
    BassChromaConfig bass_config;
    bass_config.cqt.hop_length = config.hop_length;
    bass_chroma_ = bass_chroma(audio, bass_config);
  }

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

ChordAnalyzer::ChordAnalyzer(const Chroma& chroma, const std::vector<float>& beat_times,
                             const ChordConfig& config)
    : chroma_(chroma), config_(config) {
  // Generate templates
  if (config.use_triads_only) {
    templates_ = generate_triad_templates();
  } else {
    templates_ = generate_all_chord_templates();
  }

  if (config.use_beat_sync && !beat_times.empty()) {
    analyze_chords_beat_sync(beat_times);
  } else {
    analyze_chords();
  }
}

namespace {

/// @brief Checks if a chord quality is a triad (not 7th or sus chord).
bool is_triad(ChordQuality quality) {
  return quality == ChordQuality::Major || quality == ChordQuality::Minor ||
         quality == ChordQuality::Diminished || quality == ChordQuality::Augmented;
}

float extension_threshold(ChordQuality quality) {
  switch (quality) {
    case ChordQuality::Add9:
    case ChordQuality::MinorAdd9:
    case ChordQuality::Major9:
    case ChordQuality::Dominant9:
    case ChordQuality::Sus2Add4:
    case ChordQuality::Dim7:
    case ChordQuality::HalfDim7:
      return 0.05f;
    default:
      return chord_constants::kTetradThreshold;
  }
}

/// @brief Checks whether @p chord's pattern contains @p pitch.
/// @details Uses the already-selected template directly instead of regenerating
/// the full 192-entry template set per call, which also avoids a template-set
/// mismatch when @c use_triads_only is enabled.
bool chord_contains_pitch_class(const ChordTemplate& chord, PitchClass pitch) {
  return chord.pattern[static_cast<int>(pitch)] > 0.0f || pitch == chord.root;
}

}  // namespace

ChordAnalyzer::ChordMatch ChordAnalyzer::find_best_chord_with_confidence(
    const float* chroma) const {
  // Find best triad and best tetrad separately
  float best_triad_corr = -1.0f;
  int best_triad_idx = 0;
  float best_tetrad_corr = -1.0f;
  int best_tetrad_idx = 0;

  for (size_t i = 0; i < templates_.size(); ++i) {
    float corr = templates_[i].correlate(chroma);

    if (is_triad(templates_[i].quality)) {
      if (corr > best_triad_corr) {
        best_triad_corr = corr;
        best_triad_idx = static_cast<int>(i);
      }
    } else {
      if (corr > best_tetrad_corr) {
        best_tetrad_corr = corr;
        best_tetrad_idx = static_cast<int>(i);
      }
    }
  }

  // Prefer triad unless the richer template has enough additional evidence.
  ChordMatch result;
  if (best_tetrad_corr >
      best_triad_corr + extension_threshold(templates_[best_tetrad_idx].quality)) {
    result.index = best_tetrad_idx;
    result.confidence = std::min(1.0f, std::max(0.0f, best_tetrad_corr));
  } else {
    result.index = best_triad_idx;
    result.confidence = std::min(1.0f, std::max(0.0f, best_triad_corr));
  }
  return result;
}

int ChordAnalyzer::find_best_chord(const float* chroma) const {
  return find_best_chord_with_confidence(chroma).index;
}

ChordHmmObservation ChordAnalyzer::chord_observation(const float* chroma) const {
  ChordHmmObservation observation;
  observation.candidates.reserve(templates_.size());
  for (size_t i = 0; i < templates_.size(); ++i) {
    observation.candidates.emplace_back(static_cast<int>(i), templates_[i].correlate(chroma));
  }
  return observation;
}

ChordHmmConfig ChordAnalyzer::hmm_config() const {
  ChordHmmConfig config;
  config.beam_width = config_.hmm_beam_width;
  config.use_key_context = config_.use_key_context;
  config.key_root = config_.key_root;
  config.key_mode = config_.key_mode;
  return config;
}

PitchClass ChordAnalyzer::estimate_bass_pitch_class(int start_frame, int end_frame,
                                                    const ChordTemplate& chord) const {
  const bool has_bass_source = !bass_chroma_.empty();
  const Chroma& source = has_bass_source ? bass_chroma_ : chroma_;
  if (!config_.detect_inversions || source.empty()) {
    return PitchClass::C;
  }

  start_frame = std::max(0, start_frame);
  end_frame = std::min(source.n_frames(), end_frame);
  if (start_frame >= end_frame || source.n_chroma() != 12) {
    return PitchClass::C;
  }

  std::array<float, 12> energy = {};
  for (int f = start_frame; f < end_frame; ++f) {
    for (int c = 0; c < 12; ++c) {
      energy[c] += source.at(c, f);
    }
  }

  // The chroma index c is an octave-folded pitch class, not a true pitch height,
  // so any index-linear weight (e.g. energy[c] * (1 - k*c)) biases the result
  // toward low pitch classes (C) regardless of the actual bass register. Compare
  // the raw folded bass-band energy instead; the low-register emphasis already
  // comes from bass_chroma_'s low-frequency CQT front-end.
  int best = static_cast<int>(chord.root);
  int best_non_root = -1;
  for (int c = 0; c < 12; ++c) {
    if (chord.pattern[c] <= 0.0f) {
      continue;
    }
    if (energy[c] > energy[best]) {
      best = c;
    }
    if (c != static_cast<int>(chord.root)) {
      const float non_root_previous = best_non_root < 0 ? -1.0f : energy[best_non_root];
      if (energy[c] > non_root_previous) {
        best_non_root = c;
      }
    }
  }
  if (has_bass_source && best == static_cast<int>(chord.root) && best_non_root >= 0) {
    const float root_score = energy[best];
    const float non_root_score = energy[best_non_root];
    if (non_root_score >= root_score * 0.70f) {
      best = best_non_root;
    }
  }
  const int major_or_minor_third =
      chord.quality == ChordQuality::Major || chord.quality == ChordQuality::Dominant7 ||
              chord.quality == ChordQuality::Major7 || chord.quality == ChordQuality::Augmented ||
              chord.quality == ChordQuality::Add9 || chord.quality == ChordQuality::Major9 ||
              chord.quality == ChordQuality::Dominant9
          ? (static_cast<int>(chord.root) + 4) % 12
          : (static_cast<int>(chord.root) + 3) % 12;
  if (has_bass_source && best != static_cast<int>(chord.root) &&
      chord.pattern[major_or_minor_third] > 0.0f) {
    const float third_score = energy[major_or_minor_third];
    const float best_score = energy[best];
    if (third_score >= best_score * 0.75f) {
      best = major_or_minor_third;
    }
  }
  return static_cast<PitchClass>(best);
}

void ChordAnalyzer::analyze_chords() {
  if (chroma_.empty()) {
    return;
  }

  int n_frames = chroma_.n_frames();
  // The smoothing/observation buffers are fixed at 12 pitch classes; clamp the
  // chroma iteration so a chromagram with more than 12 bins cannot overrun them.
  int n_chroma = std::min(chroma_.n_chroma(), 12);

  // Smoothing: apply running average to chroma
  int smooth_frames =
      static_cast<int>(config_.smoothing_window * chroma_.sample_rate() / chroma_.hop_length());
  smooth_frames = std::max(1, smooth_frames);

  // Detect chord for each frame
  frame_chords_.resize(n_frames);
  std::vector<float> confidences(n_frames);
  std::vector<ChordHmmObservation> observations;
  if (config_.use_hmm) {
    observations.reserve(n_frames);
  }

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

    // Find best chord using shared logic
    ChordMatch match = find_best_chord_with_confidence(smoothed.data());
    frame_chords_[f] = match.index;
    confidences[f] = match.confidence;
    if (config_.use_hmm) {
      observations.push_back(chord_observation(smoothed.data()));
    }
  }

  if (config_.use_hmm) {
    auto smoothed_sequence = viterbi_chord_sequence(observations, templates_, hmm_config());
    if (smoothed_sequence.size() == frame_chords_.size()) {
      frame_chords_ = std::move(smoothed_sequence);
      for (int f = 0; f < n_frames; ++f) {
        std::array<float, 12> frame_chroma = {};
        for (int c = 0; c < n_chroma; ++c) {
          frame_chroma[c] = chroma_.at(c, f);
        }
        confidences[f] = std::min(
            1.0f, std::max(0.0f, templates_[frame_chords_[f]].correlate(frame_chroma.data())));
      }
    }
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
      chord.bass = chord.root;
      if (config_.detect_inversions) {
        const PitchClass estimated_bass =
            estimate_bass_pitch_class(segment_start, f, templates_[current_chord]);
        if (chord_contains_pitch_class(templates_[current_chord], estimated_bass)) {
          chord.bass = estimated_bass;
        }
      }

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

void ChordAnalyzer::analyze_chords_beat_sync(const std::vector<float>& beat_times) {
  if (chroma_.empty() || beat_times.empty()) {
    return;
  }

  // The per-beat chroma buffer is fixed at 12 pitch classes; clamp iteration so
  // a chromagram with more than 12 bins cannot overrun it.
  int n_chroma = std::min(chroma_.n_chroma(), 12);
  float hop_duration = static_cast<float>(chroma_.hop_length()) / chroma_.sample_rate();

  // Detect chord at each beat position
  std::vector<int> beat_chords;
  std::vector<float> beat_confidences;
  std::vector<ChordHmmObservation> observations;
  // Retain each beat's smoothed chroma so confidences can be recomputed against
  // the Viterbi-selected templates (which may differ from the per-beat argmax).
  std::vector<std::array<float, 12>> beat_chromas;
  beat_chords.reserve(beat_times.size());
  beat_confidences.reserve(beat_times.size());
  beat_chromas.reserve(beat_times.size());
  if (config_.use_hmm) {
    observations.reserve(beat_times.size());
  }

  for (float beat_time : beat_times) {
    // Find the frame closest to this beat
    int frame = static_cast<int>(beat_time / hop_duration);
    frame = std::max(0, std::min(frame, chroma_.n_frames() - 1));

    // Average chroma over a small window around the beat (typically 1-2 frames)
    int window = 2;  // frames before and after
    int start_frame = std::max(0, frame - window);
    int end_frame = std::min(chroma_.n_frames(), frame + window + 1);

    std::array<float, 12> beat_chroma = {};
    int count = end_frame - start_frame;

    for (int f = start_frame; f < end_frame; ++f) {
      for (int c = 0; c < n_chroma; ++c) {
        beat_chroma[c] += chroma_.at(c, f);
      }
    }

    for (int c = 0; c < n_chroma; ++c) {
      beat_chroma[c] /= static_cast<float>(count);
    }

    // Find best chord using shared logic
    ChordMatch match = find_best_chord_with_confidence(beat_chroma.data());
    beat_chords.push_back(match.index);
    beat_confidences.push_back(match.confidence);
    beat_chromas.push_back(beat_chroma);
    if (config_.use_hmm) {
      observations.push_back(chord_observation(beat_chroma.data()));
    }
  }

  // Convert beat-level chords to time segments
  if (beat_chords.empty()) return;

  if (config_.use_hmm) {
    auto smoothed_sequence = viterbi_chord_sequence(observations, templates_, hmm_config());
    if (smoothed_sequence.size() == beat_chords.size()) {
      beat_chords = std::move(smoothed_sequence);
      // Viterbi may replace a beat's chord with one that the per-beat argmax did
      // not select; recompute each confidence against the chosen template so the
      // stored score reflects the final chord rather than the pre-Viterbi one.
      for (size_t i = 0; i < beat_chords.size(); ++i) {
        beat_confidences[i] = std::min(
            1.0f, std::max(0.0f, templates_[beat_chords[i]].correlate(beat_chromas[i].data())));
      }
    }
  }

  int current_chord = beat_chords[0];
  float segment_start = beat_times[0];
  float segment_confidence = beat_confidences[0];
  int confidence_count = 1;

  for (size_t i = 1; i <= beat_chords.size(); ++i) {
    bool is_last = (i == beat_chords.size());
    bool chord_changed = !is_last && (beat_chords[i] != current_chord);

    if (chord_changed || is_last) {
      // End current segment
      float segment_end = is_last ? (beat_times.back() + 0.5f) : beat_times[i];

      Chord chord;
      chord.root = templates_[current_chord].root;
      chord.quality = templates_[current_chord].quality;
      chord.start = segment_start;
      chord.end = segment_end;
      chord.confidence = segment_confidence / static_cast<float>(confidence_count);
      const int start_frame = std::max(0, static_cast<int>(segment_start / hop_duration));
      const int end_frame =
          std::min(chroma_.n_frames(), static_cast<int>(segment_end / hop_duration) + 1);
      chord.bass = chord.root;
      if (config_.detect_inversions) {
        const PitchClass estimated_bass =
            estimate_bass_pitch_class(start_frame, end_frame, templates_[current_chord]);
        if (chord_contains_pitch_class(templates_[current_chord], estimated_bass)) {
          chord.bass = estimated_bass;
        }
      }

      chords_.push_back(chord);

      if (!is_last) {
        current_chord = beat_chords[i];
        segment_start = beat_times[i];
        segment_confidence = beat_confidences[i];
        confidence_count = 1;
      }
    } else if (!is_last) {
      segment_confidence += beat_confidences[i];
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
  float pending_start = -1.0f;

  for (size_t i = 0; i < chords_.size(); ++i) {
    const Chord& chord = chords_[i];

    if (chord.duration() < config_.min_duration && !merged.empty()) {
      // Merge with previous chord
      merged.back().end = chord.end;
    } else if (chord.duration() < config_.min_duration && i + 1 < chords_.size()) {
      // Very short first chord: merge its time range into the next retained segment.
      if (pending_start < 0.0f) {
        pending_start = chord.start;
      }
      continue;
    } else if (!merged.empty() && merged.back().root == chord.root &&
               merged.back().quality == chord.quality && merged.back().bass == chord.bass) {
      // Same chord as previous: extend
      merged.back().end = chord.end;
      merged.back().confidence = (merged.back().confidence + chord.confidence) / 2.0f;
    } else {
      Chord retained = chord;
      if (pending_start >= 0.0f) {
        retained.start = pending_start;
        pending_start = -1.0f;
      }
      merged.push_back(retained);
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
                                                  Mode mode) const {
  // Calculate interval from key root
  int interval = (static_cast<int>(chord.root) - static_cast<int>(key_root) + 12) % 12;

  // Scale degree names
  // Major scale intervals: 0, 2, 4, 5, 7, 9, 11 (I, II, III, IV, V, VI, VII)
  // Natural minor intervals: 0, 2, 3, 5, 7, 8, 10 (i, ii, bIII, iv, v, bVI, bVII)
  static const std::vector<std::pair<int, std::string>> major_degrees = {
      {0, "I"}, {2, "II"}, {4, "III"}, {5, "IV"}, {7, "V"}, {9, "VI"}, {11, "VII"}};

  static const std::vector<std::pair<int, std::string>> minor_degrees = {
      {0, "I"}, {2, "II"}, {3, "III"}, {5, "IV"}, {7, "V"}, {8, "VI"}, {10, "VII"}};

  // Select scale based on mode
  const auto& scale_degrees = (mode == Mode::Minor) ? minor_degrees : major_degrees;

  // Find closest scale degree
  std::string numeral;
  bool is_chromatic = true;

  for (const auto& deg : scale_degrees) {
    if (deg.first == interval) {
      numeral = deg.second;
      is_chromatic = false;
      break;
    }
  }

  // Handle chromatic chords (flat/sharp relative to the current scale)
  if (is_chromatic) {
    // For minor mode, check if the chord is on a major scale degree (raised)
    // For major mode, check if the chord is on a minor scale degree (lowered)
    const auto& other_degrees = (mode == Mode::Minor) ? major_degrees : minor_degrees;

    for (const auto& deg : scale_degrees) {
      if ((deg.first + 1) % 12 == interval) {
        numeral = "#" + deg.second;
        break;
      }
      if ((deg.first - 1 + 12) % 12 == interval) {
        numeral = "b" + deg.second;
        break;
      }
    }

    // If still not found, try the other scale for borrowed chords
    if (numeral.empty()) {
      for (const auto& deg : other_degrees) {
        if (deg.first == interval) {
          // Borrowed chord from parallel mode.
          // In minor key, borrowed major-scale degrees are raised → use as-is.
          // In major key, borrowed minor-scale degrees are lowered → prefix with "b".
          numeral = (mode == Mode::Minor) ? deg.second : "b" + deg.second;
          is_chromatic = false;
          break;
        }
      }
    }
  }

  if (numeral.empty()) {
    numeral = "?";
  }

  // Adjust case based on chord quality
  bool is_minor_chord =
      (chord.quality == ChordQuality::Minor || chord.quality == ChordQuality::Minor7 ||
       chord.quality == ChordQuality::Diminished || chord.quality == ChordQuality::MinorAdd9 ||
       chord.quality == ChordQuality::Dim7 || chord.quality == ChordQuality::HalfDim7);

  if (is_minor_chord) {
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
    case ChordQuality::Dim7:
      numeral += "°7";
      break;
    case ChordQuality::HalfDim7:
      numeral += "ø7";
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
    case ChordQuality::Add9:
    case ChordQuality::MinorAdd9:
      numeral += "add9";
      break;
    case ChordQuality::Major9:
      numeral += "maj9";
      break;
    case ChordQuality::Dominant9:
      numeral += "9";
      break;
    case ChordQuality::Sus2Add4:
      numeral += "sus2add4";
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
  empty.bass = empty.root;
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
    empty.bass = empty.root;
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
