/// @file stream_analyzer.cpp
/// @brief Embind bindings for streaming analysis.

#ifdef __EMSCRIPTEN__

#include "analysis/chord_analyzer.h"
#include "analysis/key_analyzer.h"
#include "util/numeric_validation.h"
#include "wasm/bindings/common/common.h"

// ============================================================================
// Streaming - StreamAnalyzer
// ============================================================================

// vectorToUint8Array lives in common.cpp with the other vectorTo*Array
// helpers (declared in common.h); the quantized readFramesU8 path below uses
// the same single-bulk-memcpy idiom.

/// @brief Helper to convert int16 vector to Int16Array.
val vectorToInt16Array(const std::vector<int16_t>& vec) {
  const size_t n = vec.size();
  val result = val::global("Int16Array").new_(n);
  if (n == 0) return result;
  val view = val(typed_memory_view(n, vec.data()));
  result.call<void>("set", view);
  return result;
}

/// @brief Helper to convert int8 vector to Int8Array.
val vectorToInt8Array(const std::vector<int8_t>& vec) {
  const size_t n = vec.size();
  val result = val::global("Int8Array").new_(n);
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
    field = floatProperty(config, key, field);
  };
  read("melDbMin", qconfig.mel_db_min);
  read("melDbMax", qconfig.mel_db_max);
  read("onsetMax", qconfig.onset_max);
  read("rmsMax", qconfig.rms_max);
  read("centroidMax", qconfig.centroid_max);
  return qconfig;
}

/// @brief Builds a chord entry; barIndex, when given, is set first.
val chordEntryToVal(int root, int quality, float start_time, float confidence,
                    const int* bar_index = nullptr) {
  val c = val::object();
  if (bar_index != nullptr) {
    c.set("barIndex", *bar_index);
  }
  c.set("root", root);
  c.set("quality", quality);
  c.set("startSec", start_time);
  c.set("confidence", confidence);
  c.set("name", chord_symbol(root, quality));
  return c;
}

/// @brief Whether a constructor field was given; an absent one keeps the StreamConfig default.
bool given(const val& value) { return !value.isUndefined() && !value.isNull(); }

/// @brief JavaScript wrapper for StreamAnalyzer.
class StreamAnalyzerWrapper {
 public:
  StreamAnalyzerWrapper(const val& sample_rate_val, const val& n_fft_val, const val& hop_length_val,
                        const val& n_mels_val, const val& fmin_val, const val& fmax_val,
                        const val& tuning_ref_hz_val, const val& compute_mel_val,
                        const val& compute_chroma_val, const val& compute_onset_val,
                        const val& compute_spectral_val, const val& emit_every_n_frames_val,
                        const val& max_pending_frames_val, const val& max_progression_entries_val,
                        const val& key_update_interval_sec_val,
                        const val& bpm_update_interval_sec_val, const val& window_val) {
    // Every field but the rate may be absent, which keeps the StreamConfig default. The narrow
    // integers arrive as val because embind's integer glue wraps, so a request past 2^32 would
    // reach the config as a small legal one.
    StreamConfig config;
    config.sample_rate =
        domainNumberFromVal<int>(sample_rate_val, "sampleRate", kAudioSampleRateDomain);
    if (given(n_fft_val)) config.n_fft = checkedIntFromVal(n_fft_val, "nFft");
    if (given(hop_length_val)) config.hop_length = checkedIntFromVal(hop_length_val, "hopLength");
    if (given(n_mels_val)) config.n_mels = checkedIntFromVal(n_mels_val, "nMels");
    if (given(fmin_val)) config.fmin = checkedFloatFromVal(fmin_val, "fmin");
    if (given(fmax_val)) config.fmax = checkedFloatFromVal(fmax_val, "fmax");
    if (given(tuning_ref_hz_val)) {
      config.tuning_ref_hz = checkedFloatFromVal(tuning_ref_hz_val, "referenceHz");
    }
    if (given(compute_mel_val)) config.compute_mel = *optionalBool(compute_mel_val, "computeMel");
    if (given(compute_chroma_val)) {
      config.compute_chroma = *optionalBool(compute_chroma_val, "computeChroma");
    }
    if (given(compute_onset_val)) {
      config.compute_onset = *optionalBool(compute_onset_val, "computeOnset");
    }
    if (given(compute_spectral_val)) {
      config.compute_spectral = *optionalBool(compute_spectral_val, "computeSpectral");
    }
    if (given(emit_every_n_frames_val)) {
      config.emit_every_n_frames = checkedIntFromVal(emit_every_n_frames_val, "emitEveryNFrames");
    }
    if (given(max_pending_frames_val)) {
      config.max_pending_frames = domainNumberFromVal<std::size_t>(
          max_pending_frames_val, "maxPendingFrames", kStreamConfigMaxPendingFramesDomain);
    }
    if (given(max_progression_entries_val)) {
      config.max_progression_entries =
          domainNumberFromVal<std::size_t>(max_progression_entries_val, "maxProgressionEntries",
                                           kStreamConfigMaxProgressionEntriesDomain);
    }
    if (given(key_update_interval_sec_val)) {
      config.key_update_interval_sec =
          checkedFloatFromVal(key_update_interval_sec_val, "keyUpdateIntervalSec");
    }
    if (given(bpm_update_interval_sec_val)) {
      config.bpm_update_interval_sec =
          checkedFloatFromVal(bpm_update_interval_sec_val, "bpmUpdateIntervalSec");
    }
    if (given(window_val)) {
      // Rejects an out-of-range window ordinal, mirroring the C ABI's valid_window.
      const int window = checkedIntFromVal(window_val, "window");
      requireOrdinalInRange(window, SONARE_WINDOW_HANN, SONARE_WINDOW_RECTANGULAR, "window");
      config.window = window == 1   ? WindowType::Hamming
                      : window == 2 ? WindowType::Blackman
                      : window == 3 ? WindowType::Rectangular
                                    : WindowType::Hann;
    }
    config.compute_magnitude = false;
    // The shared facade check rather than a message of this file's own, so the four surfaces
    // answer an unsupported config identically.
    validate_soa_stream_config(config);
    config.output_format = OutputFormat::Float32;
    config_ = config;
    analyzer_ = std::make_unique<StreamAnalyzer>(config);
  }

  /// @brief Returns the sample rate.
  int sampleRate() const { return config_.sample_rate; }

  /// @param sample_offset Cumulative sample count at the start of the chunk; undefined/null keeps
  ///        the analyzer's own tracking.
  void process(val samples, const val& sample_offset_val) {
    if (!given(sample_offset_val)) {
      std::vector<float> data = float32ArrayToVector(samples, "samples");
      analyzer_->process(data.data(), data.size());
      return;
    }
    // wasmIndexArg rather than wasmCountArg: this names a position, so a request
    // past the addressable range is reported instead of saturating onto the last
    // address and corrupting the initial timestamp base.
    const size_t offset =
        wasmIndexArg(numberFromVal(sample_offset_val, "startSample"), "startSample");
    std::vector<float> data = float32ArrayToVector(samples, "samples");
    analyzer_->process(data.data(), data.size(), offset);
  }

  void finalize() { analyzer_->finalize(); }

  size_t availableFrames() const { return analyzer_->available_frames(); }

  /// @brief Reads frames in Float32 SOA format.
  val readFramesSoa(const val& max_frames_val) {
    const auto max_frames =
        static_cast<std::size_t>(checkedUintFromVal(max_frames_val, "maxFrames"));
    FrameBuffer buffer;
    analyzer_->read_frames_soa(max_frames, buffer);

    val out = val::object();
    out.set("nFrames", buffer.n_frames);
    out.set("nMels", buffer.n_mels);
    out.set("nChroma", buffer.n_chroma);
    out.set("featureFlags", buffer.feature_flags);
    out.set("timestampsSec", vectorToFloat32Array(buffer.timestamps));
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
  val readFramesU8(const val& max_frames_val, val quantize_config) {
    const auto max_frames =
        static_cast<std::size_t>(checkedUintFromVal(max_frames_val, "maxFrames"));
    QuantizedFrameBufferU8 buffer;
    QuantizeConfig qconfig = quantizeConfigFromVal(quantize_config);
    analyzer_->read_frames_quantized_u8(max_frames, buffer, qconfig);

    val out = val::object();
    out.set("nFrames", buffer.n_frames);
    out.set("nMels", buffer.n_mels);
    out.set("nChroma", buffer.n_chroma);
    out.set("featureFlags", buffer.feature_flags);
    out.set("timestampsSec", vectorToFloat32Array(buffer.timestamps));
    out.set("mel", vectorToUint8Array(buffer.mel));
    out.set("chroma", vectorToUint8Array(buffer.chroma));
    out.set("onsetStrength", vectorToUint8Array(buffer.onset_strength));
    out.set("rmsEnergy", vectorToUint8Array(buffer.rms_energy));
    out.set("spectralCentroid", vectorToUint8Array(buffer.spectral_centroid));
    out.set("spectralFlatness", vectorToUint8Array(buffer.spectral_flatness));
    out.set("chordRoot", vectorToInt8Array(buffer.chord_root));
    out.set("chordQuality", vectorToInt8Array(buffer.chord_quality));
    out.set("chordConfidence", vectorToUint8Array(buffer.chord_confidence));
    return out;
  }

  /// @brief Reads frames in quantized Int16 format (2x bandwidth reduction).
  /// @param quantize_config Optional quantization ranges (undefined = defaults);
  ///        widen these for a stream louder/quieter than the default ranges.
  val readFramesI16(const val& max_frames_val, val quantize_config) {
    const auto max_frames =
        static_cast<std::size_t>(checkedUintFromVal(max_frames_val, "maxFrames"));
    QuantizedFrameBufferI16 buffer;
    QuantizeConfig qconfig = quantizeConfigFromVal(quantize_config);
    analyzer_->read_frames_quantized_i16(max_frames, buffer, qconfig);

    val out = val::object();
    out.set("nFrames", buffer.n_frames);
    out.set("nMels", buffer.n_mels);
    out.set("nChroma", buffer.n_chroma);
    out.set("featureFlags", buffer.feature_flags);
    out.set("timestampsSec", vectorToFloat32Array(buffer.timestamps));
    out.set("mel", vectorToInt16Array(buffer.mel));
    out.set("chroma", vectorToInt16Array(buffer.chroma));
    out.set("onsetStrength", vectorToInt16Array(buffer.onset_strength));
    out.set("rmsEnergy", vectorToInt16Array(buffer.rms_energy));
    out.set("spectralCentroid", vectorToInt16Array(buffer.spectral_centroid));
    out.set("spectralFlatness", vectorToInt16Array(buffer.spectral_flatness));
    out.set("chordRoot", vectorToInt8Array(buffer.chord_root));
    out.set("chordQuality", vectorToInt8Array(buffer.chord_quality));
    out.set("chordConfidence", vectorToInt16Array(buffer.chord_confidence));
    return out;
  }

  void reset(const val& base_sample_offset) {
    analyzer_->reset(
        static_cast<std::size_t>(checkedUintFromVal(base_sample_offset, "startSample")));
  }

  val stats() {
    AnalyzerStats s = analyzer_->stats();

    val out = val::object();
    out.set("totalFrames", s.total_frames);
    // A double exactly represents every size_t value that occurs here, and JS
    // numbers are doubles; static_cast<int> would truncate/wrap streams past
    // 2^31 samples (~13.5h @44.1kHz) or a reset() with a large base offset.
    out.set("totalSamples", static_cast<double>(s.total_samples));
    out.set("durationSec", s.duration_seconds);
    out.set("pendingFrames", s.pending_frames);
    out.set("droppedOutputFrames", s.dropped_output_frames);
    out.set("droppedChordProgressionEntries", s.dropped_chord_progression_entries);
    out.set("droppedBarProgressionEntries", s.dropped_bar_progression_entries);
    out.set("nonFiniteDiscardBlocks", s.non_finite_discard_blocks);

    val estimate = val::object();
    estimate.set("bpm", s.estimate.bpm);
    estimate.set("bpmConfidence", s.estimate.bpm_confidence);
    estimate.set("bpmCandidateCount", s.estimate.bpm_candidate_count);
    estimate.set("key", s.estimate.key);
    estimate.set("keyMinor", s.estimate.key_minor);
    estimate.set("keyConfidence", s.estimate.key_confidence);
    if (s.estimate.key >= 0) {
      estimate.set("keyName", key_name(s.estimate.key, s.estimate.key_minor, false));
      estimate.set("keyShortName", key_name(s.estimate.key, s.estimate.key_minor, true));
    } else {
      estimate.set("keyName", val::null());
      estimate.set("keyShortName", val::null());
    }
    estimate.set("chordName", chord_symbol(s.estimate.chord_root, s.estimate.chord_quality));
    estimate.set("chordRoot", s.estimate.chord_root);
    estimate.set("chordQuality", s.estimate.chord_quality);
    estimate.set("chordConfidence", s.estimate.chord_confidence);
    estimate.set("chordStartSec", s.estimate.chord_start_time);

    // Chord progression (time-based)
    val chordProgression = val::array();
    for (const auto& chord : s.estimate.chord_progression) {
      val c = chordEntryToVal(chord.root, chord.quality, chord.start_time, chord.confidence);
      chordProgression.call<void>("push", c);
    }
    estimate.set("chordProgression", chordProgression);

    // Bar-synchronized chord progression (requires stable BPM)
    val barChordProgression = val::array();
    for (const auto& chord : s.estimate.bar_chord_progression) {
      val c = chordEntryToVal(chord.root, chord.quality, chord.start_time, chord.confidence,
                              &chord.bar_index);
      barChordProgression.call<void>("push", c);
    }
    estimate.set("barChordProgression", barChordProgression);
    estimate.set("currentBar", s.estimate.current_bar);
    estimate.set("barDurationSec", s.estimate.bar_duration);

    // Voted chord pattern (computed from repetitions)
    val votedPattern = val::array();
    for (const auto& chord : s.estimate.voted_pattern) {
      val c = chordEntryToVal(chord.root, chord.quality, chord.start_time, chord.confidence,
                              &chord.bar_index);
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

    estimate.set("accumulatedSec", s.estimate.accumulated_seconds);
    estimate.set("usedFrames", s.estimate.used_frames);
    estimate.set("updated", s.estimate.updated);
    out.set("estimate", estimate);

    return out;
  }

  int frameCount() const { return analyzer_->frame_count(); }
  float currentTimeSec() const { return analyzer_->current_time(); }

  /// @brief Sets the expected total duration for pattern lock timing.
  void setExpectedDuration(const val& duration_sec_val) {
    const float duration_sec = checkedFloatFromVal(duration_sec_val, "durationSec");
    analyzer_->set_expected_duration(duration_sec);
  }

  /// @brief Sets normalization gain for loud audio.
  void setNormalizationGain(const val& gain_val) {
    const float gain = checkedFloatFromVal(gain_val, "gain");
    analyzer_->set_normalization_gain(gain);
  }

  /// @brief Sets tuning reference frequency (A4).
  /// @param ref_hz_val Reference frequency for A4 (default 440 Hz)
  /// @details Use 466.16 if audio is 1 semitone sharp, 415.30 if 1 semitone flat.
  void setReferenceHz(const val& ref_hz_val) {
    const float ref_hz = checkedFloatFromVal(ref_hz_val, "referenceHz");
    analyzer_->set_tuning_ref_hz(ref_hz);
  }

 private:
  StreamConfig config_;
  std::unique_ptr<StreamAnalyzer> analyzer_;
};

void registerStreamAnalyzerBindings() {
  // Streaming - StreamAnalyzer
  class_<StreamAnalyzerWrapper>("StreamAnalyzer")
      .constructor<const val&, const val&, const val&, const val&, const val&, const val&,
                   const val&, const val&, const val&, const val&, const val&, const val&,
                   const val&, const val&, const val&, const val&, const val&>()
      .function("process", &StreamAnalyzerWrapper::process)
      .function("finalize", &StreamAnalyzerWrapper::finalize)
      .function("availableFrames", &StreamAnalyzerWrapper::availableFrames)
      .function("readFramesSoa", &StreamAnalyzerWrapper::readFramesSoa)
      .function("readFramesU8", &StreamAnalyzerWrapper::readFramesU8)
      .function("readFramesI16", &StreamAnalyzerWrapper::readFramesI16)
      .function("reset", &StreamAnalyzerWrapper::reset)
      .function("stats", &StreamAnalyzerWrapper::stats)
      .function("frameCount", &StreamAnalyzerWrapper::frameCount)
      .function("currentTimeSec", &StreamAnalyzerWrapper::currentTimeSec)
      .function("sampleRate", &StreamAnalyzerWrapper::sampleRate)
      .function("setExpectedDuration", &StreamAnalyzerWrapper::setExpectedDuration)
      .function("setNormalizationGain", &StreamAnalyzerWrapper::setNormalizationGain)
      .function("setReferenceHz", &StreamAnalyzerWrapper::setReferenceHz);
}

#endif  // __EMSCRIPTEN__
