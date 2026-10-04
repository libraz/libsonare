#include "c_api/core_internal.h"
#include "util/numeric_validation.h"

namespace {

// Shared body of the CQT-family entry points; @p transform maps (Audio, CqtConfig) to a CqtResult.
template <typename Transform>
SonareError run_cqt_family(const float* samples, size_t length, int sample_rate, int hop_length,
                           float fmin, int n_bins, int bins_per_octave, SonareCqtResult* out,
                           Transform&& transform) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  *out = {};
  if (hop_length <= 0 || !numeric::finite_positive(fmin) || n_bins <= 0 || bins_per_octave <= 0) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    CqtConfig config;
    config.hop_length = hop_length;
    config.fmin = fmin;
    config.n_bins = n_bins;
    config.bins_per_octave = bins_per_octave;
    CqtResult result = transform(audio, config);
    return fill_cqt_result(result, out);
  });
}

}  // namespace

SonareError sonare_cqt(const float* samples, size_t length, int sample_rate, int hop_length,
                       float fmin, int n_bins, int bins_per_octave, SonareCqtResult* out) {
  return run_cqt_family(
      samples, length, sample_rate, hop_length, fmin, n_bins, bins_per_octave, out,
      [](const Audio& audio, const CqtConfig& config) { return cqt(audio, config); });
}

SonareError sonare_pseudo_cqt(const float* samples, size_t length, int sample_rate, int hop_length,
                              float fmin, int n_bins, int bins_per_octave, SonareCqtResult* out) {
  return run_cqt_family(
      samples, length, sample_rate, hop_length, fmin, n_bins, bins_per_octave, out,
      [](const Audio& audio, const CqtConfig& config) { return pseudo_cqt(audio, config); });
}

SonareError sonare_hybrid_cqt(const float* samples, size_t length, int sample_rate, int hop_length,
                              float fmin, int n_bins, int bins_per_octave, SonareCqtResult* out) {
  return run_cqt_family(
      samples, length, sample_rate, hop_length, fmin, n_bins, bins_per_octave, out,
      [](const Audio& audio, const CqtConfig& config) { return hybrid_cqt(audio, config); });
}

SonareError sonare_vqt(const float* samples, size_t length, int sample_rate, int hop_length,
                       float fmin, int n_bins, int bins_per_octave, float gamma,
                       SonareCqtResult* out) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  *out = {};
  if (hop_length <= 0 || !std::isfinite(fmin) || fmin <= 0.0f || n_bins <= 0 ||
      bins_per_octave <= 0 || std::isinf(gamma)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }

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
