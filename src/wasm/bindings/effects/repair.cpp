/// @file repair.cpp
/// @brief Embind bindings for offline mastering repair APIs.

#ifdef __EMSCRIPTEN__

#include <algorithm>

#include "mastering/common/noise_profile.h"
#include "wasm/bindings/common/common.h"

// ============================================================================
// Mastering — offline repair processors (declick / denoise_classical)
// ============================================================================

namespace {

// Missing, null, undefined, and values of the wrong primitive type all retain
// the config default, so embind's undefined-to-NaN coercion never reaches an
// integer conversion in a DSP config. The Node addon refuses a wrong-typed
// value by name for most repair fields, so the two surfaces answer that case
// differently.
bool repairOptionValue(const val& options, const char* key, val* value) {
  if (!hasProperty(options, key)) return false;
  *value = options[key];
  return !value->isUndefined() && !value->isNull();
}

int repairIntOption(const val& options, const char* key, int fallback) {
  val value = val::undefined();
  if (!repairOptionValue(options, key, &value) || value.typeOf().as<std::string>() != "number") {
    return fallback;
  }
  // Range-check before narrowing, for the reason intProperty does: val::as<int>()
  // saturates, so 2^31 and 4294967295 both arrived as INT_MAX and produced one
  // identical output that every downstream positivity check accepted.
  return checkedIntFromVal(value, key);
}

float repairFloatOption(const val& options, const char* key, float fallback) {
  val value = val::undefined();
  if (!repairOptionValue(options, key, &value) || value.typeOf().as<std::string>() != "number") {
    return fallback;
  }
  // Range-check before narrowing, for the reason repairIntOption does: a value
  // past FLT_MAX becomes +inf, and the config validator refuses NaN but not inf,
  // so 3.5e38, 1e39, 1e300 and Infinity all produced one identical output.
  return checkedFloatFromVal(value, key);
}

bool repairBoolOption(const val& options, const char* key, bool fallback) {
  val value = val::undefined();
  return repairOptionValue(options, key, &value) && value.typeOf().as<std::string>() == "boolean"
             ? value.as<bool>()
             : fallback;
}

// Read a dereverb options bag over `config`, leaving absent keys alone.
mastering::repair::DereverbClassicalConfig readDereverbConfig(
    const val& options, mastering::repair::DereverbClassicalConfig config) {
  config.threshold = repairFloatOption(options, "threshold", config.threshold);
  config.attenuation = repairFloatOption(options, "attenuation", config.attenuation);
  config.n_fft = repairIntOption(options, "nFft", config.n_fft);
  config.hop_length = repairIntOption(options, "hopLength", config.hop_length);
  config.t60_sec = repairFloatOption(options, "t60Sec", config.t60_sec);
  config.late_delay_ms = repairFloatOption(options, "lateDelayMs", config.late_delay_ms);
  config.over_subtraction = repairFloatOption(options, "overSubtraction", config.over_subtraction);
  config.spectral_floor = repairFloatOption(options, "spectralFloor", config.spectral_floor);
  config.wpe_enabled = repairBoolOption(options, "wpeEnabled", config.wpe_enabled);
  config.wpe_iterations = repairIntOption(options, "wpeIterations", config.wpe_iterations);
  config.wpe_taps = repairIntOption(options, "wpeTaps", config.wpe_taps);
  config.wpe_strength = repairFloatOption(options, "wpeStrength", config.wpe_strength);
  return config;
}

// Reads a JS array of Float32Array channels for the linked entry points,
// running loadValidatedAudio over EVERY channel. The core guards channels[0]
// alone and scans no channel at all for a non-finite sample, so this loop is
// the whole non-finite guard on the set; a wrapper that validated only the
// first channel would pass a NaN straight into the mask. `entry` names the
// caller in each message.
std::vector<Audio> loadValidatedChannelSet(const val& channels, int sample_rate,
                                           const char* entry) {
  const std::string subject(entry);
  if (channels.isUndefined() || channels.isNull()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  subject + ": channels must be an array of Float32Array");
  }
  const std::size_t count = wasmArrayLikeLength(channels, "channels");
  if (count == 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  subject + ": channels must hold at least one channel");
  }
  const std::string budget = subject + " input";
  std::vector<Audio> loaded;
  loaded.reserve(std::min(count, kMaxWasmObjectArrayReserve));
  std::size_t cumulative = 0;
  std::size_t length = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const val channel = channels[index];
    if (channel.isUndefined() || channel.isNull()) {
      throw sonare::SonareException(
          sonare::ErrorCode::InvalidParameter,
          subject + ": channels[" + std::to_string(index) + "] must be a Float32Array");
    }
    const std::size_t frames =
        accumulateWasmFloat32ArrayLength(channel, "channels entry", budget.c_str(), &cumulative);
    if (index == 0) {
      length = frames;
    } else if (frames != length) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    subject + ": channel lengths must match");
    }
    loaded.push_back(loadValidatedAudio(channel, sample_rate));
  }
  return loaded;
}

// Pointers are taken only once every channel is in place, so no later growth
// can invalidate one.
std::vector<const Audio*> channelSetPointers(const std::vector<Audio>& channels) {
  std::vector<const Audio*> pointers;
  pointers.reserve(channels.size());
  for (const Audio& channel : channels) pointers.push_back(&channel);
  return pointers;
}

val channelSetToVal(const std::vector<Audio>& channels) {
  val out = val::array();
  for (const Audio& channel : channels) {
    out.call<void>("push", vectorToFloat32Array(std::vector<float>(
                               channel.data(), channel.data() + channel.size())));
  }
  return out;
}

}  // namespace

val js_mastering_repair_declick(val samples, const val& sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  mastering::repair::DeclickConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg.threshold = repairFloatOption(options, "threshold", cfg.threshold);
    cfg.neighbor_ratio = repairFloatOption(options, "neighborRatio", cfg.neighbor_ratio);
    if (hasProperty(options, "maxClickSamples")) {
      const int v =
          repairIntOption(options, "maxClickSamples", static_cast<int>(cfg.max_click_samples));
      if (v <= 0) {
        throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                      "masteringRepairDeclick: maxClickSamples must be positive");
      }
      cfg.max_click_samples = static_cast<size_t>(v);
    }
    cfg.lpc_order = repairIntOption(options, "lpcOrder", cfg.lpc_order);
    cfg.residual_ratio = repairFloatOption(options, "residualRatio", cfg.residual_ratio);
  }
  Audio result = mastering::repair::declick(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

namespace {

val declickDetectionToVal(const mastering::repair::ClickDetection& detected) {
  val out = val::object();
  out.set("count", detected.count);
  out.set("rejected", detected.rejected);
  out.set("longestRunSamples", detected.longest_run_samples);
  out.set("perSecond", detected.per_second);
  return out;
}

val declickReportToVal(const mastering::repair::DeclickReport& report) {
  val out = val::object();
  out.set("detected", declickDetectionToVal(report.detected));
  out.set("repairedRuns", report.repaired_runs);
  out.set("repairedSamples", report.repaired_samples);
  out.set("linkedRuns", report.linked_runs);
  out.set("lpcModelUsed", report.lpc_model_used);
  return out;
}

}  // namespace

// Declicks a stereo pair, selecting the repaired runs from the union of both
// channels' own detection: a common-mode click repaired on one side only would
// move the stereo image. Each channel's fill still comes from its own samples
// and its own AR model, which is why leftReport and rightReport genuinely
// differ. Calls the core directly rather than the C ABI, matching every other
// wrapper in this file -- sonare_c_mastering_repair.cpp is not part of the WASM
// binding sources, so a C-ABI call here would be the odd one out among its
// siblings.
val js_mastering_repair_declick_stereo(val left_samples, val right_samples,
                                       const val& sample_rate_val, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(left_samples, "left samples", right_samples, "right samples",
                               "masteringRepairDeclickStereo input", true);
  Audio left = loadValidatedAudio(left_samples, sample_rate);
  Audio right = loadValidatedAudio(right_samples, sample_rate);
  mastering::repair::DeclickConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg.threshold = repairFloatOption(options, "threshold", cfg.threshold);
    cfg.neighbor_ratio = repairFloatOption(options, "neighborRatio", cfg.neighbor_ratio);
    if (hasProperty(options, "maxClickSamples")) {
      const int v =
          repairIntOption(options, "maxClickSamples", static_cast<int>(cfg.max_click_samples));
      if (v <= 0) {
        throw sonare::SonareException(
            sonare::ErrorCode::InvalidParameter,
            "masteringRepairDeclickStereo: maxClickSamples must be positive");
      }
      cfg.max_click_samples = static_cast<size_t>(v);
    }
    cfg.lpc_order = repairIntOption(options, "lpcOrder", cfg.lpc_order);
    cfg.residual_ratio = repairFloatOption(options, "residualRatio", cfg.residual_ratio);
  }
  mastering::repair::DeclickStereoResult result =
      mastering::repair::declick_stereo(left, right, cfg);
  std::vector<float> left_out(result.left.data(), result.left.data() + result.left.size());
  std::vector<float> right_out(result.right.data(), result.right.data() + result.right.size());

  val out = val::object();
  out.set("left", vectorToFloat32Array(left_out));
  out.set("right", vectorToFloat32Array(right_out));
  out.set("leftReport", declickReportToVal(result.left_report));
  out.set("rightReport", declickReportToVal(result.right_report));
  return out;
}

namespace {

// Throws on an unknown name rather than falling back, matching the C ABI's own
// enum mapping, so there is no default to carry.
mastering::repair::DenoiseMode parseDenoiseMode(const std::string& name) {
  std::string s = name;
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "logmmse" || s == "log_mmse" || s == "lsa") {
    return mastering::repair::DenoiseMode::LogMmse;
  }
  if (s == "mmsestsa" || s == "mmse_stsa" || s == "stsa") {
    return mastering::repair::DenoiseMode::MmseStsa;
  }
  if (s == "spectralsubtraction" || s == "spectral_subtraction" || s == "ss") {
    return mastering::repair::DenoiseMode::SpectralSubtraction;
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown denoise mode: " + name);
}

mastering::repair::DenoiseNoiseEstimator parseDenoiseNoiseEstimator(const std::string& name) {
  std::string s = name;
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "quantile") return mastering::repair::DenoiseNoiseEstimator::Quantile;
  if (s == "mcra") return mastering::repair::DenoiseNoiseEstimator::Mcra;
  if (s == "imcra") return mastering::repair::DenoiseNoiseEstimator::Imcra;
  if (s == "spp") return mastering::repair::DenoiseNoiseEstimator::Spp;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown denoise noise estimator: " + name);
}

mastering::repair::DehumMode parseDehumMode(const std::string& name) {
  std::string s = name;
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "subtract") return mastering::repair::DehumMode::Subtract;
  if (s == "notch") return mastering::repair::DehumMode::Notch;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown dehum mode: " + name);
}

// Read a denoise options bag over `config`, leaving absent keys alone. The nFft
// and hopLength checks stay with each entry point, whose name they quote.
mastering::repair::DenoiseClassicalConfig readDenoiseConfig(
    const val& options, mastering::repair::DenoiseClassicalConfig config) {
  if (hasProperty(options, "mode")) {
    val value = val::undefined();
    if (repairOptionValue(options, "mode", &value)) {
      config.mode = parseDenoiseMode(value.as<std::string>());
    }
  }
  if (hasProperty(options, "noiseEstimator")) {
    val value = val::undefined();
    if (repairOptionValue(options, "noiseEstimator", &value)) {
      config.noise_estimator = parseDenoiseNoiseEstimator(value.as<std::string>());
    }
  }
  config.n_fft = repairIntOption(options, "nFft", config.n_fft);
  config.hop_length = repairIntOption(options, "hopLength", config.hop_length);
  config.dd_alpha = repairFloatOption(options, "ddAlpha", config.dd_alpha);
  config.reduction_db = repairFloatOption(options, "reductionDb", config.reduction_db);
  config.over_subtraction = repairFloatOption(options, "overSubtraction", config.over_subtraction);
  config.spectral_floor = repairFloatOption(options, "spectralFloor", config.spectral_floor);
  config.noise_estimation_quantile =
      repairFloatOption(options, "noiseEstimationQuantile", config.noise_estimation_quantile);
  config.speech_presence_gain =
      repairBoolOption(options, "speechPresenceGain", config.speech_presence_gain);
  config.gain_smoothing = repairBoolOption(options, "gainSmoothing", config.gain_smoothing);
  return config;
}

}  // namespace

val js_mastering_repair_denoise_classical(val samples, const val& sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  mastering::repair::DenoiseClassicalConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg = readDenoiseConfig(options, cfg);
  }
  if (cfg.n_fft <= 0 || (cfg.n_fft & (cfg.n_fft - 1)) != 0) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDenoiseClassical: nFft must be a positive power of two");
  }
  if (cfg.hop_length <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "masteringRepairDenoiseClassical: hopLength must be positive");
  }
  Audio result = mastering::repair::denoise_classical(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

namespace {

val noiseDetectionToVal(const mastering::repair::NoiseDetection& detected) {
  val out = val::object();
  out.set("floorDbfs", detected.floor_dbfs);
  std::vector<float> band_floor_dbfs(
      detected.band_floor_dbfs,
      detected.band_floor_dbfs + mastering::repair::kRepairNoiseBandCount);
  out.set("bandFloorDbfs", vectorToFloat32Array(band_floor_dbfs));
  return out;
}

val denoiseReportToVal(const mastering::repair::DenoiseReport& report) {
  val out = val::object();
  out.set("detected", noiseDetectionToVal(report.detected));
  out.set("meanReductionDb", report.mean_reduction_db);
  out.set("maxReductionDb", report.max_reduction_db);
  out.set("floorLimitedFraction", report.floor_limited_fraction);
  return out;
}

}  // namespace

// Denoises a stereo pair with one channel-linked gain mask. One `report` and
// not a per-channel pair: the mask is built from the channel-summed power and
// applied unchanged to both channels, so a pair would be two copies of one
// measurement. That summed power is also why `detected` is pair-level and
// absolute -- it reads about 3 dB above the mono entry point on the same
// material. Calls the core directly rather than the C ABI, matching every other
// wrapper in this file -- sonare_c_mastering_repair.cpp is not part of the WASM
// binding sources.
val js_mastering_repair_denoise_classical_stereo(val left_samples, val right_samples,
                                                 const val& sample_rate_val, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(left_samples, "left samples", right_samples, "right samples",
                               "masteringRepairDenoiseClassicalStereo input", true);
  Audio left = loadValidatedAudio(left_samples, sample_rate);
  Audio right = loadValidatedAudio(right_samples, sample_rate);
  mastering::repair::DenoiseClassicalConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg = readDenoiseConfig(options, cfg);
  }
  if (cfg.n_fft <= 0 || (cfg.n_fft & (cfg.n_fft - 1)) != 0) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDenoiseClassicalStereo: nFft must be a positive power of two");
  }
  if (cfg.hop_length <= 0) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDenoiseClassicalStereo: hopLength must be positive");
  }
  mastering::repair::DenoiseStereoResult result =
      mastering::repair::denoise_classical_stereo(left, right, cfg);
  std::vector<float> left_out(result.left.data(), result.left.data() + result.left.size());
  std::vector<float> right_out(result.right.data(), result.right.data() + result.right.size());

  val out = val::object();
  out.set("left", vectorToFloat32Array(left_out));
  out.set("right", vectorToFloat32Array(right_out));
  out.set("report", denoiseReportToVal(result.report));
  return out;
}

// Denoises any number of channels with one channel-linked gain mask: the
// N-channel form of the pair above, carrying the same guarantee over the whole
// set. One channel reproduces the mono entry bit for bit, two reproduce the
// stereo entry plane for plane. `report.detected` is the SET's and absolute, so
// N identical channels read 10*log10(N) above one; every other field is a
// fraction and does not move. Rejects an input shorter than nFft, the opposite
// of the dereverb linked entry, which pads. Calls the core directly rather than
// the C ABI, matching every other wrapper in this file -- so the per-channel
// validation the C ABI would have done is loadValidatedChannelSet's here.
val js_mastering_repair_denoise_classical_linked(val channels, const val& sample_rate_val,
                                                 val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const std::vector<Audio> loaded =
      loadValidatedChannelSet(channels, sample_rate, "masteringRepairDenoiseClassicalLinked");
  mastering::repair::DenoiseClassicalConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg = readDenoiseConfig(options, cfg);
  }
  if (cfg.n_fft <= 0 || (cfg.n_fft & (cfg.n_fft - 1)) != 0) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDenoiseClassicalLinked: nFft must be a positive power of two");
  }
  if (cfg.hop_length <= 0) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDenoiseClassicalLinked: hopLength must be positive");
  }
  const std::vector<const Audio*> pointers = channelSetPointers(loaded);
  std::vector<Audio> processed;
  const mastering::repair::DenoiseReport report = mastering::repair::denoise_classical_linked(
      pointers.data(), pointers.size(), &processed, cfg);

  val out = val::object();
  out.set("channels", channelSetToVal(processed));
  out.set("report", denoiseReportToVal(report));
  return out;
}

val js_mastering_repair_declip(val samples, const val& sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  mastering::repair::DeclipConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg.clip_threshold = repairFloatOption(options, "clipThreshold", cfg.clip_threshold);
    cfg.lpc_order = repairIntOption(options, "lpcOrder", cfg.lpc_order);
    cfg.iterations = repairIntOption(options, "iterations", cfg.iterations);
    cfg.lpc_blend = repairFloatOption(options, "lpcBlend", cfg.lpc_blend);
  }
  Audio result = mastering::repair::declip(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

namespace {

val declipDetectionToVal(const mastering::repair::ClipDetection& detected) {
  val out = val::object();
  out.set("sampleCount", detected.sample_count);
  out.set("sampleFraction", detected.sample_fraction);
  out.set("runCount", detected.run_count);
  out.set("longestRunSamples", detected.longest_run_samples);
  return out;
}

val declipReportToVal(const mastering::repair::DeclipReport& report) {
  val out = val::object();
  out.set("detected", declipDetectionToVal(report.detected));
  out.set("lpcReconstructedRuns", report.lpc_reconstructed_runs);
  out.set("interpolatedRuns", report.interpolated_runs);
  out.set("repairedSamples", report.repaired_samples);
  out.set("linkedRuns", report.linked_runs);
  return out;
}

}  // namespace

// Declips a stereo pair, reconstructing the union of both channels' own
// clipped runs: a channel with at least one clipped sample in a union run
// reconstructs the whole of it, so a plateau clipped in only one channel
// produces no linking, while overlapping runs of different extents do (the
// narrower channel is what reaches past its own clipped samples). Calls the
// core directly rather than the C ABI, matching every other wrapper in this
// file -- sonare_c_mastering_repair.cpp is not part of the WASM binding
// sources.
val js_mastering_repair_declip_stereo(val left_samples, val right_samples,
                                      const val& sample_rate_val, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(left_samples, "left samples", right_samples, "right samples",
                               "masteringRepairDeclipStereo input", true);
  Audio left = loadValidatedAudio(left_samples, sample_rate);
  Audio right = loadValidatedAudio(right_samples, sample_rate);
  mastering::repair::DeclipConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg.clip_threshold = repairFloatOption(options, "clipThreshold", cfg.clip_threshold);
    cfg.lpc_order = repairIntOption(options, "lpcOrder", cfg.lpc_order);
    cfg.iterations = repairIntOption(options, "iterations", cfg.iterations);
    cfg.lpc_blend = repairFloatOption(options, "lpcBlend", cfg.lpc_blend);
  }
  mastering::repair::DeclipStereoResult result = mastering::repair::declip_stereo(left, right, cfg);
  std::vector<float> left_out(result.left.data(), result.left.data() + result.left.size());
  std::vector<float> right_out(result.right.data(), result.right.data() + result.right.size());

  val out = val::object();
  out.set("left", vectorToFloat32Array(left_out));
  out.set("right", vectorToFloat32Array(right_out));
  out.set("leftReport", declipReportToVal(result.left_report));
  out.set("rightReport", declipReportToVal(result.right_report));
  return out;
}

namespace {

mastering::repair::DecrackleMode parseDecrackleMode(const std::string& name) {
  std::string s = name;
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "median") return mastering::repair::DecrackleMode::Median;
  if (s == "waveletshrinkage" || s == "wavelet_shrinkage" || s == "wavelet") {
    return mastering::repair::DecrackleMode::WaveletShrinkage;
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown decrackle mode: " + name);
}

mastering::repair::TrimSilenceMode parseTrimSilenceMode(const std::string& name) {
  std::string s = name;
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (s == "peak") return mastering::repair::TrimSilenceMode::Peak;
  if (s == "lufsgated" || s == "lufs_gated" || s == "lufs") {
    return mastering::repair::TrimSilenceMode::LufsGated;
  }
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "unknown trim silence mode: " + name);
}

// Read a trim options bag over `config`, leaving absent keys alone. `entry`
// names the caller in the paddingSamples message -- the one field here that is
// refused rather than defaulted, because the core field is a size_t and a
// negative count arrives past the validator's SIZE_MAX/2 bound rather than
// below zero.
mastering::repair::TrimSilenceConfig readTrimSilenceConfig(
    const val& options, mastering::repair::TrimSilenceConfig config, const char* entry) {
  if (options.isUndefined() || options.isNull()) return config;
  config.threshold = repairFloatOption(options, "threshold", config.threshold);
  if (hasProperty(options, "paddingSamples")) {
    const int v =
        repairIntOption(options, "paddingSamples", static_cast<int>(config.padding_samples));
    if (v < 0) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    std::string(entry) + ": paddingSamples must be non-negative");
    }
    config.padding_samples = static_cast<size_t>(v);
  }
  if (hasProperty(options, "mode")) {
    val value = val::undefined();
    if (repairOptionValue(options, "mode", &value)) {
      config.mode = parseTrimSilenceMode(value.as<std::string>());
    }
  }
  config.gate_lufs = repairFloatOption(options, "gateLufs", config.gate_lufs);
  config.window_ms = repairFloatOption(options, "windowMs", config.window_ms);
  return config;
}

}  // namespace

val js_mastering_repair_decrackle(val samples, const val& sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  mastering::repair::DecrackleConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg.threshold = repairFloatOption(options, "threshold", cfg.threshold);
    if (hasProperty(options, "mode")) {
      val value = val::undefined();
      if (repairOptionValue(options, "mode", &value)) {
        cfg.mode = parseDecrackleMode(value.as<std::string>());
      }
    }
    cfg.levels = repairIntOption(options, "levels", cfg.levels);
  }
  Audio result = mastering::repair::decrackle(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

namespace {

val crackleDetectionToVal(const mastering::repair::CrackleDetection& detected) {
  val out = val::object();
  out.set("sampleCount", detected.sample_count);
  out.set("sampleFraction", detected.sample_fraction);
  out.set("perSecond", detected.per_second);
  return out;
}

val decrackleReportToVal(const mastering::repair::DecrackleReport& report) {
  val out = val::object();
  out.set("detected", crackleDetectionToVal(report.detected));
  out.set("replacedSamples", report.replaced_samples);
  out.set("detailCoefficients", report.detail_coefficients);
  out.set("shrunkCoefficients", report.shrunk_coefficients);
  out.set("noiseSigma", report.noise_sigma);
  return out;
}

}  // namespace

// Decrackles a stereo pair, each channel on its own. Crackle is surface
// damage with no common event between the two channels, unlike declick's and
// declip's shared run selection, so there is nothing to link -- this
// entrypoint is two independent mono passes sharing one validated config.
// Calls the core directly rather than the C ABI, matching every other
// wrapper in this file -- sonare_c_mastering_repair.cpp is not part of the
// WASM binding sources.
val js_mastering_repair_decrackle_stereo(val left_samples, val right_samples,
                                         const val& sample_rate_val, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(left_samples, "left samples", right_samples, "right samples",
                               "masteringRepairDecrackleStereo input", true);
  Audio left = loadValidatedAudio(left_samples, sample_rate);
  Audio right = loadValidatedAudio(right_samples, sample_rate);
  mastering::repair::DecrackleConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg.threshold = repairFloatOption(options, "threshold", cfg.threshold);
    if (hasProperty(options, "mode")) {
      val value = val::undefined();
      if (repairOptionValue(options, "mode", &value)) {
        cfg.mode = parseDecrackleMode(value.as<std::string>());
      }
    }
    cfg.levels = repairIntOption(options, "levels", cfg.levels);
  }
  mastering::repair::DecrackleStereoResult result =
      mastering::repair::decrackle_stereo(left, right, cfg);
  std::vector<float> left_out(result.left.data(), result.left.data() + result.left.size());
  std::vector<float> right_out(result.right.data(), result.right.data() + result.right.size());

  val out = val::object();
  out.set("left", vectorToFloat32Array(left_out));
  out.set("right", vectorToFloat32Array(right_out));
  out.set("leftReport", decrackleReportToVal(result.left_report));
  out.set("rightReport", decrackleReportToVal(result.right_report));
  return out;
}

val js_mastering_repair_dehum(val samples, const val& sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  mastering::repair::DehumConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg.fundamental_hz = repairFloatOption(options, "fundamentalHz", cfg.fundamental_hz);
    cfg.harmonics = repairIntOption(options, "harmonics", cfg.harmonics);
    cfg.q = repairFloatOption(options, "q", cfg.q);
    cfg.adaptive = repairBoolOption(options, "adaptive", cfg.adaptive);
    cfg.search_range_hz = repairFloatOption(options, "searchRangeHz", cfg.search_range_hz);
    cfg.adaptation = repairFloatOption(options, "adaptation", cfg.adaptation);
    cfg.frame_size = repairIntOption(options, "frameSize", cfg.frame_size);
    cfg.pll_bandwidth = repairFloatOption(options, "pllBandwidth", cfg.pll_bandwidth);
    if (hasProperty(options, "mode")) {
      val value = val::undefined();
      if (repairOptionValue(options, "mode", &value)) {
        cfg.mode = parseDehumMode(value.as<std::string>());
      }
    }
  }
  Audio result = mastering::repair::dehum(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

namespace {

val humDetectionToVal(const mastering::repair::HumDetection& detected) {
  val out = val::object();
  out.set("fundamentalHz", detected.fundamental_hz);
  out.set("fundamentalProminence", detected.fundamental_prominence);
  out.set("harmonics", detected.harmonics);
  std::vector<float> harmonic_dbfs(detected.harmonic_dbfs,
                                   detected.harmonic_dbfs + mastering::repair::kDehumMaxHarmonics);
  out.set("harmonicDbfs", vectorToFloat32Array(harmonic_dbfs));
  return out;
}

val dehumReportToVal(const mastering::repair::DehumReport& report) {
  val out = val::object();
  out.set("detected", humDetectionToVal(report.detected));
  out.set("notchedHarmonics", report.notched_harmonics);
  out.set("appliedFundamentalHz", report.applied_fundamental_hz);
  out.set("fundamentalDriftHz", report.fundamental_drift_hz);
  return out;
}

}  // namespace

// Dehums a stereo pair. With `adaptive` set the two channels track one shared
// fundamental -- mains hum is one physical source, and tracking the channels
// apart would put the notches at two frequencies differing by whatever each
// channel's programme material pulled its own search to, an image shift the
// hum itself never had -- so both reports' appliedFundamentalHz and
// fundamentalDriftHz agree by construction while each detected still measures
// that channel's own input. With adaptive clear, the default, each channel
// runs its own fixed-frequency pass and nothing is shared. Calls the core
// directly rather than the C ABI, matching every other wrapper in this file --
// sonare_c_mastering_repair.cpp is not part of the WASM binding sources.
val js_mastering_repair_dehum_stereo(val left_samples, val right_samples,
                                     const val& sample_rate_val, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(left_samples, "left samples", right_samples, "right samples",
                               "masteringRepairDehumStereo input", true);
  Audio left = loadValidatedAudio(left_samples, sample_rate);
  Audio right = loadValidatedAudio(right_samples, sample_rate);
  mastering::repair::DehumConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg.fundamental_hz = repairFloatOption(options, "fundamentalHz", cfg.fundamental_hz);
    cfg.harmonics = repairIntOption(options, "harmonics", cfg.harmonics);
    cfg.q = repairFloatOption(options, "q", cfg.q);
    cfg.adaptive = repairBoolOption(options, "adaptive", cfg.adaptive);
    cfg.search_range_hz = repairFloatOption(options, "searchRangeHz", cfg.search_range_hz);
    cfg.adaptation = repairFloatOption(options, "adaptation", cfg.adaptation);
    cfg.frame_size = repairIntOption(options, "frameSize", cfg.frame_size);
    cfg.pll_bandwidth = repairFloatOption(options, "pllBandwidth", cfg.pll_bandwidth);
    if (hasProperty(options, "mode")) {
      val value = val::undefined();
      if (repairOptionValue(options, "mode", &value)) {
        cfg.mode = parseDehumMode(value.as<std::string>());
      }
    }
  }
  mastering::repair::DehumStereoResult result = mastering::repair::dehum_stereo(left, right, cfg);
  std::vector<float> left_out(result.left.data(), result.left.data() + result.left.size());
  std::vector<float> right_out(result.right.data(), result.right.data() + result.right.size());

  val out = val::object();
  out.set("left", vectorToFloat32Array(left_out));
  out.set("right", vectorToFloat32Array(right_out));
  out.set("leftReport", dehumReportToVal(result.left_report));
  out.set("rightReport", dehumReportToVal(result.right_report));
  return out;
}

val js_mastering_repair_dereverb_classical(val samples, const val& sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  mastering::repair::DereverbClassicalConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg = readDereverbConfig(options, cfg);
  }
  if (cfg.n_fft <= 0 || (cfg.n_fft & (cfg.n_fft - 1)) != 0) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDereverbClassical: nFft must be a positive power of two");
  }
  if (cfg.hop_length <= 0 || cfg.hop_length > cfg.n_fft) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDereverbClassical: hopLength must be in (0, nFft]");
  }
  Audio result = mastering::repair::dereverb_classical(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

namespace {

val reverbDetectionToVal(const mastering::repair::ReverbDetection& detected) {
  val out = val::object();
  out.set("lateDecayRatioDb", detected.late_decay_ratio_db);
  out.set("latePredictability", detected.late_predictability);
  return out;
}

val dereverbReportToVal(const mastering::repair::DereverbReport& report) {
  val out = val::object();
  out.set("detected", reverbDetectionToVal(report.detected));
  out.set("meanReductionDb", report.mean_reduction_db);
  out.set("suppressedFraction", report.suppressed_fraction);
  out.set("wpePredictorNorm", report.wpe_predictor_norm);
  return out;
}

}  // namespace

// Dereverberates a stereo pair with one channel-linked mask. One `report` and
// not a per-channel pair: the mask comes from the channel-summed power and the
// WPE stage accumulates over both channels, so a pair would be two copies of
// one measurement. Every field of it is a ratio or a fraction, so unlike the
// denoise pair nothing shifts with the channel count. An input shorter than
// nFft is padded rather than rejected, the opposite of that pair. Calls the
// core directly rather than the C ABI, matching every other wrapper in this
// file -- sonare_c_mastering_repair.cpp is not part of the WASM binding
// sources.
val js_mastering_repair_dereverb_classical_stereo(val left_samples, val right_samples,
                                                  const val& sample_rate_val, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(left_samples, "left samples", right_samples, "right samples",
                               "masteringRepairDereverbClassicalStereo input", true);
  Audio left = loadValidatedAudio(left_samples, sample_rate);
  Audio right = loadValidatedAudio(right_samples, sample_rate);
  mastering::repair::DereverbClassicalConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg = readDereverbConfig(options, cfg);
  }
  if (cfg.n_fft <= 0 || (cfg.n_fft & (cfg.n_fft - 1)) != 0) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDereverbClassicalStereo: nFft must be a positive power of two");
  }
  if (cfg.hop_length <= 0 || cfg.hop_length > cfg.n_fft) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDereverbClassicalStereo: hopLength must be in (0, nFft]");
  }
  mastering::repair::DereverbStereoResult result =
      mastering::repair::dereverb_classical_stereo(left, right, cfg);
  std::vector<float> left_out(result.left.data(), result.left.data() + result.left.size());
  std::vector<float> right_out(result.right.data(), result.right.data() + result.right.size());

  val out = val::object();
  out.set("left", vectorToFloat32Array(left_out));
  out.set("right", vectorToFloat32Array(right_out));
  out.set("report", dereverbReportToVal(result.report));
  return out;
}

// Dereverberates any number of channels with one channel-linked mask: the
// N-channel form of the pair above, with the WPE predictor set fitted over
// every channel's statistics rather than just two. One channel reproduces the
// mono entry bit for bit, two reproduce the stereo entry plane for plane. Every
// field of the report is a ratio or a fraction, so unlike the denoise linked
// entry nothing here moves with the channel count. An input shorter than nFft
// is padded rather than rejected, again the opposite of that entry. Calls the
// core directly rather than the C ABI, matching every other wrapper in this
// file -- so the per-channel validation the C ABI would have done is
// loadValidatedChannelSet's here.
val js_mastering_repair_dereverb_classical_linked(val channels, const val& sample_rate_val,
                                                  val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const std::vector<Audio> loaded =
      loadValidatedChannelSet(channels, sample_rate, "masteringRepairDereverbClassicalLinked");
  mastering::repair::DereverbClassicalConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg = readDereverbConfig(options, cfg);
  }
  if (cfg.n_fft <= 0 || (cfg.n_fft & (cfg.n_fft - 1)) != 0) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDereverbClassicalLinked: nFft must be a positive power of two");
  }
  if (cfg.hop_length <= 0 || cfg.hop_length > cfg.n_fft) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDereverbClassicalLinked: hopLength must be in (0, nFft]");
  }
  const std::vector<const Audio*> pointers = channelSetPointers(loaded);
  std::vector<Audio> processed;
  const mastering::repair::DereverbReport report = mastering::repair::dereverb_classical_linked(
      pointers.data(), pointers.size(), &processed, cfg);

  val out = val::object();
  out.set("channels", channelSetToVal(processed));
  out.set("report", dereverbReportToVal(report));
  return out;
}

val js_mastering_repair_dereverb_config_for_room(val estimate, val options) {
  if (estimate.isUndefined() || estimate.isNull()) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDereverbConfigForRoom: a room estimate is required");
  }
  // Read AND written: the caller's config is the base, and only the two fields
  // the measurement determines come back changed.
  mastering::repair::DereverbClassicalConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg = readDereverbConfig(options, cfg);
  }
  std::vector<float> rt60_bands;
  if (hasProperty(estimate, "rt60Bands")) {
    val bands = estimate["rt60Bands"];
    // Absent bands are an empty set, which the measurement treats as "did not
    // converge" rather than as a zero reverberation time.
    if (!bands.isUndefined() && !bands.isNull()) rt60_bands = float32ArrayToVector(bands);
  }
  // apply_room_measurement takes a non-positive or non-finite volume as "no
  // measurement" and leaves lateDelayMs at the caller's value, which is also
  // what the C ABI does with the same field.
  mastering::repair::apply_room_measurement(cfg, mid_frequency_rt60(rt60_bands),
                                            floatOption(estimate, "volume", 0.0f));

  val out = val::object();
  out.set("threshold", cfg.threshold);
  out.set("attenuation", cfg.attenuation);
  out.set("nFft", cfg.n_fft);
  out.set("hopLength", cfg.hop_length);
  out.set("t60Sec", cfg.t60_sec);
  out.set("lateDelayMs", cfg.late_delay_ms);
  out.set("overSubtraction", cfg.over_subtraction);
  out.set("spectralFloor", cfg.spectral_floor);
  out.set("wpeEnabled", cfg.wpe_enabled);
  out.set("wpeIterations", cfg.wpe_iterations);
  out.set("wpeTaps", cfg.wpe_taps);
  out.set("wpeStrength", cfg.wpe_strength);
  return out;
}

val js_mastering_repair_trim_silence(val samples, const val& sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  const mastering::repair::TrimSilenceConfig cfg = readTrimSilenceConfig(
      options, mastering::repair::TrimSilenceConfig{}, "masteringRepairTrimSilence");
  Audio result = mastering::repair::trim_silence(audio, cfg);
  std::vector<float> out(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out);
}

namespace {

val trimRangeToVal(const mastering::repair::TrimRange& range) {
  val out = val::object();
  out.set("first", range.first);
  out.set("lastExclusive", range.last_exclusive);
  return out;
}

val trimReportToVal(const mastering::repair::TrimReport& report) {
  val out = val::object();
  out.set("range", trimRangeToVal(report.range));
  out.set("removedHeadSamples", report.removed_head_samples);
  out.set("removedTailSamples", report.removed_tail_samples);
  return out;
}

// A trimmed channel can be empty, which no other repair stereo entry produces,
// and data() on an empty one may be null, so the emptiness is tested rather
// than the pointer arithmetic being left to define itself.
val trimmedChannelToVal(const Audio& channel) {
  if (channel.empty()) return vectorToFloat32Array({});
  return vectorToFloat32Array(std::vector<float>(channel.data(), channel.data() + channel.size()));
}

}  // namespace

// Unlike every other repair stereo entry this SHORTENS its input, and a pair in
// which neither channel carries signal comes back as two empty arrays and a
// success. Calls the core directly rather than the C ABI, as every wrapper in
// this file does -- sonare_c_mastering_repair.cpp is not a WASM binding source.
val js_mastering_repair_trim_silence_stereo(val left_samples, val right_samples,
                                            const val& sample_rate_val, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(left_samples, "left samples", right_samples, "right samples",
                               "masteringRepairTrimSilenceStereo input", true);
  Audio left = loadValidatedAudio(left_samples, sample_rate);
  Audio right = loadValidatedAudio(right_samples, sample_rate);
  const mastering::repair::TrimSilenceConfig cfg = readTrimSilenceConfig(
      options, mastering::repair::TrimSilenceConfig{}, "masteringRepairTrimSilenceStereo");
  mastering::repair::TrimSilenceStereoResult result =
      mastering::repair::trim_silence_stereo(left, right, cfg);

  val out = val::object();
  out.set("left", trimmedChannelToVal(result.left));
  out.set("right", trimmedChannelToVal(result.right));
  out.set("report", trimReportToVal(result.report));
  out.set("leftRange", trimRangeToVal(result.left_range));
  out.set("rightRange", trimRangeToVal(result.right_range));
  return out;
}

// ============================================================================
// Mastering — repair detection entry points
//
// These measure without repairing and allocate nothing. Like every wrapper
// above they call the core directly rather than the C ABI, so each validates on
// its own -- sonare_c_mastering_repair.cpp is not a WASM binding source.
// loadValidatedAudio refuses an empty buffer at every entry here, which flattens
// the core's split rule: detect_noise_floor and detect_reverb throw for an empty
// buffer while the other six hand back a zeroed detection or range.
// ============================================================================

val js_mastering_repair_detect_clicks(val samples, const val& sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  mastering::repair::DeclickConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg.threshold = repairFloatOption(options, "threshold", cfg.threshold);
    cfg.neighbor_ratio = repairFloatOption(options, "neighborRatio", cfg.neighbor_ratio);
    if (hasProperty(options, "maxClickSamples")) {
      const int v =
          repairIntOption(options, "maxClickSamples", static_cast<int>(cfg.max_click_samples));
      if (v <= 0) {
        throw sonare::SonareException(
            sonare::ErrorCode::InvalidParameter,
            "masteringRepairDetectClicks: maxClickSamples must be positive");
      }
      cfg.max_click_samples = static_cast<size_t>(v);
    }
    cfg.lpc_order = repairIntOption(options, "lpcOrder", cfg.lpc_order);
    cfg.residual_ratio = repairFloatOption(options, "residualRatio", cfg.residual_ratio);
  }
  return declickDetectionToVal(
      mastering::repair::detect_clicks(audio.data(), audio.size(), audio.sample_rate(), cfg));
}

// Refuses a buffer shorter than nFft, as the repair does; the dereverb detector
// below pads one instead.
val js_mastering_repair_detect_noise_floor(val samples, const val& sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  mastering::repair::DenoiseClassicalConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg = readDenoiseConfig(options, cfg);
  }
  if (cfg.n_fft <= 0 || (cfg.n_fft & (cfg.n_fft - 1)) != 0) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDetectNoiseFloor: nFft must be a positive power of two");
  }
  if (cfg.hop_length <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "masteringRepairDetectNoiseFloor: hopLength must be positive");
  }
  return noiseDetectionToVal(
      mastering::repair::detect_noise_floor(audio.data(), audio.size(), audio.sample_rate(), cfg));
}

// The bin grid the detector above reports bandFloorDbfs on. The core checks only
// positivity, so the power-of-two rule is applied here -- it lives in the C ABI
// layer, which this surface does not go through.
val js_mastering_repair_noise_band_bins(const val& n_fft_val, const val& sample_rate_val) {
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  if (n_fft <= 0 || (n_fft & (n_fft - 1)) != 0) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairNoiseBandBins: nFft must be a positive power of two");
  }
  if (sample_rate <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "masteringRepairNoiseBandBins: sampleRate must be positive");
  }
  std::vector<int> bins(mastering::common::kRepairNoiseBandCount + 1);
  mastering::common::repair_noise_band_bins(n_fft, sample_rate, bins.data());
  return vectorToInt32Array(bins);
}

// clipThreshold is the only field that reaches the result; the rest are read so
// the same config validation runs here as at the repair.
val js_mastering_repair_detect_clipping(val samples, const val& sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  mastering::repair::DeclipConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg.clip_threshold = repairFloatOption(options, "clipThreshold", cfg.clip_threshold);
    cfg.lpc_order = repairIntOption(options, "lpcOrder", cfg.lpc_order);
    cfg.iterations = repairIntOption(options, "iterations", cfg.iterations);
    cfg.lpc_blend = repairFloatOption(options, "lpcBlend", cfg.lpc_blend);
  }
  return declipDetectionToVal(
      mastering::repair::detect_clipping(audio.data(), audio.size(), audio.sample_rate(), cfg));
}

// Measured by the median criterion whatever `mode` says: wavelet shrinkage
// removes crackle without ever deciding a sample is crackle.
val js_mastering_repair_detect_crackle(val samples, const val& sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  mastering::repair::DecrackleConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg.threshold = repairFloatOption(options, "threshold", cfg.threshold);
    if (hasProperty(options, "mode")) {
      val value = val::undefined();
      if (repairOptionValue(options, "mode", &value)) {
        cfg.mode = parseDecrackleMode(value.as<std::string>());
      }
    }
    cfg.levels = repairIntOption(options, "levels", cfg.levels);
  }
  return crackleDetectionToVal(
      mastering::repair::detect_crackle(audio.data(), audio.size(), audio.sample_rate(), cfg));
}

// Always runs the estimation path, whatever `adaptive` says: the fixed path
// notches the configured frequency without ever looking for hum.
val js_mastering_repair_detect_hum(val samples, const val& sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  mastering::repair::DehumConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg.fundamental_hz = repairFloatOption(options, "fundamentalHz", cfg.fundamental_hz);
    cfg.harmonics = repairIntOption(options, "harmonics", cfg.harmonics);
    cfg.q = repairFloatOption(options, "q", cfg.q);
    cfg.adaptive = repairBoolOption(options, "adaptive", cfg.adaptive);
    cfg.search_range_hz = repairFloatOption(options, "searchRangeHz", cfg.search_range_hz);
    cfg.adaptation = repairFloatOption(options, "adaptation", cfg.adaptation);
    cfg.frame_size = repairIntOption(options, "frameSize", cfg.frame_size);
    cfg.pll_bandwidth = repairFloatOption(options, "pllBandwidth", cfg.pll_bandwidth);
    if (hasProperty(options, "mode")) {
      val value = val::undefined();
      if (repairOptionValue(options, "mode", &value)) {
        cfg.mode = parseDehumMode(value.as<std::string>());
      }
    }
  }
  return humDetectionToVal(
      mastering::repair::detect_hum(audio.data(), audio.size(), audio.sample_rate(), cfg));
}

// Pads a buffer shorter than nFft, as the repair does -- the opposite of the
// noise-floor detector above.
val js_mastering_repair_detect_reverb(val samples, const val& sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  mastering::repair::DereverbClassicalConfig cfg;
  if (!options.isUndefined() && !options.isNull()) {
    cfg = readDereverbConfig(options, cfg);
  }
  if (cfg.n_fft <= 0 || (cfg.n_fft & (cfg.n_fft - 1)) != 0) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        "masteringRepairDetectReverb: nFft must be a positive power of two");
  }
  if (cfg.hop_length <= 0 || cfg.hop_length > cfg.n_fft) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "masteringRepairDetectReverb: hopLength must be in (0, nFft]");
  }
  return reverbDetectionToVal(
      mastering::repair::detect_reverb(audio.data(), audio.size(), audio.sample_rate(), cfg));
}

// The range the repair would cut to, paddingSamples already inside it; a buffer
// with nothing above the threshold reports (length, length).
val js_mastering_repair_detect_trim_range(val samples, const val& sample_rate, val options) {
  Audio audio = loadValidatedAudio(samples, checkedIntFromVal(sample_rate, "sampleRate"));
  const mastering::repair::TrimSilenceConfig cfg = readTrimSilenceConfig(
      options, mastering::repair::TrimSilenceConfig{}, "masteringRepairDetectTrimRange");
  return trimRangeToVal(
      mastering::repair::detect_trim_range(audio.data(), audio.size(), audio.sample_rate(), cfg));
}

// The union of the two channels' ranges. A channel with nothing above the
// threshold contributes no edge, so a silent side leaves the other's range as
// it is rather than widening it to the buffer end.
val js_mastering_repair_detect_trim_range_stereo(val left_samples, val right_samples,
                                                 const val& sample_rate_val, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  validateWasmFloat32ArrayPair(left_samples, "left samples", right_samples, "right samples",
                               "masteringRepairDetectTrimRangeStereo input", true);
  Audio left = loadValidatedAudio(left_samples, sample_rate);
  Audio right = loadValidatedAudio(right_samples, sample_rate);
  const mastering::repair::TrimSilenceConfig cfg = readTrimSilenceConfig(
      options, mastering::repair::TrimSilenceConfig{}, "masteringRepairDetectTrimRangeStereo");
  return trimRangeToVal(mastering::repair::detect_trim_range_stereo(
      left.data(), right.data(), left.size(), left.sample_rate(), cfg));
}

void registerRepairBindings() {
  // Mastering — offline repair processors
  function("masteringRepairDeclick", &js_mastering_repair_declick);
  function("masteringRepairDeclickStereo", &js_mastering_repair_declick_stereo);
  function("masteringRepairDenoiseClassical", &js_mastering_repair_denoise_classical);
  function("masteringRepairDenoiseClassicalStereo", &js_mastering_repair_denoise_classical_stereo);
  function("masteringRepairDenoiseClassicalLinked", &js_mastering_repair_denoise_classical_linked);
  function("masteringRepairDeclip", &js_mastering_repair_declip);
  function("masteringRepairDeclipStereo", &js_mastering_repair_declip_stereo);
  function("masteringRepairDecrackle", &js_mastering_repair_decrackle);
  function("masteringRepairDecrackleStereo", &js_mastering_repair_decrackle_stereo);
  function("masteringRepairDehum", &js_mastering_repair_dehum);
  function("masteringRepairDehumStereo", &js_mastering_repair_dehum_stereo);
  function("masteringRepairDereverbClassical", &js_mastering_repair_dereverb_classical);
  function("masteringRepairDereverbClassicalStereo",
           &js_mastering_repair_dereverb_classical_stereo);
  function("masteringRepairDereverbClassicalLinked",
           &js_mastering_repair_dereverb_classical_linked);
  function("masteringRepairDereverbConfigForRoom", &js_mastering_repair_dereverb_config_for_room);
  function("masteringRepairTrimSilence", &js_mastering_repair_trim_silence);
  function("masteringRepairTrimSilenceStereo", &js_mastering_repair_trim_silence_stereo);
  // Mastering — repair detection, measuring without repairing
  function("masteringRepairDetectClicks", &js_mastering_repair_detect_clicks);
  function("masteringRepairDetectNoiseFloor", &js_mastering_repair_detect_noise_floor);
  function("masteringRepairDetectClipping", &js_mastering_repair_detect_clipping);
  function("masteringRepairDetectCrackle", &js_mastering_repair_detect_crackle);
  function("masteringRepairDetectHum", &js_mastering_repair_detect_hum);
  function("masteringRepairDetectReverb", &js_mastering_repair_detect_reverb);
  function("masteringRepairDetectTrimRange", &js_mastering_repair_detect_trim_range);
  function("masteringRepairDetectTrimRangeStereo", &js_mastering_repair_detect_trim_range_stereo);
  function("masteringRepairNoiseBandBins", &js_mastering_repair_noise_band_bins);
}

#endif  // __EMSCRIPTEN__
