/// @file feature_spectrogram.cpp
/// @brief Embind bindings for spectrogram, mel, inverse, and MFCC feature APIs.

#ifdef __EMSCRIPTEN__

#include "common.h"

namespace {

void validate_positive(const char* fn_name, int value, const char* arg_name) {
  if (value <= 0) {
    throw std::invalid_argument(std::string(fn_name) + ": " + arg_name + " must be positive");
  }
}

void validate_sample_rate(const char* fn_name, int sample_rate) {
  if (sample_rate < 8000 || sample_rate > 384000) {
    throw std::invalid_argument(std::string(fn_name) +
                                ": sample_rate out of supported range [8000, 384000]");
  }
}

void validate_mel_range(const char* fn_name, float fmin, float fmax, int sample_rate) {
  if (!std::isfinite(fmin) || !std::isfinite(fmax)) {
    throw std::invalid_argument(std::string(fn_name) + ": fmin/fmax must be finite");
  }
  if (fmin < 0.0f || fmax < 0.0f) {
    throw std::invalid_argument(std::string(fn_name) + ": fmin/fmax must be non-negative");
  }
  const float effective_fmax = fmax == 0.0f ? static_cast<float>(sample_rate) * 0.5f : fmax;
  if (effective_fmax <= fmin) {
    throw std::invalid_argument(std::string(fn_name) + ": fmax must be greater than fmin");
  }
}

void validate_matrix(const char* fn_name, const std::vector<float>& data, int rows, int frames,
                     const char* data_name, const char* rows_name) {
  validate_positive(fn_name, rows, rows_name);
  validate_positive(fn_name, frames, "n_frames");

  const auto expected = static_cast<size_t>(rows) * static_cast<size_t>(frames);
  if (expected != data.size()) {
    throw std::invalid_argument(std::string(fn_name) + ": " + data_name +
                                " length must equal rows * n_frames");
  }
  for (size_t i = 0; i < data.size(); ++i) {
    if (!std::isfinite(data[i])) {
      throw std::invalid_argument(std::string(fn_name) + ": " + data_name + " contains NaN or Inf");
    }
  }
}

}  // namespace

// ============================================================================
// Features - Spectrogram
// ============================================================================

val js_stft(val samples, int sample_rate, int n_fft, int hop_length) {
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

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
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

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
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

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
            float fmin, float fmax, bool htk) {
  std::vector<float> data = float32ArrayToVector(samples);
  validate_offline_audio_input(data.data(), data.size(), sample_rate);
  Audio audio = Audio::from_buffer(data.data(), data.size(), sample_rate);

  MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  config.n_mels = n_mels;
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk;

  MelSpectrogram mel = MelSpectrogram::compute(audio, config);
  std::vector<float> mfcc = mel.mfcc(n_mfcc);

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
  std::vector<float> data = float32ArrayToVector(mel_power);
  validate_sample_rate("melToStft", sample_rate);
  validate_matrix("melToStft", data, n_mels, n_frames, "melPower", "n_mels");
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
  std::vector<float> data = float32ArrayToVector(mel_power);
  validate_sample_rate("melToAudio", sample_rate);
  validate_matrix("melToAudio", data, n_mels, n_frames, "melPower", "n_mels");
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
  std::vector<float> data = float32ArrayToVector(mfcc);
  validate_matrix("mfccToMel", data, n_mfcc, n_frames, "mfccCoefficients", "n_mfcc");
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
  std::vector<float> data = float32ArrayToVector(mfcc);
  validate_sample_rate("mfccToAudio", sample_rate);
  validate_matrix("mfccToAudio", data, n_mfcc, n_frames, "mfccCoefficients", "n_mfcc");
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

void registerFeatureSpectrogramBindings() {
  function("stft", &js_stft);
  function("stftDb", &js_stft_db);
  function("melSpectrogram", &js_mel_spectrogram);
  function("mfcc", &js_mfcc);
  function("melToStft", &js_mel_to_stft);
  function("melToAudio", &js_mel_to_audio);
  function("mfccToMel", &js_mfcc_to_mel);
  function("mfccToAudio", &js_mfcc_to_audio);
}

#endif  // __EMSCRIPTEN__
