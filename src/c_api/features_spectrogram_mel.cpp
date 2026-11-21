#include <limits>

#include "c_api/feature_input_domains.h"
#include "c_api/features_internal.h"
#include "util/parameter_checks.h"

namespace {

MelConfig make_mel_config(int n_fft, int hop_length, int n_mels, float fmin, float fmax, int htk) {
  MelConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
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

SonareError sonare_stft(const float* samples, size_t length, int sample_rate, int n_fft,
                        int hop_length, SonareStftResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(out);

  return run_offline("stft", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       StftConfig config;
                       config.n_fft = n_fft;
                       config.hop_length = hop_length;
                       Spectrogram spec = Spectrogram::compute(audio, config);

                       out->n_bins = spec.n_bins();
                       out->n_frames = spec.n_frames();
                       out->n_fft = spec.n_fft();
                       out->hop_length = spec.hop_length();
                       out->sample_rate = spec.sample_rate();

                       size_t total = static_cast<size_t>(spec.n_bins()) * spec.n_frames();
                       const std::vector<float>& mag = spec.magnitude();
                       const std::vector<float>& pow = spec.power();

                       std::unique_ptr<float[]> magnitude(new float[total]);
                       std::unique_ptr<float[]> power(new float[total]);
                       std::memcpy(magnitude.get(), mag.data(), total * sizeof(float));
                       std::memcpy(power.get(), pow.data(), total * sizeof(float));
                       out->magnitude = release_array(magnitude);
                       out->power = release_array(power);
                       return SONARE_OK;
                     });
}

SonareError sonare_stft_db(const float* samples, size_t length, int sample_rate, int n_fft,
                           int hop_length, int* out_n_bins, int* out_n_frames, float** out_db) {
  SONARE_C_API_ENTRY;

  // Zero every out param up front, not just the owned out_db pointer, so a
  // validate_audio_params rejection inside run_offline (which skips the body
  // below) never leaves *out_n_bins / *out_n_frames as caller stack garbage.
  SONARE_C_ZERO_OUTS(out_n_bins, out_n_frames, out_db);
  SONARE_C_REQUIRE_NONNULL(out_n_bins);
  SONARE_C_REQUIRE_NONNULL(out_n_frames);
  SONARE_C_REQUIRE_NONNULL(out_db);

  return run_offline("stft_db", "samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       StftConfig config;
                       config.n_fft = n_fft;
                       config.hop_length = hop_length;
                       Spectrogram spec = Spectrogram::compute(audio, config);

                       *out_n_bins = spec.n_bins();
                       *out_n_frames = spec.n_frames();
                       std::vector<float> db = spec.to_db();
                       std::unique_ptr<float[]> db_out(new float[db.size()]);
                       std::memcpy(db_out.get(), db.data(), db.size() * sizeof(float));
                       *out_db = release_array(db_out);
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
                                   int hop_length, int n_mels,
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
      "mel_spectrogram", "samples", samples, length, sample_rate,
      [&](const Audio& audio) -> SonareError {
        const MelConfig config = make_mel_config(n_fft, hop_length, n_mels, resolved.fmin_hz,
                                                 resolved.fmax_hz, resolved.htk);
        MelSpectrogram mel = with_c_names(kMelOptionsRangeNames,
                                          [&] { return MelSpectrogram::compute(audio, config); });

        out->n_mels = mel.n_mels();
        out->n_frames = mel.n_frames();
        out->sample_rate = mel.sample_rate();
        out->hop_length = mel.hop_length();
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

void sonare_mfcc_options_init(SonareMfccOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
}

SonareError sonare_mfcc(const float* samples, size_t length, int sample_rate, int n_fft,
                        int hop_length, int n_mels, int n_mfcc, const SonareMfccOptions* options,
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
      "mfcc", "samples", samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        const MelConfig config = make_mel_config(n_fft, hop_length, n_mels, resolved.fmin_hz,
                                                 resolved.fmax_hz, resolved.htk);
        MelSpectrogram mel = with_c_names(kMelOptionsRangeNames,
                                          [&] { return MelSpectrogram::compute(audio, config); });
        std::vector<float> mfcc_data = mel.mfcc(n_mfcc, resolved.lifter);

        out->n_mfcc = n_mfcc;
        out->n_frames = mel.n_frames();
        out->sample_rate = mel.sample_rate();
        out->hop_length = mel.hop_length();
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
