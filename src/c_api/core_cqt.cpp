#include "c_api/core_internal.h"

SonareError sonare_cqt(const float* samples, size_t length, int sample_rate, int hop_length,
                       float fmin, int n_bins, int bins_per_octave, SonareCqtResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (hop_length <= 0 || fmin <= 0.0f || n_bins <= 0 || bins_per_octave <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  *out = {};

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    CqtConfig config;
    config.hop_length = hop_length;
    config.fmin = fmin;
    config.n_bins = n_bins;
    config.bins_per_octave = bins_per_octave;
    CqtResult result = cqt(audio, config);
    return fill_cqt_result(result, out);
  });
}

SonareError sonare_pseudo_cqt(const float* samples, size_t length, int sample_rate, int hop_length,
                              float fmin, int n_bins, int bins_per_octave, SonareCqtResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (hop_length <= 0 || fmin <= 0.0f || n_bins <= 0 || bins_per_octave <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  *out = {};

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    CqtConfig config;
    config.hop_length = hop_length;
    config.fmin = fmin;
    config.n_bins = n_bins;
    config.bins_per_octave = bins_per_octave;
    CqtResult result = pseudo_cqt(audio, config);
    return fill_cqt_result(result, out);
  });
}

SonareError sonare_hybrid_cqt(const float* samples, size_t length, int sample_rate, int hop_length,
                              float fmin, int n_bins, int bins_per_octave, SonareCqtResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (hop_length <= 0 || fmin <= 0.0f || n_bins <= 0 || bins_per_octave <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  *out = {};

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    CqtConfig config;
    config.hop_length = hop_length;
    config.fmin = fmin;
    config.n_bins = n_bins;
    config.bins_per_octave = bins_per_octave;
    CqtResult result = hybrid_cqt(audio, config);
    return fill_cqt_result(result, out);
  });
}

SonareError sonare_vqt(const float* samples, size_t length, int sample_rate, int hop_length,
                       float fmin, int n_bins, int bins_per_octave, float gamma,
                       SonareCqtResult* out) {
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  if (hop_length <= 0 || fmin <= 0.0f || n_bins <= 0 || bins_per_octave <= 0 || gamma < 0.0f) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  *out = {};

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    VqtConfig config;
    config.hop_length = hop_length;
    config.fmin = fmin;
    config.n_bins = n_bins;
    config.bins_per_octave = bins_per_octave;
    config.gamma = gamma;
    VqtResult result = vqt(audio, config);
    return fill_cqt_result(result, out);
  });
}
