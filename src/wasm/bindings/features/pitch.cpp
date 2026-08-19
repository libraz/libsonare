/// @file feature_pitch.cpp
/// @brief Embind bindings for pitch feature APIs.

#ifdef __EMSCRIPTEN__

#include "wasm/bindings/common/common.h"

// ============================================================================
// Features - Pitch
// ============================================================================

val js_pitch_yin(val samples, int sample_rate, int frame_length, int hop_length, float fmin,
                 float fmax, float threshold, bool fill_na) {
  Audio audio = loadValidatedAudio(samples, sample_rate);

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
  Audio audio = loadValidatedAudio(samples, sample_rate);

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

val js_note_segments(val f0_hz, val voiced_prob, float frame_rate, val options) {
  std::vector<float> f0 = float32ArrayToVector(f0_hz);
  std::vector<float> probabilities = float32ArrayToVector(voiced_prob);
  SonareNoteSegmenterConfig config{};
  config.struct_version = 2;
  config.segmentation_threshold_cents = floatProperty(options, "segmentationThresholdCents", 0.0f);
  config.min_note_ms = floatProperty(options, "minNoteMs", 0.0f);
  config.reference_hz = floatProperty(options, "referenceHz", 0.0f);
  config.voiced_threshold = floatProperty(options, "voicedThreshold", 0.0f);
  SonareNoteSegmentsResult result{};
  const SonareError error =
      sonare_note_segments(f0.data(), f0.size(), probabilities.data(), probabilities.size(),
                           frame_rate, &config, &result);
  if (error != SONARE_OK) {
    throw SonareException(static_cast<ErrorCode>(error), sonare_last_error_message());
  }

  val out = val::array();
  for (size_t i = 0; i < result.count; ++i) {
    const SonareNoteSegment& segment = result.segments[i];
    val row = val::object();
    row.set("frameStart", segment.frame_start);
    row.set("frameEnd", segment.frame_end);
    row.set("startSeconds", segment.start_seconds);
    row.set("endSeconds", segment.end_seconds);
    row.set("medianCents", segment.median_cents);
    out.call<void>("push", row);
  }
  sonare_free_note_segments(&result);
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
  Audio audio = loadValidatedAudio(samples, sample_rate);
  return estimate_tuning(audio, n_fft, hop_length, resolution, bins_per_octave);
}

val js_piptrack(val samples, int sample_rate, int n_fft, int hop_length, float fmin, float fmax,
                float threshold) {
  const Audio audio = loadValidatedAudio(samples, sample_rate);
  const PiptrackResult result = piptrack(audio, n_fft, hop_length, fmin, fmax, threshold);
  val out = val::object();
  out.set("nBins", result.n_bins);
  out.set("nFrames", result.n_frames);
  out.set("pitches", vectorToFloat32Array(result.pitches));
  out.set("magnitudes", vectorToFloat32Array(result.magnitudes));
  return out;
}

void registerFeaturePitchBindings() {
  function("pitchYin", &js_pitch_yin);
  function("pitchPyin", &js_pitch_pyin);
  function("noteSegments", &js_note_segments);
  function("pitchTuning", &js_pitch_tuning);
  function("estimateTuning", &js_estimate_tuning);
  function("piptrack", &js_piptrack);
}

#endif  // __EMSCRIPTEN__
