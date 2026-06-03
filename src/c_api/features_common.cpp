#include "c_api/features_internal.h"

NnlsChromaConfig make_fast_nnls_chroma_config(int hop_length) {
  NnlsChromaConfig config;
  config.cqt.bins_per_octave = 12;
  config.cqt.n_bins = 84;
  config.cqt.hop_length = hop_length;
  config.midi_min = 24;
  config.n_pitches = 60;
  config.n_harmonics = 4;
  config.max_iter = 25;
  config.tolerance = 1.0e-3f;
  return config;
}

SonareError fill_inverse_result(const std::vector<float>& data, int rows, int n_frames,
                                SonareInverseResult* out) {
  *out = {};
  out->rows = rows;
  out->n_frames = n_frames;
  if (!data.empty()) {
    std::unique_ptr<float[]> buf(new float[data.size()]);
    std::memcpy(buf.get(), data.data(), data.size() * sizeof(float));
    out->data = release_array(buf);
  }
  return SONARE_OK;
}

SonareError fill_audio_samples(const Audio& audio, float** out, size_t* out_length) {
  *out = nullptr;
  *out_length = audio.size();
  if (audio.size() == 0) return SONARE_OK;
  std::unique_ptr<float[]> buf(new float[audio.size()]);
  std::memcpy(buf.get(), audio.data(), audio.size() * sizeof(float));
  *out = release_array(buf);
  return SONARE_OK;
}

SonareError fill_pitch_result(const PitchResult& result, SonarePitchResult* out) {
  out->n_frames = result.n_frames();
  out->median_f0 = result.median_f0();
  out->mean_f0 = result.mean_f0();

  size_t n = static_cast<size_t>(result.n_frames());
  std::unique_ptr<float[]> f0(new float[n]);
  std::unique_ptr<float[]> voiced_prob(new float[n]);
  std::unique_ptr<int[]> voiced_flag(new int[n]);

  std::memcpy(f0.get(), result.f0.data(), n * sizeof(float));
  std::memcpy(voiced_prob.get(), result.voiced_prob.data(), n * sizeof(float));
  for (size_t i = 0; i < n; ++i) {
    voiced_flag[i] = result.voiced_flag[i] ? 1 : 0;
  }

  out->f0 = release_array(f0);
  out->voiced_prob = release_array(voiced_prob);
  out->voiced_flag = release_array(voiced_flag);
  return SONARE_OK;
}
