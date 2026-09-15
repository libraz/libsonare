#pragma once

/// @file dereverb_classical.h
/// @brief Classical (non-ML) STFT-domain late-reverberation suppression.

#include <cstddef>
#include <vector>

#include "core/audio.h"

namespace sonare::mastering::repair {

/// @brief Longest WPE predictor a dereverb pass accepts, in frames.
/// @details Bounds a per-bin dense solve that is cubic in the tap count and
/// independent of the input length. 32 taps past the late delay is 170 ms at the
/// default geometry, beyond where a tail is still predictable frame to frame.
inline constexpr int kDereverbMaxWpeTaps = 32;

/// @brief Most WPE refinement passes a dereverb pass accepts.
/// @details Each pass repeats the whole per-bin solve; the predictor stops moving
/// well before this, so the ceiling bounds the cost rather than the quality.
inline constexpr int kDereverbMaxWpeIterations = 16;

struct DereverbClassicalConfig {
  /// Late-reverberation detection threshold, relative to a bin's own power. A
  /// bin whose estimated late PSD does not exceed `threshold * power` is not
  /// treated as late reverberation and is left untouched. 0 suppresses every bin
  /// carrying any late energy, which is what this module has always done.
  float threshold = 0.0f;
  /// How much of the computed suppression to apply, from 0 (none) to 1 (all).
  /// 1 applies the Berouti-style subtraction verbatim, which is what this module
  /// has always done.
  float attenuation = 1.0f;
  int n_fft = 1024;
  int hop_length = 256;
  float t60_sec = 0.4f;
  float late_delay_ms = 50.0f;
  float over_subtraction = 1.0f;
  float spectral_floor = 0.08f;
  bool wpe_enabled = false;
  int wpe_iterations = 2;
  int wpe_taps = 3;
  float wpe_strength = 0.7f;
};

/// Validates every public DereverbClassicalConfig field. Named mono/linked
/// dispatch, the detector and the direct DSP entrypoints share this oracle so
/// range handling cannot drift between surfaces.
void validate_config(const DereverbClassicalConfig& config);

/// @brief What a dereverb analysis found in the input.
/// @details NOT an ISO 3382 reverberation time. There is no Schroeder backward
///   integration, no noise-floor truncation, the bands are the STFT bin grid
///   rather than octave bands, and music is not a free decay. For a graded RT60
///   use detect_acoustic(); this reports what this module itself measures while
///   deciding how much to subtract.
struct ReverbDetection {
  /// Decay across the module's own late-reverb lag, in dB: the low percentile
  /// of power[b][t] / power[b][t - delay_frames]. Zero means no decay measured.
  /// Less negative means the material sustains across the lag, which a late tail
  /// does and a dry offset does not -- so a reverberant input reads *higher*
  /// here than the same material dry.
  float late_decay_ratio_db = 0.0f;
  /// Mean WPE predictor norm, recorded BEFORE the 0.98 clamp. Higher means the
  /// frame is more predictable from the lagged one, which is what a late tail
  /// is. Zero when the WPE stage did not run.
  float late_predictability = 0.0f;
};

/// @brief What a dereverb pass found and what it removed.
struct DereverbReport {
  ReverbDetection detected;
  float mean_reduction_db = 0.0f;  ///< Mean attenuation the subtraction mask applied.
  /// Fraction of mask cells the `threshold` gate admitted as late reverberation.
  /// The only observation of that knob: 0 says the threshold suppressed nothing
  /// while the mask still reports a mean, which no other field separates.
  float suppressed_fraction = 0.0f;
  /// Mean WPE predictor norm actually applied, after the 0.98 clamp. Below
  /// `detected.late_predictability` says the clamp acted, which is otherwise a
  /// silent branch. Zero when the WPE stage did not run.
  float wpe_predictor_norm = 0.0f;
};

/// @brief Measures reverberation without dereverberating.
/// @details Runs the STFT and the module's own late-lag decay statistic. The WPE
///   analysis runs only when `config.wpe_enabled`, and then only its covariance
///   and solve -- the prediction is never subtracted.
/// @param samples Input samples; a buffer shorter than `config.n_fft` is padded
///        for analysis, as the repair pads it.
/// @param size Number of samples.
/// @param sample_rate Sample rate in Hz; must be positive.
/// @param config Analysis configuration.
/// @throws SonareException(InvalidParameter) for a rejected config, a
///         non-positive sample rate, or no samples.
ReverbDetection detect_reverb(const float* samples, std::size_t size, int sample_rate,
                              const DereverbClassicalConfig& config = {});

Audio dereverb_classical(const Audio& audio, const DereverbClassicalConfig& config = {});

/// @brief Dereverberates @p audio and reports what the pass found and did.
/// @details @p report may be null, which skips the report and nothing else: the
///   samples are the two-argument overload's, bit for bit.
Audio dereverb_classical(const Audio& audio, const DereverbClassicalConfig& config,
                         DereverbReport* report);

/// @brief Dereverberates a channel set with one channel-linked gain mask.
/// @details The mask is built from the channel-summed power and applied
///   unchanged to every channel; the WPE stage accumulates its statistics over
///   every channel and applies one predictor set to all of them. Both stages are
///   therefore identical across channels and cannot move an interchannel level
///   or phase difference. Fed one channel it reproduces @ref dereverb_classical
///   bit for bit.
/// @param channels Input channels; all the same length and sample rate.
/// @param channel_count Number of entries in @p channels; at least one.
/// @param out Receives one output per channel. Must not be null.
/// @param config Analysis configuration.
/// @throws SonareException(InvalidParameter) for a rejected config, a null
///         output, no channels, or channels that disagree on length or rate.
DereverbReport dereverb_classical_linked(const Audio* const* channels, std::size_t channel_count,
                                         std::vector<Audio>* out,
                                         const DereverbClassicalConfig& config = {});

/// @brief A dereverberated channel pair and the mask that produced it.
struct DereverbStereoResult {
  Audio left;
  Audio right;
  /// One report rather than one per channel: the linked mask and the WPE
  /// predictor set are each a single object applied to both channels, so a
  /// per-channel pair would be two copies of one measurement.
  DereverbReport report;
};

/// @brief Two-channel @ref dereverb_classical_linked.
DereverbStereoResult dereverb_classical_stereo(const Audio& left, const Audio& right,
                                               const DereverbClassicalConfig& config = {});

/// @brief Sets the two config fields a room measurement determines, leaving the
///        rest of @p config as the caller has them.
/// @details What the room decides is WHERE the tail is: @p rt60_mid_sec is how
///   long it lasts and @p volume_m3 fixes where it starts, through Polack's
///   mixing time sqrt(V) in ms -- past that the response is the diffuse tail
///   the subtraction targets rather than separable early reflections. How MUCH
///   to remove (attenuation, threshold, over_subtraction, spectral_floor) is
///   taste rather than measurement, so this does not touch it.
///
///   A non-finite or non-positive argument leaves its own field alone, so a
///   partial estimate still configures the half it measured. Takes scalars
///   rather than a RoomEstimate deliberately: this file includes nothing from
///   the acoustic layer, which the mastering library links only when the
///   acoustic simulation is built, and the caller already holds both numbers.
void apply_room_measurement(DereverbClassicalConfig& config, float rt60_mid_sec,
                            float volume_m3) noexcept;

}  // namespace sonare::mastering::repair
