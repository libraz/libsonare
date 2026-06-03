#pragma once

/// @file chain.h
/// @brief High-level mastering chain composition (multi-module ordered processing).

#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "mastering/api/named_processor.h"
#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/deesser.h"
#include "mastering/dynamics/transient_shaper.h"
#include "mastering/maximizer/true_peak_limiter.h"
#include "mastering/multiband/multiband_compressor.h"
#include "mastering/repair/declick.h"
#include "mastering/repair/declip.h"
#include "mastering/repair/decrackle.h"
#include "mastering/repair/dehum.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/dereverb_classical.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/tape.h"
#include "mastering/spectral/air_band.h"
#include "mastering/stereo/imager.h"
#include "mastering/stereo/mono_maker.h"

namespace sonare::mastering::api {

// ---------------------------------------------------------------------------
// Per-module sub-configurations
// Each carries an `enabled` flag; when false the module is skipped.
// ---------------------------------------------------------------------------

struct DeclickStage {
  bool enabled = false;
  mastering::repair::DeclickConfig config{};
};

struct DeclipStage {
  bool enabled = false;
  mastering::repair::DeclipConfig config{};
};

struct DecrackleStage {
  bool enabled = false;
  mastering::repair::DecrackleConfig config{};
};

struct DehumStage {
  bool enabled = false;
  mastering::repair::DehumConfig config{};
};

struct DereverbStage {
  bool enabled = false;
  mastering::repair::DereverbClassicalConfig config{};
};

struct DenoiseStage {
  bool enabled = false;
  mastering::repair::DenoiseClassicalConfig config{};
};

struct RepairChainConfig {
  DeclickStage declick{};
  DeclipStage declip{};
  DecrackleStage decrackle{};
  DehumStage dehum{};
  DereverbStage dereverb{};
  DenoiseStage denoise{};
};

struct TiltStage {
  bool enabled = false;
  float tilt_db = 0.0f;
  float pivot_hz = 1000.0f;
};

struct EqChainConfig {
  TiltStage tilt{};
};

struct CompressorStage {
  bool enabled = false;
  mastering::dynamics::CompressorConfig config{};
};

struct DeEsserStage {
  bool enabled = false;
  mastering::dynamics::DeEsserConfig config{};
};

struct TransientShaperStage {
  bool enabled = false;
  mastering::dynamics::TransientShaperConfig config{};
};

struct MultibandCompStage {
  bool enabled = false;
  mastering::multiband::MultibandCompressorConfig config{};
};

struct DynamicsChainConfig {
  DeEsserStage deesser{};
  TransientShaperStage transient_shaper{};
  CompressorStage compressor{};
  MultibandCompStage multiband_comp{};
};

struct TapeStage {
  bool enabled = false;
  mastering::saturation::TapeConfig config{};
};

struct ExciterStage {
  bool enabled = false;
  mastering::saturation::ExciterConfig config{};
};

struct SaturationChainConfig {
  TapeStage tape{};
  ExciterStage exciter{};
};

struct AirBandStage {
  bool enabled = false;
  mastering::spectral::AirBandConfig config{};
};

struct SpectralChainConfig {
  AirBandStage air_band{};
};

struct ImagerStage {
  bool enabled = false;
  mastering::stereo::ImagerConfig config{};
};

struct MonoMakerStage {
  bool enabled = false;
  mastering::stereo::MonoMakerConfig config{};
};

struct StereoChainConfig {
  ImagerStage imager{};
  MonoMakerStage mono_maker{};
};

struct TruePeakLimiterStage {
  bool enabled = false;
  mastering::maximizer::TruePeakLimiterConfig config{};
};

struct MaximizerChainConfig {
  TruePeakLimiterStage true_peak_limiter{};
};

struct LoudnessStage {
  bool enabled = false;
  float target_lufs = -14.0f;
  float ceiling_db = -1.0f;
  int true_peak_oversample = 4;
  float release_ms = 50.0f;
  bool apply_gain_at_input_rate = false;
};

/// @brief Full chain configuration.
/// Modules execute in the fixed order: repair → eq → dynamics → saturation →
/// spectral → stereo (stereo path only) → maximizer → loudness.
struct MasteringChainConfig {
  RepairChainConfig repair{};
  EqChainConfig eq{};
  DynamicsChainConfig dynamics{};
  SaturationChainConfig saturation{};
  SpectralChainConfig spectral{};
  StereoChainConfig stereo{};
  MaximizerChainConfig maximizer{};
  LoudnessStage loudness{};
};

// ---------------------------------------------------------------------------
// Chain results
// ---------------------------------------------------------------------------
// MonoChainResult / StereoChainResult inherit both the common audio-output
// fields (`samples` / `left+right`, `sample_rate`, LUFS, applied gain,
// latency) via @ref MonoAudioResult / @ref StereoAudioResult and the
// chain-specific measurement fields (`output_true_peak_dbtp`, `output_lra`,
// `stages`, `stage_gain_reductions`) via @ref ChainMetrics, so callers can
// access them flat on the result without going through a nested struct.
//
// @ref StageGainReduction and @ref ChainMetrics are declared in
// @ref result_types.h.

struct MonoChainResult : public MonoAudioResult, public ChainMetrics {};

struct StereoChainResult : public StereoAudioResult, public ChainMetrics {};

// ---------------------------------------------------------------------------
// MasteringChain
// One-shot composition of a mastering chain. Construct once per configuration
// and call process_mono / process_stereo per audio buffer.
// ---------------------------------------------------------------------------

class MasteringChain {
 public:
  /// @brief Progress callback. `progress` is 0.0..1.0 across the whole chain;
  /// `stage` is the stage identifier just completed (e.g. "dynamics.compressor").
  using ProgressCallback = std::function<void(float progress, const char* stage)>;

  explicit MasteringChain(MasteringChainConfig config);

  /// @brief Set callback invoked after each enabled stage completes.
  void set_progress_callback(ProgressCallback callback);

  /// @brief Process mono audio through the configured chain.
  MonoChainResult process_mono(const float* samples, std::size_t length, int sample_rate);

  /// @brief Process stereo audio through the configured chain.
  StereoChainResult process_stereo(const float* left, const float* right, std::size_t length,
                                   int sample_rate);

  /// @brief Returns the active configuration.
  const MasteringChainConfig& config() const noexcept { return config_; }

 private:
  MasteringChainConfig config_;
  ProgressCallback progress_callback_;
};

// ---------------------------------------------------------------------------
// StreamingMasteringChain
// Block-by-block streaming variant of MasteringChain. Maintains processor
// state across process_block() calls. Supports only ProcessorBase-based
// stages (eq.tilt, dynamics.compressor, saturation.tape, saturation.exciter,
// spectral.airBand, stereo.imager, stereo.monoMaker, maximizer.truePeakLimiter).
// Throws std::invalid_argument from constructor if config enables
// repair.denoise or loudness (those require whole-signal buffering).
// ---------------------------------------------------------------------------

/// @brief Optional construction parameters for StreamingMasteringChain.
///
/// The streaming chain cannot measure whole-signal integrated LUFS, so the
/// loudness stage (which every built-in preset enables) normally cannot run in
/// a realtime preview. To let a preset's streaming preview match its offline
/// render, the caller may precompute the loudness normalization gain offline
/// (e.g. `target_lufs - measured_integrated_lufs`) and supply it here. When
/// provided and `config.loudness.enabled` is set, the chain applies that fixed
/// gain per block before the loudness stage's true-peak limiter instead of
/// throwing. The true-peak ceiling is still enforced live by the
/// `maximizer.truePeakLimiter` stage that the loudness config enables.
struct StreamingMasteringChainOptions {
  /// Precomputed static loudness gain in dB. NaN (the default) means "not
  /// provided"; in that case an enabled loudness stage still throws.
  float loudness_static_gain_db = std::numeric_limits<float>::quiet_NaN();
};

class StreamingMasteringChain {
 public:
  /// @brief Construct with a configuration. Throws SonareException if the
  /// configuration enables non-streaming stages (repair.*, loudness).
  explicit StreamingMasteringChain(MasteringChainConfig config);

  /// @brief Construct with a configuration and streaming options. When the
  /// configuration enables loudness, @p options.loudness_static_gain_db must be
  /// finite; the chain then applies that fixed gain per block (see
  /// @ref StreamingMasteringChainOptions). Repair stages are still rejected.
  StreamingMasteringChain(MasteringChainConfig config, StreamingMasteringChainOptions options);
  ~StreamingMasteringChain();

  StreamingMasteringChain(const StreamingMasteringChain&) = delete;
  StreamingMasteringChain& operator=(const StreamingMasteringChain&) = delete;
  StreamingMasteringChain(StreamingMasteringChain&&) noexcept;
  StreamingMasteringChain& operator=(StreamingMasteringChain&&) noexcept;

  /// @brief Initialize processors for the given sample rate and max block size.
  /// Must be called before process_block(). @p num_channels must be 1 or 2 and
  /// determines whether stereo-only stages (stereo.imager, stereo.monoMaker)
  /// participate (they are skipped for mono).
  void prepare(double sample_rate, int max_block_size, int num_channels);

  /// @brief Process one block in place. @p num_channels must match the value
  /// passed to prepare(). @p num_samples must be <= max_block_size from prepare().
  void process_block(float* const* channels, int num_channels, int num_samples);

  /// @brief Reset all processor state without rebuilding.
  void reset();

  /// @brief Total reported latency in samples across all active processors.
  int latency_samples() const noexcept;

  /// @brief Returns the active configuration.
  const MasteringChainConfig& config() const noexcept { return config_; }

  /// @brief Returns the ordered stage names that will run (e.g. "eq.tilt").
  /// Populated after prepare().
  const std::vector<std::string>& stage_names() const noexcept { return stage_names_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  MasteringChainConfig config_;
  std::vector<std::string> stage_names_;
  int prepared_channels_ = 0;
  int max_block_size_ = 0;
  // Precomputed loudness normalization gain (linear). 1.0 when the loudness
  // stage is disabled or no static gain was supplied. Applied per block before
  // the true-peak limiter so a preset's streaming preview can match its
  // offline-rendered loudness without measuring whole-signal LUFS live.
  float loudness_static_gain_linear_ = 1.0f;
};

// ---------------------------------------------------------------------------
// Flat-params config bridge (used by C / Python / Node bindings).
// Param keys use dot notation matching the JS object schema:
//   "repair.declick.enabled"       (0 = off, non-zero = on)
//   "repair.declick.threshold"
//   "repair.dereverb.enabled"
//   "repair.dereverb.threshold"
//   "repair.denoise.enabled"
//   "repair.denoise.nFft"          (int)
//   "eq.tilt.tiltDb"               (float)
//   "dynamics.deesser.enabled"
//   "dynamics.deesser.frequencyHz"
//   "dynamics.transientShaper.enabled"
//   "dynamics.transientShaper.attackGainDb"
//   "dynamics.compressor.thresholdDb"
//   "dynamics.multibandComp.enabled"
//   "dynamics.multibandComp.lowCutoffHz"
//   "saturation.tape.driveDb"
//   "saturation.exciter.amount"
//   "spectral.airBand.amount"
//   "stereo.imager.width"          (stereo path only)
//   "stereo.monoMaker.amount"      (stereo path only)
//   "maximizer.truePeakLimiter.ceilingDb"
//   "loudness.enabled"             (0 = off; setting any loudness.* field also enables it)
//   "loudness.targetLufs"
//   "loudness.ceilingDb"
//   "loudness.truePeakOversample"
// Setting any field under a module also implicitly enables that module unless
// "<module>.enabled" is explicitly set to 0. Unknown keys throw
// std::invalid_argument.
// ---------------------------------------------------------------------------

MasteringChainConfig parse_chain_config_params(const Param* params, std::size_t count);

/// @brief Apply flat-params on top of an existing config (in-place).
/// Same key schema as parse_chain_config_params. Setting any field under a
/// module also implicitly enables that module unless "<module>.enabled" is
/// also set to 0. Unknown keys throw SonareException(InvalidParameter).
void apply_chain_config_overrides(MasteringChainConfig& config, const Param* params,
                                  std::size_t count);

/// @brief Serialize a chain configuration as canonical JSON.
/// Schema: {"version":1,"params":{"dot.notation.key":number_or_bool,...}}.
std::string chain_config_to_json(const MasteringChainConfig& config);

/// @brief Parse a chain configuration serialized by chain_config_to_json.
/// Throws std::invalid_argument for malformed JSON, unsupported versions, or
/// unknown parameter keys.
MasteringChainConfig chain_config_from_json(const std::string& json);

/// @brief Convenience: parse params, build chain, run mono once.
MonoChainResult run_chain_mono_params(const Param* params, std::size_t param_count,
                                      const float* samples, std::size_t length, int sample_rate);

/// @brief Convenience: parse params, build chain, run stereo once.
StereoChainResult run_chain_stereo_params(const Param* params, std::size_t param_count,
                                          const float* left, const float* right, std::size_t length,
                                          int sample_rate);

}  // namespace sonare::mastering::api
