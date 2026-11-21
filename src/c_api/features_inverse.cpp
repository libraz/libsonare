#include <string>

#include "c_api/features_internal.h"
#include "c_api/inverse_input_domains.h"
#include "util/parameter_checks.h"
#include "util/resource_limits.h"

namespace {

// The core's names for the inverse transforms' inputs, as the C entries spell them.
const std::array<CName, 3> kInverseNames{
    {{"sr", "sample_rate"}, {"M", "mel"}, {"mfcc", "coefficients"}}};

// The CQT-family core names, as the CQT and VQT inverse entries spell them.
const std::array<CName, 2> kCqtInverseNames{{{"fmin", "fmin_hz"}, {"sr", "sample_rate"}}};

namespace check = sonare::util::check;

// Refuses an inverse-transform input buffer that does not hold exactly rows * n_frames floats,
// guarding the product against size_t overflow (32-bit WASM); rows and n_frames are >= 1.
void check_inverse_input_length(size_t input_length, int rows, int n_frames,
                                const char* rows_subject) {
  const size_t r = static_cast<size_t>(rows);
  const size_t f = static_cast<size_t>(n_frames);
  if (r > sonare_c_detail::kMaxBufferSize / f) {
    check::refuse_relation(
        {rows_subject, "n_frames"},
        "{0} * {1} must be at most " + std::to_string(sonare_c_detail::kMaxBufferSize));
  }
  if (input_length != r * f) {
    check::refuse_relation({"input_length", rows_subject, "n_frames"}, "{0} must equal {1} * {2}");
  }
}

// Refuses a non-positive row or frame count, then an input length that does not match them.
SonareError validate_inverse_input(size_t input_length, int rows, int n_frames,
                                   const check::NumericDomain& rows_domain,
                                   const check::NumericDomain& frames_domain,
                                   const char* rows_subject) {
  return run_validation([&] {
    check::integer_within(rows, rows_domain, rows_subject);
    check::integer_within(n_frames, frames_domain, "n_frames");
    check_inverse_input_length(input_length, rows, n_frames, rows_subject);
  });
}

// Refuses the mel / mfcc inverse dimensions every entry reads before the core runs.
SonareError validate_mel_inverse(int n_mels, int n_frames, int n_fft, int sample_rate) {
  return run_validation([&] {
    check::integer_within(n_mels, sonare::kInverseMelBandsDomain, "n_mels");
    check::integer_within(n_frames, sonare::kMelToStftFramesDomain, "n_frames");
    check::integer_within(n_fft, sonare::kMelToStftFftSizeDomain, "n_fft");
    check::integer_within(sample_rate, sonare::kInverseSampleRateDomain, "sample_rate");
  });
}

size_t inverse_input_length(int rows, int n_frames) noexcept {
  if (rows <= 0 || n_frames <= 0) return 0;
  const size_t r = static_cast<size_t>(rows);
  const size_t f = static_cast<size_t>(n_frames);
  if (r > sonare_c_detail::kMaxBufferSize / f) return 0;
  return r * f;
}

// The non-finite element case is not checked here: griffinlim_cqt / griffinlim_vqt
// / mel_to_stft / mfcc_to_mel / griffin_lim reject it themselves, so the rule has
// a single home that WASM (which calls the core directly) inherits too.
SonareError validate_cqt_inverse(const float* magnitude, size_t input_length, int n_bins,
                                 int n_frames, int sample_rate, int hop_length, float fmin,
                                 int bins_per_octave, const float* gamma, int n_iter) {
  SONARE_C_REQUIRE_NONNULL(magnitude);
  return run_validation([&] {
    check::integer_within(n_bins, sonare::kCqtCountDomain, "n_bins");
    check::integer_within(n_frames, sonare_c_detail::kInverseSpectrogramExtentDomain, "n_frames");
    check::integer_within(sample_rate, sonare::kAudioSampleRateDomain, "sample_rate");
    check::integer_within(hop_length, sonare::kCqtHopLengthDomain, "hop_length");
    check::greater_than(fmin, 0.0, "fmin_hz");
    check::integer_within(bins_per_octave, sonare::kCqtCountDomain, "bins_per_octave");
    if (gamma != nullptr) validate_vqt_gamma(*gamma);
    check::integer_within(n_iter, sonare_c_detail::kGriffinLimIterationsDomain, "n_iter");
    check_inverse_input_length(input_length, n_bins, n_frames, "n_bins");
  });
}

}  // namespace

SonareError sonare_cqt_to_audio_checked(const float* magnitude, size_t input_length, int n_bins,
                                        int n_frames, int sample_rate, int hop_length,
                                        float fmin_hz, int bins_per_octave, int n_iter, float** out,
                                        size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  const SonareError validation =
      validate_cqt_inverse(magnitude, input_length, n_bins, n_frames, sample_rate, hop_length,
                           fmin_hz, bins_per_octave, nullptr, n_iter);
  if (validation != SONARE_OK) return validation;
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);

  SONARE_C_TRY
  CqtConfig config;
  config.hop_length = hop_length;
  config.fmin = fmin_hz;
  config.n_bins = n_bins;
  config.bins_per_octave = bins_per_octave;
  const Audio audio = with_c_names(kCqtInverseNames, [&] {
    return sonare::detail::with_fixed_filter_scale({"fmin", "bins_per_octave"}, "sr", [&] {
      return griffinlim_cqt(magnitude, n_bins, n_frames, config, sample_rate, n_iter);
    });
  });
  return fill_audio_samples(audio, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_cqt_to_audio(const float* magnitude, int n_bins, int n_frames, int sample_rate,
                                int hop_length, float fmin_hz, int bins_per_octave, int n_iter,
                                float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  return sonare_cqt_to_audio_checked(magnitude, inverse_input_length(n_bins, n_frames), n_bins,
                                     n_frames, sample_rate, hop_length, fmin_hz, bins_per_octave,
                                     n_iter, out, out_length);
}

SonareError sonare_vqt_to_audio_checked(const float* magnitude, size_t input_length, int n_bins,
                                        int n_frames, int sample_rate, int hop_length,
                                        float fmin_hz, int bins_per_octave, float gamma, int n_iter,
                                        float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  const SonareError validation =
      validate_cqt_inverse(magnitude, input_length, n_bins, n_frames, sample_rate, hop_length,
                           fmin_hz, bins_per_octave, &gamma, n_iter);
  if (validation != SONARE_OK) return validation;
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);

  SONARE_C_TRY
  VqtConfig config;
  config.hop_length = hop_length;
  config.fmin = fmin_hz;
  config.n_bins = n_bins;
  config.bins_per_octave = bins_per_octave;
  config.gamma = gamma;
  const Audio audio = with_c_names(kCqtInverseNames, [&] {
    return sonare::detail::with_fixed_filter_scale({"fmin", "bins_per_octave", "gamma"}, "sr", [&] {
      return griffinlim_vqt(magnitude, n_bins, n_frames, config, sample_rate, n_iter);
    });
  });
  return fill_audio_samples(audio, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_vqt_to_audio(const float* magnitude, int n_bins, int n_frames, int sample_rate,
                                int hop_length, float fmin_hz, int bins_per_octave, float gamma,
                                int n_iter, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  return sonare_vqt_to_audio_checked(magnitude, inverse_input_length(n_bins, n_frames), n_bins,
                                     n_frames, sample_rate, hop_length, fmin_hz, bins_per_octave,
                                     gamma, n_iter, out, out_length);
}

SonareError sonare_mel_to_stft_ex(const float* mel, int n_mels, int n_frames, int sample_rate,
                                  int n_fft, float fmin_hz, float fmax_hz, int htk,
                                  SonareInverseResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(mel);
  if (const SonareError err = validate_mel_inverse(n_mels, n_frames, n_fft, sample_rate);
      err != SONARE_OK) {
    return err;
  }

  SONARE_C_TRY
  with_c_names(kMelRangeNames, [&] { validate_mel_bounds(fmin_hz, fmax_hz); });
  SONARE_C_REQUIRE_NONNULL(out);
  MelConfig config;
  config.n_mels = n_mels;
  config.n_fft = n_fft;
  config.fmin = fmin_hz;
  config.fmax = fmax_hz;
  config.htk = htk != 0;
  std::vector<float> stft = with_c_names(
      kInverseNames, [&] { return mel_to_stft(mel, n_mels, n_frames, config, sample_rate); });
  return fill_inverse_result(stft, n_fft / 2 + 1, n_frames, out);
  SONARE_C_CATCH
}

SonareError sonare_mel_to_stft(const float* mel, int n_mels, int n_frames, int sample_rate,
                               int n_fft, float fmin_hz, float fmax_hz, SonareInverseResult* out) {
  SONARE_C_API_ENTRY;
  return sonare_mel_to_stft_ex(mel, n_mels, n_frames, sample_rate, n_fft, fmin_hz, fmax_hz, 0, out);
}

SonareError sonare_mel_to_audio_ex(const float* mel, int n_mels, int n_frames, int sample_rate,
                                   int n_fft, int hop_length, float fmin_hz, float fmax_hz, int htk,
                                   int n_iter, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_REQUIRE_NONNULL(mel);
  if (const SonareError err = validate_mel_inverse(n_mels, n_frames, n_fft, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] { check_stft_hop(hop_length, "hop_length"); });
      err != SONARE_OK) {
    return err;
  }

  SONARE_C_TRY
  with_c_names(kMelRangeNames, [&] { validate_mel_bounds(fmin_hz, fmax_hz); });
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  MelConfig config;
  config.n_mels = n_mels;
  config.n_fft = n_fft;
  config.hop_length = core_stft_hop(hop_length);
  config.fmin = fmin_hz;
  config.fmax = fmax_hz;
  config.htk = htk != 0;
  Audio audio = with_c_names(kInverseNames, [&] {
    return mel_to_audio(mel, n_mels, n_frames, config, n_iter, sample_rate);
  });
  return fill_audio_samples(audio, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_mel_to_audio(const float* mel, int n_mels, int n_frames, int sample_rate,
                                int n_fft, int hop_length, float fmin_hz, float fmax_hz, int n_iter,
                                float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  return sonare_mel_to_audio_ex(mel, n_mels, n_frames, sample_rate, n_fft, hop_length, fmin_hz,
                                fmax_hz, 0, n_iter, out, out_length);
}

SonareError sonare_griffin_lim(const float* magnitude, size_t input_length, int n_bins,
                               int n_frames, int n_fft, int hop_length, int sample_rate, int n_iter,
                               float momentum, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_REQUIRE_NONNULL(magnitude);
  if (const SonareError err = run_validation([&] {
        check::integer_within(n_bins, sonare_c_detail::kInverseSpectrogramExtentDomain, "n_bins");
        check::integer_within(n_frames, sonare_c_detail::kInverseSpectrogramExtentDomain,
                              "n_frames");
        check::integer_within(n_fft, sonare_c_detail::kInverseSpectrogramExtentDomain, "n_fft");
        check_stft_hop(hop_length, "hop_length");
        check::integer_within(sample_rate, sonare::kAudioSampleRateDomain, "sample_rate");
        check::integer_within(n_iter, sonare_c_detail::kGriffinLimIterationsDomain, "n_iter");
        check::in_interval(momentum, 0.0, true, 1.0, false, "momentum");
        if (n_bins != n_fft / 2 + 1)
          check::refuse_relation({"n_bins", "n_fft"}, "{0} must equal {1} / 2 + 1");
        check_inverse_input_length(input_length, n_bins, n_frames, "n_bins");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_TRY
  GriffinLimConfig config;
  config.n_iter = n_iter;
  config.momentum = momentum;
  return fill_audio_samples(griffin_lim(magnitude, n_bins, n_frames, n_fft,
                                        core_stft_hop(hop_length), sample_rate, config),
                            out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_mfcc_to_mel_ex(const float* coefficients, int n_mfcc, int n_frames, int n_mels,
                                  float lifter, SonareInverseResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(coefficients);
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  std::vector<float> mel = with_c_names(
      kInverseNames, [&] { return mfcc_to_mel(coefficients, n_mfcc, n_frames, n_mels, lifter); });
  return fill_inverse_result(mel, n_mels, n_frames, out);
  SONARE_C_CATCH
}

SonareError sonare_mfcc_to_mel(const float* coefficients, int n_mfcc, int n_frames, int n_mels,
                               SonareInverseResult* out) {
  SONARE_C_API_ENTRY;
  return sonare_mfcc_to_mel_ex(coefficients, n_mfcc, n_frames, n_mels, 0.0f, out);
}

SonareError sonare_mfcc_to_audio_ex2(const float* coefficients, int n_mfcc, int n_frames,
                                     int n_mels, int sample_rate, int n_fft, int hop_length,
                                     float fmin_hz, float fmax_hz, int htk, float lifter,
                                     int n_iter, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_REQUIRE_NONNULL(coefficients);
  if (const SonareError err = run_validation([&] {
        check::integer_within(n_fft, sonare::kMelToStftFftSizeDomain, "n_fft");
        check_stft_hop(hop_length, "hop_length");
        check::integer_within(sample_rate, sonare::kInverseSampleRateDomain, "sample_rate");
      });
      err != SONARE_OK) {
    return err;
  }

  SONARE_C_TRY
  with_c_names(kMelRangeNames, [&] { validate_mel_bounds(fmin_hz, fmax_hz); });
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  MelConfig config;
  config.n_mels = n_mels;
  config.n_fft = n_fft;
  config.hop_length = core_stft_hop(hop_length);
  config.fmin = fmin_hz;
  config.fmax = fmax_hz;
  config.htk = htk != 0;
  Audio audio = with_c_names(kInverseNames, [&] {
    return mfcc_to_audio(coefficients, n_mfcc, n_frames, config, n_iter, sample_rate, lifter);
  });
  return fill_audio_samples(audio, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_mfcc_to_audio_ex(const float* coefficients, int n_mfcc, int n_frames, int n_mels,
                                    int sample_rate, int n_fft, int hop_length, float fmin_hz,
                                    float fmax_hz, int htk, int n_iter, float** out,
                                    size_t* out_length) {
  SONARE_C_API_ENTRY;
  return sonare_mfcc_to_audio_ex2(coefficients, n_mfcc, n_frames, n_mels, sample_rate, n_fft,
                                  hop_length, fmin_hz, fmax_hz, htk, 0.0f, n_iter, out, out_length);
}

SonareError sonare_mfcc_to_audio(const float* coefficients, int n_mfcc, int n_frames, int n_mels,
                                 int sample_rate, int n_fft, int hop_length, float fmin_hz,
                                 float fmax_hz, int n_iter, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  return sonare_mfcc_to_audio_ex(coefficients, n_mfcc, n_frames, n_mels, sample_rate, n_fft,
                                 hop_length, fmin_hz, fmax_hz, 0, n_iter, out, out_length);
}

SonareError sonare_mel_to_stft_checked(const float* mel, size_t input_length, int n_mels,
                                       int n_frames, int sample_rate, int n_fft, float fmin_hz,
                                       float fmax_hz, SonareInverseResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(mel);
  if (const SonareError err =
          validate_inverse_input(input_length, n_mels, n_frames, sonare::kInverseMelBandsDomain,
                                 sonare::kMelToStftFramesDomain, "n_mels");
      err != SONARE_OK) {
    return err;
  }
  return sonare_mel_to_stft(mel, n_mels, n_frames, sample_rate, n_fft, fmin_hz, fmax_hz, out);
}

SonareError sonare_mel_to_stft_checked_ex(const float* mel, size_t input_length, int n_mels,
                                          int n_frames, int sample_rate, int n_fft, float fmin_hz,
                                          float fmax_hz, int htk, SonareInverseResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(mel);
  if (const SonareError err =
          validate_inverse_input(input_length, n_mels, n_frames, sonare::kInverseMelBandsDomain,
                                 sonare::kMelToStftFramesDomain, "n_mels");
      err != SONARE_OK) {
    return err;
  }
  return sonare_mel_to_stft_ex(mel, n_mels, n_frames, sample_rate, n_fft, fmin_hz, fmax_hz, htk,
                               out);
}

SonareError sonare_mel_to_audio_checked(const float* mel, size_t input_length, int n_mels,
                                        int n_frames, int sample_rate, int n_fft, int hop_length,
                                        float fmin_hz, float fmax_hz, int n_iter, float** out,
                                        size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_REQUIRE_NONNULL(mel);
  if (const SonareError err =
          validate_inverse_input(input_length, n_mels, n_frames, sonare::kInverseMelBandsDomain,
                                 sonare::kMelToStftFramesDomain, "n_mels");
      err != SONARE_OK) {
    return err;
  }
  return sonare_mel_to_audio(mel, n_mels, n_frames, sample_rate, n_fft, hop_length, fmin_hz,
                             fmax_hz, n_iter, out, out_length);
}

SonareError sonare_mel_to_audio_checked_ex(const float* mel, size_t input_length, int n_mels,
                                           int n_frames, int sample_rate, int n_fft, int hop_length,
                                           float fmin_hz, float fmax_hz, int htk, int n_iter,
                                           float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_REQUIRE_NONNULL(mel);
  if (const SonareError err =
          validate_inverse_input(input_length, n_mels, n_frames, sonare::kInverseMelBandsDomain,
                                 sonare::kMelToStftFramesDomain, "n_mels");
      err != SONARE_OK) {
    return err;
  }
  return sonare_mel_to_audio_ex(mel, n_mels, n_frames, sample_rate, n_fft, hop_length, fmin_hz,
                                fmax_hz, htk, n_iter, out, out_length);
}

SonareError sonare_mfcc_to_mel_checked(const float* coefficients, size_t input_length, int n_mfcc,
                                       int n_frames, int n_mels, SonareInverseResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(coefficients);
  if (const SonareError err =
          validate_inverse_input(input_length, n_mfcc, n_frames, sonare::kInverseMfccCountDomain,
                                 sonare::kInverseFramesDomain, "n_mfcc");
      err != SONARE_OK) {
    return err;
  }
  return sonare_mfcc_to_mel(coefficients, n_mfcc, n_frames, n_mels, out);
}

SonareError sonare_mfcc_to_mel_checked_ex(const float* coefficients, size_t input_length,
                                          int n_mfcc, int n_frames, int n_mels, float lifter,
                                          SonareInverseResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(coefficients);
  if (const SonareError err =
          validate_inverse_input(input_length, n_mfcc, n_frames, sonare::kInverseMfccCountDomain,
                                 sonare::kInverseFramesDomain, "n_mfcc");
      err != SONARE_OK) {
    return err;
  }
  return sonare_mfcc_to_mel_ex(coefficients, n_mfcc, n_frames, n_mels, lifter, out);
}

SonareError sonare_mfcc_to_audio_checked(const float* coefficients, size_t input_length, int n_mfcc,
                                         int n_frames, int n_mels, int sample_rate, int n_fft,
                                         int hop_length, float fmin_hz, float fmax_hz, int n_iter,
                                         float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_REQUIRE_NONNULL(coefficients);
  if (const SonareError err =
          validate_inverse_input(input_length, n_mfcc, n_frames, sonare::kInverseMfccCountDomain,
                                 sonare::kMelToStftFramesDomain, "n_mfcc");
      err != SONARE_OK) {
    return err;
  }
  return sonare_mfcc_to_audio(coefficients, n_mfcc, n_frames, n_mels, sample_rate, n_fft,
                              hop_length, fmin_hz, fmax_hz, n_iter, out, out_length);
}

SonareError sonare_mfcc_to_audio_checked_ex(const float* coefficients, size_t input_length,
                                            int n_mfcc, int n_frames, int n_mels, int sample_rate,
                                            int n_fft, int hop_length, float fmin_hz, float fmax_hz,
                                            int htk, int n_iter, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_REQUIRE_NONNULL(coefficients);
  if (const SonareError err =
          validate_inverse_input(input_length, n_mfcc, n_frames, sonare::kInverseMfccCountDomain,
                                 sonare::kMelToStftFramesDomain, "n_mfcc");
      err != SONARE_OK) {
    return err;
  }
  return sonare_mfcc_to_audio_ex(coefficients, n_mfcc, n_frames, n_mels, sample_rate, n_fft,
                                 hop_length, fmin_hz, fmax_hz, htk, n_iter, out, out_length);
}

SonareError sonare_mfcc_to_audio_checked_ex2(const float* coefficients, size_t input_length,
                                             int n_mfcc, int n_frames, int n_mels, int sample_rate,
                                             int n_fft, int hop_length, float fmin_hz,
                                             float fmax_hz, int htk, float lifter, int n_iter,
                                             float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_REQUIRE_NONNULL(coefficients);
  if (const SonareError err =
          validate_inverse_input(input_length, n_mfcc, n_frames, sonare::kInverseMfccCountDomain,
                                 sonare::kMelToStftFramesDomain, "n_mfcc");
      err != SONARE_OK) {
    return err;
  }
  return sonare_mfcc_to_audio_ex2(coefficients, n_mfcc, n_frames, n_mels, sample_rate, n_fft,
                                  hop_length, fmin_hz, fmax_hz, htk, lifter, n_iter, out,
                                  out_length);
}

void sonare_free_inverse_result(SonareInverseResult* result) {
  if (!result) return;
  delete[] result->data;
  result->data = nullptr;
  result->rows = 0;
  result->n_frames = 0;
}
