/// @file quick.cpp
/// @brief Implementation of simple function API.

#include "quick.h"

#include "analysis/beat_analyzer.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "core/audio.h"
#include "core/resample.h"

namespace sonare {
namespace quick {

namespace {

/// @brief Downsample to 22050 Hz if needed (matches MusicAnalyzer behavior).
Audio prepare_audio(const float* samples, size_t size, int sample_rate) {
  constexpr int kAnalysisSampleRate = 22050;
  Audio audio = Audio::from_buffer(samples, size, sample_rate);
  if (sample_rate > kAnalysisSampleRate) {
    return resample(audio, kAnalysisSampleRate);
  }
  return audio;
}

}  // namespace

float detect_bpm(const float* samples, size_t size, int sample_rate) {
  Audio audio = prepare_audio(samples, size, sample_rate);
  return sonare::detect_bpm(audio);
}

Key detect_key(const float* samples, size_t size, int sample_rate) {
  Audio audio = prepare_audio(samples, size, sample_rate);
  return sonare::detect_key(audio);
}

std::vector<float> detect_onsets(const float* samples, size_t size, int sample_rate) {
  Audio audio = prepare_audio(samples, size, sample_rate);
  return sonare::detect_onsets(audio);
}

std::vector<float> detect_beats(const float* samples, size_t size, int sample_rate) {
  Audio audio = prepare_audio(samples, size, sample_rate);
  return sonare::detect_beats(audio);
}

AnalysisResult analyze(const float* samples, size_t size, int sample_rate) {
  Audio audio = Audio::from_buffer(samples, size, sample_rate);
  MusicAnalyzer analyzer(audio);
  return analyzer.analyze();
}

}  // namespace quick
}  // namespace sonare
