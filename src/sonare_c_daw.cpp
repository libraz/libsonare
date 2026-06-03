#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

#if defined(SONARE_WITH_PITCH_EDITOR)
#include "editing/pitch_editor/note_editor.h"
#include "editing/pitch_editor/pitch_corrector.h"
#endif
#include "core/audio.h"
#include "sonare_c.h"
#include "sonare_c_internal.h"

using namespace sonare;
using namespace sonare_c_detail;

SonareError sonare_pitch_correct_to_midi(const float* samples, size_t length, int sample_rate,
                                         float current_midi, float target_midi, float** out,
                                         size_t* out_length) {
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;
  // The caller asserts the source pitch via current_midi; the clip is shifted by
  // (target_midi - current_midi). Reject non-finite / out-of-range MIDI so a NaN
  // does not turn into a NaN f0 and produce garbage output.
  if (!std::isfinite(current_midi) || !std::isfinite(target_midi) || current_midi < 0.0f ||
      current_midi > 127.0f || target_midi < 0.0f || target_midi > 127.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    editing::pitch_editor::PitchCorrector corrector;
    editing::pitch_editor::F0Track track;
    track.sample_rate = sample_rate;
    track.hop_length = 512;
    track.f0_hz = {editing::pitch_editor::PitchCorrector::midi_to_hz(current_midi)};
    track.voiced = {true};
    track.voiced_prob = {1.0f};
    Audio result = corrector.correct_to_midi(audio, track, target_midi);
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
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (!out || !out_length || !f0_hz || n_frames == 0 || hop_length <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  if (!std::isfinite(target_midi) || target_midi < 0.0f || target_midi > 127.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  // Reject a non-finite or negative F0 so it cannot turn into garbage output.
  for (size_t i = 0; i < n_frames; ++i) {
    if (!std::isfinite(f0_hz[i]) || f0_hz[i] < 0.0f) return SONARE_ERROR_INVALID_PARAMETER;
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

SonareError sonare_note_stretch(const float* samples, size_t length, int sample_rate,
                                int onset_sample, int offset_sample, float stretch_ratio,
                                float** out, size_t* out_length) {
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
