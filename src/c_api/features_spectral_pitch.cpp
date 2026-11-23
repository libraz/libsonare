#include <limits>
#include <string>

#include "c_api/feature_input_domains.h"
#include "c_api/features_internal.h"
#include "core/synthesis.h"
#include "feature/chroma.h"
#include "feature/onset.h"
#include "util/constants.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"

using sonare::constants::kCentsPerSemitone;
namespace check = sonare::util::check;

// Core - Synthetic audio generation
// ============================================================================

SonareError sonare_tone(float frequency_hz, int sample_rate, float duration_sec, float phase,
                        float amplitude, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_TRY
  return with_c_names(
      std::array<CName, 2>{{{"sr", "sample_rate"}, {"duration", "duration_sec"}}}, [&] {
        return fill_audio_samples(tone(frequency_hz, sample_rate, duration_sec, phase, amplitude),
                                  out, out_length);
      });
  SONARE_C_CATCH
}

SonareError sonare_chirp(float fmin_hz, float fmax_hz, int sample_rate, float duration_sec,
                         int linear, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err =
          run_validation([&] { check::integer_in_range(linear, 0, 1, "linear"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_TRY
  return with_c_names(std::array<CName, 4>{{{"fmin", "fmin_hz"},
                                            {"fmax", "fmax_hz"},
                                            {"sr", "sample_rate"},
                                            {"duration", "duration_sec"}}},
                      [&] {
                        return fill_audio_samples(
                            chirp(fmin_hz, fmax_hz, sample_rate, duration_sec, linear != 0), out,
                            out_length);
                      });
  SONARE_C_CATCH
}

SonareError sonare_clicks(const float* times, size_t time_count, int sample_rate, size_t length,
                          float frequency_hz, float click_duration_sec, float** out,
                          size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(times, time_count);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_TRY
  std::vector<float> values;
  if (time_count) values.assign(times, times + time_count);
  return with_c_names(std::array<CName, 3>{{{"sr", "sample_rate"},
                                            {"frequency", "frequency_hz"},
                                            {"click_duration", "click_duration_sec"}}},
                      [&] {
                        return fill_audio_samples(clicks(values, sample_rate, core_length(length),
                                                         frequency_hz, click_duration_sec),
                                                  out, out_length);
                      });
  SONARE_C_CATCH
}

// Features - Spectral
// ============================================================================

SonareError sonare_spectral_centroid(const float* samples, size_t length, int sample_rate,
                                     int n_fft, size_t hop_length, float** out, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_count);
  if (const SonareError err = validate_audio_params("samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_count);
  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        StftConfig config;
        config.n_fft = n_fft;
        config.hop_length = core_length(hop_length);
        Spectrogram spec = Spectrogram::compute(audio, config);
        std::vector<float> result = spectral_centroid(spec, audio.sample_rate());
        return copy_vector(result, out, out_count);
      });
}

void sonare_spectral_bandwidth_options_init(SonareSpectralBandwidthOptions* options) {
  if (options == nullptr) return;
  *options = {};
  options->struct_size = sizeof(*options);
  options->p = 2.0f;
}

SonareError sonare_spectral_bandwidth(const float* samples, size_t length, int sample_rate,
                                      int n_fft, size_t hop_length,
                                      const SonareSpectralBandwidthOptions* options, float** out,
                                      size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_count);
  SonareSpectralBandwidthOptions resolved;
  if (const SonareError err = resolve_options(options, sonare_spectral_bandwidth_options_init,
                                              "sonare_spectral_bandwidth_options_init", &resolved);
      err != SONARE_OK) {
    return err;
  }
  const float p = resolved.p;
  if (const SonareError err = validate_audio_params("samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] { check::greater_than(p, 0.0, "options.p"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_count);
  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        StftConfig config;
        config.n_fft = n_fft;
        config.hop_length = core_length(hop_length);
        Spectrogram spec = Spectrogram::compute(audio, config);
        std::vector<float> result = spectral_bandwidth(spec, audio.sample_rate(), p);
        return copy_vector(result, out, out_count);
      });
}

SonareError sonare_spectral_rolloff(const float* samples, size_t length, int sample_rate, int n_fft,
                                    size_t hop_length, float roll_percent, float** out,
                                    size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_count);
  if (const SonareError err = validate_audio_params("samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_count);
  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        StftConfig config;
        config.n_fft = n_fft;
        config.hop_length = core_length(hop_length);
        Spectrogram spec = Spectrogram::compute(audio, config);
        std::vector<float> result = spectral_rolloff(spec, audio.sample_rate(), roll_percent);
        return copy_vector(result, out, out_count);
      });
}

SonareError sonare_spectral_flatness(const float* samples, size_t length, int sample_rate,
                                     int n_fft, size_t hop_length, float** out, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_count);
  if (const SonareError err = validate_audio_params("samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_count);
  return run_prevalidated_offline(samples, length, sample_rate,
                                  [&](const Audio& audio) -> SonareError {
                                    StftConfig config;
                                    config.n_fft = n_fft;
                                    config.hop_length = core_length(hop_length);
                                    Spectrogram spec = Spectrogram::compute(audio, config);
                                    std::vector<float> result = spectral_flatness(spec);
                                    return copy_vector(result, out, out_count);
                                  });
}

SonareError sonare_spectral_flux(const float* samples, size_t length, int sample_rate, int n_fft,
                                 size_t hop_length, int lag, float** out, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_count);
  if (const SonareError err = validate_audio_params("samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err =
          run_validation([&] { check::integer_within(lag, sonare::kOnsetCountDomain, "lag"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_count);
  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        StftConfig config;
        config.n_fft = n_fft;
        config.hop_length = core_length(hop_length);
        const std::vector<float> result = spectral_flux(Spectrogram::compute(audio, config), lag);
        return copy_vector(result, out, out_count);
      });
}

SonareError sonare_zero_crossing_rate(const float* samples, size_t length, int sample_rate,
                                      size_t frame_length, size_t hop_length, float** out,
                                      size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_count);
  if (const SonareError err = validate_audio_params("samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_count);
  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        std::vector<float> result =
            zero_crossing_rate(audio, core_length(frame_length), core_length(hop_length));
        return copy_vector(result, out, out_count);
      });
}

SonareError sonare_rms_energy(const float* samples, size_t length, int sample_rate,
                              size_t frame_length, size_t hop_length, float** out,
                              size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_count);
  if (const SonareError err = validate_audio_params("samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_count);
  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        std::vector<float> result =
            rms_energy(audio, core_length(frame_length), core_length(hop_length));
        return copy_vector(result, out, out_count);
      });
}

SonareError sonare_spectral_contrast(const float* samples, size_t length, int sample_rate,
                                     int n_fft, size_t hop_length, int n_bands, float fmin_hz,
                                     float quantile, float** out, size_t* out_length,
                                     int* out_n_subbands, int* out_n_frames) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length, out_n_subbands, out_n_frames);
  if (const SonareError err = validate_audio_params("samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_REQUIRE_NONNULL(out_n_subbands);
  SONARE_C_REQUIRE_NONNULL(out_n_frames);
  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        StftConfig config;
        config.n_fft = n_fft;
        config.hop_length = core_length(hop_length);
        Spectrogram spec = Spectrogram::compute(audio, config);
        std::vector<float> result =
            with_c_names(std::array<CName, 2>{{{"fmin", "fmin_hz"}, {"sr", "sample_rate"}}}, [&] {
              return spectral_contrast(spec, audio.sample_rate(), n_bands, fmin_hz, quantile);
            });
        if (const SonareError err = copy_vector(result, out, out_length); err != SONARE_OK) {
          return err;
        }
        *out_n_subbands = n_bands + 1;
        *out_n_frames = spec.n_frames();
        return SONARE_OK;
      });
}

SonareError sonare_poly_features(const float* samples, size_t length, int sample_rate, int n_fft,
                                 size_t hop_length, int order, float** out, size_t* out_length,
                                 int* out_n_coefficients, int* out_n_frames) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length, out_n_coefficients, out_n_frames);
  if (const SonareError err = validate_audio_params("samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_REQUIRE_NONNULL(out_n_coefficients);
  SONARE_C_REQUIRE_NONNULL(out_n_frames);
  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        StftConfig config;
        config.n_fft = n_fft;
        config.hop_length = core_length(hop_length);
        Spectrogram spec = Spectrogram::compute(audio, config);
        std::vector<float> result = poly_features(spec, audio.sample_rate(), order);
        if (const SonareError err = copy_vector(result, out, out_length); err != SONARE_OK) {
          return err;
        }
        *out_n_coefficients = order + 1;
        *out_n_frames = spec.n_frames();
        return SONARE_OK;
      });
}

SonareError sonare_zero_crossings(const float* samples, size_t length, float threshold,
                                  int ref_magnitude, int pad, int zero_pos, int** out,
                                  size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_count);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(samples, length);
  if (const SonareError err = run_validation([&] { check::at_least(threshold, 0.0, "threshold"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_count);

  SONARE_C_TRY
  std::vector<int> result =
      with_c_names(std::array<CName, 2>{{{"y", "samples"}, {"n", "length"}}}, [&] {
        return zero_crossings(samples, length, threshold, ref_magnitude != 0, pad != 0,
                              zero_pos != 0);
      });
  *out_count = result.size();
  if (result.empty()) return SONARE_OK;
  std::unique_ptr<int[]> tmp(new int[result.size()]);
  std::memcpy(tmp.get(), result.data(), result.size() * sizeof(int));
  *out = release_array(tmp);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_onset_backtrack(const int* events, size_t event_count, const float* energy,
                                   size_t energy_count, int** out, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_count);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(events, event_count);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(energy, energy_count);
  // An empty envelope is legitimate here (onset_backtrack then has nothing to
  // walk back over), but a non-finite one is not: a NaN makes the descent
  // comparison false, so the walk stops on the spot and the event index is
  // returned as if it were already a local minimum. Matches the WASM
  // onsetBacktrack binding, which already rejects it via validateFiniteVector,
  // and the non-finite policy of validate_audio_params.
  //
  // This one guards the caller-supplied envelope at the boundary rather than
  // inside core onset_backtrack, unlike the decompose / nn_filter / mel-delta
  // rules. The core function is also fed envelopes the library computed itself
  // (OnsetAnalyzer::backtrack_onsets, and through it MusicAnalyzer and
  // RhythmAnalyzer), and compute_onset_strength can produce a non-finite
  // envelope from input this API accepts as valid: samples near FLT_MAX are
  // finite, so validate_audio_params admits them, but the STFT then overflows
  // to Inf. Making finiteness a core precondition would turn those analyses
  // into thrown exceptions. Finiteness is therefore a contract on envelopes
  // that come from outside, not an invariant of the core function.
  if (const SonareError err = run_validation([&] {
        for (size_t index = 0; index < energy_count; ++index) {
          if (!std::isfinite(energy[index])) {
            check::refuse("energy",
                          "contains a non-finite sample at index " + std::to_string(index));
          }
        }
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_count);
  SONARE_C_TRY
  std::vector<int> event_values;
  std::vector<float> energy_values;
  if (event_count) event_values.assign(events, events + event_count);
  if (energy_count) energy_values.assign(energy, energy + energy_count);
  const std::vector<int> result = onset_backtrack(event_values, energy_values);
  *out_count = result.size();
  if (result.empty()) return SONARE_OK;
  std::unique_ptr<int[]> buffer(new int[result.size()]);
  std::memcpy(buffer.get(), result.data(), result.size() * sizeof(int));
  *out = release_array(buffer);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_pitch_tuning(const float* frequencies, size_t length, float resolution,
                                int bins_per_octave, float* out_tuning) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_tuning);
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(frequencies, length);
  if (const SonareError err =
          run_validation([&] { validate_tuning_params(resolution, bins_per_octave); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_tuning);

  SONARE_C_TRY
  std::vector<float> freqs;
  if (frequencies && length > 0) {
    freqs.assign(frequencies, frequencies + length);
  }
  *out_tuning = pitch_tuning(freqs, resolution, bins_per_octave);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_estimate_tuning(const float* samples, size_t length, int sample_rate, int n_fft,
                                   size_t hop_length, float resolution, int bins_per_octave,
                                   float* out_tuning) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_tuning);
  if (const SonareError err = validate_audio_params("samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err =
          run_validation([&] { validate_tuning_params(resolution, bins_per_octave); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_tuning);

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        *out_tuning =
            estimate_tuning(audio, n_fft, core_length(hop_length), resolution, bins_per_octave);
        return SONARE_OK;
      });
}

SonareError sonare_tuning_to_reference_hz(float tuning_cents, float a4_hz, float* out_hz) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_hz);
  SONARE_C_TRY
  check::finite(tuning_cents, "tuning_cents");
  check::greater_than(a4_hz, 0.0, "a4_hz");
  SONARE_C_REQUIRE_NONNULL(out_hz);
  const float hz = tuning_to_reference_hz(tuning_cents / kCentsPerSemitone, a4_hz);
  if (!numeric::finite_positive(hz)) {
    check::refuse_relation({"a4_hz", "tuning_cents"},
                           "{0} * 2^({1} / 1200) must be finite and greater than 0");
  }
  *out_hz = hz;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_reference_hz_to_tuning(float reference_hz, float a4_hz,
                                          float* out_tuning_cents) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_tuning_cents);
  SONARE_C_TRY
  check::greater_than(reference_hz, 0.0, "reference_hz");
  check::greater_than(a4_hz, 0.0, "a4_hz");
  SONARE_C_REQUIRE_NONNULL(out_tuning_cents);
  *out_tuning_cents = reference_hz_to_tuning(reference_hz, a4_hz) * kCentsPerSemitone;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_piptrack(const float* samples, size_t length, int sample_rate, int n_fft,
                            size_t hop_length, float fmin_hz, float fmax_hz, float threshold,
                            float** out_pitches, float** out_magnitudes, size_t* out_length,
                            int* out_n_bins, int* out_n_frames) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_pitches, out_magnitudes, out_length, out_n_bins, out_n_frames);
  if (const SonareError err = validate_audio_params("samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_pitches);
  SONARE_C_REQUIRE_NONNULL(out_magnitudes);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_REQUIRE_NONNULL(out_n_bins);
  SONARE_C_REQUIRE_NONNULL(out_n_frames);

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        PiptrackResult result =
            with_c_names(std::array<CName, 2>{{{"fmin", "fmin_hz"}, {"fmax", "fmax_hz"}}}, [&] {
              return piptrack(audio, n_fft, core_length(hop_length), fmin_hz, fmax_hz, threshold);
            });
        const size_t total = result.pitches.size();
        if (result.magnitudes.size() != total) return SONARE_ERROR_UNKNOWN;
        std::unique_ptr<float[]> pitches(total ? new float[total] : nullptr);
        std::unique_ptr<float[]> magnitudes(total ? new float[total] : nullptr);
        if (total) {
          std::memcpy(pitches.get(), result.pitches.data(), total * sizeof(float));
          std::memcpy(magnitudes.get(), result.magnitudes.data(), total * sizeof(float));
        }
        *out_length = total;
        *out_n_bins = result.n_bins;
        *out_n_frames = result.n_frames;
        *out_pitches = release_array(pitches);
        *out_magnitudes = release_array(magnitudes);
        return SONARE_OK;
      });
}

SonareError sonare_reassigned_spectrogram(const float* samples, size_t length, int sample_rate,
                                          int n_fft, size_t hop_length, float ref_power,
                                          int fill_nan, SonareReassignedSpectrogramResult* out) {
  SONARE_C_API_ENTRY;
  // Zero the owned out-pointers BEFORE any validating early-return, so a rejected
  // input always leaves a NULL owned pointer and sonare_free_reassigned_spectrogram_result(&r)
  // never delete[]s an uninitialised pointer.
  SONARE_C_ZERO_OUTS(out);
  if (const SonareError err = validate_audio_params("samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] {
        check::even_within(n_fft, sonare::kStftNFftDomain, "n_fft");
        check::integer_within(hop_length, sonare::kStftHopDomain, "hop_length");
        check::at_least(ref_power, 0.0, "ref_power");
        check::integer_in_range(fill_nan, 0, 1, "fill_nan");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        StftConfig config;
        config.n_fft = n_fft;
        config.hop_length = core_length(hop_length);
        ReassignedSpectrogram result =
            reassigned_spectrogram(audio, config, ref_power, fill_nan != 0);
        if (result.times.size() != result.magnitude.size() ||
            result.frequencies.size() != result.magnitude.size()) {
          return SONARE_ERROR_UNKNOWN;
        }
        const int n_bins = n_fft / 2 + 1;
        if (n_bins <= 0 || result.magnitude.size() % static_cast<size_t>(n_bins) != 0) {
          return SONARE_ERROR_UNKNOWN;
        }
        const int n_frames =
            static_cast<int>(result.magnitude.size() / static_cast<size_t>(n_bins));
        std::unique_ptr<float[]> magnitude(
            result.magnitude.empty() ? nullptr : new float[result.magnitude.size()]);
        std::unique_ptr<float[]> times(result.times.empty() ? nullptr
                                                            : new float[result.times.size()]);
        std::unique_ptr<float[]> frequencies(
            result.frequencies.empty() ? nullptr : new float[result.frequencies.size()]);
        if (!result.magnitude.empty()) {
          std::memcpy(magnitude.get(), result.magnitude.data(),
                      result.magnitude.size() * sizeof(float));
          std::memcpy(times.get(), result.times.data(), result.times.size() * sizeof(float));
          std::memcpy(frequencies.get(), result.frequencies.data(),
                      result.frequencies.size() * sizeof(float));
        }
        out->n_bins = n_bins;
        out->n_frames = n_frames;
        out->magnitude = release_array(magnitude);
        out->times = release_array(times);
        out->frequencies = release_array(frequencies);
        return SONARE_OK;
      });
}

void sonare_free_reassigned_spectrogram_result(SonareReassignedSpectrogramResult* result) {
  if (!result) return;
  delete[] result->magnitude;
  delete[] result->times;
  delete[] result->frequencies;
  *result = {};
}

namespace {

template <typename PitchFn>
SonareError run_pitch_track(const char* name, const float* samples, size_t length, int sample_rate,
                            size_t frame_length, size_t hop_length, float fmin, float fmax,
                            float threshold, int fill_na, SonarePitchResult* out, PitchFn pitch) {
  if (const SonareError err = validate_audio_params(name, samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        PitchConfig config;
        config.frame_length = core_length(frame_length);
        config.hop_length = core_length(hop_length);
        config.fmin = fmin;
        config.fmax = fmax;
        config.threshold = threshold;
        config.fill_na = fill_na != 0;
        PitchResult result = with_c_names(
            std::array<CName, 3>{
                {{"fmin", "f0_min_hz"}, {"fmax", "f0_max_hz"}, {"sr", "sample_rate"}}},
            [&] { return pitch(audio, config); });
        return fill_pitch_result(result, out);
      });
}

}  // namespace

SonareError sonare_pitch_yin(const float* samples, size_t length, int sample_rate,
                             size_t frame_length, size_t hop_length, float f0_min_hz,
                             float f0_max_hz, float threshold, SonarePitchResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);

  return run_pitch_track("samples", samples, length, sample_rate, frame_length, hop_length,
                         f0_min_hz, f0_max_hz, threshold,
                         /*fill_na=*/0, out, [](const Audio& audio, const PitchConfig& config) {
                           return yin_track(audio, config);
                         });
}

SonareError sonare_pitch_pyin(const float* samples, size_t length, int sample_rate,
                              size_t frame_length, size_t hop_length, float f0_min_hz,
                              float f0_max_hz, float threshold, int fill_na,
                              SonarePitchResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);

  return run_pitch_track(
      "samples", samples, length, sample_rate, frame_length, hop_length, f0_min_hz, f0_max_hz,
      threshold, fill_na, out,
      [](const Audio& audio, const PitchConfig& config) { return pyin(audio, config); });
}

namespace {
/// Voicing probability at or above which a frame counts as voiced when no config is given.
constexpr float kDefaultVoicedThreshold = 0.5f;
}  // namespace

void sonare_note_segmenter_config_init(SonareNoteSegmenterConfig* config) {
  if (config == nullptr) return;
  const sonare::editing::pitch_editor::NoteSegmenterConfig native;
  *config = {};
  config->struct_size = sizeof(*config);
  config->segmentation_threshold_cents = native.segmentation_threshold_cents;
  config->min_note_ms = native.min_note_ms;
  config->reference_hz = native.reference_hz;
  config->voiced_threshold = kDefaultVoicedThreshold;
}

SonareError sonare_note_segments(const float* f0_hz, const float* voiced_prob,
                                 const int32_t* voiced, size_t n_frames, float frame_rate,
                                 const SonareNoteSegmenterConfig* config,
                                 SonareNoteSegmentsResult* out) {
  SONARE_C_API_ENTRY;
  // Zeroed before the gate, so sonare_free_note_segments_result on a NOT_SUPPORTED
  // result deletes a null pointer rather than whatever the caller's stack held.
  SONARE_C_ZERO_OUTS(out);
#if defined(SONARE_WITH_PITCH_EDITOR)
  SONARE_C_REQUIRE_NONNULL(f0_hz);
  if (const SonareError err = run_validation([&] {
        if (voiced_prob == nullptr && voiced == nullptr) {
          check::refuse_relation({"voiced_prob", "voiced"},
                                 "{0} and {1} {must_not_both_be_absent}");
        }
        if (voiced_prob != nullptr && voiced != nullptr) {
          check::refuse_relation({"voiced_prob", "voiced"},
                                 "{1} {must_be_absent} when {0} {is_present}");
        }
        check::integer_within(n_frames, sonare_c_detail::kNoteSegmentFrameCountDomain, "n_frames");
        check::greater_than(frame_rate, 0.0, "frame_rate");
      });
      err != SONARE_OK) {
    return err;
  }

  sonare::editing::pitch_editor::NoteSegmenterConfig segmenter_config;
  float voiced_threshold = kDefaultVoicedThreshold;
  if (config != nullptr) {
    if (const SonareError err = run_validation([&] {
          require_struct_size(config->struct_size, sizeof(SonareNoteSegmenterConfig), "config",
                              "sonare_note_segmenter_config_init");
        });
        err != SONARE_OK) {
      return err;
    }
    if (const SonareError err = run_validation_under(
            "config",
            [&] {
              check::greater_than(config->segmentation_threshold_cents, 0.0,
                                  "segmentation_threshold_cents");
              check::at_least(config->min_note_ms, 0.0, "min_note_ms");
              check::greater_than(config->reference_hz, 0.0, "reference_hz");
              check::in_range(config->voiced_threshold, 0.0, 1.0, "voiced_threshold");
            });
        err != SONARE_OK) {
      return err;
    }
    segmenter_config.segmentation_threshold_cents = config->segmentation_threshold_cents;
    segmenter_config.min_note_ms = config->min_note_ms;
    segmenter_config.reference_hz = config->reference_hz;
    voiced_threshold = config->voiced_threshold;
  }
  SONARE_C_REQUIRE_NONNULL(out);

  SONARE_C_TRY
  sonare::editing::pitch_editor::F0Track track;
  track.f0_hz.reserve(n_frames);
  track.voiced_prob.reserve(n_frames);
  track.voiced.reserve(n_frames);
  track.frame_rate_hz = frame_rate;
  for (size_t i = 0; i < n_frames; ++i) {
    const float f0 = f0_hz[i];
    // A flag reads as the probability it stands for, so flags and 0/1 probabilities agree.
    const float probability = voiced != nullptr ? (voiced[i] != 0 ? 1.0f : 0.0f) : voiced_prob[i];
    // The F0 value is not checked: a frame carrying no pitch is spelled zero,
    // negative or non-finite, and the segmenter reads all three the same way.
    // The probability is, because it is compared against a threshold below and
    // a non-finite one would lose that comparison and read as unvoiced.
    if (!std::isfinite(probability) || probability < 0.0f || probability > 1.0f) {
      const std::string subject = "voiced_prob[" + std::to_string(i) + "]";
      check::in_range(probability, 0.0, 1.0, subject.c_str());
    }
    track.f0_hz.push_back(f0);
    track.voiced_prob.push_back(probability);
    track.voiced.push_back(probability >= voiced_threshold);
  }

  const auto regions =
      sonare::editing::pitch_editor::NoteSegmenter(segmenter_config).segment(track);
  if (regions.empty()) return SONARE_OK;
  auto segments = std::make_unique<SonareNoteSegment[]>(regions.size());
  for (size_t i = 0; i < regions.size(); ++i) {
    const auto& region = regions[i];
    segments[i] = {region.frame_start, region.frame_end,
                   static_cast<float>(region.frame_start) / frame_rate,
                   static_cast<float>(region.frame_end) / frame_rate, region.median_cents};
  }
  out->count = regions.size();
  out->segments = segments.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_REQUIRE_NONNULL(out);
  return sonare_c_detail::refuse_not_compiled_in(sonare_c_detail::Feature::kPitchEditor, f0_hz,
                                                 voiced_prob, voiced, n_frames, frame_rate, config,
                                                 out);
#endif
}

void sonare_free_note_segments_result(SonareNoteSegmentsResult* result) {
  if (result == nullptr) return;
  delete[] result->segments;
  result->segments = nullptr;
  result->count = 0;
}

float sonare_hz_to_mel(float hz) { return hz_to_mel(hz); }

float sonare_mel_to_hz(float mel) { return mel_to_hz(mel); }

float sonare_hz_to_midi(float hz) { return hz_to_midi(hz); }

float sonare_midi_to_hz(float midi) { return midi_to_hz(midi); }

SonareError sonare_hz_to_note(float hz, char** out_string) {
  SONARE_C_API_ENTRY;
  if (!begin_string_output(out_string, "out_string")) return SONARE_ERROR_INVALID_PARAMETER;
  SONARE_C_TRY
  *out_string = copy_string(hz_to_note(hz));
  return SONARE_OK;
  SONARE_C_CATCH
}

float sonare_note_to_hz(const char* note) {
  SONARE_C_TRY
  if (!note) check::refuse("note", sonare::util::refusal::kMustBePresent);
  return note_to_hz(std::string(note));
  SONARE_C_CATCH_RETURN(std::numeric_limits<float>::quiet_NaN())
}

float sonare_frames_to_time(int frames, int sample_rate, size_t hop_length) {
  SONARE_C_TRY
  return frames_to_time(frames, sample_rate, core_length(hop_length));
  SONARE_C_CATCH_RETURN(std::numeric_limits<float>::quiet_NaN())
}

int sonare_time_to_frames(float time_sec, int sample_rate, size_t hop_length) {
  SONARE_C_TRY
  return time_to_frames(time_sec, sample_rate, core_length(hop_length));
  SONARE_C_CATCH_RETURN(0)
}

SonareError sonare_resample(const float* samples, size_t length, int sample_rate,
                            int target_sample_rate, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);

  SONARE_C_TRY
  // An empty span is valid even with a null data pointer and yields (NULL, 0).
  std::vector<float> result = resample_checked(samples, length, sample_rate, target_sample_rate);
  return copy_vector(result, out, out_length);
  SONARE_C_CATCH
}
