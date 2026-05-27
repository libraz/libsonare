/// @file quick.cpp
/// @brief Implementation of simple function API.

#include "quick.h"

#include "analysis/acoustic_analyzer.h"
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
  Audio audio = Audio::from_buffer(samples, size, sample_rate);
  return sonare::detect_bpm(audio);
}

Key detect_key(const float* samples, size_t size, int sample_rate) {
  Audio audio = prepare_audio(samples, size, sample_rate);
  return sonare::detect_key(audio);
}

Key detect_key(const float* samples, size_t size, int sample_rate, const KeyConfig& config) {
  Audio audio = prepare_audio(samples, size, sample_rate);
  return sonare::detect_key(audio, config);
}

std::vector<KeyCandidate> detect_key_candidates(const float* samples, size_t size, int sample_rate,
                                                const KeyConfig& config) {
  Audio audio = prepare_audio(samples, size, sample_rate);
  KeyAnalyzer analyzer(audio, config);
  return analyzer.all_candidates();
}

std::vector<float> detect_onsets(const float* samples, size_t size, int sample_rate) {
  Audio audio = prepare_audio(samples, size, sample_rate);
  return sonare::detect_onsets(audio);
}

std::vector<float> detect_beats(const float* samples, size_t size, int sample_rate) {
  Audio audio = prepare_audio(samples, size, sample_rate);
  return sonare::detect_beats(audio);
}

std::vector<float> detect_downbeats(const float* samples, size_t size, int sample_rate) {
  Audio audio = prepare_audio(samples, size, sample_rate);
  BeatAnalyzer analyzer(audio);
  std::vector<float> times;
  times.reserve(analyzer.downbeats().size());
  for (const auto& downbeat : analyzer.downbeats()) {
    times.push_back(downbeat.time);
  }
  return times;
}

AnalysisResult analyze(const float* samples, size_t size, int sample_rate) {
  Audio audio = Audio::from_buffer(samples, size, sample_rate);
  MusicAnalyzer analyzer(audio);
  return analyzer.analyze();
}

AcousticParameters detect_acoustic(const float* samples, size_t size, int sample_rate) {
  Audio audio = Audio::from_buffer(samples, size, sample_rate);
  return sonare::detect_acoustic(audio);
}

AcousticParameters analyze_impulse_response(const float* samples, size_t size, int sample_rate) {
  Audio audio = Audio::from_buffer(samples, size, sample_rate);
  return sonare::analyze_impulse_response(audio);
}

}  // namespace quick
}  // namespace sonare
