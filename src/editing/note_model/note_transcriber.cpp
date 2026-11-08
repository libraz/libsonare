#include "editing/note_model/note_transcriber.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include "core/fft.h"
#include "editing/note_model/note_extractor.h"
#include "editing/note_model/note_state_decoder.h"
#include "editing/pitch_editor/f0_provider.h"
#include "editing/pitch_editor/note_segmenter.h"
#include "editing/polyphony/f0_salience.h"
#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/polyphonic_edit.h"
#include "feature/pitch.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"

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
// Chosen with the note-state decoder's constants by 5-fold cross-validation on 40
// sung takes, among settings that keep the synthetic regressions; inside the grid.
constexpr float kTranscribeMonophonicMinNoteMs = 45.0f;
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
constexpr double kAliasWindowPeriodMargin = 1.04;
constexpr float kAliasCmnfThreshold = 0.1f;
constexpr int kAliasMinimumMultiple = 2;
constexpr int kAliasMaximumMultiple = 4;
constexpr double kAliasMaximumCents = 50.0;
constexpr double kAliasResidualEnergyRatio = 1.0e-5;
// Keep an alias candidate stable for at least the segmenter's default minimum
// note duration before it changes the contour.
constexpr double kAliasMinimumStableMs = pitch_editor::NoteSegmenterConfig{}.min_note_ms;

/// Length of a 1/@p division note at @p tempo_bpm, in milliseconds.
float note_value_ms(int division, float tempo_bpm) noexcept {
  return kWholeNoteMsAtOneBpm / (tempo_bpm * static_cast<float>(division));
}

float default_min_note_ms(const TranscribeConfig& config) noexcept {
  if (config.source != TranscribeSource::kPolyphonic) return kTranscribeMonophonicMinNoteMs;
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

void repair_monophonic_pitch_aliases(const Audio& audio, pitch_editor::F0Track& track,
                                     const PitchConfig& config) {
  const int sample_rate = audio.sample_rate();
  if (audio.empty() || sample_rate <= 0 || track.hop_length <= 0 || track.sample_rate <= 0 ||
      config.fmax <= 0.0f || !std::isfinite(config.fmax) || config.fmin <= 0.0f ||
      !std::isfinite(config.fmin) || track.voiced.size() < track.f0_hz.size() ||
      audio.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    return;
  }

  const float* const samples = audio.data();
  const int64_t audio_size = static_cast<int64_t>(audio.size());
  const size_t frame_count = track.f0_hz.size();
  std::vector<float> proposed(frame_count, 0.0f);
  std::vector<float> difference;
  std::vector<float> cmndf;
  std::unique_ptr<FFT> fft;
  std::vector<float> reference;
  std::vector<float> comparison;
  std::vector<float> correlation;
  std::vector<std::complex<float>> reference_spectrum;
  std::vector<std::complex<float>> comparison_spectrum;
  std::vector<double> squared_prefix;

  for (size_t frame = 0; frame < frame_count; ++frame) {
    const float current_hz = track.f0_hz[frame];
    if (!track.voiced[frame] || !std::isfinite(current_hz) || current_hz <= 0.0f) continue;

    const double current_period = static_cast<double>(sample_rate) / current_hz;
    const double max_lag_exact = std::ceil(current_period * kAliasWindowPeriodMargin);
    if (!std::isfinite(max_lag_exact) || max_lag_exact < 2.0 ||
        max_lag_exact > static_cast<double>(std::numeric_limits<int>::max() / 3)) {
      continue;
    }
    const int max_lag = static_cast<int>(max_lag_exact);
    const int window_length = 2 * max_lag;
    const int sample_span = window_length + max_lag;
    if (frame > static_cast<size_t>(std::numeric_limits<int64_t>::max() / track.hop_length))
      continue;
    const int64_t frame_center = static_cast<int64_t>(frame) * track.hop_length;
    const int64_t window_begin = frame_center - sample_span / 2;
    if (window_begin < 0 || window_begin > audio_size || sample_span > audio_size - window_begin) {
      continue;
    }

    // d(lag) = E(window) + E(window shifted by lag) - 2 r(lag); r via one FFT correlation.
    const int n_fft = next_power_of_2(window_length + sample_span - 1);
    if (!fft || fft->n_fft() != n_fft) {
      fft = std::make_unique<FFT>(n_fft);
      reference.assign(static_cast<size_t>(n_fft), 0.0f);
      comparison.assign(static_cast<size_t>(n_fft), 0.0f);
      correlation.assign(static_cast<size_t>(n_fft), 0.0f);
      reference_spectrum.assign(static_cast<size_t>(fft->n_bins()), {});
      comparison_spectrum.assign(static_cast<size_t>(fft->n_bins()), {});
    } else {
      std::fill(reference.begin(), reference.end(), 0.0f);
      std::fill(comparison.begin(), comparison.end(), 0.0f);
    }
    const float* const span = samples + window_begin;
    for (int offset = 0; offset < window_length; ++offset) {
      reference[static_cast<size_t>(window_length - 1 - offset)] = span[offset];
    }
    std::copy(span, span + sample_span, comparison.begin());
    fft->forward(reference.data(), reference_spectrum.data());
    fft->forward(comparison.data(), comparison_spectrum.data());
    for (size_t bin = 0; bin < reference_spectrum.size(); ++bin) {
      reference_spectrum[bin] *= comparison_spectrum[bin];
    }
    fft->inverse(reference_spectrum.data(), correlation.data());

    squared_prefix.assign(static_cast<size_t>(sample_span) + 1, 0.0);
    for (int offset = 0; offset < sample_span; ++offset) {
      const double value = span[offset];
      squared_prefix[static_cast<size_t>(offset) + 1] =
          squared_prefix[static_cast<size_t>(offset)] + value * value;
    }
    const double window_energy = squared_prefix[static_cast<size_t>(window_length)];
    difference.assign(static_cast<size_t>(max_lag) + 1, 0.0f);
    for (int lag = 1; lag <= max_lag; ++lag) {
      const double shifted_energy = squared_prefix[static_cast<size_t>(lag + window_length)] -
                                    squared_prefix[static_cast<size_t>(lag)];
      const double cross = correlation[static_cast<size_t>(window_length - 1 + lag)];
      const double value = window_energy + shifted_energy - 2.0 * cross;
      difference[static_cast<size_t>(lag)] = value <= 0.0 ? 0.0f : static_cast<float>(value);
    }
    yin_cmndf_into(difference, cmndf);

    const double minimum_period_exact =
        std::floor(static_cast<double>(sample_rate) / static_cast<double>(config.fmax));
    if (!std::isfinite(minimum_period_exact) ||
        minimum_period_exact > static_cast<double>(std::numeric_limits<int>::max())) {
      continue;
    }
    const int minimum_period = std::max(2, static_cast<int>(minimum_period_exact));
    bool has_first_trough = false;
    const float candidate_period =
        yin_find_pitch(cmndf, kAliasCmnfThreshold, minimum_period, max_lag, &has_first_trough);
    if (!has_first_trough || !std::isfinite(candidate_period) || candidate_period <= 0.0f) {
      continue;
    }

    const double candidate_hz = static_cast<double>(sample_rate) / candidate_period;
    if (!std::isfinite(candidate_hz) || candidate_hz < config.fmin || candidate_hz > config.fmax) {
      continue;
    }
    const double candidate_multiple = candidate_hz / current_hz;
    if (!std::isfinite(candidate_multiple) || candidate_multiple < 1.5 ||
        candidate_multiple >= static_cast<double>(kAliasMaximumMultiple) + 0.5) {
      continue;
    }
    const int multiple = static_cast<int>(std::llround(candidate_multiple));
    if (multiple < kAliasMinimumMultiple || multiple > kAliasMaximumMultiple) continue;
    const double cents_error =
        std::abs(constants::kCentsPerOctave * std::log2(candidate_hz / (multiple * current_hz)));
    if (!std::isfinite(cents_error) || cents_error > kAliasMaximumCents) continue;

    const double base_period = static_cast<double>(candidate_period) * multiple;
    auto fractional_difference = [&](double period, double& squared_error) {
      if (!std::isfinite(period) || period < 1.0 || period >= max_lag + 1.0) return false;
      const double integer_part_exact = std::floor(period);
      if (integer_part_exact < 1.0 || integer_part_exact > max_lag) return false;
      const int integer_part = static_cast<int>(integer_part_exact);
      const double fractional_part = period - integer_part;
      squared_error = 0.0;
      for (int offset = 1; offset < window_length - 2; ++offset) {
        const int64_t source_index = window_begin + offset;
        const int64_t delayed_index = source_index + integer_part;
        if (source_index < 1 || delayed_index < 1 || delayed_index > audio_size - 3) {
          return false;
        }
        const size_t source = static_cast<size_t>(source_index);
        const size_t delayed = static_cast<size_t>(delayed_index);
        const double a = samples[delayed - 1];
        const double b = samples[delayed];
        const double c = samples[delayed + 1];
        const double d = samples[delayed + 2];
        const double interpolated =
            b + 0.5 * fractional_part *
                    (c - a +
                     fractional_part * (2.0 * a - 5.0 * b + 4.0 * c - d +
                                        fractional_part * (3.0 * (b - c) + d - a)));
        const double delta = static_cast<double>(samples[source]) - interpolated;
        squared_error += delta * delta;
      }
      return true;
    };

    double window_mean = 0.0;
    for (int offset = 0; offset < window_length; ++offset) {
      window_mean += samples[static_cast<size_t>(window_begin + offset)];
    }
    window_mean /= window_length;
    double centered_energy = 0.0;
    for (int offset = 0; offset < window_length; ++offset) {
      const double centered =
          static_cast<double>(samples[static_cast<size_t>(window_begin + offset)]) - window_mean;
      centered_energy += centered * centered;
    }

    double short_period_error = 0.0;
    double base_period_error = 0.0;
    if (!(centered_energy > 0.0) || !std::isfinite(centered_energy) ||
        !fractional_difference(candidate_period, short_period_error) ||
        !fractional_difference(base_period, base_period_error) ||
        short_period_error > base_period_error + kAliasResidualEnergyRatio * centered_energy) {
      continue;
    }
    proposed[frame] = static_cast<float>(candidate_hz);
  }

  const double frame_duration_ms = static_cast<double>(track.hop_length) * 1000.0 / sample_rate;
  if (!std::isfinite(frame_duration_ms) || frame_duration_ms <= 0.0) return;
  for (size_t start = 0; start < proposed.size();) {
    if (!(proposed[start] > 0.0f)) {
      ++start;
      continue;
    }
    size_t end = start + 1;
    while (end < proposed.size() && proposed[end] > 0.0f) {
      const double stable_cents =
          std::abs(constants::kCentsPerOctave *
                   std::log2(static_cast<double>(proposed[end]) / proposed[start]));
      if (!std::isfinite(stable_cents) || stable_cents > kAliasMaximumCents) break;
      ++end;
    }
    const double stable_duration_ms = static_cast<double>(end - start) * frame_duration_ms;
    if (stable_duration_ms >= kAliasMinimumStableMs) {
      for (size_t frame = start; frame < end; ++frame) track.f0_hz[frame] = proposed[frame];
    }
    start = end;
  }
}

std::vector<NoteObject> monophonic_notes_from_track(const Audio& audio,
                                                    const pitch_editor::F0Track& track,
                                                    const TranscribeConfig& config) {
  NoteExtractorConfig extractor;
  extractor.segmenter.segmentation_threshold_cents = config.segmentation_threshold_cents;
  extractor.segmenter.min_note_ms = config.min_note_ms;
  extractor.segmenter.reference_hz = config.reference_hz;

  NoteDecoderInput input;
  input.track = &track;
  input.frame_rms = frame_rms_for_track(audio, track);
  input.valley_frames = narrow_valley_frames(audio, track);
  const std::vector<DecodedSpan> spans =
      decode_note_states(input, config.fmin, config.fmax, config.reference_hz,
                         config.segmentation_threshold_cents, config.min_note_ms);

  std::vector<NoteObject> notes;
  notes.reserve(spans.size());
  const int64_t sample_count = static_cast<int64_t>(audio.size());
  const int total_frames = track.n_frames();
  for (const DecodedSpan& span : spans) {
    notes.push_back(
        make_note(audio, 0, sample_count, track_window(track, span.frame_start, span.frame_end),
                  span.frame_start, total_frames, span.frame_start, span.frame_end, extractor));
  }
  repair_rederived_note_set_bounds(audio, track, notes, extractor);
  return notes;
}

std::vector<NoteObject> monophonic_notes(const Audio& audio, const TranscribeConfig& config) {
  PitchConfig pitch_config = pitch_editor::default_editing_pitch_config(audio.sample_rate());
  pitch_config.fmin = config.fmin;
  pitch_config.fmax = config.fmax;
  pitch_config.center = true;
  pitch_editor::PyinF0Provider provider(pitch_config);
  pitch_editor::F0Track track = provider.detect(audio);
  if (track.n_frames() == 0) return {};
  repair_monophonic_pitch_aliases(audio, track, pitch_config);
  return monophonic_notes_from_track(audio, track, config);
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

/// Maps measured regions to MIDI notes and velocities, dropping those under the
/// take-relative level gate.
std::vector<TranscribedNote> finalize_notes(const std::vector<NoteObject>& measured,
                                            const TranscribeConfig& resolved) {
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

}  // namespace

std::vector<TranscribedNote> transcribe_notes(const Audio& audio, const TranscribeConfig& config) {
  // No pitch-editor guard here: this unit is compiled into the pitch-editor
  // archive, so its existence is the gate. The NOT_SUPPORTED answer belongs to
  // the C-ABI entry, which is compiled either way.
  const TranscribeConfig resolved = validate_and_resolve(audio, config);
  return finalize_notes(resolved.source == TranscribeSource::kPolyphonic
                            ? polyphonic_notes(audio, resolved)
                            : monophonic_notes(audio, resolved),
                        resolved);
}

std::vector<TranscribedNote> transcribe_monophonic_track(const Audio& audio,
                                                         const pitch_editor::F0Track& track,
                                                         const TranscribeConfig& config) {
  const TranscribeConfig resolved = validate_and_resolve(audio, config);
  require(resolved.source == TranscribeSource::kMonophonic,
          "transcribe_monophonic_track: source must be monophonic");
  require(track.sample_rate == audio.sample_rate(),
          "transcribe_monophonic_track: track sample rate must match the audio");
  require(track.frame_rate() > 0.0f, "transcribe_monophonic_track: track needs a frame cadence");
  require(track.voiced.size() == track.f0_hz.size() &&
              (track.voiced_prob.empty() || track.voiced_prob.size() == track.f0_hz.size()),
          "transcribe_monophonic_track: track arrays must have one entry per frame");
  if (track.n_frames() == 0) return {};
  return finalize_notes(monophonic_notes_from_track(audio, track, resolved), resolved);
}

}  // namespace sonare::editing::note_model
