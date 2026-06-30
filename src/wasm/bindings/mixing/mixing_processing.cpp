/// @file mixing_processing.cpp
/// @brief Embind scene-based mixer facade: audio processing + zero-copy views.

#ifdef __EMSCRIPTEN__

#include "mixing_wasm.h"

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

val MixerWasm::processStereo(val left_channels, val right_channels) {
  const int count = left_channels["length"].as<int>();
  // Reject empty input to match the free js_mix_stereo contract: a zero-strip
  // call would derive a zero-length block and produce an empty master, which
  // is never a useful result. (There is no master-only path here.)
  if (count <= 0 || right_channels["length"].as<int>() != count) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "leftChannels and rightChannels must have the same non-zero length");
  }

  std::vector<std::vector<float>> left_inputs;
  std::vector<std::vector<float>> right_inputs;
  left_inputs.reserve(static_cast<size_t>(count));
  right_inputs.reserve(static_cast<size_t>(count));

  size_t length = 0;
  for (int index = 0; index < count; ++index) {
    left_inputs.push_back(float32ArrayToVector(left_channels[index]));
    right_inputs.push_back(float32ArrayToVector(right_channels[index]));
    if (left_inputs.back().size() != right_inputs.back().size()) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "left and right channel lengths must match");
    }
    if (index == 0) {
      length = left_inputs.back().size();
    } else if (left_inputs.back().size() != length) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "all strips must have the same length");
    }
  }
  if (length > static_cast<size_t>(block_size_)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "block length exceeds the mixer's configured block size");
  }

  std::vector<const float*> left_ptrs(static_cast<size_t>(count));
  std::vector<const float*> right_ptrs(static_cast<size_t>(count));
  for (int index = 0; index < count; ++index) {
    left_ptrs[static_cast<size_t>(index)] = left_inputs[static_cast<size_t>(index)].data();
    right_ptrs[static_cast<size_t>(index)] = right_inputs[static_cast<size_t>(index)].data();
  }

  std::vector<float> out_left(length, 0.0f);
  std::vector<float> out_right(length, 0.0f);
  SonareError err = sonare_mixer_process_stereo(
      mixer_, count > 0 ? left_ptrs.data() : nullptr, count > 0 ? right_ptrs.data() : nullptr,
      static_cast<size_t>(count), out_left.data(), out_right.data(), length);
  if (err != SONARE_OK) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("mixer process failed: ") + sonare_error_message(err));
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

  const int length_i = out_left["length"].as<int>();
  if (length_i <= 0 || out_right["length"].as<int>() != length_i) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "output channels must have the same non-zero length");
  }
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
    for (size_t sample = 0; sample < length; ++sample) {
      left_dest[sample] = left[sample].as<float>();
      right_dest[sample] = right[sample].as<float>();
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
  for (size_t sample = 0; sample < length; ++sample) {
    out_left.set(sample, out_scratch_left_[sample]);
    out_right.set(sample, out_scratch_right_[sample]);
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
}

// Reports the maximum processor tail length in the compiled mixer graph
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

// Drains delayed/tail audio by processing a zero-input block of num_samples
// frames. Returns { left, right, sampleRate } mirroring processStereo.
val MixerWasm::drainTailStereo(size_t num_samples) {
  std::vector<float> out_left(num_samples, 0.0f);
  std::vector<float> out_right(num_samples, 0.0f);
  SonareError err =
      sonare_mixer_drain_tail_stereo(mixer_, out_left.data(), out_right.data(), num_samples);
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
      .function("tailSamples", &MixerWasm::tailSamples)
      .function("drainTailStereo", &MixerWasm::drainTailStereo);
}

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH

#endif  // __EMSCRIPTEN__
