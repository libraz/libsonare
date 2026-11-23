/// @file streaming_retune.cpp
/// @brief Embind bindings for streaming retune APIs.

#ifdef __EMSCRIPTEN__

#include <cmath>
#include <string>

#include "util/numeric_validation.h"
#include "util/refusal_predicates.h"
#include "wasm/bindings/common/common.h"

// ---------------------------------------------------------------------------
// StreamingRetune wrapper (block-by-block voice retune / pitch shift).
// Construct via createStreamingRetune(config) factory.
// ---------------------------------------------------------------------------

/// @brief Applies the keys present in @p config on top of @p seed.
/// @details The seed is what makes a partial object a MERGE rather than a
///          replacement: `setConfig({mix: 0.5})` after `setConfig({pitchSemitones:
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
  if (const auto semitones =
          optionalDomainNumber(objectProperty(config, "pitchSemitones"), "pitchSemitones");
      semitones) {
    result.semitones = *semitones;
  }
  if (const auto mix = optionalDomainNumber(objectProperty(config, "mix"), "mix"); mix) {
    result.mix = *mix;
  }
  if (const auto grain =
          optionalNumber(objectProperty(config, "grainSizeSamples"), "grainSizeSamples");
      grain) {
    if (*grain == 0.0f) {
      throw WasmRangeError("grainSizeSamples must not be 0; omit it for an automatic grain size");
    }
    // A grain no int holds exactly is off the domain's grid, so it reaches the refusal below as -1.
    const bool integral =
        std::isfinite(*grain) && *grain == std::floor(*grain) &&
        std::fabs(*grain) <= static_cast<float>(editing::voice_changer::kMaxRetuneGrainSize);
    result.grain_size = integral ? static_cast<int>(*grain) : -1;
  }
  // The class states the domain once; the refusal quotes the fields as the caller spelled them.
  const std::string problem = editing::voice_changer::streaming_retune_config_problem(
      result, nullptr, {"pitchSemitones", "mix", "grainSizeSamples"});
  if (!problem.empty()) throw WasmRangeError(problem);
  return result;
}

val streamingRetuneConfigToVal(const editing::voice_changer::StreamingRetuneConfig& config) {
  val out = val::object();
  out.set("pitchSemitones", config.semitones);
  out.set("mix", config.mix);
  out.set("grainSizeSamples", config.grain_size);
  return out;
}

class StreamingRetuneWrapper {
 public:
  explicit StreamingRetuneWrapper(val config)
      : StreamingRetuneWrapper(streamingRetuneConfigFromVal(config)) {}

  void prepare(const val& sample_rate_val, const val& max_block_size_val) {
    const double sample_rate = checkedDoubleFromVal(sample_rate_val, "sampleRate");
    const int max_block_size = checkedIntFromVal(max_block_size_val, "maxBlockSize");
    retune_.prepare(sample_rate, max_block_size);
    max_block_size_ = max_block_size;
    prepared_ = true;
  }

  void reset() { retune_.reset(); }

  void setConfig(val config) {
    // Seed from the configured controls so an object carrying one key leaves the
    // others alone, matching Node and Python.
    retune_.set_config(streamingRetuneConfigFromVal(config, retune_.config()));
  }

  val config() const { return streamingRetuneConfigToVal(retune_.config()); }

  int grainSizeSamples() const { return retune_.grain_size(); }

  int latencySamples() const { return retune_.latency_samples(); }

  val processMono(val samples) {
    const std::size_t length = wasmFloat32ArrayLength(samples, "samples");
    ensurePrepared();
    validateBlockLength(length);
    std::vector<float> block = float32ArrayToVector(samples, "samples");
    // Refuse a non-finite sample rather than zeroing it, in the C ABI's words.
    if (!sonare::numeric::all_finite(block.data(), block.size())) {
      throw WasmRangeError("samples must contain only finite samples");
    }
    std::vector<float> out(block.size());
    retune_.process_block(block.data(), out.data(), static_cast<int>(block.size()));
    return vectorToFloat32Array(out);
  }

 private:
  explicit StreamingRetuneWrapper(const editing::voice_changer::StreamingRetuneConfig& config)
      : retune_(config) {}

  void ensurePrepared() const {
    if (!prepared_) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "StreamingRetune must be prepared before processing");
    }
  }

  void validateBlockLength(std::size_t length) const {
    if (length > static_cast<std::size_t>(max_block_size_)) {
      throw WasmRangeError("samples.length " + sonare::util::refusal::integer_range_predicate(
                                                   0.0, static_cast<double>(max_block_size_)));
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
      .function("grainSizeSamples", &StreamingRetuneWrapper::grainSizeSamples)
      .function("latencySamples", &StreamingRetuneWrapper::latencySamples)
      .function("processMono", &StreamingRetuneWrapper::processMono);
  function("createStreamingRetune", &createStreamingRetune, allow_raw_pointers());
}

#endif  // __EMSCRIPTEN__
