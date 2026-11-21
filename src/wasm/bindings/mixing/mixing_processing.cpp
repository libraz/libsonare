/// @file mixing_processing.cpp
/// @brief Embind scene-based mixer facade: audio processing + zero-copy views.

#ifdef __EMSCRIPTEN__

#include "mixing_wasm.h"

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

namespace {

// More inputs than strips is an argument the caller got wrong, so it is refused
// by both counts before the C call; fewer inputs than strips leave the rest silent.
void requireInputCountWithinStrips(size_t input_count, size_t strip_count) {
  if (input_count > strip_count) {
    throw WasmRangeError("got " + std::to_string(input_count) + " input channels for a " +
                         std::to_string(strip_count) + "-strip mixer");
  }
}

// A C INVALID_PARAMETER from the mixer is an argument refusal, not object state.
[[noreturn]] void throwMixerProcessError(SonareError err) {
  rethrowLastCRefusal();
  const std::string message = cErrorMessage(err);
  if (err == SONARE_ERROR_INVALID_PARAMETER) {
    throw WasmRangeError(message);
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidState, message);
}

}  // namespace

val MixerWasm::processStereo(val left_channels, val right_channels) {
  // require_non_zero=false: unlike the standalone mixStereo utility, this
  // mixer instance may legitimately be configured with zero strips, and the
  // `count > 0` guard below already treats that as a no-op rather than an
  // error.
  const int count = requireMatchedLength(left_channels, right_channels,
                                         "leftInputs and rightInputs", /*require_non_zero=*/false);
  requireInputCountWithinStrips(static_cast<size_t>(count), stripCount());

  std::vector<std::vector<float>> left_inputs;
  std::vector<std::vector<float>> right_inputs;
  left_inputs.reserve(static_cast<size_t>(count));
  right_inputs.reserve(static_cast<size_t>(count));

  size_t length = 0;
  size_t cumulative_count = 0;
  for (int index = 0; index < count; ++index) {
    const std::string at = "[" + std::to_string(index) + "]";
    const std::string left_subject = "leftInputs" + at;
    const std::string right_subject = "rightInputs" + at;
    const size_t left_length = accumulateWasmFloat32ArrayLength(
        left_channels[index], left_subject.c_str(), &cumulative_count);
    const size_t right_length = accumulateWasmFloat32ArrayLength(
        right_channels[index], right_subject.c_str(), &cumulative_count);
    if (left_length != right_length) {
      throw WasmRangeError("leftInputs" + at + " and rightInputs" + at +
                           " must have the same length");
    }
    if (index == 0) {
      length = left_length;
    } else if (left_length != length) {
      throw WasmRangeError("all strips must have the same length");
    }
  }
  if (length > static_cast<size_t>(block_size_)) {
    throw WasmRangeError("block length exceeds the mixer's configured block size");
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
    if (err != SONARE_OK) throwMixerProcessError(err);
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
    throw WasmRangeError("leftChannels and rightChannels must have the same length");
  }
  requireInputCountWithinStrips(static_cast<size_t>(count), stripCount());

  // require_non_zero=false: a zero-sample block is a legitimate no-op here
  // (mirrors the zero-strip tolerance above; sonare_mixer_process_stereo is
  // called with length == 0 further down without special-casing it).
  const int length_i =
      requireMatchedLength(out_left, out_right, "output channels", /*require_non_zero=*/false);
  const size_t length = static_cast<size_t>(length_i);
  if (length > static_cast<size_t>(block_size_)) {
    throw WasmRangeError("block length exceeds the mixer's configured block size");
  }

  for (int index = 0; index < count; ++index) {
    val left = left_channels[index];
    val right = right_channels[index];
    if (left["length"].as<int>() != length_i || right["length"].as<int>() != length_i) {
      throw WasmRangeError("all input and output channels must have the same length");
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
  if (err != SONARE_OK) throwMixerProcessError(err);
  if (length > 0) {
    // As above, these views borrow scratch memory only for the duration of the
    // bulk copy; no view is retained by the binding after this call returns.
    val left_view = val(typed_memory_view(length, out_scratch_left_.data()));
    out_left.call<void>("set", left_view);
    val right_view = val(typed_memory_view(length, out_scratch_right_.data()));
    out_right.call<void>("set", right_view);
  }
}

val MixerWasm::inputLeftView(const val& index_val) {
  const size_t index = static_cast<size_t>(checkedUintFromVal(index_val, "index"));
  if (index >= left_scratch_.size()) {
    throw WasmRangeError("mixer input index out of range");
  }
  return val(typed_memory_view(static_cast<size_t>(block_size_), left_scratch_[index].data()));
}

val MixerWasm::inputRightView(const val& index_val) {
  const size_t index = static_cast<size_t>(checkedUintFromVal(index_val, "index"));
  if (index >= right_scratch_.size()) {
    throw WasmRangeError("mixer input index out of range");
  }
  return val(typed_memory_view(static_cast<size_t>(block_size_), right_scratch_[index].data()));
}

val MixerWasm::outputLeftView() {
  return val(typed_memory_view(static_cast<size_t>(block_size_), out_scratch_left_.data()));
}

val MixerWasm::outputRightView() {
  return val(typed_memory_view(static_cast<size_t>(block_size_), out_scratch_right_.data()));
}

void MixerWasm::processPreparedStereo(const val& num_samples_val) {
  const size_t num_samples = static_cast<size_t>(checkedUintFromVal(num_samples_val, "numSamples"));
  if (num_samples == 0 || num_samples > static_cast<size_t>(block_size_)) {
    throw WasmRangeError("invalid prepared mixer block length");
  }
  const size_t count = left_scratch_.size();
  SonareError err = sonare_mixer_process_stereo(
      mixer_, count > 0 ? left_ptrs_.data() : nullptr, count > 0 ? right_ptrs_.data() : nullptr,
      count, out_scratch_left_.data(), out_scratch_right_.data(), num_samples);
  if (err != SONARE_OK) throwMixerProcessError(err);
}

void MixerWasm::configureMeter(bool enabled, const val& true_peak_oversample,
                               const val& measure_lufs, const val& measure_true_peak) {
  // Seeded from the C defaults; only what the caller gave overrides them.
  SonareMixerMasterMeterConfig config{};
  sonare_mixer_master_meter_config_init(&config);
  config.enabled = enabled ? 1 : 0;
  if (!true_peak_oversample.isUndefined() && !true_peak_oversample.isNull()) {
    config.true_peak_oversample = checkedIntFromVal(true_peak_oversample, "truePeakOversample");
  }
  if (const auto lufs = optionalBool(measure_lufs, "measureLufs")) {
    config.measure_lufs = *lufs ? 1 : 0;
  }
  if (const auto true_peak = optionalBool(measure_true_peak, "measureTruePeak")) {
    config.measure_true_peak = *true_peak ? 1 : 0;
  }
  checkStripError(sonare_mixer_configure_master_meter(mixer_, &config));
}

val MixerWasm::meterSnapshot() const {
  SonareMixMeterSnapshot meter{};
  const SonareError err = sonare_mixer_master_meter(mixer_, &meter);
  if (err == SONARE_ERROR_INVALID_STATE) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "mixer meter has not been enabled");
  }
  checkStripError(err);
  return mixMeterSnapshotToVal(meter);
}

bool MixerWasm::latchMeterSnapshot() {
  return sonare_mixer_master_meter(mixer_, &meter_scratch_) == SONARE_OK;
}

float MixerWasm::meterScratchValue(const val& field) const {
  const int index = checkedIntFromVal(field, "field");
  requireOrdinalInRange(index, 0, 6, "field");
  switch (index) {
    case 0:
      return meter_scratch_.peak_db_l;
    case 1:
      return meter_scratch_.peak_db_r;
    case 2:
      return meter_scratch_.rms_db_l;
    case 3:
      return meter_scratch_.rms_db_r;
    case 4:
      return meter_scratch_.correlation;
    case 5:
      return meter_scratch_.true_peak_dbtp_l;
    default:
      return meter_scratch_.true_peak_dbtp_r;
  }
}

// Reports the longest audible serial processor-tail path to the master
// (samples). Lazily compiles if the topology is dirty.
int MixerWasm::tailSamples() {
  int out = 0;
  SonareError err = sonare_mixer_tail_samples(mixer_, &out);
  if (err != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, cErrorMessage(err));
  }
  return out;
}

// Reports the compiled mixer graph's latency in samples. Lazily compiles if the
// topology is dirty.
int MixerWasm::latencySamples() {
  int out = 0;
  SonareError err = sonare_mixer_latency_samples(mixer_, &out);
  if (err != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, cErrorMessage(err));
  }
  return out;
}

int MixerWasm::maxBlockSize() {
  int out = 0;
  SonareError err = sonare_mixer_max_block_size(mixer_, &out);
  if (err != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, cErrorMessage(err));
  }
  return out;
}

// Drains delayed/tail audio by processing a zero-input block of num_samples
// frames. Returns { left, right, sampleRate } mirroring processStereo.
val MixerWasm::drainTailStereo(double num_samples) {
  const size_t count = wasmCountArg(num_samples, "mixer drain numSamples");
  if (count == 0 || count > static_cast<size_t>(block_size_)) {
    throw WasmRangeError("mixer drain numSamples must be in [1, prepared block size]");
  }
  std::vector<float> out_left(count, 0.0f);
  std::vector<float> out_right(count, 0.0f);
  SonareError err =
      sonare_mixer_drain_tail_stereo(mixer_, out_left.data(), out_right.data(), count);
  if (err != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, cErrorMessage(err));
  }
  val out = val::object();
  out.set("left", vectorToFloat32Array(out_left));
  out.set("right", vectorToFloat32Array(out_right));
  out.set("sampleRate", sample_rate_);
  return out;
}

// Folds every strip, bus and master-output meter's pending true-peak group delay
// into its snapshot at the end of a signal; extends sonare_mixer_flush_meters
// with the binding-owned master meter.
void MixerWasm::flushMeters() {
  const SonareError err = sonare_mixer_flush_meters(mixer_);
  if (err != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, cErrorMessage(err));
  }
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
      .function("latchMeterSnapshot", &MixerWasm::latchMeterSnapshot)
      .function("meterScratchValue", &MixerWasm::meterScratchValue)
      .function("tailSamples", &MixerWasm::tailSamples)
      .function("latencySamples", &MixerWasm::latencySamples)
      .function("maxBlockSize", &MixerWasm::maxBlockSize)
      .function("drainTailStereo", &MixerWasm::drainTailStereo)
      .function("flushMeters", &MixerWasm::flushMeters);
}

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH

#endif  // __EMSCRIPTEN__
