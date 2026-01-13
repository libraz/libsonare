/// @file quick.cpp
/// @brief Implementation of simple function API.

#include "quick.h"

#include "analysis/beat_analyzer.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "core/audio.h"

namespace sonare {
namespace quick {

float detect_bpm(const float* samples, size_t size, int sample_rate) {
  Audio audio = Audio::from_buffer(samples, size, sample_rate);
  return sonare::detect_bpm(audio);
}

Key detect_key(const float* samples, size_t size, int sample_rate) {
  Audio audio = Audio::from_buffer(samples, size, sample_rate);
  return sonare::detect_key(audio);
}

std::vector<float> detect_onsets(const float* samples, size_t size, int sample_rate) {
  Audio audio = Audio::from_buffer(samples, size, sample_rate);
  return sonare::detect_onsets(audio);
}

std::vector<float> detect_beats(const float* samples, size_t size, int sample_rate) {
  Audio audio = Audio::from_buffer(samples, size, sample_rate);
  return sonare::detect_beats(audio);
}

AnalysisResult analyze(const float* samples, size_t size, int sample_rate) {
  Audio audio = Audio::from_buffer(samples, size, sample_rate);
  MusicAnalyzer analyzer(audio);
  return analyzer.analyze();
}

}  // namespace quick
}  // namespace sonare
