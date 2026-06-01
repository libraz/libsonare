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

namespace {

#if defined(SONARE_WITH_PITCH_EDITOR)
SonareError copy_audio_result(const Audio& result, float** out, size_t* out_length) {
  *out_length = result.size();
  *out = new float[result.size()];
  std::memcpy(*out, result.data(), result.size() * sizeof(float));
  return SONARE_OK;
}
#endif

}  // namespace

SonareError sonare_pitch_correct_to_midi(const float* samples, size_t length, int sample_rate,
                                         float current_midi, float target_midi, float** out,
                                         size_t* out_length) {
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;

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
  (void)samples;
  (void)length;
  (void)sample_rate;
  (void)current_midi;
  (void)target_midi;
  (void)out;
  (void)out_length;
  return SONARE_ERROR_NOT_SUPPORTED;
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
  (void)samples;
  (void)length;
  (void)sample_rate;
  (void)onset_sample;
  (void)offset_sample;
  (void)stretch_ratio;
  (void)out;
  (void)out_length;
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}
