#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#if defined(SONARE_WITH_PITCH_EDITOR)
#include "editing/note_model/note_extractor.h"
#include "editing/note_model/note_renderer.h"
#include "editing/pitch_editor/note_editor.h"
#include "editing/pitch_editor/pitch_corrector.h"
#endif
#include <sonare/sonare_c.h>

#include "core/audio.h"
#include "sonare_c_internal.h"

using namespace sonare;
using namespace sonare_c_detail;

#if defined(SONARE_WITH_PITCH_EDITOR)
namespace {

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

}  // namespace
#endif

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
  out->notes = nullptr;
  out->count = 0;
  out->amplitude = nullptr;
  out->amplitude_count = 0;

  if (f0_hz == nullptr || n_frames == 0 ||
      n_frames > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      (voiced == nullptr && voiced_prob == nullptr) || !std::isfinite(frame_rate) ||
      !(frame_rate > 0.0f)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  editing::note_model::NoteExtractorConfig extractor_config;
  const SonareError config_error = resolve_extractor_config(config, extractor_config);
  if (config_error != SONARE_OK) return config_error;

  for (size_t i = 0; i < n_frames; ++i) {
    if (!std::isfinite(f0_hz[i]) || f0_hz[i] < 0.0f) return SONARE_ERROR_INVALID_PARAMETER;
    if (voiced == nullptr &&
        (!std::isfinite(voiced_prob[i]) || voiced_prob[i] < 0.0f || voiced_prob[i] > 1.0f)) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
  }

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    editing::pitch_editor::F0Track track;
    track.sample_rate = sample_rate;
    // Left at 0 deliberately: the extractor derives the hop from the cadence,
    // which is the rate the caller actually stated.
    track.hop_length = 0;
    track.frame_rate_hz = frame_rate;
    track.f0_hz.assign(f0_hz, f0_hz + n_frames);
    track.voiced.resize(n_frames);
    for (size_t i = 0; i < n_frames; ++i) {
      track.voiced[i] =
          voiced ? voiced[i] != 0 : voiced_prob[i] >= extractor_config.voiced_threshold;
    }

    const std::vector<editing::note_model::NoteObject> notes =
        editing::note_model::extract_notes(audio, track, extractor_config);
    if (notes.empty()) return SONARE_OK;

    size_t total_frames = 0;
    for (const editing::note_model::NoteObject& note : notes) {
      total_frames += note.amplitude.values.size();
    }

    auto c_notes = std::make_unique<SonareNoteObject[]>(notes.size());
    std::unique_ptr<float[]> amplitude;
    if (total_frames > 0) amplitude = std::make_unique<float[]>(total_frames);

    int64_t amplitude_offset = 0;
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
      row.edit = SonareNoteEdit{0, 0.0f, 0.0f, 1.0f, 0};
      const size_t span = note.amplitude.values.size();
      if (span > 0) {
        std::memcpy(amplitude.get() + amplitude_offset, note.amplitude.values.data(),
                    span * sizeof(float));
      }
      amplitude_offset += static_cast<int64_t>(span);
    }

    out->count = notes.size();
    out->notes = c_notes.release();
    out->amplitude_count = total_frames;
    out->amplitude = amplitude.release();
    return SONARE_OK;
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
  result->notes = nullptr;
  result->count = 0;
  result->amplitude = nullptr;
  result->amplitude_count = 0;
}

SonareError sonare_render_notes(const float* samples, size_t length, int sample_rate,
                                const SonareNoteObject* notes, size_t note_count,
                                const SonareNoteRenderConfig* config, float** out,
                                size_t* out_length) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  if (notes == nullptr && note_count != 0) return SONARE_ERROR_INVALID_PARAMETER;

  editing::note_model::NoteRenderConfig render_config;
  if (config != nullptr) {
    if (config->struct_version < 0 || config->struct_version > 1) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (!std::isfinite(config->fade_ms) || config->fade_ms < 0.0f) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (config->fade_ms > 0.0f) render_config.fade_ms = config->fade_ms;
  }

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    std::vector<editing::note_model::NoteObject> core_notes(note_count);
    for (size_t i = 0; i < note_count; ++i) {
      // Only the span and the edit are read; carrying the rest across would be
      // fields the renderer never looks at.
      core_notes[i].onset_sample = notes[i].onset_sample;
      core_notes[i].offset_sample = notes[i].offset_sample;
      editing::note_model::NoteEdit& edit = core_notes[i].edit;
      edit.time_offset_samples = notes[i].edit.time_offset_samples;
      edit.pitch_shift_semitones = notes[i].edit.pitch_shift_semitones;
      edit.gain_db = notes[i].edit.gain_db;
      // A zeroed SonareNoteEdit must be the identity, so 0 reads as 1 here.
      edit.time_stretch_ratio =
          notes[i].edit.time_stretch_ratio == 0.0f ? 1.0f : notes[i].edit.time_stretch_ratio;
      edit.muted = notes[i].edit.muted != 0;
    }
    Audio result = editing::note_model::render_notes(audio, core_notes, render_config);
    return copy_audio_result(result, out, out_length);
  });
#else
  SONARE_C_STUB_NOT_SUPPORTED(samples, length, sample_rate, notes, note_count, config, out,
                              out_length);
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
