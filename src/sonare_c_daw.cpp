#include <cmath>
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
