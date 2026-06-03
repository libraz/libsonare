/// @file feature_pitch.cpp
/// @brief Embind bindings for pitch feature APIs.

#ifdef __EMSCRIPTEN__

#include "common.h"

// ============================================================================
// Features - Pitch
// ============================================================================

val js_pitch_yin(val samples, int sample_rate, int frame_length, int hop_length, float fmin,
                 float fmax, float threshold, bool fill_na) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  PitchConfig config;
  config.frame_length = frame_length;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.fmax = fmax;
  config.threshold = threshold;
  config.fill_na = fill_na;

  PitchResult result = yin_track(audio, config);

  val out = val::object();
  out.set("f0", vectorToFloat32Array(result.f0));

  // Convert voiced_prob to Float32Array
  out.set("voicedProb", vectorToFloat32Array(result.voiced_prob));

  // Convert voiced_flag to array of bools.
  // std::vector<bool>::operator[] returns a __bit_reference proxy that embind
  // cannot marshal, so we cast to bool explicitly.
  val voiced_arr = val::array();
  for (size_t i = 0; i < result.voiced_flag.size(); ++i) {
    voiced_arr.call<void>("push", static_cast<bool>(result.voiced_flag[i]));
  }
  out.set("voicedFlag", voiced_arr);

  out.set("nFrames", result.n_frames());
  out.set("medianF0", result.median_f0());
  out.set("meanF0", result.mean_f0());

  return out;
}

val js_pitch_pyin(val samples, int sample_rate, int frame_length, int hop_length, float fmin,
                  float fmax, float threshold, bool fill_na) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  PitchConfig config;
  config.frame_length = frame_length;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.fmax = fmax;
  config.threshold = threshold;
  config.fill_na = fill_na;

  PitchResult result = pyin(audio, config);

  val out = val::object();
  out.set("f0", vectorToFloat32Array(result.f0));
  out.set("voicedProb", vectorToFloat32Array(result.voiced_prob));

  val voiced_arr = val::array();
  for (size_t i = 0; i < result.voiced_flag.size(); ++i) {
    voiced_arr.call<void>("push", static_cast<bool>(result.voiced_flag[i]));
  }
  out.set("voicedFlag", voiced_arr);

  out.set("nFrames", result.n_frames());
  out.set("medianF0", result.median_f0());
  out.set("meanF0", result.mean_f0());

  return out;
}

// Per-octave tuning offset from a list of detected pitches. Mirrors the C ABI
// sonare_pitch_tuning / librosa.pitch_tuning.
float js_pitch_tuning(val frequencies, float resolution, int bins_per_octave) {
  std::vector<float> data = float32ArrayToVector(frequencies);
  return pitch_tuning(data, resolution, bins_per_octave);
}

// Global tuning offset of an audio signal. Mirrors the C ABI
// sonare_estimate_tuning / librosa.estimate_tuning.
float js_estimate_tuning(val samples, int sample_rate, int n_fft, int hop_length, float resolution,
                         int bins_per_octave) {
  std::vector<float> data = float32ArrayToVector(samples);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);
  return estimate_tuning(audio, n_fft, hop_length, resolution, bins_per_octave);
}

void registerFeaturePitchBindings() {
  function("pitchYin", &js_pitch_yin);
  function("pitchPyin", &js_pitch_pyin);
  function("pitchTuning", &js_pitch_tuning);
  function("estimateTuning", &js_estimate_tuning);
}

#endif  // __EMSCRIPTEN__
