/// @file streaming_retune.cpp
/// @brief Embind bindings for streaming retune APIs.

#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <cmath>

#include "wasm/bindings/common/common.h"

// ---------------------------------------------------------------------------
// StreamingRetune wrapper (block-by-block voice retune / pitch shift).
// Construct via createStreamingRetune(config) factory.
// ---------------------------------------------------------------------------

/// @brief Applies the keys present in @p config on top of @p seed.
/// @details The seed is what makes a partial object a MERGE rather than a
///          replacement: `setConfig({mix: 0.5})` after `setConfig({semitones:
///          7})` used to start from a default-constructed config and silently
///          reset the pitch shift to 0. Node and Python seed from the current
///          values; the construction path passes no seed, so it still starts
///          from the documented defaults.
editing::voice_changer::StreamingRetuneConfig streamingRetuneConfigFromVal(
    val config, editing::voice_changer::StreamingRetuneConfig seed = {}) {
  editing::voice_changer::StreamingRetuneConfig result = seed;
  if (config.isNull() || config.isUndefined()) {
    return result;
  }
  // A non-finite control is a caller error, not a request to clamp: silently
  // dropping it here left a NaN request looking like an omitted key, and made
  // this the one surface that answered differently from the C ABI. Finite
  // out-of-range values stay clamped -- that is the documented contract.
  if (const auto semitones = optionalNumber(objectProperty(config, "semitones")); semitones) {
    if (!std::isfinite(*semitones)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "StreamingRetune: semitones must be finite");
    }
    result.semitones = std::clamp(*semitones, -24.0f, 24.0f);
  }
  if (const auto mix = optionalNumber(objectProperty(config, "mix")); mix) {
    if (!std::isfinite(*mix)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "StreamingRetune: mix must be finite");
    }
    result.mix = std::clamp(*mix, 0.0f, 1.0f);
  }
  const auto grain_size = optionalNumber(objectProperty(config, "grainSize"));
  const auto snake_case_grain_size = optionalNumber(objectProperty(config, "grain_size"));
  const auto grain = snake_case_grain_size ? snake_case_grain_size : grain_size;
  if (grain) {
    if (!std::isfinite(*grain)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "StreamingRetune: grainSize must be finite");
    }
    result.grain_size = static_cast<int>(std::lround(std::clamp(*grain, 0.0f, 8192.0f)));
  }
  return result;
}

val streamingRetuneConfigToVal(const editing::voice_changer::StreamingRetuneConfig& config) {
  val out = val::object();
  out.set("semitones", config.semitones);
  out.set("mix", config.mix);
  out.set("grainSize", config.grain_size);
  return out;
}

class StreamingRetuneWrapper {
 public:
  explicit StreamingRetuneWrapper(val config)
      : StreamingRetuneWrapper(streamingRetuneConfigFromVal(config)) {}

  void prepare(double sample_rate, int max_block_size) {
    retune_.prepare(sample_rate, max_block_size);
    max_block_size_ = max_block_size;
    prepared_ = true;
  }

  void reset() { retune_.reset(); }

  void setConfig(val config) {
    // Seed from the applied controls so an object carrying one key leaves the
    // others alone, matching Node and Python. grainSize is seeded from the last
    // REQUESTED value rather than from config(): once prepared, config()
    // reports the effective grain, and seeding with that would freeze the 0
    // "derive from the sample rate" sentinel into a literal, so re-preparing at
    // a new rate would keep the first rate's grain.
    editing::voice_changer::StreamingRetuneConfig seed = retune_.config();
    seed.grain_size = requested_grain_size_;
    const editing::voice_changer::StreamingRetuneConfig merged =
        streamingRetuneConfigFromVal(config, seed);
    requested_grain_size_ = merged.grain_size;
    retune_.set_config(merged);
  }

  val config() const { return streamingRetuneConfigToVal(retune_.config()); }

  int grainSize() const { return retune_.grain_size(); }

  int latencySamples() const { return retune_.latency_samples(); }

  val processMono(val samples) {
    const std::size_t length = wasmFloat32ArrayLength(samples, "StreamingRetune process block");
    ensurePrepared();
    validateBlockLength(length);
    std::vector<float> block = float32ArrayToVector(samples);
    // Refuse a non-finite sample rather than zeroing it. Zeroing kept the grain
    // history clean but told the caller nothing, so a block silently became a
    // different block; the C ABI reports it, and a surface that quietly repairs
    // its input is the asymmetry that keeps regrowing here.
    for (const float sample : block) {
      if (!std::isfinite(sample)) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "StreamingRetune: input samples must be finite");
      }
    }
    std::vector<float> out(block.size());
    retune_.process_block(block.data(), out.data(), static_cast<int>(block.size()));
    return vectorToFloat32Array(out);
  }

 private:
  explicit StreamingRetuneWrapper(const editing::voice_changer::StreamingRetuneConfig& config)
      : retune_(config), requested_grain_size_(config.grain_size) {}

  void ensurePrepared() const {
    if (!prepared_) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "StreamingRetune must be prepared before processing");
    }
  }

  void validateBlockLength(std::size_t length) const {
    if (length > static_cast<std::size_t>(max_block_size_)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "process block exceeds prepared maxBlockSize");
    }
  }

  editing::voice_changer::StreamingRetune retune_;
  // The grain size the caller last ASKED for, kept apart from the effective one
  // config() reports. Mirrors StreamingRetune::requested_grain_size_, which is
  // private to the core.
  int requested_grain_size_ = 0;
  int max_block_size_ = 0;
  bool prepared_ = false;
};

StreamingRetuneWrapper* createStreamingRetune(val config) {
  return new StreamingRetuneWrapper(config);
}

void registerStreamingRetuneBindings() {
  class_<StreamingRetuneWrapper>("StreamingRetune")
      .function("prepare", &StreamingRetuneWrapper::prepare)
      .function("reset", &StreamingRetuneWrapper::reset)
      .function("setConfig", &StreamingRetuneWrapper::setConfig)
      .function("config", &StreamingRetuneWrapper::config)
      .function("grainSize", &StreamingRetuneWrapper::grainSize)
      .function("latencySamples", &StreamingRetuneWrapper::latencySamples)
      .function("processMono", &StreamingRetuneWrapper::processMono);
  function("createStreamingRetune", &createStreamingRetune, allow_raw_pointers());
}

#endif  // __EMSCRIPTEN__
