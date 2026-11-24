#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "c_api/features_internal.h"
#include "c_api/inverse_input_domains.h"
#include "core/spectrum.h"
#include "util/constants.h"
#include "util/parameter_checks.h"
#include "util/resource_limits.h"

namespace {

// The core's names for the inverse transforms' inputs, as the C entries spell them.
const std::array<CName, 4> kInverseNames{
    {{"M", "mel"}, {"mfcc", "coefficients"}, {"fmin", "fmin_hz"}, {"fmax", "fmax_hz"}}};

// The CQT-family core names, as the CQT and VQT inverse entries spell them.
const std::array<CName, 1> kCqtInverseNames{{{"fmin", "fmin_hz"}}};

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

// Refuses the mel / mfcc inverse dimensions every entry reads before the core runs; an entry that
// resynthesizes audio also needs an FFT size that overlap-adds.
SonareError validate_mel_inverse(int n_mels, int n_frames, int n_fft, int sample_rate,
                                 bool resynthesis = false) {
  return run_validation([&] {
    check::integer_within(n_mels, sonare::kInverseMelBandsDomain, "n_mels");
    check::integer_within(n_frames, sonare::kMelToStftFramesDomain, "n_frames");
    if (resynthesis) {
      check::even_within(n_fft, sonare::kMelToAudioFftSizeDomain, "n_fft");
    } else {
      check::integer_within(n_fft, sonare::kMelToStftFftSizeDomain, "n_fft");
    }
    check::integer_within(
        sample_rate,
        resynthesis ? sonare::kInverseAudioSampleRateDomain : sonare::kInverseSampleRateDomain,
        "sample_rate");
  });
}

// The non-finite element case is not checked here: griffinlim_cqt / griffinlim_vqt
// / mel_to_stft / mfcc_to_mel / griffin_lim reject it themselves, so the rule has
// a single home that WASM (which calls the core directly) inherits too.
SonareError validate_cqt_inverse(const float* magnitude, size_t input_length, int n_bins,
                                 int n_frames, int sample_rate, size_t hop_length, float fmin,
                                 int bins_per_octave, const float* gamma, int n_iter) {
  SONARE_C_REQUIRE_NONNULL(magnitude);
  return run_validation([&] {
    check::integer_within(n_bins, sonare::kCqtCountDomain, "n_bins");
    check::integer_within(n_frames, sonare_c_detail::kInverseSpectrogramExtentDomain, "n_frames");
    check::integer_within(sample_rate, sonare::kAudioSampleRateDomain, "sample_rate");
    check::integer_within(hop_length, sonare::kCqtHopLengthDomain, "hop_length");
    check::greater_than(fmin, 0.0, "fmin_hz");
    check::integer_within(bins_per_octave, sonare::kCqtCountDomain, "bins_per_octave");
    // NaN is the automatic spelling, as in sonare_vqt; every other value is a bandwidth offset.
    if (gamma != nullptr && !std::isnan(*gamma)) check::at_least(*gamma, 0.0, "gamma");
    check::integer_within(n_iter, sonare_c_detail::kGriffinLimIterationsDomain, "n_iter");
    check_inverse_input_length(input_length, n_bins, n_frames, "n_bins");
  });
}

// Keeps every Hann resynthesis-geometry refusal rooted at the C caller's FFT/hop pair.
int validate_istft_geometry(int n_fft, size_t hop_length) {
  try {
    check_stft_hop(hop_length, "hop_length");
    const int checked_hop = core_stft_hop(hop_length);
    validate_cola_geometry(n_fft, checked_hop, WindowType::Hann, n_fft);
    return checked_hop;
  } catch (const SonareParameterRefusal& refusal) {
    bool names_n_fft = false;
    bool names_hop_length = false;
    for (const std::string& subject : refusal.subjects()) {
      names_n_fft = names_n_fft || subject == "n_fft";
      names_hop_length = names_hop_length || subject == "hop_length";
    }
    if (names_n_fft && names_hop_length) throw;

    if (refusal.subject() == "n_fft") {
      check::refuse_relation({"n_fft", "hop_length"},
                             "{0} " + refusal.predicate() + " when paired with {1}");
    }
    if (refusal.subject() == "hop_length") {
      check::refuse_relation({"n_fft", "hop_length"},
                             "{1} " + refusal.predicate() + " when paired with {0}");
    }
    if (refusal.subject() == "win_length") {
      check::refuse_relation({"n_fft", "hop_length"},
                             "the Hann window with {0} and {1} " + refusal.predicate());
    }
    check::refuse_relation({"n_fft", "hop_length"},
                           "{0} and {1} must satisfy the Hann COLA geometry");
  }
}

}  // namespace

SonareError sonare_cqt_to_audio(const float* magnitude, size_t input_length, int n_bins,
                                int n_frames, int sample_rate, size_t hop_length, float fmin_hz,
                                int bins_per_octave, int n_iter, float** out, size_t* out_length) {
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
  config.hop_length = core_length(hop_length);
  config.fmin = fmin_hz;
  config.n_bins = n_bins;
  config.bins_per_octave = bins_per_octave;
  const Audio audio = with_c_names(kCqtInverseNames, [&] {
    return sonare::detail::with_fixed_filter_scale({"fmin", "bins_per_octave"}, "sample_rate", [&] {
      return griffinlim_cqt(magnitude, n_bins, n_frames, config, sample_rate, n_iter);
    });
  });
  return fill_audio_samples(audio, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_vqt_to_audio(const float* magnitude, size_t input_length, int n_bins,
                                int n_frames, int sample_rate, size_t hop_length, float fmin_hz,
                                int bins_per_octave, float gamma, int n_iter, float** out,
                                size_t* out_length) {
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
  config.hop_length = core_length(hop_length);
  config.fmin = fmin_hz;
  config.n_bins = n_bins;
  config.bins_per_octave = bins_per_octave;
  config.gamma = gamma;
  const Audio audio = with_c_names(kCqtInverseNames, [&] {
    return sonare::detail::with_fixed_filter_scale(
        {"fmin", "bins_per_octave", "gamma"}, "sample_rate",
        [&] { return griffinlim_vqt(magnitude, n_bins, n_frames, config, sample_rate, n_iter); });
  });
  return fill_audio_samples(audio, out, out_length);
  SONARE_C_CATCH
}

void sonare_mel_to_stft_options_init(SonareMelToStftOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
}

SonareError sonare_mel_to_stft(const float* mel, size_t input_length, int n_mels, int n_frames,
                               int sample_rate, int n_fft, float fmin_hz, float fmax_hz,
                               const SonareMelToStftOptions* options, SonareInverseResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SonareMelToStftOptions resolved;
  if (const SonareError err = resolve_options(options, sonare_mel_to_stft_options_init,
                                              "sonare_mel_to_stft_options_init", &resolved);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(mel);
  if (const SonareError err =
          validate_inverse_input(input_length, n_mels, n_frames, sonare::kInverseMelBandsDomain,
                                 sonare::kMelToStftFramesDomain, "n_mels");
      err != SONARE_OK) {
    return err;
  }
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
  config.htk = resolved.htk != 0;
  std::vector<float> stft = with_c_names(
      kInverseNames, [&] { return mel_to_stft(mel, n_mels, n_frames, config, sample_rate); });
  return fill_inverse_result(stft, n_fft / 2 + 1, n_frames, out);
  SONARE_C_CATCH
}

void sonare_mel_to_audio_options_init(SonareMelToAudioOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
}

SonareError sonare_mel_to_audio(const float* mel, size_t input_length, int n_mels, int n_frames,
                                int sample_rate, int n_fft, size_t hop_length, float fmin_hz,
                                float fmax_hz, int n_iter, const SonareMelToAudioOptions* options,
                                float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SonareMelToAudioOptions resolved;
  if (const SonareError err = resolve_options(options, sonare_mel_to_audio_options_init,
                                              "sonare_mel_to_audio_options_init", &resolved);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(mel);
  if (const SonareError err =
          validate_inverse_input(input_length, n_mels, n_frames, sonare::kInverseMelBandsDomain,
                                 sonare::kMelToStftFramesDomain, "n_mels");
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = validate_mel_inverse(n_mels, n_frames, n_fft, sample_rate, true);
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
  config.htk = resolved.htk != 0;
  Audio audio = with_c_names(kInverseNames, [&] {
    return mel_to_audio(mel, n_mels, n_frames, config, n_iter, sample_rate);
  });
  return fill_audio_samples(audio, out, out_length);
  SONARE_C_CATCH
}

SonareError sonare_griffin_lim(const float* magnitude, size_t input_length, int n_bins,
                               int n_frames, int n_fft, size_t hop_length, int sample_rate,
                               int n_iter, float momentum, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_REQUIRE_NONNULL(magnitude);
  if (const SonareError err = run_validation([&] {
        check::integer_within(n_bins, sonare_c_detail::kInverseSpectrogramExtentDomain, "n_bins");
        check::integer_within(n_frames, sonare_c_detail::kInverseSpectrogramExtentDomain,
                              "n_frames");
        check::even_within(n_fft, sonare::kColaNFftDomain, "n_fft");
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

void sonare_istft_options_init(SonareIstftOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
}

SonareError sonare_istft(const float* magnitude, const float* phase, int n_bins, int n_frames,
                         int n_fft, size_t hop_length, const SonareIstftOptions* options,
                         float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SonareIstftOptions resolved;
  if (const SonareError err = resolve_options(options, sonare_istft_options_init,
                                              "sonare_istft_options_init", &resolved);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(magnitude);
  SONARE_C_REQUIRE_NONNULL(phase);

  size_t total = 0;
  int checked_hop = 0;
  if (const SonareError err = run_validation([&] {
        check::integer_within(n_bins, sonare_c_detail::kInverseSpectrogramExtentDomain, "n_bins");
        check::integer_within(n_frames, sonare_c_detail::kInverseSpectrogramExtentDomain,
                              "n_frames");
        checked_hop = validate_istft_geometry(n_fft, hop_length);
        if (n_bins != n_fft / 2 + 1) {
          check::refuse_relation({"n_bins", "n_fft"}, "{0} must equal {1} / 2 + 1");
        }
        check::integer_within(
            resolved.length_samples,
            check::integer_domain(0, static_cast<long long>(sonare::kMaxAudioBufferSize)),
            "length_samples");

        const size_t bins = static_cast<size_t>(n_bins);
        const size_t frames = static_cast<size_t>(n_frames);
        if (bins > sonare_c_detail::kMaxBufferSize / frames) {
          check::refuse_relation(
              {"n_bins", "n_frames"},
              "{0} * {1} must be at most " + std::to_string(sonare_c_detail::kMaxBufferSize));
        }
        total = bins * frames;

        const std::uint64_t full_length =
            static_cast<std::uint64_t>(n_frames - 1) * static_cast<std::uint64_t>(checked_hop) +
            static_cast<std::uint64_t>(n_fft);
        if (full_length > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
          check::refuse_relation({"n_frames", "hop_length", "n_fft"},
                                 "({0} - 1) * {1} + {2} must fit in int");
        }
      });
      err != SONARE_OK) {
    return err;
  }

  if (const SonareError err = run_validation([&] {
        for (size_t i = 0; i < total; ++i) {
          check::finite_float(magnitude[i], "magnitude");
          check::finite_float(phase[i], "phase");
          check::at_least(magnitude[i], 0.0, "magnitude");
        }
      });
      err != SONARE_OK) {
    return err;
  }

  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_TRY
  std::vector<std::complex<float>> spectrum(total);
  for (size_t i = 0; i < total; ++i) spectrum[i] = std::polar(magnitude[i], phase[i]);
  Spectrogram spec =
      Spectrogram::from_complex(std::move(spectrum), n_bins, n_frames, n_fft, checked_hop,
                                sonare::constants::kDefaultSampleRate, WindowType::Hann);
  const Audio audio = spec.to_audio(static_cast<int>(resolved.length_samples));
  return fill_audio_samples(audio, out, out_length);
  SONARE_C_CATCH
}

void sonare_mfcc_to_mel_options_init(SonareMfccToMelOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
}

SonareError sonare_mfcc_to_mel(const float* coefficients, size_t input_length, int n_mfcc,
                               int n_frames, int n_mels, const SonareMfccToMelOptions* options,
                               SonareInverseResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SonareMfccToMelOptions resolved;
  if (const SonareError err = resolve_options(options, sonare_mfcc_to_mel_options_init,
                                              "sonare_mfcc_to_mel_options_init", &resolved);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(coefficients);
  if (const SonareError err =
          validate_inverse_input(input_length, n_mfcc, n_frames, sonare::kInverseMfccCountDomain,
                                 sonare::kInverseFramesDomain, "n_mfcc");
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  std::vector<float> mel = with_c_names(kInverseNames, [&] {
    return mfcc_to_mel(coefficients, n_mfcc, n_frames, n_mels, resolved.lifter);
  });
  return fill_inverse_result(mel, n_mels, n_frames, out);
  SONARE_C_CATCH
}

void sonare_mfcc_to_audio_options_init(SonareMfccToAudioOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
}

SonareError sonare_mfcc_to_audio(const float* coefficients, size_t input_length, int n_mfcc,
                                 int n_frames, int n_mels, int sample_rate, int n_fft,
                                 size_t hop_length, float fmin_hz, float fmax_hz, int n_iter,
                                 const SonareMfccToAudioOptions* options, float** out,
                                 size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SonareMfccToAudioOptions resolved;
  if (const SonareError err = resolve_options(options, sonare_mfcc_to_audio_options_init,
                                              "sonare_mfcc_to_audio_options_init", &resolved);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(coefficients);
  if (const SonareError err =
          validate_inverse_input(input_length, n_mfcc, n_frames, sonare::kInverseMfccCountDomain,
                                 sonare::kMelToStftFramesDomain, "n_mfcc");
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] {
        check::even_within(n_fft, sonare::kMelToAudioFftSizeDomain, "n_fft");
        check_stft_hop(hop_length, "hop_length");
        check::integer_within(sample_rate, sonare::kInverseAudioSampleRateDomain, "sample_rate");
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
  config.htk = resolved.htk != 0;
  Audio audio = with_c_names(kInverseNames, [&] {
    return mfcc_to_audio(coefficients, n_mfcc, n_frames, config, n_iter, sample_rate,
                         resolved.lifter);
  });
  return fill_audio_samples(audio, out, out_length);
  SONARE_C_CATCH
}

void sonare_free_inverse_result(SonareInverseResult* result) {
  if (!result) return;
  delete[] result->data;
  result->data = nullptr;
  result->rows = 0;
  result->n_frames = 0;
}
