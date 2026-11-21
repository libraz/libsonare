/// @file feature_spectrogram.cpp
/// @brief Embind bindings for spectrogram, mel, inverse, and MFCC feature APIs.

#ifdef __EMSCRIPTEN__

#include "c_api/feature_input_domains.h"
#include "c_api/inverse_input_domains.h"
#include "core/spectrum.h"
#include "feature/cqt.h"
#include "feature/inverse.h"
#include "feature/mel_spectrogram.h"
#include "filters/mel.h"
#include "util/numeric_validation.h"
#include "wasm/bindings/common/common.h"

namespace {

void validate_positive(int value, const char* arg_name) {
  if (value <= 0) {
    throw WasmRangeError(std::string(arg_name) + " must be an integer in [1, 2147483647]");
  }
}

void validate_sample_rate(int sample_rate) {
  if (sample_rate < sonare::kMinAudioSampleRate || sample_rate > sonare::kMaxAudioSampleRate) {
    throw WasmRangeError("sampleRate out of supported range [" +
                         std::to_string(sonare::kMinAudioSampleRate) + ", " +
                         std::to_string(sonare::kMaxAudioSampleRate) + "]");
  }
}

/// Validates a JS matrix argument's size against its already domain-checked shape and copies it
/// out. The non-finite element case is deliberately absent: the core transforms this helper feeds
/// reject it themselves, so the rule has a single home shared with every other
/// surface.
std::vector<float> load_validated_matrix(val input, int rows, int frames, const char* data_name) {
  std::size_t expected = 0;
  if (!sonare::numeric::checked_size_product(static_cast<std::size_t>(rows),
                                             static_cast<std::size_t>(frames),
                                             kMaxWasmFloat32Elements, &expected)) {
    throw WasmRangeError("matrix shape exceeds WASM budget");
  }
  const std::size_t actual = wasmFloat32ArrayLength(input, data_name);
  if (expected != actual) {
    throw WasmRangeError(std::string(data_name) + " length must equal rows * nFrames");
  }
  return float32ArrayToVector(input, data_name);
}

}  // namespace

// ============================================================================
// Features - Spectrogram
// ============================================================================

// A true `db` also returns the magnitude in decibels as `db`; undefined/null (or false) omits it.
val js_stft(val samples, const val& sample_rate_val, const val& n_fft_val,
            const val& hop_length_val, const val& db_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kStftHopDomain);
  const bool with_db = optionalBool(db_val, "db").value_or(false);
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);

  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, config);

  val out = val::object();
  out.set("nBins", spec.n_bins());
  out.set("nFrames", spec.n_frames());
  out.set("nFft", spec.n_fft());
  out.set("hopLength", spec.hop_length());
  out.set("sampleRate", spec.sample_rate());
  out.set("magnitude", vectorToFloat32Array(spec.magnitude()));
  out.set("power", vectorToFloat32Array(spec.power()));
  if (with_db) {
    out.set("db", vectorToFloat32Array(spec.to_db()));
  }

  return out;
}

// ============================================================================
// Features - Mel Spectrogram
// ============================================================================

val js_mel_spectrogram(val samples, const val& sample_rate_val, const val& n_fft_val,
                       const val& hop_length_val, const val& n_mels_val, const val& fmin_val,
                       const val& fmax_val, bool htk) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kStftHopDomain);
  const int n_mels = domainNumberFromVal<int>(n_mels_val, "nMels", kMelBandsDomain);
  const float fmin = checkedFloatFromVal(fmin_val, "fmin");
  const float fmax = checkedFloatFromVal(fmax_val, "fmax");
  validate_mel_bounds(fmin, fmax);
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);

  MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk;

  MelSpectrogram mel = MelSpectrogram::compute(audio, config);

  val out = val::object();
  out.set("nMels", mel.n_mels());
  out.set("nFrames", mel.n_frames());
  out.set("sampleRate", mel.sample_rate());
  out.set("hopLength", mel.hop_length());
  out.set("nFft", config.n_fft);
  out.set("fmin", config.fmin);
  out.set("fmax", config.resolved_fmax(mel.sample_rate()));
  out.set("htk", config.htk);
  out.set("isDb", false);

  // Power values
  std::vector<float> power_vec(mel.power_data(), mel.power_data() + mel.n_mels() * mel.n_frames());
  out.set("power", vectorToFloat32Array(power_vec));

  // dB values
  out.set("db", vectorToFloat32Array(mel.to_db()));

  return out;
}

val js_mfcc(val samples, const val& sample_rate_val, const val& n_fft_val,
            const val& hop_length_val, const val& n_mels_val, const val& n_mfcc_val,
            const val& fmin_val, const val& fmax_val, bool htk, const val& lifter_val) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kStftHopDomain);
  const int n_mels = domainNumberFromVal<int>(n_mels_val, "nMels", kMelBandsDomain);
  const int n_mfcc = domainNumberFromVal<int>(n_mfcc_val, "nMfcc", kMfccCountDomain);
  const float fmin = checkedFloatFromVal(fmin_val, "fmin");
  const float fmax = checkedFloatFromVal(fmax_val, "fmax");
  const float lifter = checkedFloatFromVal(lifter_val, "lifter");
  validate_mel_bounds(fmin, fmax);
  Audio audio = loadValidatedAudio("samples", samples, sample_rate);

  MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk;

  MelSpectrogram mel = MelSpectrogram::compute(audio, config);
  std::vector<float> mfcc = mel.mfcc(n_mfcc, lifter);

  val out = val::object();
  out.set("nMfcc", n_mfcc);
  out.set("nFrames", mel.n_frames());
  out.set("sampleRate", mel.sample_rate());
  out.set("hopLength", mel.hop_length());
  out.set("nFft", config.n_fft);
  out.set("nMels", mel.n_mels());
  out.set("fmin", config.fmin);
  out.set("fmax", config.resolved_fmax(mel.sample_rate()));
  out.set("htk", config.htk);
  out.set("isDb", false);
  out.set("lifter", lifter);
  out.set("coefficients", vectorToFloat32Array(mfcc));

  return out;
}

val js_mel_delta(val features, const val& n_features_val, const val& n_frames_val,
                 const val& width_val) {
  const int n_features =
      domainNumberFromVal<int>(n_features_val, "nFeatures", sonare_c_detail::kMelDeltaShapeDomain);
  const int n_frames =
      domainNumberFromVal<int>(n_frames_val, "nFrames", sonare_c_detail::kMelDeltaShapeDomain);
  const int width = checkedIntFromVal(width_val, "width");
  std::vector<float> data = load_validated_matrix(features, n_features, n_frames, "features");
  return vectorToFloat32Array(MelSpectrogram::delta(data.data(), n_features, n_frames, width));
}

val js_reassigned_spectrogram(val samples, const val& sample_rate_val, const val& n_fft_val,
                              const val& hop_length_val, const val& ref_power_val, bool fill_nan) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = domainNumberFromVal<int>(hop_length_val, "hopLength", kStftHopDomain);
  const float ref_power = checkedFloatFromVal(ref_power_val, "refPower");
  const Audio audio = loadValidatedAudio("samples", samples, sample_rate);
  if (ref_power < 0.0f) {
    throw WasmRangeError("refPower must be at least 0");
  }
  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  const ReassignedSpectrogram result = reassigned_spectrogram(audio, config, ref_power, fill_nan);
  const int n_bins = n_fft / 2 + 1;
  val out = val::object();
  out.set("nBins", n_bins);
  out.set("nFrames", static_cast<int>(result.magnitude.size() / static_cast<size_t>(n_bins)));
  out.set("magnitude", vectorToFloat32Array(result.magnitude));
  out.set("times", vectorToFloat32Array(result.times));
  out.set("frequencies", vectorToFloat32Array(result.frequencies));
  return out;
}

// Inverse: Mel power spectrogram [n_mels x n_frames] -> STFT magnitude spectrogram
// [(n_fft/2 + 1) x n_frames]. Mirrors feature::mel_to_stft.
//
// hop_length is intentionally absent: feature::mel_to_stft does not consume it.
val js_mel_to_stft(val mel_power, const val& n_mels_val, const val& n_frames_val,
                   const val& sample_rate_val, const val& n_fft_val, const val& fmin_val,
                   const val& fmax_val, bool htk) {
  const int n_mels = domainNumberFromVal<int>(n_mels_val, "nMels", kInverseMelBandsDomain);
  const int n_frames = domainNumberFromVal<int>(n_frames_val, "nFrames", kMelToStftFramesDomain);
  const int sample_rate =
      domainNumberFromVal<int>(sample_rate_val, "sampleRate", kInverseSampleRateDomain);
  const int n_fft = domainNumberFromVal<int>(n_fft_val, "nFft", kMelToStftFftSizeDomain);
  const float fmin = checkedFloatFromVal(fmin_val, "fmin");
  const float fmax = checkedFloatFromVal(fmax_val, "fmax");
  std::vector<float> data = load_validated_matrix(mel_power, n_mels, n_frames, "power");
  validate_mel_bounds(fmin, fmax);

  MelConfig config;
  config.n_fft = n_fft;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk;

  std::vector<float> stft = mel_to_stft(data.data(), n_mels, n_frames, config, sample_rate);

  val out = val::object();
  out.set("nBins", n_fft / 2 + 1);
  out.set("nFrames", n_frames);
  out.set("magnitude", vectorToFloat32Array(stft));
  return out;
}

// Inverse: Mel power spectrogram -> audio via Griffin-Lim. Mirrors
// feature::mel_to_audio.
val js_mel_to_audio(val mel_power, const val& n_mels_val, const val& n_frames_val,
                    const val& sample_rate_val, const val& n_fft_val, const val& hop_length_val,
                    const val& fmin_val, const val& fmax_val, const val& n_iter_val, bool htk) {
  const int n_mels = domainNumberFromVal<int>(n_mels_val, "nMels", kInverseMelBandsDomain);
  const int n_frames = domainNumberFromVal<int>(n_frames_val, "nFrames", kMelToStftFramesDomain);
  const int sample_rate =
      domainNumberFromVal<int>(sample_rate_val, "sampleRate", kInverseAudioSampleRateDomain);
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = resynthesisHopFromVal(hop_length_val);
  const int n_iter = checkedIntFromVal(n_iter_val, "nIter");
  const float fmin = checkedFloatFromVal(fmin_val, "fmin");
  const float fmax = checkedFloatFromVal(fmax_val, "fmax");
  std::vector<float> data = load_validated_matrix(mel_power, n_mels, n_frames, "power");
  validate_mel_bounds(fmin, fmax);

  MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk;

  Audio result = mel_to_audio(data.data(), n_mels, n_frames, config, n_iter, sample_rate);
  return vectorToFloat32Array(result.data(), result.size());
}

val js_griffin_lim(val magnitude, const val& n_bins_val, const val& n_frames_val,
                   const val& sample_rate_val, const val& n_fft_val, const val& hop_length_val,
                   const val& n_iter_val, const val& momentum_val) {
  const int n_bins = domainNumberFromVal<int>(n_bins_val, "nBins",
                                              sonare_c_detail::kInverseSpectrogramExtentDomain);
  const int n_frames = domainNumberFromVal<int>(n_frames_val, "nFrames",
                                                sonare_c_detail::kInverseSpectrogramExtentDomain);
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = resynthesisHopFromVal(hop_length_val);
  const int n_iter =
      domainNumberFromVal<int>(n_iter_val, "nIter", sonare_c_detail::kGriffinLimIterationsDomain);
  const float momentum = checkedFloatFromVal(momentum_val, "momentum");
  validate_sample_rate(sample_rate);
  std::vector<float> data = load_validated_matrix(magnitude, n_bins, n_frames, "magnitude");
  if (n_bins != n_fft / 2 + 1 || momentum < 0.0f || momentum >= 1.0f) {
    throw WasmRangeError("invalid shape or options");
  }
  GriffinLimConfig config;
  config.n_iter = n_iter;
  config.momentum = momentum;
  const Audio result =
      griffin_lim(data.data(), n_bins, n_frames, n_fft, hop_length, sample_rate, config);
  return vectorToFloat32Array(result.data(), result.size());
}

// Inverse: MFCC matrix [n_mfcc x n_frames] -> Mel power spectrogram.
// Mirrors feature::mfcc_to_mel.
val js_mfcc_to_mel(val mfcc, const val& n_mfcc_val, const val& n_frames_val, const val& n_mels_val,
                   const val& lifter_val) {
  const int n_mfcc = domainNumberFromVal<int>(n_mfcc_val, "nMfcc", kInverseMfccCountDomain);
  const int n_frames = domainNumberFromVal<int>(n_frames_val, "nFrames", kInverseFramesDomain);
  const int n_mels = checkedIntFromVal(n_mels_val, "nMels");
  const float lifter = checkedFloatFromVal(lifter_val, "lifter");
  std::vector<float> data = load_validated_matrix(mfcc, n_mfcc, n_frames, "coefficients");

  std::vector<float> mel = mfcc_to_mel(data.data(), n_mfcc, n_frames, n_mels, lifter);

  val out = val::object();
  out.set("nMels", n_mels);
  out.set("nFrames", n_frames);
  out.set("power", vectorToFloat32Array(mel));
  return out;
}

// Inverse: MFCC matrix -> audio via Griffin-Lim. Mirrors feature::mfcc_to_audio.
val js_mfcc_to_audio(val mfcc, const val& n_mfcc_val, const val& n_frames_val,
                     const val& n_mels_val, const val& sample_rate_val, const val& n_fft_val,
                     const val& hop_length_val, const val& fmin_val, const val& fmax_val,
                     const val& n_iter_val, bool htk, const val& lifter_val) {
  const int n_mfcc = domainNumberFromVal<int>(n_mfcc_val, "nMfcc", kInverseMfccCountDomain);
  const int n_frames = domainNumberFromVal<int>(n_frames_val, "nFrames", kMelToStftFramesDomain);
  const int n_mels = checkedIntFromVal(n_mels_val, "nMels");
  const int sample_rate =
      domainNumberFromVal<int>(sample_rate_val, "sampleRate", kInverseAudioSampleRateDomain);
  const int n_fft = checkedIntFromVal(n_fft_val, "nFft");
  const int hop_length = resynthesisHopFromVal(hop_length_val);
  const int n_iter = checkedIntFromVal(n_iter_val, "nIter");
  const float fmin = checkedFloatFromVal(fmin_val, "fmin");
  const float fmax = checkedFloatFromVal(fmax_val, "fmax");
  const float lifter = checkedFloatFromVal(lifter_val, "lifter");
  std::vector<float> data = load_validated_matrix(mfcc, n_mfcc, n_frames, "coefficients");
  validate_mel_bounds(fmin, fmax);

  MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk;

  Audio result = mfcc_to_audio(data.data(), n_mfcc, n_frames, config, n_iter, sample_rate, lifter);
  return vectorToFloat32Array(result.data(), result.size());
}

val js_cqt_to_audio(val magnitude, const val& n_bins_val, const val& n_frames_val,
                    const val& sample_rate_val, const val& hop_length_val, const val& fmin_val,
                    const val& bins_per_octave_val, const val& n_iter_val) {
  const int n_bins = domainNumberFromVal<int>(n_bins_val, "nBins", kCqtCountDomain);
  const int n_frames = domainNumberFromVal<int>(n_frames_val, "nFrames",
                                                sonare_c_detail::kInverseSpectrogramExtentDomain);
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  const int bins_per_octave = checkedIntFromVal(bins_per_octave_val, "binsPerOctave");
  const int n_iter =
      domainNumberFromVal<int>(n_iter_val, "nIter", sonare_c_detail::kGriffinLimIterationsDomain);
  const float fmin = checkedFloatFromVal(fmin_val, "fminHz");
  validate_sample_rate(sample_rate);
  std::vector<float> data = load_validated_matrix(magnitude, n_bins, n_frames, "magnitude");
  validate_positive(hop_length, "hopLength");
  validate_positive(bins_per_octave, "binsPerOctave");
  validate_cqt_fmin(fmin);
  CqtConfig config;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.n_bins = n_bins;
  config.bins_per_octave = bins_per_octave;
  const Audio result = sonare::util::check::with_camel_paths(
      [&] {
        return sonare::detail::with_fixed_filter_scale({"fmin", "bins_per_octave"}, "sr", [&] {
          return griffinlim_cqt(data.data(), n_bins, n_frames, config, sample_rate, n_iter);
        });
      },
      kCqtRequestKeys);
  return vectorToFloat32Array(result.data(), result.size());
}

val js_vqt_to_audio(val magnitude, const val& n_bins_val, const val& n_frames_val,
                    const val& sample_rate_val, const val& hop_length_val, const val& fmin_val,
                    const val& bins_per_octave_val, float gamma, const val& n_iter_val) {
  const int n_bins = domainNumberFromVal<int>(n_bins_val, "nBins", kCqtCountDomain);
  const int n_frames = domainNumberFromVal<int>(n_frames_val, "nFrames",
                                                sonare_c_detail::kInverseSpectrogramExtentDomain);
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const int hop_length = checkedIntFromVal(hop_length_val, "hopLength");
  const int bins_per_octave = checkedIntFromVal(bins_per_octave_val, "binsPerOctave");
  const int n_iter =
      domainNumberFromVal<int>(n_iter_val, "nIter", sonare_c_detail::kGriffinLimIterationsDomain);
  const float fmin = checkedFloatFromVal(fmin_val, "fminHz");
  validate_sample_rate(sample_rate);
  std::vector<float> data = load_validated_matrix(magnitude, n_bins, n_frames, "magnitude");
  validate_positive(hop_length, "hopLength");
  validate_positive(bins_per_octave, "binsPerOctave");
  validate_cqt_fmin(fmin);
  validate_vqt_gamma(gamma);
  VqtConfig config;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.n_bins = n_bins;
  config.bins_per_octave = bins_per_octave;
  config.gamma = gamma;
  const Audio result = sonare::util::check::with_camel_paths(
      [&] {
        return sonare::detail::with_fixed_filter_scale(
            {"fmin", "bins_per_octave", "gamma"}, "sr", [&] {
              return griffinlim_vqt(data.data(), n_bins, n_frames, config, sample_rate, n_iter);
            });
      },
      kCqtRequestKeys);
  return vectorToFloat32Array(result.data(), result.size());
}

void registerFeatureSpectrogramBindings() {
  function("stft", &js_stft);
  function("melSpectrogram", &js_mel_spectrogram);
  function("mfcc", &js_mfcc);
  function("melDelta", &js_mel_delta);
  function("reassignedSpectrogram", &js_reassigned_spectrogram);
  function("melToStft", &js_mel_to_stft);
  function("melToAudio", &js_mel_to_audio);
  function("griffinLim", &js_griffin_lim);
  function("mfccToMel", &js_mfcc_to_mel);
  function("mfccToAudio", &js_mfcc_to_audio);
  function("cqtToAudio", &js_cqt_to_audio);
  function("vqtToAudio", &js_vqt_to_audio);
}

#endif  // __EMSCRIPTEN__
