#pragma once

/// @file audio_profile.h
/// @brief Mastering assistant audio profiling.

#include <cstddef>
#include <string>
#include <vector>

#include "core/audio.h"
#include "core/spectrum.h"
#include "util/constants.h"

namespace sonare::mastering::assistant {

struct LoudnessProfile {
  float integrated_lufs = 0.0f;
  float lra_lu = 0.0f;
  float true_peak_db = 0.0f;
  float crest_factor_db = 0.0f;
};

struct SpectralProfile {
  float sub_rms_db = 0.0f;       ///< 20-60 Hz
  float low_rms_db = 0.0f;       ///< 60-250 Hz
  float low_mid_rms_db = 0.0f;   ///< 250-500 Hz
  float mid_rms_db = 0.0f;       ///< 500-2000 Hz
  float high_mid_rms_db = 0.0f;  ///< 2-6 kHz
  float high_rms_db = 0.0f;      ///< 6-12 kHz
  float air_rms_db = 0.0f;       ///< 12 kHz-Nyquist
  float centroid_hz = 0.0f;
  float flatness = 0.0f;
  float rolloff_hz = 0.0f;
};

struct DynamicsProfile {
  float short_term_lufs_std = 0.0f;
  /// Onset peaks per second above a fixed floor of percussive rise; 0 for steady material.
  float attack_density = 0.0f;
  /// Share of frames at or above 0.35 of the RMS reference, which sets aside outlying
  /// events (@ref summary_reference): 0 = transient-heavy, 1 = sustained.
  float sustain_ratio = 0.0f;
};

/// @brief What the six repair detectors measured in the profiled signal.
/// @details Mono fields carry each detector's own scalar; interleaved fields use
///          the reductions documented by @ref analyze_audio_profile_interleaved.
///          The repair headers define what each measures. Filled only when
///          @ref AudioProfileConfig::detect_defects asks for it, so @ref measured
///          is what separates a clean recording from one nothing looked at --
///          the two are the same field values otherwise.
///
///          Each detector runs with its own default config, except hum: that
///          search spans a couple of Hz around its configured fundamental and so
///          reaches only one mains frequency, reporting the other at the search
///          boundary with a prominence barely above clean programme material.
///          Hum is therefore searched at both 50 and 60 Hz and the more
///          prominent result kept, and @ref hum_fundamental_hz says which won.
struct DefectProfile {
  /// True once all six detectors have run over this signal. False says none of
  /// them did: the caller did not ask, or the input was shorter than the noise
  /// detector's STFT.
  bool measured = false;

  // repair/declick.h
  std::size_t click_count = 0;
  /// Outlier runs the criteria excluded. A large value says the detector could
  /// not decide on this material, not that the material is clean.
  std::size_t click_rejected = 0;
  std::size_t click_longest_run_samples = 0;
  float click_per_second = 0.0f;

  // repair/decrackle.h
  std::size_t crackle_sample_count = 0;
  float crackle_sample_fraction = 0.0f;
  float crackle_per_second = 0.0f;

  // repair/declip.h
  std::size_t clip_sample_count = 0;
  std::size_t clip_run_count = 0;
  std::size_t clip_longest_run_samples = 0;
  float clip_sample_fraction = 0.0f;
  /// Flat tops, which answer a different question from the four fields above.
  /// Those are read against declip's configured threshold, so they count the apex
  /// of any waveform that reaches it and miss material clipped before it was
  /// attenuated; these survive a gain change and do not fire on a sine.
  /// clip_flat_level is the level the runs sit at, which is the threshold a
  /// declip pass has to use to reach them. It is the largest pinned-run
  /// level once the two highest runs are set aside, so louder audio without runs
  /// cannot move it. Interleaved aggregation keeps the highest channel level;
  /// @ref declip_threshold_safe says whether that shared threshold is safe for every channel.
  std::size_t clip_flat_run_count = 0;
  std::size_t clip_flat_sample_count = 0;
  std::size_t clip_longest_flat_run_samples = 0;
  float clip_flat_level = 0.0f;
  /// True when the aggregated flat level is safe to use as a declip threshold.
  /// A profile (mono or multi-channel) is unsafe when any channel's peak exceeds
  /// clip_flat_level by more than kDeclipFlatRunLevelWindowDb, or when a channel without
  /// flat-top evidence reaches min(clip_flat_level, 1), the effective shared repair threshold.
  /// Defaults to true for profiles constructed directly by the caller. Read by
  /// the C++ suggester only; not serialized.
  bool declip_threshold_safe = true;

  // repair/denoise_classical.h
  float noise_floor_dbfs = 0.0f;
  /// Loudest of the detector's 32 bands, and which band it is. The profile
  /// carries the peak rather than the array: a caller decides from the level and
  /// from whether the floor sits low or high, and reads the shape out of
  /// detect_noise_floor() when it needs the rest.
  float noise_band_peak_dbfs = 0.0f;
  int noise_band_peak_index = -1;  ///< -1 when no band was measured.

  // repair/dehum.h
  float hum_fundamental_hz = 0.0f;
  /// How far the winning candidate stood above the rest of the search. 1.0 means
  /// the search found no peak at all -- the detector could not decide, which is
  /// not the same answer as no hum.
  float hum_fundamental_prominence = 1.0f;
  int hum_harmonics = 0;
  float hum_fundamental_dbfs = 0.0f;    ///< Input level at f0.
  float hum_peak_harmonic_dbfs = 0.0f;  ///< Loudest k*f0, carried instead of the
                                        ///  detector's per-harmonic array.

  // repair/dereverb_classical.h
  /// Decay across the dereverb module's own late lag, in dB. NOT an RT60: less
  /// negative means the material sustains across the lag, so a reverberant input
  /// reads *higher* here than the same material dry. The detector's
  /// late_predictability is not carried because the WPE stage it comes from is
  /// off in the default config, which would make it zero in every profile.
  float late_decay_ratio_db = 0.0f;
};

struct AudioProfile {
  float duration_sec = 0.0f;
  float bpm = 0.0f;
  float bpm_confidence = 0.0f;
  LoudnessProfile loudness{};
  SpectralProfile spectral{};
  DynamicsProfile dynamics{};
  DefectProfile defects{};
};

/// @brief Rate at which @ref AudioProfileConfig states its window and hop.
inline constexpr int kProfileReferenceRate =
    static_cast<int>(sonare::constants::kDefaultDawSampleRate);

struct AudioProfileConfig {
  /// Window length in samples at @ref kProfileReferenceRate, converted to the input
  /// rate. Band levels are normalized to a Hann window of this length at that rate.
  int n_fft = 2048;
  /// Hop in samples at @ref kProfileReferenceRate, converted to the input rate.
  int hop_length = 512;
  int true_peak_oversample = 4;
  /// Measure @ref AudioProfile::defects. Off by default: the six detectors are
  /// six analysis passes, two of them a further STFT.
  bool detect_defects = false;
};

AudioProfile analyze_audio_profile(const float* samples, std::size_t length, int sample_rate,
                                   const AudioProfileConfig& config = {});
AudioProfile analyze_audio_profile(const Audio& audio, const AudioProfileConfig& config = {});

/// @brief Profiling overloads that hand back the analysis STFT.
/// @details The spectral and dynamics blocks are measured from one STFT of the
///          profiled signal. A caller that needs the same spectrogram reads it
///          here rather than computing a second one: nothing in the tree caches a
///          spectrogram, so the duplicate costs a whole STFT. Check the geometry
///          with @ref sonare::validate_reused_geometry before reading it — the
///          framing is this profiler's, not the caller's.
/// @param spec_out Receives the STFT; left empty when the input is degenerate and
///          no profile is measured. May be null.
/// @{
AudioProfile analyze_audio_profile(const float* samples, std::size_t length, int sample_rate,
                                   const AudioProfileConfig& config, Spectrogram* spec_out);
AudioProfile analyze_audio_profile(const Audio& audio, const AudioProfileConfig& config,
                                   Spectrogram* spec_out);
/// @}

/// @brief Multi-channel counterpart preserving BS.1770 channel summing.
/// @details The `loudness` block is measured from the channels: integrated
///          LUFS and LRA come from the channel-summed program and the true peak
///          is the largest across the channels, so decorrelated stereo is not
///          read roughly 6 dB low the way a `0.5 * (L + R)` downmix reads it.
///          The spectral, dynamics and tempo fields describe the downmix. Defect
///          detectors run independently on every channel: counts sum, rates and
///          fractions average, longest runs take the maximum, flat evidence keeps
///          the highest plateau, noise floors sum in power (including every
///          detector band), the strongest complete hum candidate wins, and late
///          decay takes the maximum. `dynamics.shortTermLufsStd` is a spread
///          rather than a level, so the downmix's near-constant loudness offset
///          cancels out of it.
/// @param samples Pointer to `frames * channels` interleaved samples.
/// @param frames Number of sample frames.
/// @param channels Channel count; must be positive.
/// @param sample_rate Sample rate in Hz; must be positive.
AudioProfile analyze_audio_profile_interleaved(const float* samples, std::size_t frames,
                                               int channels, int sample_rate,
                                               const AudioProfileConfig& config = {});

/// @brief Multi-channel profiling that hands back the analysis STFT.
/// @details The STFT is the one measured over the downmix, which is the signal
///          the spectral block describes; the loudness block is measured from the
///          channels and has no spectrogram.
/// @param spec_out Receives the downmix STFT. May be null.
AudioProfile analyze_audio_profile_interleaved(const float* samples, std::size_t frames,
                                               int channels, int sample_rate,
                                               const AudioProfileConfig& config,
                                               Spectrogram* spec_out);
/// @brief The STFT geometry @p config describes at @p sample_rate.
/// @details Window and hop are converted from @ref kProfileReferenceRate; the hop is
///          at least one sample. Unchanged at the reference rate.
StftConfig profile_stft_config(const AudioProfileConfig& config, int sample_rate);

/// @brief Factor that rescales one bin of @p spec's power to the reference geometry.
/// @details The reference is a Hann window of @p n_fft_at_reference_rate samples at
///          @ref kProfileReferenceRate. The factor is the density ratio
///          `(ref_sr * Σw_ref²) / (sr * Σw²)` over the spectrogram's actual window, so a
///          bin's power describes the same spectral density whatever the input rate. A
///          sum over a band's bins also needs @ref reference_bin_width_ratio. Exactly 1
///          at the reference rate with the reference window.
double reference_power_scale(const Spectrogram& spec, int n_fft_at_reference_rate);

/// @brief `(sr / n_fft) / (ref_sr / n_fft_at_reference_rate)`: the spectrogram's bin
///        width over the reference bin width. Exactly 1 at the reference rate.
double reference_bin_width_ratio(const Spectrogram& spec, int n_fft_at_reference_rate);

/// @brief Heights of an envelope's local maxima that rise out of the frames before them.
/// @return `series[i]` for each i with `series[i] > series[i - 1]`, `series[i] >= series[i + 1]`
///         and `series[i] > rise * min(series[i - lookback .. i - 1])`, in frame order.
std::vector<float> rising_peak_heights(const std::vector<float>& series, int lookback, float rise);

/// @brief Level an assistant summary measure takes its relative threshold against.
/// @param events Heights of the measured events.
/// @param duration_sec Length of the measured signal.
/// @param series_max Maximum of the whole series.
/// @return `max_excluding_top(events, K)` with K = min(kReferenceIgnoredTopEvents,
///         floor(duration_sec / 10 s)); @p series_max when K is 0 or @p events holds no more
///         than K values.
/// @details Below ten seconds there are too few events to tell an outlier from the body,
///          so nothing is set aside.
float summary_reference(std::vector<float> events, float duration_sec, float series_max);

std::string audio_profile_to_json(const AudioProfile& profile);

/// @brief Every dotted field path @ref audio_profile_to_json emits.
/// @details The JSON crosses to user code as a string each facade parses and
///          casts, so nothing type-checks it on arrival. This list is what the
///          per-surface declarations are compared against, and a set equality
///          against a serialized fixture is what keeps it from drifting either
///          way. An array contributes its element's paths under a `[]` segment
///          and nothing of its own.
const std::vector<std::string>& audio_profile_schema_paths();

}  // namespace sonare::mastering::assistant
