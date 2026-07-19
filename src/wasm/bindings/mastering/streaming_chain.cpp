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
/// `loudnessStaticGainPeakDb`. Absent fields keep their NaN ("not provided")
/// defaults so an enabled loudness stage behaves as before.
mastering::api::StreamingMasteringChainOptions streamingOptionsFromVal(val config) {
  mastering::api::StreamingMasteringChainOptions options;
  if (hasProperty(config, "loudnessStaticGainDb")) {
    options.loudness_static_gain_db =
        floatProperty(config, "loudnessStaticGainDb", options.loudness_static_gain_db);
  }
  if (hasProperty(config, "loudnessStaticGainPeakDb")) {
    options.loudness_static_gain_peak_db =
        floatProperty(config, "loudnessStaticGainPeakDb", options.loudness_static_gain_peak_db);
  }
  return options;
}

}  // namespace

class StreamingMasteringChainWrapper {
 public:
  explicit StreamingMasteringChainWrapper(val config)
      : chain_(masteringChainConfigFromVal(config), streamingOptionsFromVal(config)) {}

  void prepare(double sample_rate, int max_block_size, int num_channels) {
    chain_.prepare(sample_rate, max_block_size, num_channels);
    max_block_size_ = max_block_size;
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

  void reset() { chain_.reset(); }

  int latencySamples() const { return chain_.latency_samples(); }

  val stageNames() const {
    val out = val::array();
    for (const auto& name : chain_.stage_names()) {
      out.call<void>("push", name);
    }
    return out;
  }

 private:
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
      .function("reset", &StreamingMasteringChainWrapper::reset)
      .function("latencySamples", &StreamingMasteringChainWrapper::latencySamples)
      .function("stageNames", &StreamingMasteringChainWrapper::stageNames);
  function("createStreamingMasteringChain", &createStreamingMasteringChain, allow_raw_pointers());
}

#endif  // __EMSCRIPTEN__
