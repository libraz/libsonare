/// @file feature_spectrogram.cpp
/// @brief Embind bindings for spectrogram, mel, inverse, and MFCC feature APIs.

#ifdef __EMSCRIPTEN__

#include "util/numeric_validation.h"
#include "util/resource_limits.h"
#include "wasm/bindings/common/common.h"

namespace {

void validate_positive(const char* fn_name, int value, const char* arg_name) {
  if (value <= 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(fn_name) + ": " + arg_name + " must be positive");
  }
}

void validate_sample_rate(const char* fn_name, int sample_rate) {
  if (sample_rate < sonare::kMinAudioSampleRate || sample_rate > sonare::kMaxAudioSampleRate) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(fn_name) + ": sample_rate out of supported range [" +
                              std::to_string(sonare::kMinAudioSampleRate) + ", " +
                              std::to_string(sonare::kMaxAudioSampleRate) + "]");
  }
}

void validate_mel_range(const char* fn_name, float fmin, float fmax, int sample_rate) {
  if (!std::isfinite(fmin) || !std::isfinite(fmax)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(fn_name) + ": fmin/fmax must be finite");
  }
  if (fmin < 0.0f || fmax < 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(fn_name) + ": fmin/fmax must be non-negative");
  }
  const float effective_fmax = fmax == 0.0f ? static_cast<float>(sample_rate) * 0.5f : fmax;
  if (effective_fmax <= fmin) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(fn_name) + ": fmax must be greater than fmin");
  }
}

std::vector<float> load_validated_matrix(const char* fn_name, val input, int rows, int frames,
                                         const char* data_name, const char* rows_name) {
  validate_positive(fn_name, rows, rows_name);
  validate_positive(fn_name, frames, "n_frames");

  std::size_t expected = 0;
  if (!sonare::numeric::checked_size_product(static_cast<std::size_t>(rows),
                                             static_cast<std::size_t>(frames),
                                             kMaxWasmFloat32Elements, &expected)) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string(fn_name) + ": matrix shape exceeds WASM budget");
  }
  const std::size_t actual = wasmFloat32ArrayLength(input, data_name);
  if (expected != actual) {
    throw SonareException(ErrorCode::InvalidParameter, std::string(fn_name) + ": " + data_name +
                                                           " length must equal rows * n_frames");
  }
  std::vector<float> data = float32ArrayToVector(input);
  for (size_t i = 0; i < data.size(); ++i) {
    if (!std::isfinite(data[i])) {
      throw SonareException(ErrorCode::InvalidParameter,
                            std::string(fn_name) + ": " + data_name + " contains NaN or Inf");
    }
  }
  return data;
}

}  // namespace

// ============================================================================
// Features - Spectrogram
// ============================================================================

val js_stft(val samples, int sample_rate, int n_fft, int hop_length) {
  Audio audio = loadValidatedAudio(samples, sample_rate);

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

  return out;
}

val js_stft_db(val samples, int sample_rate, int n_fft, int hop_length) {
  Audio audio = loadValidatedAudio(samples, sample_rate);

  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;

  Spectrogram spec = Spectrogram::compute(audio, config);

  val out = val::object();
  out.set("nBins", spec.n_bins());
  out.set("nFrames", spec.n_frames());
  out.set("db", vectorToFloat32Array(spec.to_db()));

  return out;
}

// ============================================================================
// Features - Mel Spectrogram
// ============================================================================

val js_mel_spectrogram(val samples, int sample_rate, int n_fft, int hop_length, int n_mels,
                       float fmin, float fmax, bool htk) {
  Audio audio = loadValidatedAudio(samples, sample_rate);

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

  // Power values
  std::vector<float> power_vec(mel.power_data(), mel.power_data() + mel.n_mels() * mel.n_frames());
  out.set("power", vectorToFloat32Array(power_vec));

  // dB values
  out.set("db", vectorToFloat32Array(mel.to_db()));

  return out;
}

val js_mfcc(val samples, int sample_rate, int n_fft, int hop_length, int n_mels, int n_mfcc,
            float fmin, float fmax, bool htk, float lifter) {
  Audio audio = loadValidatedAudio(samples, sample_rate);

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
  out.set("coefficients", vectorToFloat32Array(mfcc));

  return out;
}

// Inverse: Mel power spectrogram [n_mels x n_frames] -> STFT power spectrogram
// [(n_fft/2 + 1) x n_frames]. Mirrors feature::mel_to_stft.
//
// hop_length is intentionally absent: feature::mel_to_stft does not consume it.
val js_mel_to_stft(val mel_power, int n_mels, int n_frames, int sample_rate, int n_fft, float fmin,
                   float fmax, bool htk) {
  validate_sample_rate("melToStft", sample_rate);
  std::vector<float> data =
      load_validated_matrix("melToStft", mel_power, n_mels, n_frames, "melPower", "n_mels");
  validate_positive("melToStft", n_fft, "n_fft");
  validate_mel_range("melToStft", fmin, fmax, sample_rate);

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
  out.set("power", vectorToFloat32Array(stft));
  return out;
}

// Inverse: Mel power spectrogram -> audio via Griffin-Lim. Mirrors
// feature::mel_to_audio.
val js_mel_to_audio(val mel_power, int n_mels, int n_frames, int sample_rate, int n_fft,
                    int hop_length, float fmin, float fmax, int n_iter, bool htk) {
  validate_sample_rate("melToAudio", sample_rate);
  std::vector<float> data =
      load_validated_matrix("melToAudio", mel_power, n_mels, n_frames, "melPower", "n_mels");
  validate_positive("melToAudio", n_fft, "n_fft");
  validate_positive("melToAudio", hop_length, "hop_length");
  validate_positive("melToAudio", n_iter, "n_iter");
  validate_mel_range("melToAudio", fmin, fmax, sample_rate);

  MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk;

  Audio result = mel_to_audio(data.data(), n_mels, n_frames, config, n_iter, sample_rate);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

// Inverse: MFCC matrix [n_mfcc x n_frames] -> Mel power spectrogram.
// Mirrors feature::mfcc_to_mel.
val js_mfcc_to_mel(val mfcc, int n_mfcc, int n_frames, int n_mels) {
  std::vector<float> data =
      load_validated_matrix("mfccToMel", mfcc, n_mfcc, n_frames, "mfccCoefficients", "n_mfcc");
  validate_positive("mfccToMel", n_mels, "n_mels");

  std::vector<float> mel = mfcc_to_mel(data.data(), n_mfcc, n_frames, n_mels);

  val out = val::object();
  out.set("nMels", n_mels);
  out.set("nFrames", n_frames);
  out.set("power", vectorToFloat32Array(mel));
  return out;
}

// Inverse: MFCC matrix -> audio via Griffin-Lim. Mirrors feature::mfcc_to_audio.
val js_mfcc_to_audio(val mfcc, int n_mfcc, int n_frames, int n_mels, int sample_rate, int n_fft,
                     int hop_length, float fmin, float fmax, int n_iter, bool htk) {
  validate_sample_rate("mfccToAudio", sample_rate);
  std::vector<float> data =
      load_validated_matrix("mfccToAudio", mfcc, n_mfcc, n_frames, "mfccCoefficients", "n_mfcc");
  validate_positive("mfccToAudio", n_mels, "n_mels");
  validate_positive("mfccToAudio", n_fft, "n_fft");
  validate_positive("mfccToAudio", hop_length, "hop_length");
  validate_positive("mfccToAudio", n_iter, "n_iter");
  validate_mel_range("mfccToAudio", fmin, fmax, sample_rate);

  MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk;

  Audio result = mfcc_to_audio(data.data(), n_mfcc, n_frames, config, n_iter, sample_rate);
  std::vector<float> out_vec(result.data(), result.data() + result.size());
  return vectorToFloat32Array(out_vec);
}

val js_cqt_to_audio(val magnitude, int n_bins, int n_frames, int sample_rate, int hop_length,
                    float fmin, int bins_per_octave, int n_iter) {
  validate_sample_rate("cqtToAudio", sample_rate);
  std::vector<float> data =
      load_validated_matrix("cqtToAudio", magnitude, n_bins, n_frames, "magnitude", "n_bins");
  validate_positive("cqtToAudio", hop_length, "hop_length");
  validate_positive("cqtToAudio", bins_per_octave, "bins_per_octave");
  validate_positive("cqtToAudio", n_iter, "n_iter");
  if (!std::isfinite(fmin) || fmin <= 0.0f || n_iter > sonare::resource::kMaxGriffinLimIterations) {
    throw SonareException(ErrorCode::InvalidParameter, "cqtToAudio: invalid fmin or n_iter");
  }
  CqtConfig config;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.n_bins = n_bins;
  config.bins_per_octave = bins_per_octave;
  const Audio result = griffinlim_cqt(data.data(), n_bins, n_frames, config, sample_rate, n_iter);
  return vectorToFloat32Array(std::vector<float>(result.data(), result.data() + result.size()));
}

val js_vqt_to_audio(val magnitude, int n_bins, int n_frames, int sample_rate, int hop_length,
                    float fmin, int bins_per_octave, float gamma, int n_iter) {
  validate_sample_rate("vqtToAudio", sample_rate);
  std::vector<float> data =
      load_validated_matrix("vqtToAudio", magnitude, n_bins, n_frames, "magnitude", "n_bins");
  validate_positive("vqtToAudio", hop_length, "hop_length");
  validate_positive("vqtToAudio", bins_per_octave, "bins_per_octave");
  validate_positive("vqtToAudio", n_iter, "n_iter");
  if (!std::isfinite(fmin) || fmin <= 0.0f || !std::isfinite(gamma) || gamma < 0.0f ||
      n_iter > sonare::resource::kMaxGriffinLimIterations) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "vqtToAudio: invalid fmin, gamma, or n_iter");
  }
  VqtConfig config;
  config.hop_length = hop_length;
  config.fmin = fmin;
  config.n_bins = n_bins;
  config.bins_per_octave = bins_per_octave;
  config.gamma = gamma;
  const Audio result = griffinlim_vqt(data.data(), n_bins, n_frames, config, sample_rate, n_iter);
  return vectorToFloat32Array(std::vector<float>(result.data(), result.data() + result.size()));
}

void registerFeatureSpectrogramBindings() {
  function("stft", &js_stft);
  function("stftDb", &js_stft_db);
  function("melSpectrogram", &js_mel_spectrogram);
  function("mfcc", &js_mfcc);
  function("melToStft", &js_mel_to_stft);
  function("melToAudio", &js_mel_to_audio);
  function("mfccToMel", &js_mfcc_to_mel);
  function("mfccToAudio", &js_mfcc_to_audio);
  function("cqtToAudio", &js_cqt_to_audio);
  function("vqtToAudio", &js_vqt_to_audio);
}

#endif  // __EMSCRIPTEN__
