#include "c_api/features_internal.h"

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

    out->n_chroma = chroma.n_chroma();
    out->n_frames = chroma.n_frames();
    out->sample_rate = chroma.sample_rate();
    out->hop_length = chroma.hop_length();

    size_t total = static_cast<size_t>(chroma.n_chroma()) * chroma.n_frames();
    std::unique_ptr<float[]> features(new float[total]);
    std::memcpy(features.get(), chroma.data(), total * sizeof(float));
    out->features = release_array(features);

    auto mean = chroma.mean_energy();
    std::unique_ptr<float[]> mean_out(new float[chroma.n_chroma()]);
    std::memcpy(mean_out.get(), mean.data(), chroma.n_chroma() * sizeof(float));
    out->mean_energy = release_array(mean_out);
    return SONARE_OK;
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
