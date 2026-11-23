/// @file streaming_equalizer.cpp
/// @brief Embind bindings for streaming equalizer APIs.

#ifdef __EMSCRIPTEN__

#include <array>
#include <cmath>

#include "c_api/mastering_input_domains.h"
#include "util/refusal_predicates.h"
#include "wasm/bindings/common/common.h"

// Included after common.h, which opens `using namespace sonare` for the
// unqualified `mastering::eq` spelling used throughout this file.
#include "mastering/eq/band_strings.h"

// ---------------------------------------------------------------------------
// StreamingEqualizer wrapper (block-by-block streaming EqualizerProcessor).
// Construct via createEqualizer(config) factory.
// ---------------------------------------------------------------------------

mastering::eq::EqBandType eqBandTypeFromString(const std::string& value) {
  const auto parsed = mastering::eq::band_type_from_string(value);
  if (!parsed) {
    throw WasmRangeError("unknown EQ band type: " + value + " (expected " +
                         mastering::eq::band_type_tokens() + ")");
  }
  return *parsed;
}

mastering::eq::BiquadCoeffMode eqCoeffModeFromString(const std::string& value) {
  const auto parsed = mastering::eq::coeff_mode_from_string(value);
  if (!parsed) {
    throw WasmRangeError("unknown EQ coefficient mode: " + value + " (expected " +
                         mastering::eq::coeff_mode_tokens() + ")");
  }
  return *parsed;
}

mastering::eq::StereoPlacement eqPlacementFromString(const std::string& value) {
  const auto parsed = mastering::eq::placement_from_string(value);
  if (!parsed) {
    throw WasmRangeError("unknown EQ placement: " + value + " (expected " +
                         mastering::eq::placement_tokens() + ")");
  }
  return *parsed;
}

mastering::eq::PhaseMode eqBandPhaseFromString(const std::string& value) {
  const auto parsed = mastering::eq::phase_mode_from_string(value);
  if (!parsed) {
    throw WasmRangeError("unknown EQ band phase mode: " + value + " (expected " +
                         mastering::eq::phase_mode_tokens() + ")");
  }
  return *parsed;
}

mastering::eq::PhaseMode eqPhaseFromInt(int mode) {
  const auto parsed = mastering::eq::phase_mode_from_int(mode);
  if (!parsed) {
    throw WasmRangeError("unknown EQ phase mode");
  }
  return *parsed;
}

mastering::eq::StereoPlacement eqPlacementFromInt(int placement) {
  const auto parsed = mastering::eq::placement_from_int(placement);
  if (!parsed) {
    throw WasmRangeError("unknown EQ band placement");
  }
  return *parsed;
}

mastering::eq::EqBand eqBandFromVal(val band) {
  mastering::eq::EqBand result;
  result.type = eqBandTypeFromString(
      stringProperty(band, "type", mastering::eq::band_type_to_string(result.type)));
  result.coeff_mode = eqCoeffModeFromString(
      stringProperty(band, "coeffMode", mastering::eq::coeff_mode_to_string(result.coeff_mode)));
  result.frequency_hz = floatProperty(band, "frequencyHz", result.frequency_hz);
  result.gain_db = floatProperty(band, "gainDb", result.gain_db);
  result.q = floatProperty(band, "q", result.q);
  result.enabled = boolProperty(band, "enabled", result.enabled);
  result.slope_db_oct = intProperty(band, "slopeDbOct", result.slope_db_oct);
  result.placement = eqPlacementFromString(
      stringProperty(band, "placement", mastering::eq::placement_to_string(result.placement)));
  result.phase = eqBandPhaseFromString(
      stringProperty(band, "phase", mastering::eq::phase_mode_to_string(result.phase)));
  result.soloed = boolProperty(band, "soloed", result.soloed);
  result.bypassed = boolProperty(band, "bypassed", result.bypassed);
  result.proportional_q = boolProperty(band, "proportionalQ", result.proportional_q);
  result.proportional_q_strength =
      floatProperty(band, "proportionalQStrength", result.proportional_q_strength);

  result.dyn.enabled = boolProperty(band, "dynamic", result.dyn.enabled);
  result.dyn.threshold_db = floatProperty(band, "thresholdDb", result.dyn.threshold_db);
  result.dyn.auto_threshold = boolProperty(band, "autoThreshold", result.dyn.auto_threshold);
  result.dyn.ratio = floatProperty(band, "ratio", result.dyn.ratio);
  result.dyn.range_db = floatProperty(band, "rangeDb", result.dyn.range_db);
  result.dyn.attack_ms = floatProperty(band, "attackMs", result.dyn.attack_ms);
  result.dyn.release_ms = floatProperty(band, "releaseMs", result.dyn.release_ms);
  result.dyn.detector_delay_ms =
      floatProperty(band, "detectorDelayMs", result.dyn.detector_delay_ms);
  result.dyn.external_sidechain =
      boolProperty(band, "externalSidechain", result.dyn.external_sidechain);
  result.dyn.sidechain_freq_hz =
      floatProperty(band, "sidechainFreqHz", result.dyn.sidechain_freq_hz);
  result.dyn.sidechain_q = floatProperty(band, "sidechainQ", result.dyn.sidechain_q);
  return result;
}

class EqualizerWrapper {
 public:
  explicit EqualizerWrapper(val config) {
    const double sample_rate = floatProperty(config, "sampleRate", 48000.0f);
    const int max_block_size =
        intProperty(config, "maxBlockSize", mastering::eq::EqualizerProcessor::kDefaultMaxBlock);
    processor_.prepare(sample_rate, max_block_size, 2);
    sample_rate_ = sample_rate;
    max_block_size_ = max_block_size;
  }

  void setBand(const val& index, val band) {
    processor_.set_band(checkedUintFromVal(index, "index"), eqBandFromVal(band));
  }

  void clear() { processor_.clear(); }

  void setPhaseMode(const val& mode) {
    processor_.set_phase_mode(eqPhaseFromInt(checkedIntFromVal(mode, "mode")));
  }

  void setAutoGain(bool enabled) { processor_.set_auto_gain_enabled(enabled); }

  void setGainScale(const val& scale_val) {
    const float scale = checkedFloatFromVal(scale_val, "scale");
    processor_.set_gain_scale(scale);
  }

  void setOutputGainDb(const val& gain_db_val) {
    const float gain_db = checkedFloatFromVal(gain_db_val, "gainDb");
    processor_.set_output_gain_db(gain_db);
  }

  void setOutputPan(const val& pan_val) {
    const float pan = checkedFloatFromVal(pan_val, "pan");
    processor_.set_output_pan(pan);
  }

  // Borrows a mono external sidechain key for dynamic bands that opt into
  // DynamicParams::external_sidechain. The samples are copied into an owned
  // buffer so they remain valid until the next set/clear call.
  void setSidechainMono(val samples) {
    const std::size_t length = wasmFloat32ArrayLength(samples, "samples");
    validateBlockLength(length, "samples");
    std::vector<float> staged_left = float32ArrayToVector(samples, "samples");
    if (staged_left.empty()) return clearSidechain();
    validateSidechainBuffer("samples", staged_left);

    // The wrapper preflight covers every core rejection condition. Commit the
    // owned storage first, then lend the core only the member pointer array.
    sidechain_left_.swap(staged_left);
    sidechain_right_.clear();
    sidechain_channels_ = {sidechain_left_.data(), nullptr};
    processor_.set_sidechain(sidechain_channels_.data(), 1,
                             static_cast<int>(sidechain_left_.size()));
  }

  // Borrows a stereo external sidechain key. Both channels must match in length.
  void setSidechainStereo(val left_samples, val right_samples) {
    const std::size_t left_length = wasmFloat32ArrayLength(left_samples, "left");
    const std::size_t right_length = wasmFloat32ArrayLength(right_samples, "right");
    validatePlainElementBudget({{&left_samples, "left"}, {&right_samples, "right"}});
    if (left_length != right_length) {
      throw WasmRangeError("left and right must have the same length");
    }
    if (left_length == 0) return clearSidechain();
    validateBlockLength(left_length, "left");

    std::vector<float> staged_left = float32ArrayToVector(left_samples, "left");
    std::vector<float> staged_right = float32ArrayToVector(right_samples, "right");
    validateSidechainBuffer("left", staged_left);
    validateSidechainBuffer("right", staged_right);
    sidechain_left_.swap(staged_left);
    sidechain_right_.swap(staged_right);
    sidechain_channels_ = {sidechain_left_.data(), sidechain_right_.data()};
    processor_.set_sidechain(sidechain_channels_.data(), 2,
                             static_cast<int>(sidechain_left_.size()));
  }

  void clearSidechain() {
    processor_.clear_sidechain();
    sidechain_left_.clear();
    sidechain_right_.clear();
    sidechain_channels_ = {};
  }

  float lastAutoGainDb() const { return processor_.last_auto_gain_db(); }

  int latencySamples() const { return processor_.latency_samples(); }
  int maxBlockSize() const { return processor_.max_block_size(); }

  // Number of blocks in which the EQ discarded recursive state because a
  // non-finite value had reached it. See sonare_eq_non_finite_discard_count.
  uint32_t nonFiniteDiscardCount() const { return processor_.non_finite_discard_count(); }

  val processMono(val samples) {
    const std::size_t length = wasmFloat32ArrayLength(samples, "samples");
    validateBlockLength(length, "samples");
    std::vector<float> block = float32ArrayToVector(samples, "samples");
    if (!block.empty()) {
      float* channels[] = {block.data()};
      processor_.process(channels, 1, static_cast<int>(block.size()));
    }
    return vectorToFloat32Array(block);
  }

  val processStereo(val left_samples, val right_samples) {
    const std::size_t left_length = wasmFloat32ArrayLength(left_samples, "left");
    const std::size_t right_length = wasmFloat32ArrayLength(right_samples, "right");
    validatePlainElementBudget({{&left_samples, "left"}, {&right_samples, "right"}});
    if (left_length != right_length) {
      throw WasmRangeError("left and right must have the same length");
    }
    validateBlockLength(left_length, "left");
    std::vector<float> left = float32ArrayToVector(left_samples, "left");
    std::vector<float> right = float32ArrayToVector(right_samples, "right");
    if (!left.empty()) {
      float* channels[] = {left.data(), right.data()};
      processor_.process(channels, 2, static_cast<int>(left.size()));
    }
    val out = val::object();
    out.set("left", vectorToFloat32Array(left));
    out.set("right", vectorToFloat32Array(right));
    return out;
  }

  // The curve to draw over the analyzer `spectrum()` returns. Takes the
  // placement as the same ordinal the C ABI does rather than a string, so the
  // wrapper stays a marshaller and the naming lives in the TS facade.
  val magnitudeResponse(const val& placement, val frequencies_hz) const {
    const int placement_id = checkedIntFromVal(placement, "placement");
    std::vector<float> frequencies = float32ArrayToVector(frequencies_hz, "frequenciesHz");
    std::vector<float> out(frequencies.size(), 0.0f);
    processor_.magnitude_response_db(eqPlacementFromInt(placement_id), frequencies.data(),
                                     frequencies.size(), out.data());
    return vectorToFloat32Array(out);
  }

  val spectrum() const {
    const mastering::eq::EqualizerSpectrumSnapshot snapshot = processor_.spectrum_snapshot();

    std::vector<float> pre_left(snapshot.pre_count);
    std::vector<float> pre_right(snapshot.pre_count);
    for (size_t i = 0; i < snapshot.pre_count; ++i) {
      pre_left[i] = snapshot.pre[i].left;
      pre_right[i] = snapshot.pre[i].right;
    }
    std::vector<float> post_left(snapshot.post_count);
    std::vector<float> post_right(snapshot.post_count);
    for (size_t i = 0; i < snapshot.post_count; ++i) {
      post_left[i] = snapshot.post[i].left;
      post_right[i] = snapshot.post[i].right;
    }
    val out = val::object();
    out.set("preLeft", vectorToFloat32Array(pre_left));
    out.set("preRight", vectorToFloat32Array(pre_right));
    out.set("postLeft", vectorToFloat32Array(post_left));
    out.set("postRight", vectorToFloat32Array(post_right));
    out.set("bandGainDb",
            vectorToFloat32Array(snapshot.band_gain_db.data(), snapshot.band_gain_db.size()));
    out.set("profileDb",
            vectorToFloat32Array(snapshot.profile_db.data(), snapshot.profile_db.size()));
    out.set("lastAutoGainDb", processor_.last_auto_gain_db());
    out.set("seq", static_cast<double>(snapshot.seq));
    return out;
  }

  void match(val source, val reference, val options) {
    validateWasmFloat32ArrayPair(source, "source", reference, "reference", false);
    std::vector<float> src = float32ArrayToVector(source, "source");
    std::vector<float> ref = float32ArrayToVector(reference, "reference");
    const int sample_rate =
        intProperty(options, "sampleRate", static_cast<int>(std::lround(sample_rate_)));
    // An omitted reference rate is the source's.
    const int reference_sample_rate = intProperty(options, "referenceSampleRate", sample_rate);
    const size_t max_bands =
        optionalDomainValue<size_t>(objectProperty(options, "maxBands"), "maxBands",
                                    sonare_c_detail::kEqMatchMaxBandsDomain)
            .value_or(8);
    validate_offline_audio_input("source", src.data(), src.size(), sample_rate);
    validate_offline_audio_input("reference", ref.data(), ref.size(), reference_sample_rate);
    mastering::match::MatchEqConfig match_config;
    match_config.max_bands = max_bands;
    mastering::match::configure_equalizer_from_audio(
        processor_, Audio::from_buffer(src.data(), src.size(), sample_rate),
        Audio::from_buffer(ref.data(), ref.size(), reference_sample_rate), match_config);
  }

 private:
  // Refuses a block longer than the prepared maximum by the caller's array, as the C ABI does.
  void validateBlockLength(std::size_t length, const char* array) const {
    if (length > static_cast<std::size_t>(max_block_size_)) {
      throw WasmRangeError(std::string(array) + ".length " +
                           sonare::util::refusal::integer_range_predicate(
                               0.0, static_cast<double>(max_block_size_)));
    }
  }

  void validateSidechainBuffer(const char* name, const std::vector<float>& samples) const {
    validate_offline_audio_input(name, samples.data(), samples.size(),
                                 static_cast<int>(std::lround(sample_rate_)));
  }

  mastering::eq::EqualizerProcessor processor_;
  double sample_rate_ = 48000.0;
  int max_block_size_ = mastering::eq::EqualizerProcessor::kDefaultMaxBlock;
  std::vector<float> sidechain_left_;
  std::vector<float> sidechain_right_;
  std::array<const float*, 2> sidechain_channels_{};
};

EqualizerWrapper* createEqualizer(val config) { return new EqualizerWrapper(config); }

void registerStreamingEqualizerBindings() {
  class_<EqualizerWrapper>("StreamingEqualizer")
      .function("setBand", &EqualizerWrapper::setBand)
      .function("clear", &EqualizerWrapper::clear)
      .function("setPhaseMode", &EqualizerWrapper::setPhaseMode)
      .function("setAutoGain", &EqualizerWrapper::setAutoGain)
      .function("setGainScale", &EqualizerWrapper::setGainScale)
      .function("setOutputGainDb", &EqualizerWrapper::setOutputGainDb)
      .function("setOutputPan", &EqualizerWrapper::setOutputPan)
      .function("setSidechainMono", &EqualizerWrapper::setSidechainMono)
      .function("setSidechainStereo", &EqualizerWrapper::setSidechainStereo)
      .function("clearSidechain", &EqualizerWrapper::clearSidechain)
      .function("lastAutoGainDb", &EqualizerWrapper::lastAutoGainDb)
      .function("latencySamples", &EqualizerWrapper::latencySamples)
      .function("maxBlockSize", &EqualizerWrapper::maxBlockSize)
      .function("nonFiniteDiscardCount", &EqualizerWrapper::nonFiniteDiscardCount)
      .function("processMono", &EqualizerWrapper::processMono)
      .function("processStereo", &EqualizerWrapper::processStereo)
      .function("magnitudeResponse", &EqualizerWrapper::magnitudeResponse)
      .function("spectrum", &EqualizerWrapper::spectrum)
      .function("match", &EqualizerWrapper::match);
  function("createEqualizer", &createEqualizer, allow_raw_pointers());
}

#endif  // __EMSCRIPTEN__
