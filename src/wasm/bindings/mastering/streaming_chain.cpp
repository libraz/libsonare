/// @file streaming_mastering_chain.cpp
/// @brief Embind bindings for streaming mastering chain APIs.

#ifdef __EMSCRIPTEN__

#include <limits>

#include "util/refusal_predicates.h"
#include "wasm/bindings/common/common.h"

// ---------------------------------------------------------------------------
// StreamingMasteringChain wrapper (block-by-block streaming).
// Construct via createStreamingMasteringChain(configJson, options) factory. Throws if the
// configuration enables non-streaming stages (repair.denoise, loudness).
// ---------------------------------------------------------------------------

namespace {

/// @brief Build StreamingMasteringChainOptions from an optional options val.
///
/// Reads the optional numeric fields `loudnessStaticGainDb` and
/// `loudnessStaticGainPeakDbtp`; absent, `undefined` or `null` leaves both at the NaN that
/// StreamingMasteringChainOptions spells "not provided" with, and a written NaN or infinity is
/// refused by name, except a `-Infinity` peak, the measured peak of digital silence. An enabled
/// loudness stage still refuses a gain it was never given.
mastering::api::StreamingMasteringChainOptions streamingOptionsFromVal(val options_val) {
  mastering::api::StreamingMasteringChainOptions options;
  if (options_val.isUndefined() || options_val.isNull()) return options;
  if (options_val.typeOf().as<std::string>() != "object") {
    throw WasmTypeError("streaming chain options must be an object");
  }
  options.loudness_static_gain_db =
      floatProperty(options_val, "loudnessStaticGainDb", options.loudness_static_gain_db);
  // -Infinity is the measured peak of digital silence and leaves the gain unclamped.
  const val peak = options_val["loudnessStaticGainPeakDbtp"];
  const bool silent_peak =
      peak.isNumber() && peak.as<double>() == -std::numeric_limits<double>::infinity();
  options.loudness_static_gain_peak_dbtp =
      silent_peak ? -std::numeric_limits<float>::infinity()
                  : floatProperty(options_val, "loudnessStaticGainPeakDbtp",
                                  options.loudness_static_gain_peak_dbtp);
  return options;
}

}  // namespace

class StreamingMasteringChainWrapper {
 public:
  StreamingMasteringChainWrapper(val config_json, val options)
      // The constructor's request object is the chain config itself, so its keys have no root.
      : chain_(masteringChainConfigFromDocument(config_json, true, ""),
               streamingOptionsFromVal(options)) {}

  // Every argument arrives as val: embind's integer glue wraps 2^32 + n to n, and
  // its double glue lets a NaN or infinite sample rate through unrefused.
  void prepare(const val& sample_rate_val, const val& max_block_size, const val& num_channels) {
    const double sample_rate = checkedDoubleFromVal(sample_rate_val, "sampleRate");
    const int block_size = max_block_size.isUndefined() || max_block_size.isNull()
                               ? mastering::api::MasteringChain::kDefaultMaxBlock
                               : checkedIntFromVal(max_block_size, "maxBlockSize");
    const int channels = domainNumberFromVal<int>(
        num_channels, "numChannels", mastering::api::kStreamingChainChannelCountDomain);
    chain_.prepare(sample_rate, block_size, channels);
    max_block_size_ = block_size;
  }

  val processMono(val samples) {
    const std::size_t length = wasmFloat32ArrayLength(samples, "samples");
    validateBlockLength(length, "samples");
    std::vector<float> block = float32ArrayToVector(samples, "samples");
    validateFinite(block, "samples");
    if (!block.empty()) {
      float* channels[] = {block.data()};
      chain_.process_block(channels, 1, static_cast<int>(block.size()));
    }
    return vectorToFloat32Array(block);
  }

  val processStereo(val left_samples, val right_samples) {
    validateWasmFloat32ArrayPair(left_samples, "left", right_samples, "right", true);
    const std::size_t length = wasmFloat32ArrayLength(left_samples, "left");
    validateBlockLength(length, "left");
    std::vector<float> left = float32ArrayToVector(left_samples, "left");
    std::vector<float> right = float32ArrayToVector(right_samples, "right");
    validateFinite(left, "left");
    validateFinite(right, "right");
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

  void setParameter(const std::string& key, const val& value) {
    if (value.typeOf().as<std::string>() != "number") {
      throw WasmTypeError("value must be a number");
    }
    chain_.set_parameter(key, checkedDoubleFromVal(value, "value"));
  }

  int latencySamples() const { return chain_.latency_samples(); }
  int maxBlockSize() const { return chain_.max_block_size(); }

  val stageNames() const { return stringVectorToVal(chain_.stage_names()); }

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
  size_t nonFiniteDiscardCount() const { return chain_.non_finite_discard_count(); }

 private:
  void ensurePreparedForFlush() const {
    if (max_block_size_ <= 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                    "StreamingMasteringChain must be prepared before flush");
    }
  }

  // Refuses a block longer than the prepared maximum by the caller's array, as the C ABI does.
  void validateBlockLength(std::size_t length, const char* array) const {
    if (max_block_size_ > 0 && length > static_cast<std::size_t>(max_block_size_)) {
      throw WasmRangeError(std::string(array) + ".length " +
                           sonare::util::refusal::integer_range_predicate(
                               0.0, static_cast<double>(max_block_size_)));
    }
  }

  // Refuses a block holding a non-finite sample by the caller's array, as the C ABI does.
  static void validateFinite(const std::vector<float>& block, const char* array) {
    sonare::validate_offline_audio_samples(array, block.data(), block.size(), 0);
  }

  mastering::api::StreamingMasteringChain chain_;
  int max_block_size_ = 0;
};

StreamingMasteringChainWrapper* createStreamingMasteringChain(val config_json, val options) {
  return new StreamingMasteringChainWrapper(config_json, options);
}

void registerStreamingMasteringChainBindings() {
  class_<StreamingMasteringChainWrapper>("StreamingMasteringChain")
      .function("prepare", &StreamingMasteringChainWrapper::prepare)
      .function("processMono", &StreamingMasteringChainWrapper::processMono)
      .function("processStereo", &StreamingMasteringChainWrapper::processStereo)
      .function("flushMono", &StreamingMasteringChainWrapper::flushMono)
      .function("flushStereo", &StreamingMasteringChainWrapper::flushStereo)
      .function("reset", &StreamingMasteringChainWrapper::reset)
      .function("setParameter", &StreamingMasteringChainWrapper::setParameter)
      .function("latencySamples", &StreamingMasteringChainWrapper::latencySamples)
      .function("maxBlockSize", &StreamingMasteringChainWrapper::maxBlockSize)
      .function("stageNames", &StreamingMasteringChainWrapper::stageNames)
      .function("nonFiniteSubstitutionCount",
                &StreamingMasteringChainWrapper::nonFiniteSubstitutionCount)
      .function("nonFiniteDiscardCount", &StreamingMasteringChainWrapper::nonFiniteDiscardCount);
  function("createStreamingMasteringChain", &createStreamingMasteringChain, allow_raw_pointers());
}

#endif  // __EMSCRIPTEN__
