#include "editing/note_model/note_transcriber.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include "editing/note_model/note_extractor.h"
#include "editing/pitch_editor/f0_provider.h"
#include "editing/pitch_editor/note_segmenter.h"
#include "editing/polyphony/f0_salience.h"
#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/polyphonic_edit.h"
#include "feature/pitch.h"
#include "util/constants.h"
#include "util/exception.h"

namespace sonare::editing::note_model {

using constants::kSemitonesPerOctave;
using constants::kSilenceDb;

TranscribeF0Range resolve_transcribe_f0_range(const TranscribeConfig& config) noexcept {
  TranscribeF0Range defaults;
  if (config.source == TranscribeSource::kPolyphonic) {
    const polyphony::SalienceConfig source;
    defaults = {source.f0_min_hz, source.f0_max_hz};
  } else {
    const PitchConfig source;
    defaults = {source.fmin, source.fmax};
  }
  return {config.fmin == 0.0f ? defaults.fmin : config.fmin,
          config.fmax == 0.0f ? defaults.fmax : config.fmax};
}

namespace {

// Transcription defaults, apart from the editing chain's, measured on rendered
// piano scores ([transcribe_eval_sweep]): chords without octave doublings scored
// onset F 0.32 / 0.36 / 0.42 at 6 / 8 / 10 voices.
constexpr int kTranscribeMaxPolyphony = 10;
// A recorded piano solo breaks real notes into 60-99 ms ridges that 100 ms dropped by
// ear; 60 ms keeps them. On rendered scores 60-100 ms scored alike, and broadband
// noise at -20 dBFS yields about 25 short notes a second at 60 ms (5.8 at 100).
constexpr float kTranscribePolyphonicMinNoteMs = 60.0f;
// A whole note is four beats of 60000 ms / BPM.
constexpr float kWholeNoteMsAtOneBpm = 4.0f * 60000.0f;
// Unchanged from the editing chain: the sweep never moved it.
constexpr float kTranscribeMinFramePeakRatio = 0.20f;
// Unchanged from the editing chain: the sweep never moved it.
constexpr float kTranscribeMinRidgePeakRatio = 0.10f;
// Re-strikes scored F 1.0 at 1.5 and 2.0 and 0.86 at 2.5, none splitting a held
// note; 2.0 had the higher mean piano F (0.717 against 0.678).
constexpr float kTranscribeReattackRatio = 2.0f;
// A held piano note loses its candidate for one or two frames at a time (a
// beat minimum, or a frame's search ending first); two frames at the 11.6 ms hop.
constexpr float kTranscribeMaxGapMs = 25.0f;
// A note this far under the take's reference level is a tail or a ghost. On renders
// peaking near 0 dBFS, 36 dB scored the same F as a -48 dBFS absolute floor
// (0.564 / 0.695 against 0.565 / 0.696) and keeps a note 34 dB under its neighbours.
constexpr float kNoteGateBelowReferenceDb = 36.0f;
// The reference sets aside one loudest note per four, up to three.
constexpr size_t kNotesPerSetAside = 4;
constexpr size_t kMaxSetAsideNotes = 3;
// A 1 ms grid resolves the brief RMS dips between repeated attacks while a
// two-period window follows each parent's measured pitch.
constexpr double kReattackGridMs = 1.0;
constexpr double kReattackContextMs = 20.0;
constexpr double kReattackValleyRatio = 0.5;
constexpr double kReattackLowRmsRatio = 0.7;
constexpr double kReattackMaximumValleyWidthMs = 15.0;
constexpr double kReattackCrestFloor = 1.0e-6;

/// Length of a 1/@p division note at @p tempo_bpm, in milliseconds.
float note_value_ms(int division, float tempo_bpm) noexcept {
  return kWholeNoteMsAtOneBpm / (tempo_bpm * static_cast<float>(division));
}

float default_min_note_ms(const TranscribeConfig& config) noexcept {
  if (config.source != TranscribeSource::kPolyphonic) {
    return pitch_editor::NoteSegmenterConfig{}.min_note_ms;
  }
  // A duration, not a note value: the tempo may be only the caller's grid.
  return kTranscribePolyphonicMinNoteMs;
}

/// A sentinel-coded ratio: 0 => @p fallback, negative => a real 0.
float resolve_ratio(float value, float fallback) noexcept {
  if (value == 0.0f) return fallback;
  return value < 0.0f ? 0.0f : value;
}

}  // namespace

TranscribePolyphonyLimits resolve_transcribe_polyphony_limits(
    const TranscribeConfig& config) noexcept {
  TranscribePolyphonyLimits limits;
  if (config.min_note_ms != 0.0f) {
    limits.min_note_ms = config.min_note_ms;
  } else if (config.min_note_division != 0) {
    limits.min_note_ms = note_value_ms(config.min_note_division, config.tempo_bpm);
  } else {
    limits.min_note_ms = default_min_note_ms(config);
  }
  limits.max_polyphony = config.max_polyphony == 0 ? kTranscribeMaxPolyphony : config.max_polyphony;
  limits.min_frame_peak_ratio =
      resolve_ratio(config.min_frame_peak_ratio, kTranscribeMinFramePeakRatio);
  limits.min_ridge_peak_ratio =
      resolve_ratio(config.min_ridge_peak_ratio, kTranscribeMinRidgePeakRatio);
  limits.reattack_ratio = resolve_ratio(config.reattack_ratio, kTranscribeReattackRatio);
  return limits;
}

namespace {

constexpr int kMaxMidiNote = 127;
constexpr int kMaxMinNoteDivision = 128;
constexpr uint8_t kMinVelocity = 1;
constexpr uint8_t kMaxVelocity = 127;

void require(bool condition, const char* message) {
  if (!condition) throw SonareException(ErrorCode::InvalidParameter, message);
}

TranscribeConfig validate_and_resolve(const Audio& audio, const TranscribeConfig& config) {
  // No sample-rate check: Audio::from_buffer / from_vector refuse a non-positive
  // rate, and the only Audio that carries one is the default-constructed one,
  // which the emptiness check below already refuses. A guard here could never
  // fire.
  require(!audio.empty(), "transcribe_notes: audio must not be empty");
  require(std::isfinite(config.reference_hz) && config.reference_hz > 0.0f,
          "transcribe_notes: reference_hz must be finite and positive");
  require(std::isfinite(config.min_note_ms) && config.min_note_ms >= 0.0f,
          "transcribe_notes: min_note_ms must be finite and non-negative");
  require(std::isfinite(config.tempo_bpm) && config.tempo_bpm >= 0.0f,
          "transcribe_notes: tempo_bpm must be finite and non-negative");
  require(config.min_note_division >= 0 && config.min_note_division <= kMaxMinNoteDivision,
          "transcribe_notes: min_note_division must be 0 or within [1, 128]");
  require(config.min_note_division == 0 || config.min_note_ms == 0.0f,
          "transcribe_notes: min_note_division and min_note_ms cannot both be set");
  require(config.min_note_division == 0 || config.tempo_bpm > 0.0f,
          "transcribe_notes: min_note_division needs a positive tempo_bpm");
  require(std::isfinite(config.segmentation_threshold_cents) &&
              config.segmentation_threshold_cents > 0.0f,
          "transcribe_notes: segmentation_threshold_cents must be finite and positive");
  require(std::isfinite(config.velocity_floor_db) && config.velocity_floor_db < 0.0f,
          "transcribe_notes: velocity_floor_db must be finite and negative");
  require(config.fixed_velocity == 0 ||
              (config.fixed_velocity >= kMinVelocity && config.fixed_velocity <= kMaxVelocity),
          "transcribe_notes: fixed_velocity must be 0 or within [1, 127]");
  require(config.source == TranscribeSource::kMonophonic ||
              config.source == TranscribeSource::kPolyphonic,
          "transcribe_notes: source is not a known TranscribeSource");
  require(config.max_polyphony == 0 ||
              (config.max_polyphony >= 1 && config.max_polyphony <= polyphony::kMaxPolyphonyVoices),
          "transcribe_notes: max_polyphony must be 0 or within [1, 64]");
  require(std::isfinite(config.min_frame_peak_ratio) && config.min_frame_peak_ratio <= 1.0f,
          "transcribe_notes: min_frame_peak_ratio must be finite and at most 1");
  require(std::isfinite(config.min_ridge_peak_ratio) && config.min_ridge_peak_ratio <= 1.0f,
          "transcribe_notes: min_ridge_peak_ratio must be finite and at most 1");
  require(std::isfinite(config.reattack_ratio) &&
              (config.reattack_ratio <= 0.0f || config.reattack_ratio > 1.0f),
          "transcribe_notes: reattack_ratio must be 0, negative, or finite and above 1");
  if (config.source == TranscribeSource::kMonophonic) {
    require(config.max_polyphony == 0,
            "transcribe_notes: max_polyphony applies to polyphonic transcription only");
    require(config.min_frame_peak_ratio == 0.0f,
            "transcribe_notes: min_frame_peak_ratio applies to polyphonic transcription only");
    require(config.min_ridge_peak_ratio == 0.0f,
            "transcribe_notes: min_ridge_peak_ratio applies to polyphonic transcription only");
    require(config.reattack_ratio == 0.0f,
            "transcribe_notes: reattack_ratio applies to polyphonic transcription only");
  }

  // Resolve omitted endpoints before checking the ordering; the two chains have different ranges.
  TranscribeConfig resolved = config;
  const TranscribeF0Range range = resolve_transcribe_f0_range(config);
  resolved.fmin = range.fmin;
  resolved.fmax = range.fmax;
  resolved.min_note_ms = resolve_transcribe_polyphony_limits(config).min_note_ms;
  require(std::isfinite(resolved.fmin) && resolved.fmin > 0.0f,
          "transcribe_notes: fmin must be finite and positive");
  require(std::isfinite(resolved.fmax) && resolved.fmax > resolved.fmin,
          "transcribe_notes: fmax must be finite and greater than fmin");
  return resolved;
}

/// Peak of a note's per-frame RMS curve. 0 for a note carrying no curve, which
/// reads as the floor rather than as an error: the curve is a measurement the
/// note may not have.
float peak_rms(const NoteObject& note) noexcept {
  float peak = 0.0f;
  for (const float value : note.amplitude.values) {
    if (std::isfinite(value) && value > peak) peak = value;
  }
  return peak;
}

int64_t sample_at_frame(int frame, double samples_per_frame, int64_t sample_count) noexcept {
  if (frame <= 0 || !(samples_per_frame > 0.0)) return 0;
  const double sample = static_cast<double>(frame) * samples_per_frame;
  if (sample >= static_cast<double>(sample_count)) return sample_count;
  return static_cast<int64_t>(sample);
}

/// Finds RMS dips within a note. The squared-sample prefix is rebuilt from the
/// current note's audio region so scratch memory stays bounded by the longest
/// parent note, even on long recordings.
std::vector<int> reattack_frames(const Audio& audio, const NoteObject& parent,
                                 const pitch_editor::F0Track& track,
                                 const NoteExtractorConfig& extractor, int frame_length,
                                 int64_t minimum_child_samples, std::vector<double>& squared_prefix,
                                 std::vector<int64_t>& center_offsets,
                                 std::vector<double>& rms_values) {
  std::vector<int> boundaries;
  const int sample_rate = audio.sample_rate();
  if (!std::isfinite(parent.median_hz) || !(parent.median_hz > 0.0f) || sample_rate <= 0 ||
      frame_length <= 0 || parent.onset_sample < 0 || parent.offset_sample <= parent.onset_sample ||
      parent.offset_sample > static_cast<int64_t>(audio.size()) || parent.frame_start < 0 ||
      parent.frame_end <= parent.frame_start || parent.frame_end > track.n_frames()) {
    return boundaries;
  }

  const int64_t region_samples = parent.offset_sample - parent.onset_sample;
  const double samples_per_frame = track.samples_per_frame();
  if (!std::isfinite(samples_per_frame) || !(samples_per_frame > 0.0)) return boundaries;

  const double minimum_child_ms = static_cast<double>(extractor.segmenter.min_note_ms);
  const double edge_guard_ms =
      std::max(minimum_child_ms, 500.0 * static_cast<double>(frame_length) / sample_rate);
  const int64_t edge_guard_samples =
      static_cast<int64_t>(std::ceil(edge_guard_ms * sample_rate / 1000.0));

  const double requested_window = 2.0 * sample_rate / parent.median_hz;
  if (!std::isfinite(requested_window) || requested_window > region_samples) return boundaries;
  const int64_t window_samples = std::max<int64_t>(2, std::llround(requested_window));
  if (window_samples > region_samples) return boundaries;
  const int64_t left_radius = window_samples / 2;
  const int64_t right_radius = window_samples - left_radius;
  const int64_t grid_step =
      std::max<int64_t>(1, std::llround(sample_rate * kReattackGridMs / 1000.0));
  const int64_t context_samples =
      std::max<int64_t>(1, std::llround(sample_rate * kReattackContextMs / 1000.0));
  const size_t context_points = static_cast<size_t>(
      std::max<int64_t>(1, std::llround(static_cast<double>(context_samples) / grid_step)));
  const int64_t maximum_valley_width =
      std::llround(sample_rate * kReattackMaximumValleyWidthMs / 1000.0);

  const size_t region_size = static_cast<size_t>(region_samples);
  squared_prefix.resize(region_size + 1);
  squared_prefix[0] = 0.0;
  for (size_t i = 0; i < region_size; ++i) {
    const double sample = audio[static_cast<size_t>(parent.onset_sample) + i];
    if (!std::isfinite(sample)) return boundaries;
    squared_prefix[i + 1] = squared_prefix[i] + sample * sample;
  }

  center_offsets.clear();
  rms_values.clear();
  if (region_samples < left_radius + right_radius) return boundaries;
  const int64_t last_center = region_samples - right_radius;
  for (int64_t center = left_radius; center <= last_center; center += grid_step) {
    const int64_t begin = center - left_radius;
    const int64_t end = begin + window_samples;
    const double energy =
        squared_prefix[static_cast<size_t>(end)] - squared_prefix[static_cast<size_t>(begin)];
    center_offsets.push_back(center);
    rms_values.push_back(std::sqrt(std::max(0.0, energy) / window_samples));
    if (last_center - center < grid_step) break;
  }
  if (rms_values.size() < 2 * context_points + 1) return boundaries;

  const int64_t sample_count = static_cast<int64_t>(audio.size());
  int last_boundary_frame = parent.frame_start;
  for (size_t i = context_points; i + context_points < rms_values.size(); ++i) {
    if (rms_values[i] > rms_values[i - 1] || rms_values[i] >= rms_values[i + 1]) continue;

    const auto left_max =
        std::max_element(rms_values.begin() + static_cast<std::ptrdiff_t>(i - context_points),
                         rms_values.begin() + static_cast<std::ptrdiff_t>(i));
    const auto right_max =
        std::max_element(rms_values.begin() + static_cast<std::ptrdiff_t>(i + 1),
                         rms_values.begin() + static_cast<std::ptrdiff_t>(i + context_points + 1));
    const double crest = std::min(*left_max, *right_max);
    if (!std::isfinite(crest) || crest < kReattackCrestFloor ||
        !(rms_values[i] < kReattackValleyRatio * crest)) {
      continue;
    }

    const double left_low_rms_limit = kReattackLowRmsRatio * *left_max;
    const double right_low_rms_limit = kReattackLowRmsRatio * *right_max;
    size_t low_begin = i;
    while (low_begin > i - context_points && rms_values[low_begin] < left_low_rms_limit)
      --low_begin;
    size_t low_end = i;
    while (low_end < i + context_points && rms_values[low_end] < right_low_rms_limit) ++low_end;
    if (center_offsets[low_end] - center_offsets[low_begin] > maximum_valley_width) continue;

    const int64_t candidate_sample = parent.onset_sample + center_offsets[i];
    const int64_t low_begin_sample = parent.onset_sample + center_offsets[low_begin];
    const int64_t low_end_sample = parent.onset_sample + center_offsets[low_end];
    if (candidate_sample - parent.onset_sample < edge_guard_samples ||
        parent.offset_sample - candidate_sample < edge_guard_samples ||
        low_begin_sample - parent.onset_sample < edge_guard_samples ||
        parent.offset_sample - low_end_sample < edge_guard_samples) {
      continue;
    }

    const double frame_position = static_cast<double>(candidate_sample) / samples_per_frame;
    if (!std::isfinite(frame_position) || frame_position < 0.0 ||
        frame_position > static_cast<double>(track.n_frames())) {
      continue;
    }
    const int frame = static_cast<int>(std::llround(frame_position));
    if (frame <= parent.frame_start || frame >= parent.frame_end || frame <= last_boundary_frame) {
      continue;
    }
    const int64_t boundary_sample = sample_at_frame(frame, samples_per_frame, sample_count);
    const int64_t preceding_sample =
        sample_at_frame(last_boundary_frame, samples_per_frame, sample_count);
    if (boundary_sample - preceding_sample < minimum_child_samples ||
        parent.offset_sample - boundary_sample < minimum_child_samples) {
      continue;
    }
    boundaries.push_back(frame);
    last_boundary_frame = frame;
  }
  return boundaries;
}

pitch_editor::F0Track track_window(const pitch_editor::F0Track& track, int frame_start,
                                   int frame_end) {
  pitch_editor::F0Track window;
  const auto begin = static_cast<size_t>(frame_start);
  const auto end = static_cast<size_t>(frame_end);
  window.f0_hz.assign(track.f0_hz.begin() + static_cast<std::ptrdiff_t>(begin),
                      track.f0_hz.begin() + static_cast<std::ptrdiff_t>(end));
  if (!track.voiced_prob.empty()) {
    window.voiced_prob.assign(track.voiced_prob.begin() + static_cast<std::ptrdiff_t>(begin),
                              track.voiced_prob.begin() + static_cast<std::ptrdiff_t>(end));
  }
  if (!track.voiced.empty()) {
    window.voiced.assign(track.voiced.begin() + static_cast<std::ptrdiff_t>(begin),
                         track.voiced.begin() + static_cast<std::ptrdiff_t>(end));
  }
  window.hop_length = track.hop_length;
  window.sample_rate = track.sample_rate;
  window.frame_rate_hz = track.frame_rate_hz;
  return window;
}

std::vector<NoteObject> split_monophonic_reattacks(const Audio& audio,
                                                   const pitch_editor::F0Track& track,
                                                   const NoteExtractorConfig& extractor,
                                                   int frame_length,
                                                   std::vector<NoteObject> notes) {
  if (notes.empty()) return notes;

  std::vector<NoteObject> result;
  result.reserve(notes.size());
  std::vector<double> squared_prefix;
  std::vector<int64_t> center_offsets;
  std::vector<double> rms_values;
  const int64_t sample_count = static_cast<int64_t>(audio.size());
  const int total_frames = track.n_frames();
  const double minimum_child_samples_exact =
      static_cast<double>(extractor.segmenter.min_note_ms) * audio.sample_rate() / 1000.0;
  if (!std::isfinite(minimum_child_samples_exact) ||
      minimum_child_samples_exact > static_cast<double>(sample_count)) {
    return notes;
  }
  const int64_t minimum_child_samples =
      static_cast<int64_t>(std::ceil(minimum_child_samples_exact));
  bool has_terminal_split = false;

  for (NoteObject& parent : notes) {
    const std::vector<int> cuts =
        reattack_frames(audio, parent, track, extractor, frame_length, minimum_child_samples,
                        squared_prefix, center_offsets, rms_values);
    if (cuts.empty()) {
      result.push_back(std::move(parent));
      continue;
    }

    std::vector<int> edges;
    edges.reserve(cuts.size() + 2);
    edges.push_back(parent.frame_start);
    edges.insert(edges.end(), cuts.begin(), cuts.end());
    edges.push_back(parent.frame_end);

    std::vector<NoteObject> children;
    children.reserve(edges.size() - 1);
    bool children_meet_minimum = true;
    for (size_t i = 1; i < edges.size(); ++i) {
      const int frame_start = edges[i - 1];
      const int frame_end = edges[i];
      const pitch_editor::F0Track window = track_window(track, frame_start, frame_end);
      NoteObject child = make_note(audio, 0, sample_count, window, frame_start, total_frames,
                                   frame_start, frame_end, extractor);
      if (child.length_samples() < minimum_child_samples) {
        children_meet_minimum = false;
        break;
      }
      children.push_back(std::move(child));
    }
    if (!children_meet_minimum || children.size() != edges.size() - 1) {
      result.push_back(std::move(parent));
      continue;
    }
    has_terminal_split = has_terminal_split || children.back().frame_end == total_frames;
    for (NoteObject& child : children) result.push_back(std::move(child));
  }

  if (has_terminal_split) {
    bool needs_repair = false;
    for (size_t i = 1; i < result.size(); ++i) {
      if (result[i].frame_end == total_frames &&
          result[i - 1].offset_sample > result[i].onset_sample) {
        needs_repair = true;
        break;
      }
    }
    if (needs_repair) repair_rederived_note_set_bounds(audio, track, result, extractor);
  }
  return result;
}

std::vector<NoteObject> monophonic_notes(const Audio& audio, const TranscribeConfig& config) {
  PitchConfig pitch_config = pitch_editor::default_editing_pitch_config(audio.sample_rate());
  pitch_config.fmin = config.fmin;
  pitch_config.fmax = config.fmax;
  pitch_config.center = true;
  pitch_editor::PyinF0Provider provider(pitch_config);
  const pitch_editor::F0Track track = provider.detect(audio);
  if (track.n_frames() == 0) return {};

  NoteExtractorConfig extractor;
  extractor.segmenter.segmentation_threshold_cents = config.segmentation_threshold_cents;
  extractor.segmenter.min_note_ms = config.min_note_ms;
  extractor.segmenter.reference_hz = config.reference_hz;
  std::vector<NoteObject> notes = extract_notes(audio, track, extractor);
  return split_monophonic_reattacks(audio, track, extractor, pitch_config.frame_length,
                                    std::move(notes));
}

std::vector<NoteObject> polyphonic_notes(const Audio& audio, const TranscribeConfig& config) {
  polyphony::PolyphonicEditConfig poly;
  // The range belongs to the estimator's F0 axis. The cent spectrum keeps its
  // own frequency ceiling, because it also has to hold the harmonics it sums;
  // its floor follows fmin, or a fundamental under 55 Hz would never be read.
  poly.extraction.estimation.salience.f0_min_hz = config.fmin;
  poly.extraction.estimation.salience.f0_max_hz = config.fmax;
  poly.extraction.spectrum.ref_hz = std::min(poly.extraction.spectrum.ref_hz, config.fmin);
  // One note per ridge, so the shortest note is the shortest ridge kept;
  // segmenter.min_note_ms is never consulted on this path (see masked_notes.h).
  const TranscribePolyphonyLimits limits = resolve_transcribe_polyphony_limits(config);
  poly.extraction.ridges.min_duration_ms = limits.min_note_ms;
  poly.extraction.ridges.min_ridge_peak_ratio = limits.min_ridge_peak_ratio;
  poly.extraction.ridges.reattack_ratio = limits.reattack_ratio;
  poly.extraction.ridges.max_gap_ms = kTranscribeMaxGapMs;
  // A note begins where its partials' magnitude crossed half of their attack;
  // the framing is stated at the reference rate.
  poly.extraction.ridges.attack_window_ms = 1000.0f *
                                            static_cast<float>(poly.extraction.stft.n_fft) /
                                            static_cast<float>(polyphony::kPolyphonyReferenceRate);
  poly.extraction.estimation.max_polyphony = limits.max_polyphony;
  poly.extraction.estimation.min_frame_peak_ratio = limits.min_frame_peak_ratio;
  poly.notes.segmenter.segmentation_threshold_cents = config.segmentation_threshold_cents;
  poly.notes.segmenter.reference_hz = config.reference_hz;
  // Transcription reads only the notes, so the analysis is never held.
  return polyphony::transcribe_polyphonic(audio, poly);
}

}  // namespace

int midi_note_for_hz(float hz, float reference_hz) noexcept {
  if (!std::isfinite(hz) || hz <= 0.0f) return -1;
  if (!std::isfinite(reference_hz) || reference_hz <= 0.0f) return -1;
  const float semitones = kSemitonesPerOctave * std::log2(hz / reference_hz);
  if (!std::isfinite(semitones)) return -1;
  const float note = std::round(semitones) + constants::kMidiA4;
  if (note < 0.0f || note > static_cast<float>(kMaxMidiNote)) return -1;
  return static_cast<int>(note);
}

uint8_t velocity_for_peak_rms(float peak_rms_linear, float velocity_floor_db) noexcept {
  if (!std::isfinite(velocity_floor_db) || velocity_floor_db >= 0.0f) return kMinVelocity;
  if (!std::isfinite(peak_rms_linear) || peak_rms_linear <= 0.0f) return kMinVelocity;
  const float level_db = 20.0f * std::log10(peak_rms_linear);
  if (!std::isfinite(level_db)) return kMinVelocity;
  const float fraction = (level_db - velocity_floor_db) / -velocity_floor_db;
  const float velocity =
      static_cast<float>(kMinVelocity) + fraction * static_cast<float>(kMaxVelocity - kMinVelocity);
  if (velocity <= static_cast<float>(kMinVelocity)) return kMinVelocity;
  if (velocity >= static_cast<float>(kMaxVelocity)) return kMaxVelocity;
  return static_cast<uint8_t>(std::lround(velocity));
}

namespace {

float level_db(float peak_rms_linear) noexcept {
  if (!std::isfinite(peak_rms_linear) || peak_rms_linear <= 0.0f) return constants::kFloorDb;
  return std::max(constants::kFloorDb, 20.0f * std::log10(peak_rms_linear));
}

/// Level under which a note is dropped: the reference level less the gate, where
/// the reference is the peak of the loudest note once a few loudest are set
/// aside, so one dominant strike does not set the bar; never under silence.
float note_gate_db(std::vector<float> levels_db) noexcept {
  if (levels_db.empty()) return kSilenceDb;
  const size_t set_aside = std::min(kMaxSetAsideNotes, levels_db.size() / kNotesPerSetAside);
  std::nth_element(levels_db.begin(), levels_db.begin() + static_cast<std::ptrdiff_t>(set_aside),
                   levels_db.end(), std::greater<>());
  return std::max(kSilenceDb, levels_db[set_aside] - kNoteGateBelowReferenceDb);
}

}  // namespace

std::vector<TranscribedNote> transcribe_notes(const Audio& audio, const TranscribeConfig& config) {
  // No pitch-editor guard here: this unit is compiled into the pitch-editor
  // archive, so its existence is the gate. The NOT_SUPPORTED answer belongs to
  // the C-ABI entry, which is compiled either way.
  const TranscribeConfig resolved = validate_and_resolve(audio, config);

  const std::vector<NoteObject> measured = resolved.source == TranscribeSource::kPolyphonic
                                               ? polyphonic_notes(audio, resolved)
                                               : monophonic_notes(audio, resolved);

  struct Candidate {
    const NoteObject* note;
    int midi_note;
    float peak;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(measured.size());
  for (const NoteObject& note : measured) {
    if (note.offset_sample <= note.onset_sample) continue;
    const int midi_note = midi_note_for_hz(note.median_hz, resolved.reference_hz);
    if (midi_note < 0) continue;
    candidates.push_back({&note, midi_note, peak_rms(note)});
  }

  // The gate is relative to the take, so a quiet recording keeps its notes while a
  // decay tail or a ghost ridge far under the take's own notes does not arrive.
  std::vector<float> levels_db;
  levels_db.reserve(candidates.size());
  for (const Candidate& candidate : candidates) levels_db.push_back(level_db(candidate.peak));
  const float gate_db = note_gate_db(levels_db);

  std::vector<TranscribedNote> notes;
  notes.reserve(candidates.size());
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (levels_db[i] < gate_db) continue;
    const Candidate& candidate = candidates[i];
    const uint8_t velocity =
        resolved.fixed_velocity != 0
            ? static_cast<uint8_t>(resolved.fixed_velocity)
            : velocity_for_peak_rms(candidate.peak, resolved.velocity_floor_db);
    notes.push_back(TranscribedNote{candidate.note->onset_sample, candidate.note->offset_sample,
                                    static_cast<uint8_t>(candidate.midi_note), velocity,
                                    candidate.note->median_hz});
  }

  std::sort(notes.begin(), notes.end(),
            [](const TranscribedNote& a, const TranscribedNote& b) noexcept {
              if (a.onset_sample != b.onset_sample) return a.onset_sample < b.onset_sample;
              return a.note < b.note;
            });
  return notes;
}

}  // namespace sonare::editing::note_model
