#pragma once

#include <cstddef>
#include <vector>

#include "core/audio.h"
#include "util/parameter_checks.h"

namespace sonare::mastering::repair {

/// @brief Longest clipped run reconstructed with the LPC (Janssen) solver.
/// @details The solver's normal matrix is (gap x gap), so an uncapped run makes both peak
/// memory and compute unbounded in the input length. Runs longer than this are left clipped
/// and counted in @c DeclipReport::unreconstructed_runs. 512 samples is ~10.7 ms at 48 kHz,
/// which covers hard clipping down to the deepest bass fundamentals.
inline constexpr size_t kDeclipMaxLpcGapSamples = 512;

/// @brief Longest one-sided context window handed to the LPC solver.
/// @details Bounds the context even when @c DeclipConfig::lpc_order is large;
/// without it the order-derived term could pull the whole input into the solver.
/// Never binds for a gap-derived context, which is already at most
/// 8 * @ref kDeclipMaxLpcGapSamples.
inline constexpr size_t kDeclipMaxLpcContextRadius = 8 * kDeclipMaxLpcGapSamples;

/// @brief Upper bound on the LPC solver's dense working set, in bytes.
/// @details Derived from the two caps above: the double (gap x gap) normal matrix, its
/// free-column copy and factorization, plus four context-length vectors (the context, the
/// residual and the Burg recursion's two). The bound is a constant, independent of the
/// input length and of the clip pattern.
inline constexpr size_t kDeclipMaxLpcWorkingSetBytes =
    sizeof(double) * (3 * kDeclipMaxLpcGapSamples * kDeclipMaxLpcGapSamples +
                      4 * (2 * kDeclipMaxLpcContextRadius + kDeclipMaxLpcGapSamples));

/// @brief Highest peak a reconstructed run may reach above the clip threshold, in dB.
/// @details Past it the run is left clipped and counted rather than written. Noise driven up
/// to 40x reconstructs at most +18.5 dB; a 200 Hz sine driven 10x lets the solve diverge to
/// +52.5 dB, the ill-conditioned case this ceiling exists for.
inline constexpr float kDeclipMaxReconstructionGainDb = 20.0f;

/// @brief Shortest run of bit-identical samples counted as a flat top.
/// @details Two is reachable by rounding alone. The shallowest clipped run in the
/// evaluation corpus is four samples, so three separates with margin either way.
inline constexpr size_t kDeclipMinFlatRunSamples = 3;

/// @brief How far from the flat level a flat run may sit and still count, in dB.
/// @details A clipper pins samples at one ceiling, so the runs that matter sit at
/// the flat level and this window only admits the rounding spread around it.
inline constexpr float kDeclipFlatRunLevelWindowDb = 1.0f;

/// @brief Quietest magnitude at which a run of identical samples is a flat-top candidate.
/// @details -40 dBFS. A design value, not calibrated: flat runs quieter than this are
/// not treated as repairable clipping.
inline constexpr float kDeclipFlatRunAbsoluteFloor = 0.01f;

/// @brief Largest accepted @c DeclipConfig::lpc_order.
/// @details The solver lowers the order to a quarter of its context, which spans at most two
/// capped radii plus the gap, so this is the largest order it runs as requested at every gap.
inline constexpr int kDeclipMaxLpcOrder = static_cast<int>(2 * kDeclipMaxLpcContextRadius / 4);

/// @brief Most outer reconstruction rounds a declip pass accepts.
/// @details Each round re-estimates the AR model over the whole input; the estimate has
/// converged by the second, and the detection sweeps run up to 7.
inline constexpr int kDeclipMaxIterations = 8;

/// @brief Domains of the integer @c DeclipConfig members, stated once for the core check.
inline constexpr util::check::NumericDomain kDeclipLpcOrderDomain =
    util::check::integer_domain(0, kDeclipMaxLpcOrder);
inline constexpr util::check::NumericDomain kDeclipIterationsDomain =
    util::check::integer_domain(1, kDeclipMaxIterations);

struct DeclipConfig {
  float clip_threshold = 0.98f;
  int lpc_order = 36;
  int iterations = 2;
  /// Multichannel entry points only: reconstruct every channel over the union of all channels'
  /// clipped runs, so the channels' repair errors agree, at the cost of re-estimating unclipped
  /// samples in the quieter channel. Use on near-mono material; off, each channel repairs its own.
  bool link_channels = false;
};

/// Validates every public DeclipConfig field. The mono, stereo and detection
/// entrypoints share this oracle so range handling cannot drift between them.
void validate_config(const DeclipConfig& config);

/// @brief The clip threshold given in dBFS as the linear DeclipConfig::clip_threshold.
/// @details Refused outside [kMinPositiveGainDb, 0], the dB range whose linear value lies in
///          (0, 1]; stated in dB so the published bound does not depend on the platform's pow.
float clip_threshold_from_db(float threshold_db, const char* subject);

/// @brief What a declip analysis found.
struct ClipDetection {
  size_t sample_count = 0;       ///< Samples at or past clip_threshold.
  float sample_fraction = 0.0f;  ///< sample_count / size.
  size_t run_count = 0;
  size_t longest_run_samples = 0;  ///< Compare against kDeclipMaxLpcGapSamples:
                                   ///  a longer run is left clipped.

  /// @name Flat-top analysis
  /// Runs of bit-identical samples sitting at the channel's flat level. The fields above
  /// count what is at or past clip_threshold now, so they see only clipping that
  /// still reaches that ceiling; a flat top survives a later gain change and so
  /// reports material clipped before it was attenuated. Neither drives the
  /// repair, which reconstructs what crosses clip_threshold. A waveform that is
  /// genuinely flat on top -- a square or pulse train, a fully limited master --
  /// counts here and cannot be told from clipping in the time domain. The reverse
  /// error is the one to state: anything that moves samples independently erases
  /// a real flat top, so zero is not proof the material was never clipped.
  /// Resampling and lossy coding do it, and so does a stereo downmix -- measured,
  /// a shallow clipped tone reports 440 runs per channel and none at all after
  /// the two are averaged.
  /// @{
  size_t flat_run_count = 0;
  size_t longest_flat_run_samples = 0;
  size_t flat_sample_count = 0;
  /// Largest run magnitude after the top runs are set aside; 0 when none.
  float flat_level = 0.0f;
  /// @}
};

ClipDetection detect_clipping(const float* samples, size_t size, int sample_rate,
                              const DeclipConfig& config = {});

/// @brief What a declip pass found in one channel and what it did to it.
struct DeclipReport {
  ClipDetection detected;             ///< This channel's own analysis of the input.
  size_t lpc_reconstructed_runs = 0;  ///< Runs the Janssen solver filled.
  size_t unreconstructed_runs = 0;    ///< Runs left as they were: past
                                      ///  kDeclipMaxLpcGapSamples, without a usable
                                      ///  solve, or reconstructed past
                                      ///  kDeclipMaxReconstructionGainDb.
  size_t repaired_samples = 0;        ///< Samples the reconstructed runs overwrote.
  size_t linked_runs = 0;             ///< Of the repaired runs, those reaching
                                      ///  past this channel's own clipped
                                      ///  samples because the other channel's
                                      ///  run was wider. Always 0 unless
                                      ///  DeclipConfig::link_channels is set
                                      ///  on a multichannel entrypoint.
};

Audio declip(const Audio& audio, const DeclipConfig& config = {});

/// @brief Declips @p audio and reports what the pass found and did.
Audio declip(const Audio& audio, const DeclipConfig& config, DeclipReport* report);

/// @brief A declipped stereo pair and what each channel's pass did.
struct DeclipStereoResult {
  Audio left;
  Audio right;
  DeclipReport left_report;
  DeclipReport right_report;
};

/// @brief Declips a stereo pair.
/// @details By default each channel reconstructs its own clipped runs and leaves every
///   sample it did not clip bit-identical. With DeclipConfig::link_channels the union of
///   the two channels' runs is the region set, and each channel reconstructs the whole of
///   every union run it has at least one clipped sample in; a channel with none is left
///   untouched there.
DeclipStereoResult declip_stereo(const Audio& left, const Audio& right,
                                 const DeclipConfig& config = {});

/// @brief Declips any number of channels.
/// @details The N-channel form of @ref declip_stereo with the same rule over the whole set:
///   each channel its own runs, or with DeclipConfig::link_channels every union run it has at
///   least one clipped sample in. One channel reproduces @ref declip and two reproduce
///   @ref declip_stereo bit for bit.
/// @param out Receives one declipped channel per input channel, in input order.
/// @return One report per channel, in input order, each with its own channel's detection.
std::vector<DeclipReport> declip_linked(const Audio* const* channels, size_t channel_count,
                                        std::vector<Audio>* out, const DeclipConfig& config);

}  // namespace sonare::mastering::repair
