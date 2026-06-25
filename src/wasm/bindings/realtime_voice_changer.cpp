/// @file realtime_voice_changer.cpp
/// @brief Embind bindings for realtime voice changer streaming APIs.

#ifdef __EMSCRIPTEN__

#include "common.h"

std::string realtimeVoiceChangerConfigTextFromVal(val config) {
  if (config.isNull() || config.isUndefined()) return "neutral-monitor";
  if (config.typeOf().as<std::string>() == "string") return config.as<std::string>();
  return val::global("JSON").call<std::string>("stringify", config);
}

class RealtimeVoiceChangerWrapper {
 public:
  explicit RealtimeVoiceChangerWrapper(val config)
      : changer_(editing::voice_changer::realtime_voice_changer_config_from_json(
            realtimeVoiceChangerConfigTextFromVal(config))) {}

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
  }

  void reset() { changer_.reset(); }

  void setConfig(val config) {
    changer_.set_config(editing::voice_changer::realtime_voice_changer_config_from_json(
        realtimeVoiceChangerConfigTextFromVal(config)));
  }

  std::string configJson() const {
    return editing::voice_changer::realtime_voice_changer_config_to_json(changer_.config());
  }

  int latencySamples() const { return changer_.latency_samples(); }

  // Element-wise legacy path. NOT RT-safe for high block rates; AudioWorklet
  // consumers should prefer the prepared API below (getMonoInputBuffer /
  // processPreparedMono / getMonoOutputBuffer) which avoids per-sample JS↔C++
  // crossings and per-call allocations entirely.
  val processMono(val samples) {
    require_prepared();
    const int length = samples["length"].as<int>();
    ensure_mono_capacity(static_cast<size_t>(length));
    copyFloat32Array(samples, mono_input_, static_cast<size_t>(length));
    changer_.process_block(mono_input_.data(), mono_output_.data(), length);
    val output = val::global("Float32Array").new_(length);
    val view = val(typed_memory_view(static_cast<size_t>(length), mono_output_.data()));
    output.call<void>("set", view);
    return output;
  }

  void processMonoInto(val samples, val output) {
    require_prepared();
    const int length = samples["length"].as<int>();
    if (output["length"].as<int>() < length) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "output buffer is too small");
    }
    ensure_mono_capacity(static_cast<size_t>(length));
    copyFloat32Array(samples, mono_input_, static_cast<size_t>(length));
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
    ensure_interleaved_capacity(frames, channels);
    for (int ch = 0; ch < channels; ++ch) {
      for (size_t i = 0; i < frames; ++i) {
        const int index =
            static_cast<int>((i * static_cast<size_t>(channels)) + static_cast<size_t>(ch));
        planar_[static_cast<size_t>(ch)][i] = samples[index].as<float>();
      }
    }
    changer_.process_block(channel_ptrs_.data(), channels, static_cast<int>(frames));
    for (size_t i = 0; i < frames; ++i) {
      for (int ch = 0; ch < channels; ++ch) {
        const int index =
            static_cast<int>((i * static_cast<size_t>(channels)) + static_cast<size_t>(ch));
        output.set(index, planar_[static_cast<size_t>(ch)][i]);
      }
    }
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
  static void copyFloat32Array(val source, std::vector<float>& dest, size_t length) {
    for (size_t i = 0; i < length; ++i) dest[i] = source[static_cast<int>(i)].as<float>();
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

  /// Reject a channel count that differs from the prepared layout. The voice
  /// changer is configured for prepared_channels_ at prepare() time, so feeding
  /// a different count would grow the scratch buffers and call process_block
  /// with a count the changer never allocated state for. This mirrors the planar
  /// path's per-channel range check (getPlanarChannelBuffer) so every interleaved
  /// entry point rejects a runtime channel-count mismatch identically.
  void require_prepared_channels(int channels) const {
    if (channels != prepared_channels_) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          "RealtimeVoiceChanger: channel count must match the prepared layout");
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
  bool prepared_ = false;
};

RealtimeVoiceChangerWrapper* createRealtimeVoiceChanger(val config) {
  return new RealtimeVoiceChangerWrapper(config);
}

val realtimeVoiceChangerPresetNames() {
  val out = val::array();
  const auto names = editing::voice_changer::realtime_voice_changer_preset_names();
  for (size_t i = 0; i < names.size(); ++i) out.call<void>("push", names[i]);
  return out;
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
      .function("configJson", &RealtimeVoiceChangerWrapper::configJson)
      .function("latencySamples", &RealtimeVoiceChangerWrapper::latencySamples)
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
