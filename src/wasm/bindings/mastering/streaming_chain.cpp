/// @file streaming_mastering_chain.cpp
/// @brief Embind bindings for streaming mastering chain APIs.

#ifdef __EMSCRIPTEN__

#include "wasm/bindings/common/common.h"

// ---------------------------------------------------------------------------
// StreamingMasteringChain wrapper (block-by-block streaming).
// Construct via createStreamingMasteringChain(config) factory. Throws if the
// configuration enables non-streaming stages (repair.denoise, loudness).
// ---------------------------------------------------------------------------

namespace {

/// @brief Build StreamingMasteringChainOptions from an optional config val.
///
/// Reads the optional numeric fields `loudnessStaticGainDb` and
/// `loudnessStaticGainPeakDb`. Both defaults are the NaN that
/// StreamingMasteringChainOptions spells "not provided" with, so floatOption is
/// the reader: a non-finite value means the same thing, and an enabled loudness
/// stage still refuses a gain it was never given.
mastering::api::StreamingMasteringChainOptions streamingOptionsFromVal(val config) {
  mastering::api::StreamingMasteringChainOptions options;
  if (hasProperty(config, "loudnessStaticGainDb")) {
    options.loudness_static_gain_db =
        floatOption(config, "loudnessStaticGainDb", options.loudness_static_gain_db);
  }
  if (hasProperty(config, "loudnessStaticGainPeakDb")) {
    options.loudness_static_gain_peak_db =
        floatOption(config, "loudnessStaticGainPeakDb", options.loudness_static_gain_peak_db);
  }
  return options;
}

}  // namespace

class StreamingMasteringChainWrapper {
 public:
  explicit StreamingMasteringChainWrapper(val config)
      : chain_(masteringChainConfigFromVal(config), streamingOptionsFromVal(config)) {}

  // The two block dimensions arrive as val rather than as int: embind's integer
  // glue wraps, so 2^32 + n reaches a narrow parameter as n and asks for a block
  // nobody requested. sample_rate is a double, which is what a JS number already
  // is, so it has nothing to wrap into.
  void prepare(double sample_rate, const val& max_block_size, const val& num_channels) {
    const int block_size = checkedIntFromVal(max_block_size, "maxBlockSize");
    const int channels = checkedIntFromVal(num_channels, "numChannels");
    chain_.prepare(sample_rate, block_size, channels);
    max_block_size_ = block_size;
  }

  val processMono(val samples) {
    const std::size_t length = wasmFloat32ArrayLength(samples, "mono process block");
    validateBlockLength(length);
    std::vector<float> block = float32ArrayToVector(samples);
    if (!block.empty()) {
      float* channels[] = {block.data()};
      chain_.process_block(channels, 1, static_cast<int>(block.size()));
    }
    return vectorToFloat32Array(block);
  }

  val processStereo(val left_samples, val right_samples) {
    validateWasmFloat32ArrayPair(left_samples, "left process block", right_samples,
                                 "right process block", "streaming mastering stereo block", true);
    const std::size_t length = wasmFloat32ArrayLength(left_samples, "left process block");
    validateBlockLength(length);
    std::vector<float> left = float32ArrayToVector(left_samples);
    std::vector<float> right = float32ArrayToVector(right_samples);
    if (!left.empty()) {
      float* channels[] = {left.data(), right.data()};
      chain_.process_block(channels, 2, static_cast<int>(left.size()));
    }
    val out = val::object();
    out.set("left", vectorToFloat32Array(left));
    out.set("right", vectorToFloat32Array(right));
    return out;
  }

  val flushMono() {
    ensurePreparedForFlush();
    std::vector<float> block(static_cast<std::size_t>(max_block_size_));
    float* channels[] = {block.data()};
    const int written = chain_.flush(channels, 1, max_block_size_);
    block.resize(static_cast<std::size_t>(written));
    return vectorToFloat32Array(block);
  }

  val flushStereo() {
    ensurePreparedForFlush();
    std::vector<float> left(static_cast<std::size_t>(max_block_size_));
    std::vector<float> right(static_cast<std::size_t>(max_block_size_));
    float* channels[] = {left.data(), right.data()};
    const int written = chain_.flush(channels, 2, max_block_size_);
    left.resize(static_cast<std::size_t>(written));
    right.resize(static_cast<std::size_t>(written));
    val out = val::object();
    out.set("left", vectorToFloat32Array(left));
    out.set("right", vectorToFloat32Array(right));
    return out;
  }

  void reset() { chain_.reset(); }

  int latencySamples() const { return chain_.latency_samples(); }

  val stageNames() const {
    val out = val::array();
    for (const auto& name : chain_.stage_names()) {
      out.call<void>("push", name);
    }
    return out;
  }

  // double rather than the uint32_t the chain reports: embind marshals an
  // unsigned to a JS number anyway, and saying so here keeps the saturated
  // maximum readable instead of arriving as a negative int.
  double nonFiniteSubstitutionCount() const {
    return static_cast<double>(chain_.non_finite_substitution_count());
  }

  // The companion count: a whole stage returning to its post-reset value,
  // once per call however many stages did it -- not the same measurement as
  // nonFiniteSubstitutionCount, which sums replaced samples. See
  // sonare_streaming_mastering_chain_non_finite_discard_count.
  double nonFiniteDiscardCount() const {
    return static_cast<double>(chain_.non_finite_discard_count());
  }

 private:
  void ensurePreparedForFlush() const {
    if (max_block_size_ <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "StreamingMasteringChain must be prepared before flush");
    }
  }

  void validateBlockLength(std::size_t length) const {
    if (max_block_size_ > 0 && length > static_cast<std::size_t>(max_block_size_)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "process block exceeds prepared maxBlockSize");
    }
  }

  mastering::api::StreamingMasteringChain chain_;
  int max_block_size_ = 0;
};

StreamingMasteringChainWrapper* createStreamingMasteringChain(val config) {
  return new StreamingMasteringChainWrapper(config);
}

void registerStreamingMasteringChainBindings() {
  class_<StreamingMasteringChainWrapper>("StreamingMasteringChain")
      .function("prepare", &StreamingMasteringChainWrapper::prepare)
      .function("processMono", &StreamingMasteringChainWrapper::processMono)
      .function("processStereo", &StreamingMasteringChainWrapper::processStereo)
      .function("flushMono", &StreamingMasteringChainWrapper::flushMono)
      .function("flushStereo", &StreamingMasteringChainWrapper::flushStereo)
      .function("reset", &StreamingMasteringChainWrapper::reset)
      .function("latencySamples", &StreamingMasteringChainWrapper::latencySamples)
      .function("stageNames", &StreamingMasteringChainWrapper::stageNames)
      .function("nonFiniteSubstitutionCount",
                &StreamingMasteringChainWrapper::nonFiniteSubstitutionCount)
      .function("nonFiniteDiscardCount", &StreamingMasteringChainWrapper::nonFiniteDiscardCount);
  function("createStreamingMasteringChain", &createStreamingMasteringChain, allow_raw_pointers());
}

#endif  // __EMSCRIPTEN__
