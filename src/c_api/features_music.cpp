#include "c_api/features_internal.h"

namespace {

SonareError fill_chroma_result(const Chroma& chroma, SonareChromaResult* out) {
  out->n_chroma = chroma.n_chroma();
  out->n_frames = chroma.n_frames();
  out->sample_rate = chroma.sample_rate();
  out->hop_length = chroma.hop_length();

  const size_t total = static_cast<size_t>(chroma.n_chroma()) * chroma.n_frames();
  if (total > 0) {
    std::unique_ptr<float[]> features(new float[total]);
    std::memcpy(features.get(), chroma.data(), total * sizeof(float));
    out->features = release_array(features);
  }

  auto mean = chroma.mean_energy();
  if (!mean.empty()) {
    std::unique_ptr<float[]> mean_out(new float[mean.size()]);
    std::memcpy(mean_out.get(), mean.data(), mean.size() * sizeof(float));
    out->mean_energy = release_array(mean_out);
  }
  return SONARE_OK;
}

}  // namespace

// Features - Onset
// ============================================================================

SonareError sonare_onset_strength(const float* samples, size_t length, int sr, int n_fft,
                                  int hop_length, int n_mels, float** out, size_t* out_length) {
  if (!out || !out_length) return SONARE_ERROR_INVALID_PARAMETER;

  *out = nullptr;
  *out_length = 0;

  return run_offline(samples, length, sr, [&](const Audio& audio) -> SonareError {
    MelConfig mel_config;
    mel_config.n_fft = n_fft;
    mel_config.hop_length = hop_length;
    mel_config.n_mels = n_mels;
    std::vector<float> env = compute_onset_strength(audio, mel_config, OnsetConfig());

    *out_length = env.size();
    if (env.empty()) return SONARE_OK;
    std::unique_ptr<float[]> data(new float[env.size()]);
    std::memcpy(data.get(), env.data(), env.size() * sizeof(float));
    *out = release_array(data);
    return SONARE_OK;
  });
}

SonareError sonare_onset_strength_multi(const float* samples, size_t length, int sr, int n_fft,
                                        int hop_length, int n_mels, int n_bands, float** out,
                                        size_t* out_length, int* out_n_frames) {
  if (!out || !out_length || !out_n_frames) return SONARE_ERROR_INVALID_PARAMETER;
  if (n_bands <= 0) return SONARE_ERROR_INVALID_PARAMETER;

  *out = nullptr;
  *out_length = 0;
  *out_n_frames = 0;

  return run_offline(samples, length, sr, [&](const Audio& audio) -> SonareError {
    MelConfig mel_config;
    mel_config.n_fft = n_fft;
    mel_config.hop_length = hop_length;
    mel_config.n_mels = n_mels;
    MelSpectrogram mel = MelSpectrogram::compute(audio, mel_config);
    std::vector<float> env = onset_strength_multi(mel, n_bands, OnsetConfig());

    *out_n_frames = mel.n_frames();
    *out_length = env.size();
    if (env.empty()) return SONARE_OK;
    std::unique_ptr<float[]> data(new float[env.size()]);
    std::memcpy(data.get(), env.data(), env.size() * sizeof(float));
    *out = release_array(data);
    return SONARE_OK;
  });
}

// Features - Chroma
// ============================================================================

SonareError sonare_chroma(const float* samples, size_t length, int sample_rate, int n_fft,
                          int hop_length, SonareChromaResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;

  out->features = nullptr;
  out->mean_energy = nullptr;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    ChromaConfig config;
    config.n_fft = n_fft;
    config.hop_length = hop_length;
    Chroma chroma = Chroma::compute(audio, config);

    return fill_chroma_result(chroma, out);
  });
}

SonareError sonare_chroma_cens(const float* samples, size_t length, int sample_rate, int hop_length,
                               int n_chroma, SonareChromaResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (hop_length <= 0 || n_chroma <= 0) return SONARE_ERROR_INVALID_PARAMETER;

  *out = {};

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    ChromaCensConfig config;
    config.base.cqt.hop_length = hop_length;
    config.base.n_chroma = n_chroma;
    Chroma chroma = chroma_cens(audio, config);
    return fill_chroma_result(chroma, out);
  });
}

SonareError sonare_bass_chroma(const float* samples, size_t length, int sample_rate, int hop_length,
                               int n_chroma, SonareChromaResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (hop_length <= 0 || n_chroma <= 0) return SONARE_ERROR_INVALID_PARAMETER;

  *out = {};

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    BassChromaConfig config;
    config.cqt.hop_length = hop_length;
    config.n_chroma = n_chroma;
    Chroma chroma = bass_chroma(audio, config);
    return fill_chroma_result(chroma, out);
  });
}

SonareError sonare_nnls_chroma(const float* samples, size_t length, int sr, float** out,
                               size_t* out_length, int* out_n_frames) {
  if (!out || !out_length || !out_n_frames) return SONARE_ERROR_INVALID_PARAMETER;

  *out = nullptr;

  return run_offline(samples, length, sr, [&](const Audio& audio) -> SonareError {
    Chroma chroma = nnls_chroma(audio, make_fast_nnls_chroma_config());

    *out_n_frames = chroma.n_frames();
    size_t total = static_cast<size_t>(chroma.n_chroma()) * chroma.n_frames();
    *out_length = total;
    if (total == 0) return SONARE_OK;
    std::unique_ptr<float[]> features(new float[total]);
    std::memcpy(features.get(), chroma.data(), total * sizeof(float));
    *out = release_array(features);
    return SONARE_OK;
  });
}
