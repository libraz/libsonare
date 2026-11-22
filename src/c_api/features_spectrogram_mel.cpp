#include <limits>

#include "c_api/feature_input_domains.h"
#include "c_api/features_internal.h"
#include "filters/mel.h"
#include "util/parameter_checks.h"

namespace {

MelConfig make_mel_config(int n_fft, size_t hop_length, int n_mels, float fmin, float fmax,
                          int htk) {
  MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = core_length(hop_length);
  config.n_mels = n_mels;
  with_c_names(kMelOptionsRangeNames, [&] { validate_mel_bounds(fmin, fmax); });
  config.fmin = fmin;
  config.fmax = fmax;
  config.htk = htk != 0;
  return config;
}

}  // namespace

// Features - Spectrogram
// ============================================================================

void sonare_stft_options_init(SonareStftOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
}

SonareError sonare_stft(const float* samples, size_t length, int sample_rate, int n_fft,
                        size_t hop_length, const SonareStftOptions* options,
                        SonareStftResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SonareStftOptions resolved;
  if (const SonareError err =
          resolve_options(options, sonare_stft_options_init, "sonare_stft_options_init", &resolved);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline("samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       StftConfig config;
                       config.n_fft = n_fft;
                       config.hop_length = core_length(hop_length);
                       Spectrogram spec = Spectrogram::compute(audio, config);

                       size_t total = static_cast<size_t>(spec.n_bins()) * spec.n_frames();
                       const std::vector<float>& mag = spec.magnitude();
                       const std::vector<float>& pow = spec.power();

                       std::unique_ptr<float[]> magnitude(new float[total]);
                       std::unique_ptr<float[]> power(new float[total]);
                       std::unique_ptr<float[]> db_out;
                       std::memcpy(magnitude.get(), mag.data(), total * sizeof(float));
                       std::memcpy(power.get(), pow.data(), total * sizeof(float));
                       if (resolved.db != 0) {
                         const std::vector<float> db = spec.to_db();
                         db_out.reset(new float[db.size()]);
                         std::memcpy(db_out.get(), db.data(), db.size() * sizeof(float));
                       }

                       out->n_bins = spec.n_bins();
                       out->n_frames = spec.n_frames();
                       out->n_fft = spec.n_fft();
                       out->hop_length = static_cast<size_t>(spec.hop_length());
                       out->sample_rate = spec.sample_rate();
                       out->magnitude = release_array(magnitude);
                       out->power = release_array(power);
                       out->db = db_out ? release_array(db_out) : nullptr;
                       return SONARE_OK;
                     });
}

// Features - Mel
// ============================================================================

void sonare_mel_spectrogram_options_init(SonareMelSpectrogramOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
}

SonareError sonare_mel_spectrogram(const float* samples, size_t length, int sample_rate, int n_fft,
                                   size_t hop_length, int n_mels,
                                   const SonareMelSpectrogramOptions* options,
                                   SonareMelResult* out) {
  SONARE_C_API_ENTRY;

  if (out != nullptr) {
    *out = {};
  }
  SonareMelSpectrogramOptions resolved;
  if (const SonareError err = resolve_options(options, sonare_mel_spectrogram_options_init,
                                              "sonare_mel_spectrogram_options_init", &resolved);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline(
      "samples", samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        const MelConfig config = make_mel_config(n_fft, hop_length, n_mels, resolved.fmin_hz,
                                                 resolved.fmax_hz, resolved.htk);
        MelSpectrogram mel = with_c_names(kMelOptionsRangeNames,
                                          [&] { return MelSpectrogram::compute(audio, config); });

        out->n_mels = mel.n_mels();
        out->n_frames = mel.n_frames();
        out->sample_rate = mel.sample_rate();
        out->hop_length = static_cast<size_t>(mel.hop_length());
        out->n_fft = config.n_fft;
        out->fmin_hz = config.fmin;
        out->fmax_hz = config.resolved_fmax(mel.sample_rate());
        out->htk = config.htk ? 1 : 0;
        out->is_db = 0;

        size_t total = static_cast<size_t>(mel.n_mels()) * mel.n_frames();
        std::unique_ptr<float[]> power(new float[total]);
        std::memcpy(power.get(), mel.power_data(), total * sizeof(float));

        std::vector<float> db = mel.to_db();
        std::unique_ptr<float[]> db_out(new float[total]);
        std::memcpy(db_out.get(), db.data(), total * sizeof(float));
        out->power = release_array(power);
        out->db = release_array(db_out);
        return SONARE_OK;
      });
}

void sonare_mel_filterbank_options_init(SonareMelFilterbankOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
  options->norm = SONARE_MEL_NORM_SLANEY;
}

SonareError sonare_mel_filterbank(int sample_rate, int n_fft, int n_mels,
                                  const SonareMelFilterbankOptions* options, float** out,
                                  size_t* out_length, int* out_n_mels, int* out_n_bins) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length, out_n_mels, out_n_bins);
  SonareMelFilterbankOptions resolved;
  if (const SonareError err = resolve_options(options, sonare_mel_filterbank_options_init,
                                              "sonare_mel_filterbank_options_init", &resolved);
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] {
        namespace check = sonare::util::check;
        check::integer_within(sample_rate, sonare::kAudioSampleRateDomain, "sample_rate");
        check::integer_within(n_fft, sonare::kStftNFftDomain, "n_fft");
        check::integer_within(n_mels, sonare::kMelBandsDomain, "n_mels");
        with_c_names(kMelOptionsRangeNames,
                     [&] { validate_mel_bounds(resolved.fmin_hz, resolved.fmax_hz); });
        check::integer_in_range(resolved.norm, SONARE_MEL_NORM_NONE, SONARE_MEL_NORM_SLANEY,
                                "options.norm");
        const float nyquist = static_cast<float>(sample_rate) / 2.0f;
        const bool read_as_nyquist = resolved.fmax_hz == 0.0f || resolved.fmax_hz > nyquist;
        if ((read_as_nyquist ? nyquist : resolved.fmax_hz) <= resolved.fmin_hz) {
          check::refuse_relation({"options.fmax_hz", "options.fmin_hz"},
                                 "{0} must be greater than {1}");
        }
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_REQUIRE_NONNULL(out_n_mels);
  SONARE_C_REQUIRE_NONNULL(out_n_bins);

  SONARE_C_TRY
  MelFilterConfig config;
  config.n_mels = n_mels;
  config.fmin = resolved.fmin_hz;
  config.fmax = resolved.fmax_hz;
  config.htk = resolved.htk != 0;
  config.norm = resolved.norm == SONARE_MEL_NORM_SLANEY ? MelNorm::Slaney : MelNorm::None;
  const std::vector<float> filterbank = create_mel_filterbank(sample_rate, n_fft, config);
  if (const SonareError err = copy_vector(filterbank, out, out_length); err != SONARE_OK) {
    return err;
  }
  *out_n_mels = n_mels;
  *out_n_bins = n_fft / 2 + 1;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_mel_frequencies(int n_mels, float fmin_hz, float fmax_hz, int32_t htk,
                                   float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err = run_validation([&] {
        namespace check = sonare::util::check;
        check::integer_within(n_mels, sonare::kMelFrequencyCountDomain, "n_mels");
        check::at_least(fmin_hz, 0.0, "fmin_hz");
        if (!(std::isfinite(fmax_hz) && fmax_hz > fmin_hz)) {
          check::refuse_relation({"fmax_hz", "fmin_hz"}, "{0} must be finite and greater than {1}");
        }
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);

  SONARE_C_TRY
  return copy_vector(mel_frequency_points(n_mels, fmin_hz, fmax_hz, htk != 0), out, out_length);
  SONARE_C_CATCH
}

void sonare_mfcc_options_init(SonareMfccOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
}

SonareError sonare_mfcc(const float* samples, size_t length, int sample_rate, int n_fft,
                        size_t hop_length, int n_mels, int n_mfcc, const SonareMfccOptions* options,
                        SonareMfccResult* out) {
  SONARE_C_API_ENTRY;

  if (out != nullptr) {
    *out = {};
  }
  SonareMfccOptions resolved;
  if (const SonareError err =
          resolve_options(options, sonare_mfcc_options_init, "sonare_mfcc_options_init", &resolved);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline(
      "samples", samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        const MelConfig config = make_mel_config(n_fft, hop_length, n_mels, resolved.fmin_hz,
                                                 resolved.fmax_hz, resolved.htk);
        MelSpectrogram mel = with_c_names(kMelOptionsRangeNames,
                                          [&] { return MelSpectrogram::compute(audio, config); });
        std::vector<float> mfcc_data = mel.mfcc(n_mfcc, resolved.lifter);

        out->n_mfcc = n_mfcc;
        out->n_frames = mel.n_frames();
        out->sample_rate = mel.sample_rate();
        out->hop_length = static_cast<size_t>(mel.hop_length());
        out->n_fft = config.n_fft;
        out->n_mels = mel.n_mels();
        out->fmin_hz = config.fmin;
        out->fmax_hz = config.resolved_fmax(mel.sample_rate());
        out->htk = config.htk ? 1 : 0;
        out->is_db = 0;
        out->lifter = resolved.lifter;
        std::unique_ptr<float[]> coeffs(new float[mfcc_data.size()]);
        std::memcpy(coeffs.get(), mfcc_data.data(), mfcc_data.size() * sizeof(float));
        out->coefficients = release_array(coeffs);
        return SONARE_OK;
      });
}

SonareError sonare_mel_delta(const float* features, int n_features, int n_frames, int width,
                             float** out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(features);
  if (const SonareError err = run_validation([&] {
        namespace check = sonare::util::check;
        constexpr int kIntMax = std::numeric_limits<int>::max();
        check::integer_within(n_features, sonare_c_detail::kMelDeltaShapeDomain, "n_features");
        check::integer_within(n_frames, sonare_c_detail::kMelDeltaShapeDomain, "n_frames");
        if (n_features > kIntMax / n_frames) {
          check::refuse_relation(
              {"n_features", "n_frames"},
              "{0} * {1} must be at most " + sonare::util::refusal::integer_bound_text(kIntMax));
        }
        check::odd_within(width, sonare::kMelDeltaWidthDomain, "width");
      });
      err != SONARE_OK) {
    return err;
  }
  // Non-finite elements of `features` are rejected by MelSpectrogram::delta
  // itself; SONARE_C_CATCH maps that to SONARE_ERROR_INVALID_PARAMETER.
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  const std::vector<float> result = MelSpectrogram::delta(features, n_features, n_frames, width);
  std::unique_ptr<float[]> buffer(copy_vector(result));
  *out = release_array(buffer);
  return SONARE_OK;
  SONARE_C_CATCH
}
