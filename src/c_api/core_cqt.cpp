#include "c_api/core_internal.h"
#include "util/parameter_checks.h"

namespace {

// The core's names for the lowest frequency and the rate, as every CQT-family entry spells them.
const std::array<CName, 2> kCqtNames{{{"fmin", "fmin_hz"}, {"sr", "sample_rate"}}};

// Shared body of the CQT-family entry points; @p transform maps (Audio, CqtConfig) to a CqtResult.
template <typename Transform>
SonareError run_cqt_family(const char* fn, const char* name, const float* samples, size_t length,
                           int sample_rate, int hop_length, float fmin, int n_bins,
                           int bins_per_octave, float filter_scale, SonareCqtResult* out,
                           Transform&& transform) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  if (const SonareError err = validate_audio_params(fn, name, samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] {
        CqtConfig config;
        config.hop_length = hop_length;
        config.fmin = fmin;
        config.n_bins = n_bins;
        config.bins_per_octave = bins_per_octave;
        config.filter_scale = filter_scale;
        with_c_names(kCqtNames, [&] { validate_cqt_config(config); });
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        CqtConfig config;
        config.hop_length = hop_length;
        config.fmin = fmin;
        config.n_bins = n_bins;
        config.bins_per_octave = bins_per_octave;
        config.filter_scale = filter_scale;
        CqtResult result = with_c_names(kCqtNames, [&] { return transform(audio, config); });
        return fill_cqt_result(result, out);
      });
}

}  // namespace

SonareError sonare_cqt(const float* samples, size_t length, int sample_rate, int hop_length,
                       float fmin_hz, int n_bins, int bins_per_octave, float filter_scale,
                       SonareCqtResult* out) {
  SONARE_C_API_ENTRY;
  return run_cqt_family(
      "cqt", "samples", samples, length, sample_rate, hop_length, fmin_hz, n_bins, bins_per_octave,
      filter_scale, out,
      [](const Audio& audio, const CqtConfig& config) { return cqt(audio, config); });
}

SonareError sonare_pseudo_cqt(const float* samples, size_t length, int sample_rate, int hop_length,
                              float fmin_hz, int n_bins, int bins_per_octave, float filter_scale,
                              SonareCqtResult* out) {
  SONARE_C_API_ENTRY;
  return run_cqt_family(
      "pseudo_cqt", "samples", samples, length, sample_rate, hop_length, fmin_hz, n_bins,
      bins_per_octave, filter_scale, out,
      [](const Audio& audio, const CqtConfig& config) { return pseudo_cqt(audio, config); });
}

SonareError sonare_hybrid_cqt(const float* samples, size_t length, int sample_rate, int hop_length,
                              float fmin_hz, int n_bins, int bins_per_octave, float filter_scale,
                              SonareCqtResult* out) {
  SONARE_C_API_ENTRY;
  return run_cqt_family(
      "hybrid_cqt", "samples", samples, length, sample_rate, hop_length, fmin_hz, n_bins,
      bins_per_octave, filter_scale, out,
      [](const Audio& audio, const CqtConfig& config) { return hybrid_cqt(audio, config); });
}

SonareError sonare_vqt(const float* samples, size_t length, int sample_rate, int hop_length,
                       float fmin_hz, int n_bins, int bins_per_octave, float filter_scale,
                       float gamma, SonareCqtResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  if (const SonareError err = validate_audio_params("vqt", "samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] {
        VqtConfig config;
        config.hop_length = hop_length;
        config.fmin = fmin_hz;
        config.n_bins = n_bins;
        config.bins_per_octave = bins_per_octave;
        config.filter_scale = filter_scale;
        with_c_names(kCqtNames, [&] { validate_cqt_config(config.to_cqt_config()); });
        // NaN is the automatic spelling; every other value is a bandwidth offset in Hz.
        if (!std::isnan(gamma)) util::check::at_least(gamma, 0.0, "gamma");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        VqtConfig config;
        config.hop_length = hop_length;
        config.fmin = fmin_hz;
        config.n_bins = n_bins;
        config.bins_per_octave = bins_per_octave;
        config.filter_scale = filter_scale;
        config.gamma = gamma;
        VqtResult result = with_c_names(kCqtNames, [&] { return vqt(audio, config); });
        const SonareError filled = fill_cqt_result(result, out);
        if (filled == SONARE_OK) out->gamma = resolve_vqt_gamma(config);
        return filled;
      });
}
