/// @file feature_pitch.cpp
/// @brief Embind bindings for pitch feature APIs.

#ifdef __EMSCRIPTEN__

#include "wasm/bindings/common/common.h"

// ============================================================================
// Features - Pitch
// ============================================================================

namespace {

val pitch_result_to_val(const PitchResult& result) {
  val out = val::object();
  out.set("f0", vectorToFloat32Array(result.f0));
  out.set("voicedProb", vectorToFloat32Array(result.voiced_prob));

  // std::vector<bool>::operator[] returns a bit-reference proxy that embind
  // cannot marshal, so cast each element explicitly.
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

template <typename PitchFn>
val run_pitch_track(val samples, const val& sample_rate_val, const val& frame_length_val,
                    const val& hop_length_val, const val& fmin_val, const val& fmax_val,
                    const val& threshold_val, bool fill_na, PitchFn pitch) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int frame_length = checkedIntFromVal(frame_length_val, "frameLength");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  const float fmin = checkedFloatFromVal(fmin_val, "fmin");
  const float fmax = checkedFloatFromVal(fmax_val, "fmax");
  const float threshold = checkedFloatFromVal(threshold_val, "threshold");
  Audio audio = loadValidatedAudio(samples, sample_rate);

  PitchConfig config;
  config.frame_length = frame_length;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.fmax = fmax;
  config.threshold = threshold;
  config.fill_na = fill_na;

  PitchResult result = pitch(audio, config);
  return pitch_result_to_val(result);
}

}  // namespace

val js_pitch_yin(val samples, const val& sample_rate_val, const val& frame_length_val,
                 const val& hop_length_val, const val& fmin_val, const val& fmax_val,
                 const val& threshold_val, bool fill_na) {
  return run_pitch_track(
      samples, sample_rate_val, frame_length_val, hop_length_val, fmin_val, fmax_val, threshold_val,
      fill_na,
      [](const Audio& audio, const PitchConfig& config) { return yin_track(audio, config); });
}

val js_pitch_pyin(val samples, const val& sample_rate_val, const val& frame_length_val,
                  const val& hop_length_val, const val& fmin_val, const val& fmax_val,
                  const val& threshold_val, bool fill_na) {
  return run_pitch_track(
      samples, sample_rate_val, frame_length_val, hop_length_val, fmin_val, fmax_val, threshold_val,
      fill_na, [](const Audio& audio, const PitchConfig& config) { return pyin(audio, config); });
}

val js_note_segments(val f0_hz, val voiced_prob, const val& frame_rate_val, val options) {
  const float frame_rate = checkedFloatFromVal(frame_rate_val, "frameRate");
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
float js_pitch_tuning(val frequencies, const val& resolution_val, const val& bins_per_octave_val) {
  const int bins_per_octave = checkedIntFromVal(bins_per_octave_val, "binsPerOctave");
  const float resolution = checkedFloatFromVal(resolution_val, "resolution");
  std::vector<float> data = float32ArrayToVector(frequencies);
  return pitch_tuning(data, resolution, bins_per_octave);
}

// Global tuning offset of an audio signal. Mirrors the C ABI
// sonare_estimate_tuning / librosa.estimate_tuning.
float js_estimate_tuning(val samples, const val& sample_rate_val, const val& n_fft_val,
                         const val& hop_length_val, const val& resolution_val,
                         const val& bins_per_octave_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  const int bins_per_octave = checkedIntFromVal(bins_per_octave_val, "binsPerOctave");
  const float resolution = checkedFloatFromVal(resolution_val, "resolution");
  Audio audio = loadValidatedAudio(samples, sample_rate);
  return estimate_tuning(audio, n_fft, hop_length, resolution, bins_per_octave);
}

// Mirrors the C ABI sonare_tuning_to_reference_hz / sonare_reference_hz_to_tuning, whose
// validation they reuse so the refusals read the same everywhere.
float js_tuning_to_reference_hz(const val& tuning_val, const val& a4_val) {
  const float tuning = checkedFloatFromVal(tuning_val, "tuning");
  const float a4 = checkedFloatFromVal(a4_val, "a4");
  float hz = 0.0f;
  const SonareError error = sonare_tuning_to_reference_hz(tuning, a4, &hz);
  if (error != SONARE_OK) {
    throw SonareException(static_cast<ErrorCode>(error), sonare_last_error_message());
  }
  return hz;
}

float js_reference_hz_to_tuning(const val& hz_val, const val& a4_val) {
  const float hz = checkedFloatFromVal(hz_val, "hz");
  const float a4 = checkedFloatFromVal(a4_val, "a4");
  float tuning = 0.0f;
  const SonareError error = sonare_reference_hz_to_tuning(hz, a4, &tuning);
  if (error != SONARE_OK) {
    throw SonareException(static_cast<ErrorCode>(error), sonare_last_error_message());
  }
  return tuning;
}

val js_piptrack(val samples, const val& sample_rate_val, const val& n_fft_val,
                const val& hop_length_val, const val& fmin_val, const val& fmax_val,
                const val& threshold_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  const float fmin = checkedFloatFromVal(fmin_val, "fmin");
  const float fmax = checkedFloatFromVal(fmax_val, "fmax");
  const float threshold = checkedFloatFromVal(threshold_val, "threshold");
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
  function("tuningToReferenceHz", &js_tuning_to_reference_hz);
  function("referenceHzToTuning", &js_reference_hz_to_tuning);
  function("piptrack", &js_piptrack);
}

#endif  // __EMSCRIPTEN__
