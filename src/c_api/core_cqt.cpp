#include "c_api/core_internal.h"

namespace {

// Shared body of the CQT-family entry points; @p transform maps (Audio, CqtConfig) to a CqtResult.
template <typename Transform>
SonareError run_cqt_family(const char* fn, const char* name, const float* samples, size_t length,
                           int sample_rate, int hop_length, float fmin, int n_bins,
                           int bins_per_octave, SonareCqtResult* out, Transform&& transform) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  *out = {};
  if (const SonareError err = run_validation([&] {
        CqtConfig config;
        config.hop_length = hop_length;
        config.fmin = fmin;
        config.n_bins = n_bins;
        config.bins_per_octave = bins_per_octave;
        validate_cqt_config(config);
      });
      err != SONARE_OK) {
    return err;
  }

  return run_offline(fn, name, samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
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
                       float fmin_hz, int n_bins, int bins_per_octave, SonareCqtResult* out) {
  SONARE_C_API_ENTRY;
  return run_cqt_family(
      "cqt", "samples", samples, length, sample_rate, hop_length, fmin_hz, n_bins, bins_per_octave,
      out, [](const Audio& audio, const CqtConfig& config) { return cqt(audio, config); });
}

SonareError sonare_pseudo_cqt(const float* samples, size_t length, int sample_rate, int hop_length,
                              float fmin_hz, int n_bins, int bins_per_octave,
                              SonareCqtResult* out) {
  SONARE_C_API_ENTRY;
  return run_cqt_family(
      "pseudo_cqt", "samples", samples, length, sample_rate, hop_length, fmin_hz, n_bins,
      bins_per_octave, out,
      [](const Audio& audio, const CqtConfig& config) { return pseudo_cqt(audio, config); });
}

SonareError sonare_hybrid_cqt(const float* samples, size_t length, int sample_rate, int hop_length,
                              float fmin_hz, int n_bins, int bins_per_octave,
                              SonareCqtResult* out) {
  SONARE_C_API_ENTRY;
  return run_cqt_family(
      "hybrid_cqt", "samples", samples, length, sample_rate, hop_length, fmin_hz, n_bins,
      bins_per_octave, out,
      [](const Audio& audio, const CqtConfig& config) { return hybrid_cqt(audio, config); });
}

SonareError sonare_vqt(const float* samples, size_t length, int sample_rate, int hop_length,
                       float fmin_hz, int n_bins, int bins_per_octave, float gamma,
                       SonareCqtResult* out) {
  SONARE_C_API_ENTRY;
  if (!out) return SONARE_ERROR_INVALID_PARAMETER;
  *out = {};
  if (const SonareError err = run_validation([&] {
        VqtConfig config;
        config.hop_length = hop_length;
        config.fmin = fmin_hz;
        config.n_bins = n_bins;
        config.bins_per_octave = bins_per_octave;
        validate_cqt_config(config.to_cqt_config());
        validate_vqt_gamma(gamma);
      });
      err != SONARE_OK) {
    return err;
  }

  return run_offline("vqt", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       VqtConfig config;
                       config.hop_length = hop_length;
                       config.fmin = fmin_hz;
                       config.n_bins = n_bins;
                       config.bins_per_octave = bins_per_octave;
                       config.gamma = gamma;
                       VqtResult result = vqt(audio, config);
                       return fill_cqt_result(result, out);
                     });
}
