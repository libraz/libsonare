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

editing::voice_changer::StreamingRetuneConfig streamingRetuneConfigFromVal(val config) {
  editing::voice_changer::StreamingRetuneConfig result;
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
  explicit StreamingRetuneWrapper(val config) : retune_(streamingRetuneConfigFromVal(config)) {}

  void prepare(double sample_rate, int max_block_size) {
    retune_.prepare(sample_rate, max_block_size);
    max_block_size_ = max_block_size;
    prepared_ = true;
  }

  void reset() { retune_.reset(); }

  void setConfig(val config) { retune_.set_config(streamingRetuneConfigFromVal(config)); }

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
