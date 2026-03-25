/// @file quick.cpp
/// @brief Implementation of simple function API.

#include "quick.h"

#include "core/audio.h"

namespace sonare {
namespace quick {

float detect_bpm(const float* samples, size_t size, int sample_rate) {
  Audio audio = Audio::from_buffer(samples, size, sample_rate);
  MusicAnalyzer analyzer(audio);
  return analyzer.bpm();
}

Key detect_key(const float* samples, size_t size, int sample_rate) {
  Audio audio = Audio::from_buffer(samples, size, sample_rate);
  MusicAnalyzer analyzer(audio);
  return analyzer.key();
}

std::vector<float> detect_onsets(const float* samples, size_t size, int sample_rate) {
  Audio audio = Audio::from_buffer(samples, size, sample_rate);
  MusicAnalyzer analyzer(audio);
  return analyzer.onset_analyzer().onset_times();
}

std::vector<float> detect_beats(const float* samples, size_t size, int sample_rate) {
  Audio audio = Audio::from_buffer(samples, size, sample_rate);
  MusicAnalyzer analyzer(audio);
  return analyzer.beat_times();
}

AnalysisResult analyze(const float* samples, size_t size, int sample_rate) {
  Audio audio = Audio::from_buffer(samples, size, sample_rate);
  MusicAnalyzer analyzer(audio);
  return analyzer.analyze();
}

}  // namespace quick
}  // namespace sonare
