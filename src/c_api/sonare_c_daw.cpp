#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#if defined(SONARE_WITH_PITCH_EDITOR)
#include "editing/note_model/note_extractor.h"
#include "editing/note_model/note_renderer.h"
#include "editing/note_model/note_split_merge.h"
#include "editing/note_model/pitch_decomposition.h"
#include "editing/pitch_editor/note_editor.h"
#include "editing/pitch_editor/pitch_corrector.h"
#endif
#include <sonare/sonare_c.h>

#include "core/audio.h"
#include "sonare_c_internal.h"

using namespace sonare;
using namespace sonare_c_detail;

namespace {

/// Clears every field of a note-object result, so a failed call never leaves the
/// caller's struct holding stale pointers. Ungated: the free entry point shares
/// it and is compiled in every configuration.
void clear_note_objects_result(SonareNoteObjectsResult& out) {
  out.notes = nullptr;
  out.count = 0;
  out.amplitude = nullptr;
  out.amplitude_count = 0;
  out.envelopes = nullptr;
  out.envelope_count = 0;
}

#if defined(SONARE_WITH_PITCH_EDITOR)

bool valid_pitch_track_f0(float f0_hz, bool voiced, int sample_rate) {
  if (!std::isfinite(f0_hz)) {
    return !voiced && std::isnan(f0_hz);
  }
  return f0_hz >= 0.0f && f0_hz <= 0.5f * static_cast<float>(sample_rate);
}

/// Resolves a versioned note-extractor config onto the core defaults. Every
/// float takes its default at 0, matching sonare_note_segments.
SonareError resolve_extractor_config(const SonareNoteExtractorConfig* config,
                                     editing::note_model::NoteExtractorConfig& out) {
  if (config == nullptr) return SONARE_OK;
  if (config->struct_version < 0 || config->struct_version > 1) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!std::isfinite(config->segmentation_threshold_cents) || !std::isfinite(config->min_note_ms) ||
      !std::isfinite(config->reference_hz) || !std::isfinite(config->voiced_threshold) ||
      config->segmentation_threshold_cents < 0.0f || config->min_note_ms < 0.0f ||
      config->reference_hz < 0.0f || config->voiced_threshold < 0.0f ||
      config->voiced_threshold > 1.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (config->segmentation_threshold_cents > 0.0f) {
    out.segmenter.segmentation_threshold_cents = config->segmentation_threshold_cents;
  }
  if (config->min_note_ms > 0.0f) out.segmenter.min_note_ms = config->min_note_ms;
  if (config->reference_hz > 0.0f) out.segmenter.reference_hz = config->reference_hz;
  if (config->voiced_threshold > 0.0f) out.voiced_threshold = config->voiced_threshold;
  return SONARE_OK;
}

/// Validates the F0 frame array every note entry point takes.
SonareError validate_f0_frames(const float* f0_hz, size_t n_frames, float frame_rate) {
  if (f0_hz == nullptr || n_frames == 0 ||
      n_frames > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      !std::isfinite(frame_rate) || !(frame_rate > 0.0f)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  for (size_t i = 0; i < n_frames; ++i) {
    if (!std::isfinite(f0_hz[i]) || f0_hz[i] < 0.0f) return SONARE_ERROR_INVALID_PARAMETER;
  }
  return SONARE_OK;
}

/// Validates the whole F0 track the extraction-shaped entry points take.
SonareError validate_track_args(const float* f0_hz, const float* voiced_prob, const int32_t* voiced,
                                size_t n_frames, float frame_rate) {
  if (voiced == nullptr && voiced_prob == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  const SonareError error = validate_f0_frames(f0_hz, n_frames, frame_rate);
  if (error != SONARE_OK) return error;
  if (voiced != nullptr) return SONARE_OK;
  for (size_t i = 0; i < n_frames; ++i) {
    if (!std::isfinite(voiced_prob[i]) || voiced_prob[i] < 0.0f || voiced_prob[i] > 1.0f) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }
  return SONARE_OK;
}

/// Builds the core track from the caller's frame arrays.
editing::pitch_editor::F0Track make_track(const float* f0_hz, const float* voiced_prob,
                                          const int32_t* voiced, size_t n_frames, float frame_rate,
                                          int sample_rate, float voiced_threshold) {
  editing::pitch_editor::F0Track track;
  track.sample_rate = sample_rate;
  // Left at 0 deliberately: the extractor derives the hop from the cadence,
  // which is the rate the caller actually stated.
  track.hop_length = 0;
  track.frame_rate_hz = frame_rate;
  track.f0_hz.assign(f0_hz, f0_hz + n_frames);
  track.voiced.resize(n_frames);
  for (size_t i = 0; i < n_frames; ++i) {
    track.voiced[i] = voiced ? voiced[i] != 0 : voiced_prob[i] >= voiced_threshold;
  }
  return track;
}

/// Copies one note's envelope slice out of the caller's pool.
SonareError read_envelope(const SonareNoteEdit& edit, const float* envelopes, size_t envelope_count,
                          std::vector<float>& out) {
  out.clear();
  if (edit.envelope_count == 0) return SONARE_OK;
  if (envelopes == nullptr || edit.envelope_offset < 0 ||
      static_cast<uint64_t>(edit.envelope_offset) > static_cast<uint64_t>(envelope_count)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const size_t offset = static_cast<size_t>(edit.envelope_offset);
  // Subtraction rather than offset + count, which would wrap.
  if (edit.envelope_count > envelope_count - offset) return SONARE_ERROR_INVALID_PARAMETER;
  out.assign(envelopes + offset, envelopes + offset + edit.envelope_count);
  return SONARE_OK;
}

/// Marshals a C edit onto its core mirror, resolving the envelope slice.
SonareError to_core_edit(const SonareNoteEdit& edit, const float* envelopes, size_t envelope_count,
                         editing::note_model::NoteEdit& out) {
  out.time_offset_samples = edit.time_offset_samples;
  out.pitch_shift_semitones = edit.pitch_shift_semitones;
  out.gain_db = edit.gain_db;
  // A zeroed SonareNoteEdit must be the identity, so 0 reads as 1 here.
  out.time_stretch_ratio = edit.time_stretch_ratio == 0.0f ? 1.0f : edit.time_stretch_ratio;
  out.formant_shift_semitones = edit.formant_shift_semitones;
  out.vibrato_depth_change = edit.vibrato_depth_change;
  out.drift_change = edit.drift_change;
  out.muted = edit.muted != 0;
  return read_envelope(edit, envelopes, envelope_count, out.amplitude_envelope);
}

/// Marshals a core edit back out, pointing it at its slice of the result's pool.
void from_core_edit(const editing::note_model::NoteEdit& edit, int64_t envelope_offset,
                    SonareNoteEdit& out) {
  out.time_offset_samples = edit.time_offset_samples;
  out.envelope_offset = envelope_offset;
  out.envelope_count = edit.amplitude_envelope.size();
  out.pitch_shift_semitones = edit.pitch_shift_semitones;
  out.gain_db = edit.gain_db;
  out.time_stretch_ratio = edit.time_stretch_ratio;
  out.formant_shift_semitones = edit.formant_shift_semitones;
  out.vibrato_depth_change = edit.vibrato_depth_change;
  out.drift_change = edit.drift_change;
  out.muted = edit.muted ? 1 : 0;
}

/// Marshals a C edit for split and merge, which never render: the envelope
/// values the renderer would have rejected are checked here instead.
SonareError to_core_edit_checked(const SonareNoteEdit& edit, const float* envelopes,
                                 size_t envelope_count, editing::note_model::NoteEdit& out) {
  const SonareError error = to_core_edit(edit, envelopes, envelope_count, out);
  if (error != SONARE_OK) return error;
  for (const float value : out.amplitude_envelope) {
    if (!std::isfinite(value) || value < 0.0f) return SONARE_ERROR_INVALID_PARAMETER;
  }
  return SONARE_OK;
}

/// Marshals a core note list into a heap-owned C result: the notes, their
/// concatenated amplitude curves, and their concatenated edit envelopes.
SonareError fill_note_objects_result(const std::vector<editing::note_model::NoteObject>& notes,
                                     SonareNoteObjectsResult* out) {
  if (notes.empty()) return SONARE_OK;

  size_t total_amplitude = 0;
  size_t total_envelope = 0;
  for (const editing::note_model::NoteObject& note : notes) {
    total_amplitude += note.amplitude.values.size();
    total_envelope += note.edit.amplitude_envelope.size();
  }

  auto c_notes = std::make_unique<SonareNoteObject[]>(notes.size());
  std::unique_ptr<float[]> amplitude;
  if (total_amplitude > 0) amplitude = std::make_unique<float[]>(total_amplitude);
  std::unique_ptr<float[]> envelopes;
  if (total_envelope > 0) envelopes = std::make_unique<float[]>(total_envelope);

  int64_t amplitude_offset = 0;
  int64_t envelope_offset = 0;
  for (size_t i = 0; i < notes.size(); ++i) {
    const editing::note_model::NoteObject& note = notes[i];
    SonareNoteObject& row = c_notes[i];
    row.onset_sample = note.onset_sample;
    row.offset_sample = note.offset_sample;
    row.amplitude_offset = amplitude_offset;
    row.frame_start = note.frame_start;
    row.frame_end = note.frame_end;
    row.median_hz = note.median_hz;
    row.median_cents = note.median_cents;
    row.f0_stability = note.f0_stability;
    from_core_edit(note.edit, envelope_offset, row.edit);

    const size_t span = note.amplitude.values.size();
    if (span > 0) {
      std::memcpy(amplitude.get() + amplitude_offset, note.amplitude.values.data(),
                  span * sizeof(float));
    }
    amplitude_offset += static_cast<int64_t>(span);

    const size_t points = note.edit.amplitude_envelope.size();
    if (points > 0) {
      std::memcpy(envelopes.get() + envelope_offset, note.edit.amplitude_envelope.data(),
                  points * sizeof(float));
    }
    envelope_offset += static_cast<int64_t>(points);
  }

  out->count = notes.size();
  out->notes = c_notes.release();
  out->amplitude_count = total_amplitude;
  out->amplitude = amplitude.release();
  out->envelope_count = total_envelope;
  out->envelopes = envelopes.release();
  return SONARE_OK;
}

/// Shared body of sonare_split_note and sonare_merge_notes: both validate the
/// same arguments, rebuild the same note set, and return the same result shape.
/// @p out must already be non-null and cleared.
template <typename Fn>
SonareError run_note_set_edit(const float* samples, size_t length, int sample_rate,
                              const float* f0_hz, const float* voiced_prob, const int32_t* voiced,
                              size_t n_frames, float frame_rate,
                              const SonareNoteExtractorConfig* config,
                              const SonareNoteObject* notes, size_t note_count,
                              const float* envelopes, size_t envelope_count,
                              SonareNoteObjectsResult* out, Fn apply) {
  const SonareError track_error =
      validate_track_args(f0_hz, voiced_prob, voiced, n_frames, frame_rate);
  if (track_error != SONARE_OK) return track_error;
  if (notes == nullptr && note_count != 0) return SONARE_ERROR_INVALID_PARAMETER;

  editing::note_model::NoteExtractorConfig extractor_config;
  const SonareError config_error = resolve_extractor_config(config, extractor_config);
  if (config_error != SONARE_OK) return config_error;

  // An envelope slice outside the caller's pool is a bad argument rather than a
  // measurement, so the edits are resolved before any audio work.
  std::vector<editing::note_model::NoteEdit> edits(note_count);
  for (size_t i = 0; i < note_count; ++i) {
    const SonareError edit_error =
        to_core_edit_checked(notes[i].edit, envelopes, envelope_count, edits[i]);
    if (edit_error != SONARE_OK) return edit_error;
  }

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    const editing::pitch_editor::F0Track track =
        make_track(f0_hz, voiced_prob, voiced, n_frames, frame_rate, sample_rate,
                   extractor_config.voiced_threshold);
    // SonareNoteObject carries no curves, so every note is re-derived from the
    // audio and the track the way extraction derived it; make_note validates the
    // frame span, which is what these two cut on.
    std::vector<editing::note_model::NoteObject> core_notes;
    core_notes.reserve(note_count);
    for (size_t i = 0; i < note_count; ++i) {
      core_notes.push_back(editing::note_model::make_note(audio, track, notes[i].frame_start,
                                                          notes[i].frame_end, extractor_config));
      core_notes.back().edit = std::move(edits[i]);
    }
    return fill_note_objects_result(apply(audio, track, core_notes, extractor_config), out);
  });
}

#endif

}  // namespace

SonareError sonare_pitch_correct_to_midi(const float* samples, size_t length, int sample_rate,
                                         float current_midi, float target_midi, float** out,
                                         size_t* out_length) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    editing::pitch_editor::PitchCorrector corrector;
    Audio result = corrector.correct_to_midi(audio, current_midi, target_midi);
    return copy_audio_result(result, out, out_length);
  });
#else
  SONARE_C_STUB_NOT_SUPPORTED(samples, length, sample_rate, current_midi, target_midi, out,
                              out_length);
#endif
}

SonareError sonare_pitch_correct_to_midi_timevarying(const float* samples, size_t length,
                                                     int sample_rate, const float* f0_hz,
                                                     const float* voiced_prob,
                                                     const int32_t* voiced, size_t n_frames,
                                                     int hop_length, float target_midi, float** out,
                                                     size_t* out_length) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (!out || !out_length || !f0_hz || n_frames == 0 || hop_length <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!std::isfinite(target_midi) || target_midi < 0.0f || target_midi > 127.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // pYIN represents unvoiced F0 as NaN. Accept that canonical representation
  // only when the matching voiced flag is false; reject infinities, negative
  // frequencies, and pitches above Nyquist.
  for (size_t i = 0; i < n_frames; ++i) {
    const bool is_voiced = voiced ? (voiced[i] != 0) : true;
    if (!valid_pitch_track_f0(f0_hz[i], is_voiced, sample_rate)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (voiced_prob && (!std::isfinite(voiced_prob[i]))) return SONARE_ERROR_INVALID_PARAMETER;
  }

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    editing::pitch_editor::PitchCorrector corrector;
    editing::pitch_editor::F0Track track;
    track.sample_rate = sample_rate;
    track.hop_length = hop_length;
    track.f0_hz.assign(f0_hz, f0_hz + n_frames);
    track.voiced.resize(n_frames);
    track.voiced_prob.resize(n_frames);
    for (size_t i = 0; i < n_frames; ++i) {
      const bool is_voiced = voiced ? (voiced[i] != 0) : true;
      track.voiced[i] = is_voiced;
      track.voiced_prob[i] = voiced_prob ? voiced_prob[i] : (is_voiced ? 1.0f : 0.0f);
    }
    Audio result = corrector.correct_to_midi_timevarying(audio, track, target_midi);
    return copy_audio_result(result, out, out_length);
  });
#else
  SONARE_C_STUB_NOT_SUPPORTED(samples, length, sample_rate, f0_hz, voiced_prob, voiced, n_frames,
                              hop_length, target_midi, out, out_length);
#endif
}

SonareError sonare_pitch_correction_config_default(SonarePitchCorrectionConfig* config) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (!config) return SONARE_ERROR_INVALID_PARAMETER;
  const editing::pitch_editor::PitchCorrectionConfig defaults{};
  config->target_mode = SONARE_PITCH_TARGET_FIXED_MIDI;
  config->target_midi = constants::kMidiA4;
  config->scale_root = defaults.scale.root;
  config->scale_mode_mask = defaults.scale.mode_mask;
  config->scale_reference_midi = defaults.scale.reference_midi;
  config->retune_amount = defaults.retune_amount;
  config->max_correction_semitones = defaults.max_correction_semitones;
  config->retune_speed_ms = defaults.retune_speed_ms;
  config->vibrato_threshold_cents = defaults.vibrato_threshold_cents;
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(config);
#endif
}

SonareError sonare_pitch_correct_timevarying(const float* samples, size_t length, int sample_rate,
                                             const float* f0_hz, const float* voiced_prob,
                                             const int32_t* voiced, size_t n_frames, int hop_length,
                                             const SonarePitchCorrectionConfig* config, float** out,
                                             size_t* out_length) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (!out || !out_length || !f0_hz || n_frames == 0 || hop_length <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  // Resolve the config (NULL = library defaults), validating every numeric knob
  // so a NaN/out-of-range value cannot poison the retune pipeline.
  editing::pitch_editor::PitchCorrectionConfig core_config{};
  bool scale_mode = false;
  if (config) {
    scale_mode = config->target_mode == SONARE_PITCH_TARGET_SCALE;
    if (config->target_mode != SONARE_PITCH_TARGET_FIXED_MIDI &&
        config->target_mode != SONARE_PITCH_TARGET_SCALE) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (!scale_mode && (!std::isfinite(config->target_midi) || config->target_midi < 0.0f ||
                        config->target_midi > 127.0f)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (!editing::pitch_editor::valid_scale_args(config->scale_root,
                                                 static_cast<uint16_t>(config->scale_mode_mask)) ||
        (config->scale_mode_mask & ~uint32_t{0x0FFF}) != 0 ||
        !std::isfinite(config->scale_reference_midi) || !std::isfinite(config->retune_amount) ||
        config->retune_amount < 0.0f || config->retune_amount > 1.0f ||
        !std::isfinite(config->max_correction_semitones) ||
        config->max_correction_semitones < 0.0f || !std::isfinite(config->retune_speed_ms) ||
        config->retune_speed_ms < 0.0f || !std::isfinite(config->vibrato_threshold_cents) ||
        config->vibrato_threshold_cents < 0.0f) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    core_config.scale.root = config->scale_root;
    core_config.scale.mode_mask = static_cast<uint16_t>(config->scale_mode_mask);
    core_config.scale.reference_midi = config->scale_reference_midi;
    core_config.retune_amount = config->retune_amount;
    core_config.max_correction_semitones = config->max_correction_semitones;
    core_config.retune_speed_ms = config->retune_speed_ms;
    core_config.vibrato_threshold_cents = config->vibrato_threshold_cents;
  }
  const float target_midi = config ? config->target_midi : constants::kMidiA4;

  // Match the fixed-target entry point's pYIN/Nyquist contract.
  for (size_t i = 0; i < n_frames; ++i) {
    const bool is_voiced = voiced ? (voiced[i] != 0) : true;
    if (!valid_pitch_track_f0(f0_hz[i], is_voiced, sample_rate)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (voiced_prob && !std::isfinite(voiced_prob[i])) return SONARE_ERROR_INVALID_PARAMETER;
  }

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    editing::pitch_editor::PitchCorrector corrector(core_config);
    editing::pitch_editor::F0Track track;
    track.sample_rate = sample_rate;
    track.hop_length = hop_length;
    track.f0_hz.assign(f0_hz, f0_hz + n_frames);
    track.voiced.resize(n_frames);
    track.voiced_prob.resize(n_frames);
    for (size_t i = 0; i < n_frames; ++i) {
      const bool is_voiced = voiced ? (voiced[i] != 0) : true;
      track.voiced[i] = is_voiced;
      track.voiced_prob[i] = voiced_prob ? voiced_prob[i] : (is_voiced ? 1.0f : 0.0f);
    }
    Audio result = scale_mode ? corrector.correct_to_scale_timevarying(audio, track)
                              : corrector.correct_to_midi_timevarying(audio, track, target_midi);
    return copy_audio_result(result, out, out_length);
  });
#else
  SONARE_C_STUB_NOT_SUPPORTED(samples, length, sample_rate, f0_hz, voiced_prob, voiced, n_frames,
                              hop_length, config, out, out_length);
#endif
}

SonareError sonare_note_stretch(const float* samples, size_t length, int sample_rate,
                                int onset_sample, int offset_sample, float stretch_ratio,
                                float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    editing::pitch_editor::NoteRegion region;
    region.onset_sample = onset_sample;
    region.offset_sample = offset_sample;
    editing::pitch_editor::NoteEditor editor;
    Audio result = editor.stretch_note(audio, region, stretch_ratio);
    return copy_audio_result(result, out, out_length);
  });
#else
  SONARE_C_STUB_NOT_SUPPORTED(samples, length, sample_rate, onset_sample, offset_sample,
                              stretch_ratio, out, out_length);
#endif
}

SonareError sonare_extract_notes(const float* samples, size_t length, int sample_rate,
                                 const float* f0_hz, const float* voiced_prob,
                                 const int32_t* voiced, size_t n_frames, float frame_rate,
                                 const SonareNoteExtractorConfig* config,
                                 SonareNoteObjectsResult* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  clear_note_objects_result(*out);

  const SonareError track_error =
      validate_track_args(f0_hz, voiced_prob, voiced, n_frames, frame_rate);
  if (track_error != SONARE_OK) return track_error;

  editing::note_model::NoteExtractorConfig extractor_config;
  const SonareError config_error = resolve_extractor_config(config, extractor_config);
  if (config_error != SONARE_OK) return config_error;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    const editing::pitch_editor::F0Track track =
        make_track(f0_hz, voiced_prob, voiced, n_frames, frame_rate, sample_rate,
                   extractor_config.voiced_threshold);
    // Extraction produces identity edits, so the result's envelope pool stays
    // NULL.
    return fill_note_objects_result(
        editing::note_model::extract_notes(audio, track, extractor_config), out);
  });
#else
  SONARE_C_STUB_NOT_SUPPORTED(samples, length, sample_rate, f0_hz, voiced_prob, voiced, n_frames,
                              frame_rate, config, out);
#endif
}

void sonare_free_note_objects(SonareNoteObjectsResult* result) {
  if (result == nullptr) return;
  delete[] result->notes;
  delete[] result->amplitude;
  delete[] result->envelopes;
  clear_note_objects_result(*result);
}

SonareError sonare_render_notes(const float* samples, size_t length, int sample_rate,
                                const SonareNoteObject* notes, size_t note_count,
                                const float* envelopes, size_t envelope_count, const float* f0_hz,
                                size_t n_frames, float frame_rate,
                                const SonareNoteRenderConfig* config, float** out,
                                size_t* out_length) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  if (notes == nullptr && note_count != 0) return SONARE_ERROR_INVALID_PARAMETER;

  // The track is optional; only a curve edit reads it.
  if (f0_hz != nullptr) {
    const SonareError track_error = validate_f0_frames(f0_hz, n_frames, frame_rate);
    if (track_error != SONARE_OK) return track_error;
  } else if (n_frames != 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  editing::note_model::NoteRenderConfig render_config;
  if (config != nullptr) {
    if (config->struct_version < 0 || config->struct_version > 1) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (!std::isfinite(config->fade_ms) || config->fade_ms < 0.0f ||
        !std::isfinite(config->vibrato_cutoff_hz) || config->vibrato_cutoff_hz < 0.0f) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (config->fade_ms > 0.0f) render_config.fade_ms = config->fade_ms;
    if (config->vibrato_cutoff_hz > 0.0f) {
      render_config.decomposition.vibrato_cutoff_hz = config->vibrato_cutoff_hz;
    }
  }

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    std::vector<editing::note_model::NoteObject> core_notes(note_count);
    for (size_t i = 0; i < note_count; ++i) {
      // The metrics and the amplitude offset are not carried: the renderer never
      // looks at them.
      const SonareNoteObject& row = notes[i];
      core_notes[i].onset_sample = row.onset_sample;
      core_notes[i].offset_sample = row.offset_sample;
      core_notes[i].frame_start = row.frame_start;
      core_notes[i].frame_end = row.frame_end;
      core_notes[i].median_hz = row.median_hz;
      if (f0_hz != nullptr) {
        // The note's pitch curve is the caller's track sliced by its own bounds.
        if (row.frame_start < 0 || row.frame_end < row.frame_start ||
            static_cast<size_t>(row.frame_end) > n_frames) {
          return SONARE_ERROR_INVALID_PARAMETER;
        }
        core_notes[i].f0_hz.values.assign(f0_hz + row.frame_start, f0_hz + row.frame_end);
        core_notes[i].f0_hz.frame_rate_hz = frame_rate;
        core_notes[i].f0_hz.frame_offset = row.frame_start;
      } else if (row.edit.vibrato_depth_change != 0.0f || row.edit.drift_change != 0.0f) {
        // A curve edit with no track has nothing to act on.
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      const SonareError edit_error =
          to_core_edit(row.edit, envelopes, envelope_count, core_notes[i].edit);
      if (edit_error != SONARE_OK) return edit_error;
    }
    Audio result = editing::note_model::render_notes(audio, core_notes, render_config);
    return copy_audio_result(result, out, out_length);
  });
#else
  SONARE_C_STUB_NOT_SUPPORTED(samples, length, sample_rate, notes, note_count, envelopes,
                              envelope_count, f0_hz, n_frames, frame_rate, config, out, out_length);
#endif
}

SonareError sonare_decompose_note_pitch(const float* f0_hz, size_t n_frames, float frame_rate,
                                        float median_hz, float vibrato_cutoff_hz,
                                        SonarePitchDecompositionResult* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  out->centre_hz = 0.0f;
  out->drift_cents = nullptr;
  out->vibrato_cents = nullptr;
  out->count = 0;

  if (!std::isfinite(median_hz) || median_hz < 0.0f || !std::isfinite(vibrato_cutoff_hz) ||
      vibrato_cutoff_hz < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  const SonareError track_error = validate_f0_frames(f0_hz, n_frames, frame_rate);
  if (track_error != SONARE_OK) return track_error;

  SONARE_C_TRY
  editing::note_model::NoteObject note;
  note.median_hz = median_hz;
  note.f0_hz.values.assign(f0_hz, f0_hz + n_frames);
  note.f0_hz.frame_rate_hz = frame_rate;

  editing::note_model::PitchDecompositionConfig decomposition_config;
  if (vibrato_cutoff_hz > 0.0f) decomposition_config.vibrato_cutoff_hz = vibrato_cutoff_hz;

  const editing::note_model::PitchDecomposition split =
      editing::note_model::decompose_pitch(note, decomposition_config);
  // A note with no usable pitch comes back with a zero centre and empty curves,
  // which marshals to NULL and 0 without a special case.
  std::unique_ptr<float[]> drift(copy_vector(split.drift));
  std::unique_ptr<float[]> vibrato(copy_vector(split.vibrato));
  out->centre_hz = split.centre_hz;
  out->count = split.drift.size();
  out->drift_cents = drift.release();
  out->vibrato_cents = vibrato.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(f0_hz, n_frames, frame_rate, median_hz, vibrato_cutoff_hz, out);
#endif
}

void sonare_free_pitch_decomposition(SonarePitchDecompositionResult* result) {
  if (result == nullptr) return;
  delete[] result->drift_cents;
  delete[] result->vibrato_cents;
  result->centre_hz = 0.0f;
  result->drift_cents = nullptr;
  result->vibrato_cents = nullptr;
  result->count = 0;
}

SonareError sonare_split_note(const float* samples, size_t length, int sample_rate,
                              const float* f0_hz, const float* voiced_prob, const int32_t* voiced,
                              size_t n_frames, float frame_rate,
                              const SonareNoteExtractorConfig* config,
                              const SonareNoteObject* notes, size_t note_count,
                              const float* envelopes, size_t envelope_count, size_t index,
                              int32_t frame, SonareNoteObjectsResult* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  clear_note_objects_result(*out);
  if (index >= note_count) return SONARE_ERROR_INVALID_PARAMETER;

  return run_note_set_edit(samples, length, sample_rate, f0_hz, voiced_prob, voiced, n_frames,
                           frame_rate, config, notes, note_count, envelopes, envelope_count, out,
                           [&](const Audio& audio, const editing::pitch_editor::F0Track& track,
                               const std::vector<editing::note_model::NoteObject>& core_notes,
                               const editing::note_model::NoteExtractorConfig& extractor_config) {
                             return editing::note_model::split_note(audio, track, core_notes, index,
                                                                    frame, extractor_config);
                           });
#else
  SONARE_C_STUB_NOT_SUPPORTED(samples, length, sample_rate, f0_hz, voiced_prob, voiced, n_frames,
                              frame_rate, config, notes, note_count, envelopes, envelope_count,
                              index, frame, out);
#endif
}

SonareError sonare_merge_notes(const float* samples, size_t length, int sample_rate,
                               const float* f0_hz, const float* voiced_prob, const int32_t* voiced,
                               size_t n_frames, float frame_rate,
                               const SonareNoteExtractorConfig* config,
                               const SonareNoteObject* notes, size_t note_count,
                               const float* envelopes, size_t envelope_count, size_t first,
                               size_t last, SonareNoteObjectsResult* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  clear_note_objects_result(*out);
  if (!(first < last) || last >= note_count) return SONARE_ERROR_INVALID_PARAMETER;

  return run_note_set_edit(samples, length, sample_rate, f0_hz, voiced_prob, voiced, n_frames,
                           frame_rate, config, notes, note_count, envelopes, envelope_count, out,
                           [&](const Audio& audio, const editing::pitch_editor::F0Track& track,
                               const std::vector<editing::note_model::NoteObject>& core_notes,
                               const editing::note_model::NoteExtractorConfig& extractor_config) {
                             return editing::note_model::merge_notes(audio, track, core_notes,
                                                                     first, last, extractor_config);
                           });
#else
  SONARE_C_STUB_NOT_SUPPORTED(samples, length, sample_rate, f0_hz, voiced_prob, voiced, n_frames,
                              frame_rate, config, notes, note_count, envelopes, envelope_count,
                              first, last, out);
#endif
}

SonareError sonare_note_move(const float* samples, size_t length, int sample_rate, int onset_sample,
                             int offset_sample, int target_onset_sample, float** out,
                             size_t* out_length) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    editing::pitch_editor::NoteRegion region;
    region.onset_sample = onset_sample;
    region.offset_sample = offset_sample;
    editing::pitch_editor::NoteEditor editor;
    Audio result = editor.move_note(audio, region, target_onset_sample);
    return copy_audio_result(result, out, out_length);
  });
#else
  SONARE_C_STUB_NOT_SUPPORTED(samples, length, sample_rate, onset_sample, offset_sample,
                              target_onset_sample, out, out_length);
#endif
}
