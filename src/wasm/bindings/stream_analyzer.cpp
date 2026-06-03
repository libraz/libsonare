/// @file stream_analyzer.cpp
/// @brief Embind bindings for streaming analysis.

#ifdef __EMSCRIPTEN__

#include "common.h"

// ============================================================================
// Streaming - StreamAnalyzer
// ============================================================================

/// @brief Helper to convert uint8 vector to Uint8Array.
///
/// Uses the same single-bulk-memcpy path as vectorToFloat32Array/Int32Array: a
/// non-owning typed_memory_view over the C++ buffer plus a JS-side
/// TypedArray.prototype.set, avoiding one JS<->WASM boundary crossing per
/// element (the per-element set() defeats the bandwidth-reduction purpose of the
/// quantized readFramesU8/readFramesI16 fast paths).
val vectorToUint8Array(const std::vector<uint8_t>& vec) {
  const size_t n = vec.size();
  val result = val::global("Uint8Array").new_(n);
  if (n == 0) return result;
  val view = val(typed_memory_view(n, vec.data()));
  result.call<void>("set", view);
  return result;
}

/// @brief Helper to convert int16 vector to Int16Array.
val vectorToInt16Array(const std::vector<int16_t>& vec) {
  const size_t n = vec.size();
  val result = val::global("Int16Array").new_(n);
  if (n == 0) return result;
  val view = val(typed_memory_view(n, vec.data()));
  result.call<void>("set", view);
  return result;
}

/// @brief Reads an optional quantization-range config from a JS value.
/// @details An undefined/null value (or a missing field) keeps the library
///          default for that range, so a partial object overrides only the
///          ranges the caller supplied.
QuantizeConfig quantizeConfigFromVal(const val& config) {
  QuantizeConfig qconfig;
  if (config.isUndefined() || config.isNull()) return qconfig;
  const auto read = [&](const char* key, float& field) {
    const val value = config[key];
    if (!value.isUndefined() && !value.isNull()) field = value.as<float>();
  };
  read("melDbMin", qconfig.mel_db_min);
  read("melDbMax", qconfig.mel_db_max);
  read("onsetMax", qconfig.onset_max);
  read("rmsMax", qconfig.rms_max);
  read("centroidMax", qconfig.centroid_max);
  return qconfig;
}

/// @brief JavaScript wrapper for StreamAnalyzer.
class StreamAnalyzerWrapper {
 public:
  StreamAnalyzerWrapper(int sample_rate, int n_fft, int hop_length, int n_mels, float fmin,
                        float fmax, float tuning_ref_hz, bool compute_magnitude, bool compute_mel,
                        bool compute_chroma, bool compute_onset, bool compute_spectral,
                        int emit_every_n_frames, int magnitude_downsample,
                        float key_update_interval_sec, float bpm_update_interval_sec, int window,
                        int output_format) {
    StreamConfig config;
    config.sample_rate = sample_rate;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    config.n_mels = n_mels;
    config.window = window == 1   ? WindowType::Hamming
                    : window == 2 ? WindowType::Blackman
                    : window == 3 ? WindowType::Rectangular
                                  : WindowType::Hann;
    config.fmin = fmin;
    config.fmax = fmax;
    config.tuning_ref_hz = tuning_ref_hz;
    config.compute_magnitude = compute_magnitude;
    config.compute_mel = compute_mel;
    config.compute_chroma = compute_chroma;
    config.compute_onset = compute_onset;
    config.compute_spectral = compute_spectral;
    config.emit_every_n_frames = emit_every_n_frames;
    config.magnitude_downsample = magnitude_downsample;
    config.output_format = output_format == 1   ? OutputFormat::Int16
                           : output_format == 2 ? OutputFormat::Uint8
                                                : OutputFormat::Float32;
    config.key_update_interval_sec = key_update_interval_sec;
    config.bpm_update_interval_sec = bpm_update_interval_sec;
    config_ = config;
    analyzer_ = std::make_unique<StreamAnalyzer>(config);
  }

  /// @brief Returns the sample rate.
  int sampleRate() const { return config_.sample_rate; }

  void process(val samples) {
    std::vector<float> data = float32ArrayToVector(samples);
    analyzer_->process(data.data(), data.size());
  }

  void processWithOffset(val samples, size_t sample_offset) {
    std::vector<float> data = float32ArrayToVector(samples);
    analyzer_->process(data.data(), data.size(), sample_offset);
  }

  size_t availableFrames() const { return analyzer_->available_frames(); }

  /// @brief Reads frames in Float32 SOA format.
  val readFramesSoa(size_t max_frames) {
    FrameBuffer buffer;
    analyzer_->read_frames_soa(max_frames, buffer);

    val out = val::object();
    out.set("nFrames", buffer.n_frames);
    out.set("nMels", config_.n_mels);
    out.set("timestamps", vectorToFloat32Array(buffer.timestamps));
    out.set("mel", vectorToFloat32Array(buffer.mel));
    out.set("chroma", vectorToFloat32Array(buffer.chroma));
    out.set("onsetStrength", vectorToFloat32Array(buffer.onset_strength));
    out.set("rmsEnergy", vectorToFloat32Array(buffer.rms_energy));
    out.set("spectralCentroid", vectorToFloat32Array(buffer.spectral_centroid));
    out.set("spectralFlatness", vectorToFloat32Array(buffer.spectral_flatness));
    out.set("chordRoot", vectorToInt32Array(buffer.chord_root));
    out.set("chordQuality", vectorToInt32Array(buffer.chord_quality));
    out.set("chordConfidence", vectorToFloat32Array(buffer.chord_confidence));
    return out;
  }

  /// @brief Reads frames in quantized Uint8 format (4x bandwidth reduction).
  /// @param quantize_config Optional quantization ranges (undefined = defaults);
  ///        widen these for a stream louder/quieter than the default ranges.
  val readFramesU8(size_t max_frames, val quantize_config) {
    QuantizedFrameBufferU8 buffer;
    QuantizeConfig qconfig = quantizeConfigFromVal(quantize_config);
    analyzer_->read_frames_quantized_u8(max_frames, buffer, qconfig);

    val out = val::object();
    out.set("nFrames", buffer.n_frames);
    out.set("nMels", buffer.n_mels);
    out.set("timestamps", vectorToFloat32Array(buffer.timestamps));
    out.set("mel", vectorToUint8Array(buffer.mel));
    out.set("chroma", vectorToUint8Array(buffer.chroma));
    out.set("onsetStrength", vectorToUint8Array(buffer.onset_strength));
    out.set("rmsEnergy", vectorToUint8Array(buffer.rms_energy));
    out.set("spectralCentroid", vectorToUint8Array(buffer.spectral_centroid));
    out.set("spectralFlatness", vectorToUint8Array(buffer.spectral_flatness));
    return out;
  }

  /// @brief Reads frames in quantized Int16 format (2x bandwidth reduction).
  /// @param quantize_config Optional quantization ranges (undefined = defaults);
  ///        widen these for a stream louder/quieter than the default ranges.
  val readFramesI16(size_t max_frames, val quantize_config) {
    QuantizedFrameBufferI16 buffer;
    QuantizeConfig qconfig = quantizeConfigFromVal(quantize_config);
    analyzer_->read_frames_quantized_i16(max_frames, buffer, qconfig);

    val out = val::object();
    out.set("nFrames", buffer.n_frames);
    out.set("nMels", buffer.n_mels);
    out.set("timestamps", vectorToFloat32Array(buffer.timestamps));
    out.set("mel", vectorToInt16Array(buffer.mel));
    out.set("chroma", vectorToInt16Array(buffer.chroma));
    out.set("onsetStrength", vectorToInt16Array(buffer.onset_strength));
    out.set("rmsEnergy", vectorToInt16Array(buffer.rms_energy));
    out.set("spectralCentroid", vectorToInt16Array(buffer.spectral_centroid));
    out.set("spectralFlatness", vectorToInt16Array(buffer.spectral_flatness));
    return out;
  }

  void reset(size_t base_sample_offset) { analyzer_->reset(base_sample_offset); }

  val stats() {
    AnalyzerStats s = analyzer_->stats();

    val out = val::object();
    out.set("totalFrames", s.total_frames);
    out.set("totalSamples", static_cast<int>(s.total_samples));
    out.set("durationSeconds", s.duration_seconds);

    val estimate = val::object();
    estimate.set("bpm", s.estimate.bpm);
    estimate.set("bpmConfidence", s.estimate.bpm_confidence);
    estimate.set("bpmCandidateCount", s.estimate.bpm_candidate_count);
    estimate.set("key", s.estimate.key);
    estimate.set("keyMinor", s.estimate.key_minor);
    estimate.set("keyConfidence", s.estimate.key_confidence);
    estimate.set("chordRoot", s.estimate.chord_root);
    estimate.set("chordQuality", s.estimate.chord_quality);
    estimate.set("chordConfidence", s.estimate.chord_confidence);
    estimate.set("chordStartTime", s.estimate.chord_start_time);

    // Chord progression (time-based)
    val chordProgression = val::array();
    for (const auto& chord : s.estimate.chord_progression) {
      val c = val::object();
      c.set("root", chord.root);
      c.set("quality", chord.quality);
      c.set("startTime", chord.start_time);
      c.set("confidence", chord.confidence);
      chordProgression.call<void>("push", c);
    }
    estimate.set("chordProgression", chordProgression);

    // Bar-synchronized chord progression (requires stable BPM)
    val barChordProgression = val::array();
    for (const auto& chord : s.estimate.bar_chord_progression) {
      val c = val::object();
      c.set("barIndex", chord.bar_index);
      c.set("root", chord.root);
      c.set("quality", chord.quality);
      c.set("startTime", chord.start_time);
      c.set("confidence", chord.confidence);
      barChordProgression.call<void>("push", c);
    }
    estimate.set("barChordProgression", barChordProgression);
    estimate.set("currentBar", s.estimate.current_bar);
    estimate.set("barDuration", s.estimate.bar_duration);

    // Voted chord pattern (computed from repetitions)
    val votedPattern = val::array();
    for (const auto& chord : s.estimate.voted_pattern) {
      val c = val::object();
      c.set("barIndex", chord.bar_index);
      c.set("root", chord.root);
      c.set("quality", chord.quality);
      c.set("startTime", chord.start_time);
      c.set("confidence", chord.confidence);
      votedPattern.call<void>("push", c);
    }
    estimate.set("votedPattern", votedPattern);
    estimate.set("patternLength", s.estimate.pattern_length);

    // Best matching progression pattern
    estimate.set("detectedPatternName", val(s.estimate.detected_pattern_name));
    estimate.set("detectedPatternScore", val(s.estimate.detected_pattern_score));

    // All pattern scores
    val allPatternScores = val::array();
    for (const auto& ps_pair : s.estimate.all_pattern_scores) {
      val ps = val::object();
      ps.set("name", ps_pair.first);
      ps.set("score", ps_pair.second);
      allPatternScores.call<void>("push", ps);
    }
    estimate.set("allPatternScores", allPatternScores);

    estimate.set("accumulatedSeconds", s.estimate.accumulated_seconds);
    estimate.set("usedFrames", s.estimate.used_frames);
    estimate.set("updated", s.estimate.updated);
    out.set("estimate", estimate);

    return out;
  }

  int frameCount() const { return analyzer_->frame_count(); }
  float currentTime() const { return analyzer_->current_time(); }

  /// @brief Sets the expected total duration for pattern lock timing.
  void setExpectedDuration(float duration_seconds) {
    analyzer_->set_expected_duration(duration_seconds);
  }

  /// @brief Sets normalization gain for loud audio.
  void setNormalizationGain(float gain) { analyzer_->set_normalization_gain(gain); }

  /// @brief Sets tuning reference frequency (A4).
  /// @param ref_hz Reference frequency for A4 (default 440 Hz)
  /// @details Use 466.16 if audio is 1 semitone sharp, 415.30 if 1 semitone flat.
  void setTuningRefHz(float ref_hz) { analyzer_->set_tuning_ref_hz(ref_hz); }

 private:
  StreamConfig config_;
  std::unique_ptr<StreamAnalyzer> analyzer_;
};

void registerStreamAnalyzerBindings() {
  // Streaming - StreamAnalyzer
  class_<StreamAnalyzerWrapper>("StreamAnalyzer")
      .constructor<int, int, int, int, float, float, float, bool, bool, bool, bool, bool, int, int,
                   float, float, int, int>()
      .function("process", &StreamAnalyzerWrapper::process)
      .function("processWithOffset", &StreamAnalyzerWrapper::processWithOffset)
      .function("availableFrames", &StreamAnalyzerWrapper::availableFrames)
      .function("readFramesSoa", &StreamAnalyzerWrapper::readFramesSoa)
      .function("readFramesU8", &StreamAnalyzerWrapper::readFramesU8)
      .function("readFramesI16", &StreamAnalyzerWrapper::readFramesI16)
      .function("reset", &StreamAnalyzerWrapper::reset)
      .function("stats", &StreamAnalyzerWrapper::stats)
      .function("frameCount", &StreamAnalyzerWrapper::frameCount)
      .function("currentTime", &StreamAnalyzerWrapper::currentTime)
      .function("sampleRate", &StreamAnalyzerWrapper::sampleRate)
      .function("setExpectedDuration", &StreamAnalyzerWrapper::setExpectedDuration)
      .function("setNormalizationGain", &StreamAnalyzerWrapper::setNormalizationGain)
      .function("setTuningRefHz", &StreamAnalyzerWrapper::setTuningRefHz);
}

#endif  // __EMSCRIPTEN__
