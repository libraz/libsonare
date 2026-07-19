/// @file streaming_equalizer.cpp
/// @brief Embind bindings for streaming equalizer APIs.

#ifdef __EMSCRIPTEN__

#include <array>
#include <cmath>

#include "wasm/bindings/common/common.h"

// ---------------------------------------------------------------------------
// StreamingEqualizer wrapper (block-by-block streaming EqualizerProcessor).
// Construct via createEqualizer(config) factory.
// ---------------------------------------------------------------------------

mastering::eq::EqBandType eqBandTypeFromString(const std::string& value) {
  using mastering::eq::EqBandType;
  if (value == "Peak" || value == "peak" || value == "Bell" || value == "bell") {
    return EqBandType::Peak;
  }
  if (value == "LowShelf" || value == "lowShelf") return EqBandType::LowShelf;
  if (value == "HighShelf" || value == "highShelf") return EqBandType::HighShelf;
  if (value == "LowPass" || value == "lowPass" || value == "HighCut" || value == "highCut") {
    return EqBandType::LowPass;
  }
  if (value == "HighPass" || value == "highPass" || value == "LowCut" || value == "lowCut") {
    return EqBandType::HighPass;
  }
  if (value == "BandPass" || value == "bandPass") return EqBandType::BandPass;
  if (value == "Notch" || value == "notch") return EqBandType::Notch;
  if (value == "TiltShelf" || value == "tiltShelf") return EqBandType::TiltShelf;
  if (value == "FlatTilt" || value == "flatTilt") return EqBandType::FlatTilt;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown EQ band type: " + value);
}

mastering::eq::BiquadCoeffMode eqCoeffModeFromString(const std::string& value) {
  using mastering::eq::BiquadCoeffMode;
  if (value == "Rbj" || value == "RBJ" || value == "rbj") return BiquadCoeffMode::Rbj;
  if (value == "Vicanek" || value == "vicanek") return BiquadCoeffMode::Vicanek;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown EQ coefficient mode: " + value);
}

mastering::eq::StereoPlacement eqPlacementFromString(const std::string& value) {
  using mastering::eq::StereoPlacement;
  if (value == "Stereo" || value == "stereo") return StereoPlacement::Stereo;
  if (value == "Left" || value == "left") return StereoPlacement::Left;
  if (value == "Right" || value == "right") return StereoPlacement::Right;
  if (value == "Mid" || value == "mid") return StereoPlacement::Mid;
  if (value == "Side" || value == "side") return StereoPlacement::Side;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown EQ placement: " + value);
}

mastering::eq::PhaseMode eqBandPhaseFromString(const std::string& value) {
  using mastering::eq::PhaseMode;
  if (value == "Inherit" || value == "inherit") return PhaseMode::Inherit;
  if (value == "ZeroLatency" || value == "zeroLatency") return PhaseMode::ZeroLatency;
  if (value == "NaturalPhase" || value == "naturalPhase") return PhaseMode::NaturalPhase;
  if (value == "LinearPhase" || value == "linearPhase") return PhaseMode::LinearPhase;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown EQ band phase mode: " + value);
}

mastering::eq::PhaseMode eqPhaseFromInt(int mode) {
  using mastering::eq::PhaseMode;
  switch (mode) {
    case 1:
      return PhaseMode::ZeroLatency;
    case 2:
      return PhaseMode::NaturalPhase;
    case 3:
      return PhaseMode::LinearPhase;
    default:
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown EQ phase mode");
  }
}

mastering::eq::EqBand eqBandFromVal(val band) {
  mastering::eq::EqBand result;
  result.type = eqBandTypeFromString(stringProperty(band, "type", "Peak"));
  result.coeff_mode = eqCoeffModeFromString(stringProperty(band, "coeffMode", "Rbj"));
  result.frequency_hz = floatProperty(band, "frequencyHz", result.frequency_hz);
  result.gain_db = floatProperty(band, "gainDb", result.gain_db);
  result.q = floatProperty(band, "q", result.q);
  result.enabled = boolProperty(band, "enabled", result.enabled);
  result.slope_db_oct = intProperty(band, "slopeDbOct", result.slope_db_oct);
  result.placement = eqPlacementFromString(stringProperty(band, "placement", "Stereo"));
  result.phase = eqBandPhaseFromString(stringProperty(band, "phase", "Inherit"));
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
  result.dyn.lookahead_ms = floatProperty(band, "lookaheadMs", result.dyn.lookahead_ms);
  result.dyn.external_sidechain =
      boolProperty(band, "externalSidechain", result.dyn.external_sidechain);
  result.dyn.sidechain_freq_hz =
      floatProperty(band, "sidechainFreqHz", result.dyn.sidechain_freq_hz);
  result.dyn.sidechain_q = floatProperty(band, "sidechainQ", result.dyn.sidechain_q);
  return result;
}

class EqualizerWrapper {
 public:
  explicit EqualizerWrapper(val config) : processor_(makeConfig()) {
    const double sample_rate = floatProperty(config, "sampleRate", 48000.0f);
    const int max_block_size = intProperty(config, "maxBlockSize", 512);
    processor_.prepare(sample_rate, max_block_size);
    sample_rate_ = sample_rate;
    max_block_size_ = max_block_size;
  }

  void setBand(int index, val band) {
    processor_.set_band(static_cast<size_t>(index), eqBandFromVal(band));
  }

  void clear() { processor_.clear(); }

  void setPhaseMode(int mode) { processor_.set_phase_mode(eqPhaseFromInt(mode)); }

  void setAutoGain(bool enabled) { processor_.set_auto_gain_enabled(enabled); }

  void setGainScale(float scale) { processor_.set_gain_scale(scale); }

  void setOutputGainDb(float gain_db) { processor_.set_output_gain_db(gain_db); }

  void setOutputPan(float pan) { processor_.set_output_pan(pan); }

  // Borrows a mono external sidechain key for dynamic bands that opt into
  // DynamicParams::external_sidechain. The samples are copied into an owned
  // buffer so they remain valid until the next set/clear call.
  void setSidechainMono(val samples) {
    const std::size_t length = wasmFloat32ArrayLength(samples, "mono sidechain");
    validateBlockLength(length, "sidechain");
    std::vector<float> staged_left = float32ArrayToVector(samples);
    if (staged_left.empty()) return clearSidechain();
    validateSidechainBuffer(staged_left);

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
    const std::size_t left_length = wasmFloat32ArrayLength(left_samples, "left sidechain");
    const std::size_t right_length = wasmFloat32ArrayLength(right_samples, "right sidechain");
    validateWasmFloat32ElementBudget({left_length, right_length}, "stereo sidechain");
    if (left_length != right_length) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "sidechain channel lengths must match");
    }
    if (left_length == 0) return clearSidechain();
    validateBlockLength(left_length, "sidechain");

    std::vector<float> staged_left = float32ArrayToVector(left_samples);
    std::vector<float> staged_right = float32ArrayToVector(right_samples);
    validateSidechainBuffer(staged_left);
    validateSidechainBuffer(staged_right);
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

  val processMono(val samples) {
    const std::size_t length = wasmFloat32ArrayLength(samples, "mono process block");
    validateBlockLength(length, "process block");
    std::vector<float> block = float32ArrayToVector(samples);
    if (!block.empty()) {
      float* channels[] = {block.data()};
      processor_.process(channels, 1, static_cast<int>(block.size()));
    }
    return vectorToFloat32Array(block);
  }

  val processStereo(val left_samples, val right_samples) {
    const std::size_t left_length = wasmFloat32ArrayLength(left_samples, "left process block");
    const std::size_t right_length = wasmFloat32ArrayLength(right_samples, "right process block");
    validateWasmFloat32ElementBudget({left_length, right_length}, "stereo process block");
    if (left_length != right_length) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "stereo channel lengths must match");
    }
    validateBlockLength(left_length, "process block");
    std::vector<float> left = float32ArrayToVector(left_samples);
    std::vector<float> right = float32ArrayToVector(right_samples);
    if (!left.empty()) {
      float* channels[] = {left.data(), right.data()};
      processor_.process(channels, 2, static_cast<int>(left.size()));
    }
    val out = val::object();
    out.set("left", vectorToFloat32Array(left));
    out.set("right", vectorToFloat32Array(right));
    return out;
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
    std::vector<float> band_gain_db(snapshot.band_gain_db.begin(), snapshot.band_gain_db.end());
    std::vector<float> profile_db(snapshot.profile_db.begin(), snapshot.profile_db.end());

    val out = val::object();
    out.set("preLeft", vectorToFloat32Array(pre_left));
    out.set("preRight", vectorToFloat32Array(pre_right));
    out.set("postLeft", vectorToFloat32Array(post_left));
    out.set("postRight", vectorToFloat32Array(post_right));
    out.set("bandGainDb", vectorToFloat32Array(band_gain_db));
    out.set("profileDb", vectorToFloat32Array(profile_db));
    out.set("lastAutoGainDb", processor_.last_auto_gain_db());
    out.set("seq", static_cast<double>(snapshot.seq));
    return out;
  }

  void match(val source, val reference, val options) {
    validateWasmFloat32ArrayPair(source, "source samples", reference, "reference samples",
                                 "StreamingEqualizer.match input", false);
    std::vector<float> src = float32ArrayToVector(source);
    std::vector<float> ref = float32ArrayToVector(reference);
    const int sample_rate =
        intProperty(options, "sampleRate", static_cast<int>(std::lround(sample_rate_)));
    const int max_bands = intProperty(options, "maxBands", 8);
    validate_offline_audio_input(src.data(), src.size(), sample_rate);
    validate_offline_audio_input(ref.data(), ref.size(), sample_rate);
    if (max_bands <= 0) {
      throw SonareException(ErrorCode::InvalidParameter,
                            "StreamingEqualizer.match: maxBands must be positive");
    }
    Audio src_audio = Audio::from_buffer(src.data(), src.size(), sample_rate);
    Audio ref_audio = Audio::from_buffer(ref.data(), ref.size(), sample_rate);
    mastering::match::MatchEqConfig match_config;
    match_config.max_bands = static_cast<size_t>(max_bands);
    mastering::match::configure_equalizer_from_match(
        processor_, mastering::match::reference_spectrum(src_audio),
        mastering::match::reference_spectrum(ref_audio), match_config);
  }

 private:
  void validateBlockLength(std::size_t length, const char* subject) const {
    if (length > static_cast<std::size_t>(max_block_size_)) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    std::string(subject) + " exceeds prepared maxBlockSize");
    }
  }

  void validateSidechainBuffer(const std::vector<float>& samples) const {
    validate_offline_audio_input(samples.data(), samples.size(),
                                 static_cast<int>(std::lround(sample_rate_)));
  }

  static mastering::eq::EqualizerProcessorConfig makeConfig() {
    mastering::eq::EqualizerProcessorConfig config;
    config.max_channels = 2;
    return config;
  }

  mastering::eq::EqualizerProcessor processor_;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 512;
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
      .function("processMono", &EqualizerWrapper::processMono)
      .function("processStereo", &EqualizerWrapper::processStereo)
      .function("spectrum", &EqualizerWrapper::spectrum)
      .function("match", &EqualizerWrapper::match);
  function("createEqualizer", &createEqualizer, allow_raw_pointers());
}

#endif  // __EMSCRIPTEN__
