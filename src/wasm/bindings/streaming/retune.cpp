/// @file streaming_retune.cpp
/// @brief Embind bindings for streaming retune APIs.

#ifdef __EMSCRIPTEN__

#include <cmath>
#include <string>

#include "editing/voice_changer/streaming_retune.h"
#include "streaming/realtime_limits.h"
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

namespace {

void checkStreamingRetune(SonareError error) {
  if (error != SONARE_OK) {
    throwCodedCError(error, CErrorReasonMode::kCurrentEntry);
  }
}

}  // namespace

class StreamingRetuneWrapper {
 public:
  explicit StreamingRetuneWrapper(val config) {
    const editing::voice_changer::StreamingRetuneConfig resolved =
        streamingRetuneConfigFromVal(config);
    checkStreamingRetune(sonare_streaming_retune_create(resolved.semitones, resolved.mix,
                                                        resolved.grain_size, &retune_));
    if (retune_ == nullptr) throwLastCError();
  }

  ~StreamingRetuneWrapper() { sonare_streaming_retune_destroy(retune_); }

  StreamingRetuneWrapper(const StreamingRetuneWrapper&) = delete;
  StreamingRetuneWrapper& operator=(const StreamingRetuneWrapper&) = delete;

  void prepare(const val& sample_rate_val, const val& max_block_size_val) {
    // Check every argument's shape before any domain reader can refuse a value.
    if (!sample_rate_val.isNumber()) throw WasmTypeError("sampleRate must be a number");
    if (!max_block_size_val.isNumber()) throw WasmTypeError("maxBlockSize must be a number");
    const int sample_rate =
        domainNumberFromVal<int>(sample_rate_val, "sampleRate", sonare::kAudioSampleRateDomain);
    const int max_block_size = domainNumberFromVal<int>(max_block_size_val, "maxBlockSize",
                                                        sonare::kRealtimeBlockSizeDomain);
    checkStreamingRetune(sonare_streaming_retune_prepare(retune_, sample_rate, max_block_size));
    // Publish the local allocation bound only after the C entry has committed its new stage.
    max_block_size_ = max_block_size;
  }

  void reset() { checkStreamingRetune(sonare_streaming_retune_reset(retune_)); }

  void setConfig(val config) {
    float semitones = 0.0f;
    float mix = 1.0f;
    int grain_size = 0;
    checkStreamingRetune(sonare_streaming_retune_config(retune_, &semitones, &mix, &grain_size));
    editing::voice_changer::StreamingRetuneConfig seed;
    seed.semitones = semitones;
    seed.mix = mix;
    seed.grain_size = grain_size;
    const editing::voice_changer::StreamingRetuneConfig resolved =
        streamingRetuneConfigFromVal(config, seed);
    checkStreamingRetune(sonare_streaming_retune_set_config(retune_, resolved.semitones,
                                                            resolved.mix, resolved.grain_size));
  }

  val config() const {
    editing::voice_changer::StreamingRetuneConfig config;
    checkStreamingRetune(sonare_streaming_retune_config(retune_, &config.semitones, &config.mix,
                                                        &config.grain_size));
    return streamingRetuneConfigToVal(config);
  }

  int grainSizeSamples() const {
    int grain_size = 0;
    checkStreamingRetune(sonare_streaming_retune_grain_size_samples(retune_, &grain_size));
    return grain_size;
  }

  int latencySamples() const {
    int latency = 0;
    checkStreamingRetune(sonare_streaming_retune_latency_samples(retune_, &latency));
    return latency;
  }

  val processMono(val samples) {
    const std::size_t length = wasmFloat32ArrayLength(samples, "samples");
    if (length == 0) {
      checkStreamingRetune(sonare_streaming_retune_process_mono(retune_, nullptr, 0));
      return vectorToFloat32Array(std::vector<float>{});
    }
    if (max_block_size_ == 0) {
      throw WasmCodedError(sonare::ErrorCode::InvalidState,
                           editing::voice_changer::kStreamingRetuneNotPreparedMessage,
                           std::nullopt);
    }
    validateBlockLength(length);
    std::vector<float> block = float32ArrayToVector(samples, "samples");
    checkStreamingRetune(
        sonare_streaming_retune_process_mono(retune_, elementData(block), block.size()));
    return vectorToFloat32Array(block);
  }

 private:
  void validateBlockLength(std::size_t length) const {
    // Keep the allocation preflight before copying an untrusted JS-sized block; the C entry
    // repeats this domain check before it touches the buffer.
    if (length > static_cast<std::size_t>(max_block_size_)) {
      throw WasmRangeError("samples.length " + sonare::util::refusal::integer_range_predicate(
                                                   0.0, static_cast<double>(max_block_size_)));
    }
  }

  SonareStreamingRetune* retune_ = nullptr;
  int max_block_size_ = 0;
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
