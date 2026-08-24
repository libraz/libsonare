/// @file realtime_voice_changer.cpp
/// @brief Embind bindings for realtime voice changer streaming APIs.

#ifdef __EMSCRIPTEN__

#include "wasm/bindings/common/common.h"

std::string realtimeVoiceChangerConfigTextFromVal(val config) {
  if (config.isNull() || config.isUndefined()) return "neutral-monitor";
  if (config.typeOf().as<std::string>() == "string") return config.as<std::string>();
  return val::global("JSON").call<std::string>("stringify", config);
}

// Flat POD transport used by the AudioWorklet control plane.  Unlike the
// general config path, this neither stringifies a JS object nor invokes the
// nested JSON parser on the audio rendering thread.
#define SONARE_WASM_VC_POD_FIELDS(X)                   \
  X(input_gain_db, inputGainDb)                        \
  X(output_gain_db, outputGainDb)                      \
  X(wet_mix, wetMix)                                   \
  X(retune.semitones, retuneSemitones)                 \
  X(retune.mix, retuneMix)                             \
  X(retune.grain_size, retuneGrainSize)                \
  X(formant.factor, formantFactor)                     \
  X(formant.amount, formantAmount)                     \
  X(formant.body, formantBody)                         \
  X(formant.brightness, formantBrightness)             \
  X(formant.nasal, formantNasal)                       \
  X(eq.highpass_hz, eqHighpassHz)                      \
  X(eq.body_db, eqBodyDb)                              \
  X(eq.presence_db, eqPresenceDb)                      \
  X(eq.air_db, eqAirDb)                                \
  X(gate.threshold_db, gateThresholdDb)                \
  X(gate.attack_ms, gateAttackMs)                      \
  X(gate.release_ms, gateReleaseMs)                    \
  X(gate.range_db, gateRangeDb)                        \
  X(compressor.threshold_db, compressorThresholdDb)    \
  X(compressor.ratio, compressorRatio)                 \
  X(compressor.attack_ms, compressorAttackMs)          \
  X(compressor.release_ms, compressorReleaseMs)        \
  X(compressor.makeup_gain_db, compressorMakeupGainDb) \
  X(deesser.frequency_hz, deesserFrequencyHz)          \
  X(deesser.threshold_db, deesserThresholdDb)          \
  X(deesser.ratio, deesserRatio)                       \
  X(deesser.range_db, deesserRangeDb)                  \
  X(reverb.mix, reverbMix)                             \
  X(reverb.time_ms, reverbTimeMs)                      \
  X(reverb.damping, reverbDamping)                     \
  X(reverb.seed, reverbSeed)                           \
  X(limiter.ceiling_db, limiterCeilingDb)              \
  X(limiter.release_ms, limiterReleaseMs)              \
  X(limiter.isp_ceiling_dbtp, limiterIspCeilingDbtp)

editing::voice_changer::RealtimeVoiceChangerConfig realtimeVoiceChangerConfigFromPodVal(val pod) {
  if (pod.isNull() || pod.isUndefined() || pod.typeOf().as<std::string>() != "object") {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "voice changer POD config must be an object");
  }
  editing::voice_changer::RealtimeVoiceChangerConfig parsed;
  static constexpr char kSubject[] = "voice changer POD config";
  // Every field is REQUIRED: unlike the general JSON path (which normalizes a
  // partial preset against defaults), the flat POD transport used by the
  // AudioWorklet control plane must reject a partial object rather than
  // silently zero-fill it. A missing limiterEnableIspLimiter used to read as
  // JS `false` here, which can turn the ISP limiter off and let the DAC clip.
#define X(cpp_path, js_key) \
  parsed.cpp_path = requireProperty<decltype(parsed.cpp_path)>(pod, #js_key, kSubject);
  SONARE_WASM_VC_POD_FIELDS(X)
#undef X
  parsed.limiter.enable_isp_limiter =
      requireProperty<bool>(pod, "limiterEnableIspLimiter", kSubject);
  return parsed;
}

editing::voice_changer::RealtimeVoiceChangerConfig realtimeVoiceChangerConfigFromVal(val config) {
  editing::voice_changer::RealtimeVoiceChangerConfig parsed;
  std::string error;
  if (!editing::voice_changer::realtime_voice_changer_config_from_input(
          realtimeVoiceChangerConfigTextFromVal(config), &parsed, &error)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, error);
  }
  return parsed;
}

class RealtimeVoiceChangerWrapper {
 public:
  explicit RealtimeVoiceChangerWrapper(val config)
      : changer_(realtimeVoiceChangerConfigFromVal(config)) {}

  void prepare(double sample_rate, int max_block_size, int channels) {
    changer_.prepare(sample_rate, max_block_size, channels);
    // Pre-warm the per-instance scratch buffers so the first process* call
    // does not trigger an allocation. The `ensure_*_capacity` helpers only
    // grow; once warmed up to (channels, max_block_size) they stay that size.
    ensure_mono_capacity(static_cast<size_t>(max_block_size));
    ensure_interleaved_capacity(static_cast<size_t>(max_block_size), channels);
    max_block_size_ = max_block_size;
    prepared_channels_ = channels;
    prepared_ = true;
    ++buffer_generation_;
  }

  void reset() { changer_.reset(); }

  void setConfig(val config) { changer_.set_config(realtimeVoiceChangerConfigFromVal(config)); }

  void setPodConfig(val config) {
    changer_.set_config(realtimeVoiceChangerConfigFromPodVal(config));
  }

  std::string configJson() const {
    return editing::voice_changer::realtime_voice_changer_config_to_json(changer_.config());
  }

  int latencySamples() const { return changer_.latency_samples(); }
  uint32_t bufferGeneration() const { return buffer_generation_; }

  // Element-wise legacy path. NOT RT-safe for high block rates; AudioWorklet
  // consumers should prefer the prepared API below (getMonoInputBuffer /
  // processPreparedMono / getMonoOutputBuffer) which avoids per-sample JS↔C++
  // crossings and per-call allocations entirely.
  val processMono(val samples) {
    require_prepared();
    const int length = samples["length"].as<int>();
    require_block_within_max(length);
    ensure_mono_capacity(static_cast<size_t>(length));
    copyFloat32Array(samples, mono_input_.data(), static_cast<size_t>(length));
    changer_.process_block(mono_input_.data(), mono_output_.data(), length);
    val output = val::global("Float32Array").new_(length);
    val view = val(typed_memory_view(static_cast<size_t>(length), mono_output_.data()));
    output.call<void>("set", view);
    return output;
  }

  void processMonoInto(val samples, val output) {
    require_prepared();
    const int length = samples["length"].as<int>();
    require_block_within_max(length);
    if (output["length"].as<int>() < length) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "output buffer is too small");
    }
    ensure_mono_capacity(static_cast<size_t>(length));
    copyFloat32Array(samples, mono_input_.data(), static_cast<size_t>(length));
    changer_.process_block(mono_input_.data(), mono_output_.data(), length);
    val view = val(typed_memory_view(static_cast<size_t>(length), mono_output_.data()));
    output.call<void>("set", view);
  }

  val processInterleaved(val samples, int channels) {
    require_prepared();
    const int length = samples["length"].as<int>();
    val output = val::global("Float32Array").new_(length);
    processInterleavedInto(samples, channels, output);
    return output;
  }

  void processInterleavedInto(val samples, int channels, val output) {
    require_prepared();
    const int length = samples["length"].as<int>();
    if (channels <= 0 || length % channels != 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "invalid interleaved channel count");
    }
    require_prepared_channels(channels);
    if (output["length"].as<int>() < length) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "output buffer is too small");
    }
    const size_t frames = static_cast<size_t>(length / channels);
    require_block_within_max(static_cast<int>(frames));
    ensure_interleaved_capacity(frames, channels);
    copyFloat32Array(samples, interleaved_input_.data(), static_cast<size_t>(length));
    for (int ch = 0; ch < channels; ++ch) {
      for (size_t i = 0; i < frames; ++i) {
        planar_[static_cast<size_t>(ch)][i] =
            interleaved_input_[i * static_cast<size_t>(channels) + static_cast<size_t>(ch)];
      }
    }
    changer_.process_block(channel_ptrs_.data(), channels, static_cast<int>(frames));
    for (size_t i = 0; i < frames; ++i) {
      for (int ch = 0; ch < channels; ++ch) {
        interleaved_output_[i * static_cast<size_t>(channels) + static_cast<size_t>(ch)] =
            planar_[static_cast<size_t>(ch)][i];
      }
    }
    output.call<void>(
        "set", val(typed_memory_view(static_cast<size_t>(length), interleaved_output_.data())));
  }

  // ---- Zero-copy "prepared" API ----------------------------------------
  // Caller fills the input view (returned as a typed_memory_view onto the
  // WASM heap), calls processPrepared*, then reads the output view. No JS↔C++
  // sample-level crossings and no allocations on the audio thread.

  val getMonoInputBuffer(int num_samples) {
    require_prepared();
    if (num_samples <= 0 || num_samples > max_block_size_) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeVoiceChanger.getMonoInputBuffer: out-of-range length");
    }
    ensure_mono_capacity(static_cast<size_t>(num_samples));
    return val(typed_memory_view(static_cast<size_t>(num_samples), mono_input_.data()));
  }

  val getMonoOutputBuffer(int num_samples) {
    require_prepared();
    if (num_samples <= 0 || num_samples > max_block_size_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.getMonoOutputBuffer: out-of-range length");
    }
    ensure_mono_capacity(static_cast<size_t>(num_samples));
    return val(typed_memory_view(static_cast<size_t>(num_samples), mono_output_.data()));
  }

  void processPreparedMono(int num_samples) {
    require_prepared();
    if (num_samples <= 0 || num_samples > max_block_size_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.processPreparedMono: out-of-range length");
    }
    if (mono_input_.size() < static_cast<size_t>(num_samples) ||
        mono_output_.size() < static_cast<size_t>(num_samples)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeVoiceChanger.processPreparedMono: getMonoInputBuffer/"
                                    "getMonoOutputBuffer must be called first");
    }
    changer_.process_block(mono_input_.data(), mono_output_.data(), num_samples);
  }

  val getInterleavedInputBuffer(int num_frames, int num_channels) {
    require_prepared();
    if (num_frames <= 0 || num_channels <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeVoiceChanger.getInterleavedInputBuffer: bad dims");
    }
    if (num_frames > max_block_size_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.getInterleavedInputBuffer: frames exceed max block size");
    }
    require_prepared_channels(num_channels);
    ensure_interleaved_capacity(static_cast<size_t>(num_frames), num_channels);
    const size_t length = static_cast<size_t>(num_frames) * static_cast<size_t>(num_channels);
    return val(typed_memory_view(length, interleaved_input_.data()));
  }

  val getInterleavedOutputBuffer(int num_frames, int num_channels) {
    require_prepared();
    if (num_frames <= 0 || num_channels <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeVoiceChanger.getInterleavedOutputBuffer: bad dims");
    }
    if (num_frames > max_block_size_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.getInterleavedOutputBuffer: frames exceed max block size");
    }
    require_prepared_channels(num_channels);
    ensure_interleaved_capacity(static_cast<size_t>(num_frames), num_channels);
    const size_t length = static_cast<size_t>(num_frames) * static_cast<size_t>(num_channels);
    return val(typed_memory_view(length, interleaved_output_.data()));
  }

  void processPreparedInterleaved(int num_frames, int num_channels) {
    require_prepared();
    if (num_frames <= 0 || num_channels <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeVoiceChanger.processPreparedInterleaved: bad dims");
    }
    if (num_frames > max_block_size_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.processPreparedInterleaved: frames exceed max block size");
    }
    require_prepared_channels(num_channels);
    const size_t frames = static_cast<size_t>(num_frames);
    const size_t channel_count = static_cast<size_t>(num_channels);
    const size_t length = frames * channel_count;
    if (interleaved_input_.size() < length || interleaved_output_.size() < length ||
        planar_.size() < channel_count) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.processPreparedInterleaved: getInterleavedInputBuffer/"
          "getInterleavedOutputBuffer must be called first with matching dims");
    }
    for (size_t ch = 0; ch < channel_count; ++ch) {
      float* dst = planar_[ch].data();
      const float* src = interleaved_input_.data() + ch;
      for (size_t i = 0; i < frames; ++i) {
        dst[i] = src[i * channel_count];
      }
    }
    changer_.process_block(channel_ptrs_.data(), num_channels, num_frames);
    for (size_t ch = 0; ch < channel_count; ++ch) {
      const float* src = planar_[ch].data();
      float* dst = interleaved_output_.data() + ch;
      for (size_t i = 0; i < frames; ++i) {
        dst[i * channel_count] = src[i];
      }
    }
  }

  // ---- Planar zero-copy stereo path -----------------------------------
  // Match AudioWorklet's native planar layout: each channel is its own
  // Float32Array, so the worklet can hand the in/out buffers straight
  // through with no interleave/deinterleave passes.

  val getPlanarChannelBuffer(int channel, int num_frames) {
    require_prepared();
    if (num_frames <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeVoiceChanger.getPlanarChannelBuffer: bad frames");
    }
    if (num_frames > max_block_size_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.getPlanarChannelBuffer: frames exceed max block size");
    }
    if (channel < 0 || channel >= prepared_channels_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.getPlanarChannelBuffer: channel out of range");
    }
    ensure_interleaved_capacity(static_cast<size_t>(num_frames), prepared_channels_);
    return val(typed_memory_view(static_cast<size_t>(num_frames),
                                 planar_[static_cast<size_t>(channel)].data()));
  }

  void processPreparedPlanar(int num_frames) {
    require_prepared();
    if (num_frames <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeVoiceChanger.processPreparedPlanar: bad frames");
    }
    if (num_frames > max_block_size_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.processPreparedPlanar: frames exceed max block size");
    }
    const size_t channel_count = static_cast<size_t>(prepared_channels_);
    if (planar_.size() < channel_count) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.processPreparedPlanar: getPlanarChannelBuffer must be called for "
          "each channel before processing");
    }
    for (size_t ch = 0; ch < channel_count; ++ch) {
      if (planar_[ch].size() < static_cast<size_t>(num_frames)) {
        throw sonare::SonareException(
            sonare::ErrorCode::InvalidParameter,
            "RealtimeVoiceChanger.processPreparedPlanar: planar buffer too small for requested "
            "frames");
      }
    }
    changer_.process_block(channel_ptrs_.data(), prepared_channels_, num_frames);
  }

 private:
  static void copyFloat32Array(val source, float* destination, size_t length) {
    val(typed_memory_view(length, destination)).call<void>("set", source);
  }

  void ensure_mono_capacity(size_t samples) {
    if (mono_input_.size() < samples) {
      mono_input_.resize(samples);
      mono_output_.resize(samples);
    }
  }

  void ensure_interleaved_capacity(size_t frames, int channels) {
    const size_t channel_count = static_cast<size_t>(channels);
    if (planar_.size() < channel_count) planar_.resize(channel_count);
    if (channel_ptrs_.size() < channel_count) channel_ptrs_.resize(channel_count, nullptr);
    for (size_t ch = 0; ch < channel_count; ++ch) {
      if (planar_[ch].size() < frames) planar_[ch].resize(frames);
      channel_ptrs_[ch] = planar_[ch].data();
    }
    const size_t length = frames * channel_count;
    if (interleaved_input_.size() < length) interleaved_input_.resize(length);
    if (interleaved_output_.size() < length) interleaved_output_.resize(length);
  }

  void require_prepared() const {
    if (!prepared_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger.prepare() must be called before processing");
    }
  }

  /// Reject a per-call block that exceeds the prepared max_block_size. The core
  /// process_block early-returns (emitting stale/garbage scratch) for an
  /// oversized block; the prepared API already guards this, so the legacy
  /// element-wise paths must too — mirroring the C-ABI behaviour of throwing.
  void require_block_within_max(int block) const {
    if (block > max_block_size_) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "RealtimeVoiceChanger: block size exceeds the prepared "
                                    "max block size");
    }
  }

  /// Accept any channel count in [1, prepared_channels_], matching the C-ABI
  /// oracle (sonare_realtime_voice_changer_process_interleaved rejects only
  /// num_channels < 1 || num_channels > handle->num_channels) and the Node and
  /// Python surfaces. The changer is prepared for prepared_channels_, and the
  /// grow-only scratch is already sized for that maximum, so processing a
  /// narrower layout (e.g. a mono block on a stereo-prepared instance) is safe.
  /// A count above the prepared maximum is rejected: it would exceed the
  /// allocated planar state the changer was configured for.
  void require_prepared_channels(int channels) const {
    if (channels < 1 || channels > prepared_channels_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger: channel count must be between 1 and the prepared layout");
    }
  }

  editing::voice_changer::RealtimeVoiceChanger changer_;
  std::vector<float> mono_input_;
  std::vector<float> mono_output_;
  std::vector<std::vector<float>> planar_;
  std::vector<float*> channel_ptrs_;
  std::vector<float> interleaved_input_;
  std::vector<float> interleaved_output_;
  int max_block_size_ = 0;
  int prepared_channels_ = 0;
  uint32_t buffer_generation_ = 0;
  bool prepared_ = false;
};

RealtimeVoiceChangerWrapper* createRealtimeVoiceChanger(val config) {
  return new RealtimeVoiceChangerWrapper(config);
}

val realtimeVoiceChangerPresetNames() {
  return stringVectorToVal(editing::voice_changer::realtime_voice_changer_preset_names());
}

std::string realtimeVoiceChangerPresetJson(const std::string& id) {
  return editing::voice_changer::realtime_voice_changer_preset_json(
      editing::voice_changer::realtime_voice_changer_preset_from_id(id));
}

val validateRealtimeVoiceChangerPresetJson(const std::string& json) {
  // Full schema-level validation (schemaVersion, id/name string limits,
  // unknown-key rejection, every value range) — must match the C/Node/
  // Python contract. Earlier this only did a from_json→to_json roundtrip,
  // which silently accepted incomplete presets.
  val out = val::object();
  try {
    std::string normalized;
    std::string error;
    if (editing::voice_changer::validate_realtime_voice_changer_preset_json(json, &normalized,
                                                                            &error)) {
      out.set("ok", true);
      out.set("normalizedJson", normalized);
    } else {
      out.set("ok", false);
      out.set("error", error.empty() ? std::string("invalid preset JSON") : error);
    }
  } catch (const std::exception& ex) {
    out.set("ok", false);
    out.set("error", std::string(ex.what()));
  }
  return out;
}

void registerRealtimeVoiceChangerStreamingBindings() {
  class_<RealtimeVoiceChangerWrapper>("RealtimeVoiceChanger")
      .function("prepare", &RealtimeVoiceChangerWrapper::prepare)
      .function("reset", &RealtimeVoiceChangerWrapper::reset)
      .function("setConfig", &RealtimeVoiceChangerWrapper::setConfig)
      .function("setPodConfig", &RealtimeVoiceChangerWrapper::setPodConfig)
      .function("configJson", &RealtimeVoiceChangerWrapper::configJson)
      .function("latencySamples", &RealtimeVoiceChangerWrapper::latencySamples)
      .function("bufferGeneration", &RealtimeVoiceChangerWrapper::bufferGeneration)
      .function("processMono", &RealtimeVoiceChangerWrapper::processMono)
      .function("processMonoInto", &RealtimeVoiceChangerWrapper::processMonoInto)
      .function("processInterleaved", &RealtimeVoiceChangerWrapper::processInterleaved)
      .function("processInterleavedInto", &RealtimeVoiceChangerWrapper::processInterleavedInto)
      .function("getMonoInputBuffer", &RealtimeVoiceChangerWrapper::getMonoInputBuffer)
      .function("getMonoOutputBuffer", &RealtimeVoiceChangerWrapper::getMonoOutputBuffer)
      .function("processPreparedMono", &RealtimeVoiceChangerWrapper::processPreparedMono)
      .function("getInterleavedInputBuffer",
                &RealtimeVoiceChangerWrapper::getInterleavedInputBuffer)
      .function("getInterleavedOutputBuffer",
                &RealtimeVoiceChangerWrapper::getInterleavedOutputBuffer)
      .function("processPreparedInterleaved",
                &RealtimeVoiceChangerWrapper::processPreparedInterleaved)
      .function("getPlanarChannelBuffer", &RealtimeVoiceChangerWrapper::getPlanarChannelBuffer)
      .function("processPreparedPlanar", &RealtimeVoiceChangerWrapper::processPreparedPlanar);
  function("createRealtimeVoiceChanger", &createRealtimeVoiceChanger, allow_raw_pointers());
  function("realtimeVoiceChangerPresetNames", &realtimeVoiceChangerPresetNames);
  function("realtimeVoiceChangerPresetJson", &realtimeVoiceChangerPresetJson);
  function("validateRealtimeVoiceChangerPresetJson", &validateRealtimeVoiceChangerPresetJson);
}

#endif  // __EMSCRIPTEN__
