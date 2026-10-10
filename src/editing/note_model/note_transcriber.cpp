#include "editing/note_model/note_transcriber.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
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
// The polyphonic default is a thirty-second note at the tempo, held within these bounds.
constexpr int kTranscribePolyphonicMinNoteDivision = 32;
// A recorded piano solo breaks real notes into 60-99 ms ridges that 100 ms dropped by
// ear; 60 ms keeps them. On rendered scores 60-100 ms scored alike, and broadband
// noise at -20 dBFS yields about 25 short notes a second at 60 ms (5.8 at 100).
constexpr float kTranscribePolyphonicMaxMinNoteMs = 60.0f;
// The segmenter's own shortest note; below it a ridge is a few analysis frames.
constexpr float kTranscribePolyphonicMinMinNoteMs = 30.0f;
// A whole note is four beats of 60000 ms / BPM.
constexpr float kWholeNoteMsAtOneBpm = 4.0f * 60000.0f;
// Unchanged from the editing chain: the sweep never moved it.
constexpr float kTranscribeMinFramePeakRatio = 0.20f;
// Unchanged from the editing chain: the sweep never moved it.
constexpr float kTranscribeMinRidgePeakRatio = 0.10f;
// Re-strikes scored F 1.0 at 1.5 and 2.0 and 0.86 at 2.5, none splitting a held
// note; 2.0 had the higher mean piano F (0.717 against 0.678).
constexpr float kTranscribeReattackRatio = 2.0f;

/// Length of a 1/@p division note at @p tempo_bpm, in milliseconds.
float note_value_ms(int division, float tempo_bpm) noexcept {
  return kWholeNoteMsAtOneBpm / (tempo_bpm * static_cast<float>(division));
}

float default_min_note_ms(const TranscribeConfig& config) noexcept {
  if (config.source != TranscribeSource::kPolyphonic) {
    return pitch_editor::NoteSegmenterConfig{}.min_note_ms;
  }
  if (!(config.tempo_bpm > 0.0f)) return kTranscribePolyphonicMaxMinNoteMs;
  return std::clamp(note_value_ms(kTranscribePolyphonicMinNoteDivision, config.tempo_bpm),
                    kTranscribePolyphonicMinMinNoteMs, kTranscribePolyphonicMaxMinNoteMs);
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

std::vector<NoteObject> monophonic_notes(const Audio& audio, const TranscribeConfig& config) {
  PitchConfig pitch_config;
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
  return extract_notes(audio, track, extractor);
}

std::vector<NoteObject> polyphonic_notes(const Audio& audio, const TranscribeConfig& config) {
  polyphony::PolyphonicEditConfig poly;
  // The range belongs to the estimator's F0 axis. The cent spectrum keeps its
  // own frequency ceiling, because it also has to hold the harmonics it sums.
  poly.extraction.estimation.salience.f0_min_hz = config.fmin;
  poly.extraction.estimation.salience.f0_max_hz = config.fmax;
  // One note per ridge, so the shortest note is the shortest ridge kept;
  // segmenter.min_note_ms is never consulted on this path (see masked_notes.h).
  const TranscribePolyphonyLimits limits = resolve_transcribe_polyphony_limits(config);
  poly.extraction.ridges.min_duration_ms = limits.min_note_ms;
  poly.extraction.ridges.min_ridge_peak_ratio = limits.min_ridge_peak_ratio;
  poly.extraction.ridges.reattack_ratio = limits.reattack_ratio;
  poly.extraction.estimation.max_polyphony = limits.max_polyphony;
  poly.extraction.estimation.min_frame_peak_ratio = limits.min_frame_peak_ratio;
  poly.notes.segmenter.segmentation_threshold_cents = config.segmentation_threshold_cents;
  poly.notes.segmenter.reference_hz = config.reference_hz;
  return polyphony::analyze_polyphonic(audio, poly).notes;
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

std::vector<TranscribedNote> transcribe_notes(const Audio& audio, const TranscribeConfig& config) {
  // No pitch-editor guard here: this unit is compiled into the pitch-editor
  // archive, so its existence is the gate. The NOT_SUPPORTED answer belongs to
  // the C-ABI entry, which is compiled either way.
  const TranscribeConfig resolved = validate_and_resolve(audio, config);

  const std::vector<NoteObject> measured = resolved.source == TranscribeSource::kPolyphonic
                                               ? polyphonic_notes(audio, resolved)
                                               : monophonic_notes(audio, resolved);

  std::vector<TranscribedNote> notes;
  notes.reserve(measured.size());
  for (const NoteObject& note : measured) {
    if (note.offset_sample <= note.onset_sample) continue;
    const int midi_note = midi_note_for_hz(note.median_hz, resolved.reference_hz);
    if (midi_note < 0) continue;
    const uint8_t velocity =
        resolved.fixed_velocity != 0
            ? static_cast<uint8_t>(resolved.fixed_velocity)
            : velocity_for_peak_rms(peak_rms(note), resolved.velocity_floor_db);
    notes.push_back(TranscribedNote{note.onset_sample, note.offset_sample,
                                    static_cast<uint8_t>(midi_note), velocity, note.median_hz});
  }

  std::sort(notes.begin(), notes.end(),
            [](const TranscribedNote& a, const TranscribedNote& b) noexcept {
              if (a.onset_sample != b.onset_sample) return a.onset_sample < b.onset_sample;
              return a.note < b.note;
            });
  return notes;
}

}  // namespace sonare::editing::note_model
