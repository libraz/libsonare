/// @file mixing_processing.cpp
/// @brief Embind scene-based mixer facade: audio processing + zero-copy views.

#ifdef __EMSCRIPTEN__

#include "mixing_wasm.h"

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

val MixerWasm::processStereo(val left_channels, val right_channels) {
  // require_non_zero=false: unlike the standalone mixStereo utility, this
  // mixer instance may legitimately be configured with zero strips, and the
  // `count > 0` guard below already treats that as a no-op rather than an
  // error.
  const int count = requireMatchedLength(
      left_channels, right_channels, "leftChannels and rightChannels", /*require_non_zero=*/false);

  std::vector<std::vector<float>> left_inputs;
  std::vector<std::vector<float>> right_inputs;
  left_inputs.reserve(static_cast<size_t>(count));
  right_inputs.reserve(static_cast<size_t>(count));

  size_t length = 0;
  size_t cumulative_count = 0;
  for (int index = 0; index < count; ++index) {
    const size_t left_length = accumulateWasmFloat32ArrayLength(
        left_channels[index], "left channel", "mixer process input", &cumulative_count);
    const size_t right_length = accumulateWasmFloat32ArrayLength(
        right_channels[index], "right channel", "mixer process input", &cumulative_count);
    if (left_length != right_length) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "left and right channel lengths must match");
    }
    if (index == 0) {
      length = left_length;
    } else if (left_length != length) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "all strips must have the same length");
    }
  }
  if (length > static_cast<size_t>(block_size_)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "block length exceeds the mixer's configured block size");
  }
  for (int index = 0; index < count; ++index) {
    left_inputs.push_back(float32ArrayToVector(left_channels[index]));
    right_inputs.push_back(float32ArrayToVector(right_channels[index]));
  }

  std::vector<const float*> left_ptrs(static_cast<size_t>(count));
  std::vector<const float*> right_ptrs(static_cast<size_t>(count));
  for (int index = 0; index < count; ++index) {
    left_ptrs[static_cast<size_t>(index)] = left_inputs[static_cast<size_t>(index)].data();
    right_ptrs[static_cast<size_t>(index)] = right_inputs[static_cast<size_t>(index)].data();
  }

  std::vector<float> out_left(length, 0.0f);
  std::vector<float> out_right(length, 0.0f);
  if (count > 0) {
    SonareError err = sonare_mixer_process_stereo(mixer_, left_ptrs.data(), right_ptrs.data(),
                                                  static_cast<size_t>(count), out_left.data(),
                                                  out_right.data(), length);
    if (err != SONARE_OK) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidState,
          std::string("mixer process failed: ") + sonare_error_message(err));
    }
  }

  val out = val::object();
  out.set("left", vectorToFloat32Array(out_left));
  out.set("right", vectorToFloat32Array(out_right));
  out.set("sampleRate", sample_rate_);
  return out;
}

void MixerWasm::processStereoInto(val left_channels, val right_channels, val out_left,
                                  val out_right) {
  const int count = left_channels["length"].as<int>();
  if (count < 0 || right_channels["length"].as<int>() != count) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "leftChannels and rightChannels must have the same length");
  }
  if (static_cast<size_t>(count) != left_scratch_.size()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "input channel count must match the mixer's strip count");
  }

  // require_non_zero=false: a zero-sample block is a legitimate no-op here
  // (mirrors the zero-strip tolerance above; sonare_mixer_process_stereo is
  // called with length == 0 further down without special-casing it).
  const int length_i =
      requireMatchedLength(out_left, out_right, "output channels", /*require_non_zero=*/false);
  const size_t length = static_cast<size_t>(length_i);
  if (length > static_cast<size_t>(block_size_)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "block length exceeds the mixer's configured block size");
  }

  for (int index = 0; index < count; ++index) {
    val left = left_channels[index];
    val right = right_channels[index];
    if (left["length"].as<int>() != length_i || right["length"].as<int>() != length_i) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "all input and output channels must have the same length");
    }
    auto& left_dest = left_scratch_[static_cast<size_t>(index)];
    auto& right_dest = right_scratch_[static_cast<size_t>(index)];
    if (length > 0) {
      // The view is temporary and only borrows the already-prepared scratch
      // storage. Calling Float32Array.set once per channel keeps this bridge
      // O(number of channels) at the JS/WASM boundary instead of O(frames).
      val left_view = val(typed_memory_view(length, left_dest.data()));
      left_view.call<void>("set", left);
      val right_view = val(typed_memory_view(length, right_dest.data()));
      right_view.call<void>("set", right);
    }
  }

  SonareError err = sonare_mixer_process_stereo(
      mixer_, count > 0 ? left_ptrs_.data() : nullptr, count > 0 ? right_ptrs_.data() : nullptr,
      static_cast<size_t>(count), out_scratch_left_.data(), out_scratch_right_.data(), length);
  if (err != SONARE_OK) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("mixer process failed: ") + sonare_error_message(err));
  }
  if (length > 0) {
    // As above, these views borrow scratch memory only for the duration of the
    // bulk copy; no view is retained by the binding after this call returns.
    val left_view = val(typed_memory_view(length, out_scratch_left_.data()));
    out_left.call<void>("set", left_view);
    val right_view = val(typed_memory_view(length, out_scratch_right_.data()));
    out_right.call<void>("set", right_view);
  }
}

val MixerWasm::inputLeftView(size_t index) {
  if (index >= left_scratch_.size()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "mixer input index out of range");
  }
  return val(typed_memory_view(static_cast<size_t>(block_size_), left_scratch_[index].data()));
}

val MixerWasm::inputRightView(size_t index) {
  if (index >= right_scratch_.size()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "mixer input index out of range");
  }
  return val(typed_memory_view(static_cast<size_t>(block_size_), right_scratch_[index].data()));
}

val MixerWasm::outputLeftView() {
  return val(typed_memory_view(static_cast<size_t>(block_size_), out_scratch_left_.data()));
}

val MixerWasm::outputRightView() {
  return val(typed_memory_view(static_cast<size_t>(block_size_), out_scratch_right_.data()));
}

void MixerWasm::processPreparedStereo(size_t num_samples) {
  if (num_samples == 0 || num_samples > static_cast<size_t>(block_size_)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "invalid prepared mixer block length");
  }
  const size_t count = left_scratch_.size();
  SonareError err = sonare_mixer_process_stereo(
      mixer_, count > 0 ? left_ptrs_.data() : nullptr, count > 0 ? right_ptrs_.data() : nullptr,
      count, out_scratch_left_.data(), out_scratch_right_.data(), num_samples);
  if (err != SONARE_OK) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("mixer process failed: ") + sonare_error_message(err));
  }
  if (meter_active_ && meter_.has_value()) {
    // Metered here rather than by the caller so every block reaches the meter.
    // The true-peak filter carries cross-block history, and an inter-sample peak
    // straddling a block boundary is only measured when the blocks arrive
    // unbroken — skipping blocks would reintroduce the under-reporting the
    // oversampling exists to remove.
    float* channels[2] = {out_scratch_left_.data(), out_scratch_right_.data()};
    meter_->process(channels, 2, static_cast<int>(num_samples));
  }
}

void MixerWasm::configureMeter(bool enabled, int true_peak_oversample) {
  if (!enabled) {
    meter_active_ = false;
    return;
  }
  // Same acceptance as the offline meteringTruePeakDb entry point; the core
  // resolves the request to a factor its realtime filter implements and raises
  // anything below the BS.1770-4 4x minimum.
  const int factor = true_peak_oversample == 0 ? 4 : true_peak_oversample;
  if (factor < 1 || factor > 16 || (factor & (factor - 1)) != 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "meter oversample must be 0 or a power of two from 1 to 16");
  }
  if (!meter_.has_value() || meter_oversample_ != factor) {
    sonare::mixing::MeterConfig config;
    config.measure_true_peak = true;
    config.true_peak_oversample = factor;
    // LUFS stays off: the caller reports it as unavailable rather than paying
    // for the K-weighting filters, and a floor value would read as silence.
    config.measure_lufs = false;
    meter_.emplace(config);
    meter_->prepare(static_cast<double>(sample_rate_), block_size_);
    meter_oversample_ = factor;
  } else {
    meter_->reset();
  }
  meter_active_ = true;
}

val MixerWasm::meterSnapshot() const {
  if (!meter_.has_value()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "mixer meter has not been enabled");
  }
  const sonare::mixing::MeterSnapshot meter = meter_->snapshot();
  val out = val::object();
  out.set("peakDbL", meter.peak_db[0]);
  out.set("peakDbR", meter.peak_db[1]);
  out.set("rmsDbL", meter.rms_db[0]);
  out.set("rmsDbR", meter.rms_db[1]);
  out.set("correlation", meter.correlation);
  out.set("truePeakDbL", meter.true_peak_db[0]);
  out.set("truePeakDbR", meter.true_peak_db[1]);
  return out;
}

// Reports the longest audible serial processor-tail path to the master
// (samples). Lazily compiles if the topology is dirty.
int MixerWasm::tailSamples() {
  int out = 0;
  SonareError err = sonare_mixer_tail_samples(mixer_, &out);
  if (err != SONARE_OK) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("failed to read mixer tail samples: ") + sonare_error_message(err));
  }
  return out;
}

// Reports the compiled mixer graph's latency in samples. Lazily compiles if the
// topology is dirty.
int MixerWasm::latencySamples() {
  int out = 0;
  SonareError err = sonare_mixer_latency_samples(mixer_, &out);
  if (err != SONARE_OK) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("failed to read mixer latency samples: ") + sonare_error_message(err));
  }
  return out;
}

// Drains delayed/tail audio by processing a zero-input block of num_samples
// frames. Returns { left, right, sampleRate } mirroring processStereo.
val MixerWasm::drainTailStereo(double num_samples) {
  if (!std::isfinite(num_samples) || std::floor(num_samples) != num_samples || num_samples <= 0.0 ||
      num_samples > static_cast<double>(block_size_)) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "mixer drain numSamples must be an integer in [1, prepared block size]");
  }
  const auto count = static_cast<size_t>(num_samples);
  std::vector<float> out_left(count, 0.0f);
  std::vector<float> out_right(count, 0.0f);
  SonareError err =
      sonare_mixer_drain_tail_stereo(mixer_, out_left.data(), out_right.data(), count);
  if (err != SONARE_OK) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("mixer drain tail failed: ") + sonare_error_message(err));
  }
  val out = val::object();
  out.set("left", vectorToFloat32Array(out_left));
  out.set("right", vectorToFloat32Array(out_right));
  out.set("sampleRate", sample_rate_);
  return out;
}

void registerMixerProcessing(class_<MixerWasm>& cls) {
  cls.function("processStereo", &MixerWasm::processStereo)
      .function("processStereoInto", &MixerWasm::processStereoInto)
      .function("inputLeftView", &MixerWasm::inputLeftView)
      .function("inputRightView", &MixerWasm::inputRightView)
      .function("outputLeftView", &MixerWasm::outputLeftView)
      .function("outputRightView", &MixerWasm::outputRightView)
      .function("processPreparedStereo", &MixerWasm::processPreparedStereo)
      .function("configureMeter", &MixerWasm::configureMeter)
      .function("meterSnapshot", &MixerWasm::meterSnapshot)
      .function("tailSamples", &MixerWasm::tailSamples)
      .function("latencySamples", &MixerWasm::latencySamples)
      .function("drainTailStereo", &MixerWasm::drainTailStereo);
}

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH

#endif  // __EMSCRIPTEN__
