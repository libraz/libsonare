#include "analysis/pitch_editor/f0_provider.h"

namespace sonare::analysis::pitch_editor {

PyinF0Provider::PyinF0Provider(PitchConfig config) : config_(config) {}

F0Track PyinF0Provider::detect(const Audio& audio) {
  const PitchResult result = pyin(audio, config_);
  F0Track track;
  track.f0_hz = result.f0;
  track.voiced_prob = result.voiced_prob;
  track.voiced = result.voiced_flag;
  track.hop_length = config_.hop_length;
  track.sample_rate = audio.sample_rate();
  return track;
}

}  // namespace sonare::analysis::pitch_editor
