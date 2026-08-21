#pragma once

/// @file realtime_voice_changer.h
/// @brief Integrated realtime DSP chain for character voice changing.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "editing/voice_changer/control_cadence.h"
#include "editing/voice_changer/isp_limiter.h"
#include "editing/voice_changer/streaming_formant.h"
#include "editing/voice_changer/streaming_retune.h"
#include "editing/voice_changer/streaming_reverb.h"
#include "rt/biquad_design.h"
#include "rt/param_smoother.h"
#include "rt/seqlock_cell.h"

namespace sonare::editing::voice_changer {

/// @brief Version of the realtime voice changer preset JSON schema.
/// @details Bumped whenever the JSON shape changes incompatibly (renamed/removed
///          fields, range tightening that rejects previously-valid documents).
///          Mirrored by the JSON Schema files under @c schemas/ and by the
///          @c "schemaVersion" literal emitted by @ref
///          realtime_voice_changer_config_to_json.
inline constexpr int kVoiceChangerPresetSchemaVersion = 1;

/// @brief ABI version of the realtime voice changer POD config struct.
/// @details Bumped whenever @ref SonareRealtimeVoiceChangerConfig
///          (declared in @c sonare_c.h) changes layout incompatibly — field
///          additions/removals/reorders that would corrupt POD memcpy across
///          binding boundaries. Bindings call @c sonare_voice_changer_abi_version()
///          at load time and refuse to attach if the runtime value disagrees
///          with the compile-time expectation. Separate from the JSON schema
///          version because JSON bindings (Python/Node) tolerate layout drift
///          while POD bindings (Rust FFI, raw C ABI consumers) do not.
// v2: added limiter_enable_isp_limiter (int) and limiter_isp_ceiling_dbtp
//     (float) to SonareRealtimeVoiceChangerConfig.
inline constexpr std::uint32_t kVoiceChangerAbiVersion = 2u;

enum class VoiceCharacterPreset {
  NeutralMonitor,
  BrightIdol,
  SoftWhisper,
  DeepNarrator,
  RobotMascot,
  DarkVillain,
};

struct CharacterEqConfig {
  float highpass_hz = 80.0f;
  float body_db = 0.0f;
  float presence_db = 1.0f;
  float air_db = 0.0f;
};

struct VoiceGateConfig {
  float threshold_db = -55.0f;
  float attack_ms = 2.0f;
  float release_ms = 100.0f;
  float range_db = 18.0f;
};

struct VoiceCompressorConfig {
  float threshold_db = -22.0f;
  float ratio = 2.5f;
  float attack_ms = 6.0f;
  float release_ms = 90.0f;
  float makeup_gain_db = 1.0f;
};

struct VoiceDeesserConfig {
  float frequency_hz = 7200.0f;
  float threshold_db = -28.0f;
  float ratio = 4.0f;     ///< Slope above threshold (1 = no reduction, 20 = brick).
  float range_db = 8.0f;  ///< Maximum sibilance gain reduction in dB.
};

/// @brief Configuration for the voice changer's per-channel reverb stage.
/// @details Type alias for @ref StreamingReverbConfig (extracted out of this
///          header into @c streaming_reverb.h for reuse). Keeping the old
///          name preserves the existing public API and JSON / POD schema.
using ReverbConfig = StreamingReverbConfig;

struct LimiterConfig {
  float ceiling_db = -1.0f;
  float release_ms = 50.0f;
  /// @brief Enables the optional 4x-oversampled inter-sample peak (true-peak)
  ///        limiter as the final output stage.
  /// @details When @c true (the default), an additional ISP limiter sits after
  ///          the existing sample-domain limiter and the dry/wet mix to keep
  ///          inter-sample peaks at or below @ref isp_ceiling_dbtp dBTP. This
  ///          prevents downstream DAC oversampling from clipping even when the
  ///          sample-domain limiter has kept every audible sample under
  ///          @ref ceiling_db. Adds the sample-rate-dependent latency reported
  ///          by @c IspLimiter::latency_samples (the 6-sample FIR group delay
  ///          plus @c ceil(5 * 0.1 ms * sample_rate) attack-settle samples;
  ///          31 samples at 48 kHz)
  ///          to the chain latency.
  bool enable_isp_limiter = true;
  /// @brief True-peak ceiling in dBTP. Defaults to -1.0 dBTP per the EBU R128
  ///        / AES streaming recommendation. Ignored when
  ///        @ref enable_isp_limiter is @c false.
  float isp_ceiling_dbtp = -1.0f;
};

struct RealtimeVoiceChangerConfig {
  float input_gain_db = 0.0f;
  float output_gain_db = 0.0f;
  /// @brief Dry/wet ratio in [0,1]. 1.0 = full processed signal. Capped to 0.45
  ///        inside the reverb stage to keep speech intelligibility; this knob
  ///        controls the overall dry/wet of the whole processing chain.
  float wet_mix = 1.0f;
  StreamingRetuneConfig retune;
  StreamingFormantConfig formant;
  CharacterEqConfig eq;
  VoiceGateConfig gate;
  VoiceCompressorConfig compressor;
  VoiceDeesserConfig deesser;
  ReverbConfig reverb;
  LimiterConfig limiter;
};

class RealtimeVoiceChanger {
 public:
  /// @brief Maximum reverb decay time (ms) used to size per-channel comb buffers.
  ///        Must agree with the schema/validator upper bound on
  ///        @ref ReverbConfig::time_ms.
  static constexpr float kMaxReverbTimeMs = StreamingReverb::kMaxTimeMs;

  explicit RealtimeVoiceChanger(RealtimeVoiceChangerConfig config = {});

  void prepare(double sample_rate, int max_block_size, int num_channels = 1);
  /// @brief Clears streaming DSP state.
  /// @details Control-thread operation. It must not run concurrently with
  ///          @ref process_block on the same instance: reset writes filter,
  ///          envelope, reverb, and ISP-limiter state directly. To update a
  ///          live processor safely, use @ref set_config instead.
  void reset();
  /// @brief Publishes a new configuration to the realtime processing chain.
  /// @details Safe to call concurrently with @ref process_block on the same
  ///          instance: the configuration is normalized and stored into a
  ///          lock-free single-writer/single-reader cell (see @c
  ///          rt::SeqlockCell), and the audio thread atomically adopts it at
  ///          the start of the next block via @ref process_block. Derived
  ///          coefficients and per-channel DSP state are re-applied on the
  ///          audio thread when the new value is adopted, so no @c
  ///          BiquadState / sub-stage member is ever written concurrently
  ///          with sample processing. Realtime-safe itself: unlike @c
  ///          rt::RtPublisher's shared_ptr snapshots, storing into a @c
  ///          rt::SeqlockCell never allocates, locks, or throws on the writer
  ///          side either, so this may be called from the audio thread itself
  ///          — required on WASM, where an AudioWorkletProcessor's port
  ///          message handler runs on the same single rendering thread as
  ///          @ref process_block, collapsing the "configuration thread" and
  ///          the audio thread into one. Still single-producer: two threads
  ///          MUST NOT call @ref set_config concurrently with each other.
  void set_config(const RealtimeVoiceChangerConfig& config);
  /// @brief Returns the most recently published configuration as observed by
  ///        the configuration thread.
  /// @details NOT realtime-safe and NOT safe to call concurrently with
  ///          @ref set_config (the returned reference may be invalidated by a
  ///          subsequent publish). Intended for UI sync / round-trip tests on
  ///          the configuration thread.
  const RealtimeVoiceChangerConfig& config() const noexcept { return config_; }

  /// @brief Process a mono block. RT-safe and @c noexcept.
  /// @details Pre-condition violations (no @ref prepare, @p num_samples > @c
  ///          max_block_size_, null buffers) cause a silent no-op rather than
  ///          throwing. When @p sample_rate_ is zero the @p output buffer is
  ///          zero-filled for @p num_samples samples so callers always observe
  ///          a defined buffer state.
  void process_block(const float* input, float* output, int num_samples) noexcept;
  /// @brief Process a planar multi-channel block. RT-safe and @c noexcept.
  /// @details Pre-condition violations (no @ref prepare, @p num_samples > @c
  ///          max_block_size_, bad channel count, null pointers) cause a
  ///          silent no-op rather than throwing. Caller-owned buffers are left
  ///          untouched in that case.
  void process_block(float* const* channels, int num_channels, int num_samples) noexcept;
  /// @brief Reports the prepared chain's processing latency in samples.
  /// @details Dry and wet paths are both aligned to the retune OLA's fixed
  ///          one-grain delay, so this value never changes when @c wet_mix or
  ///          @c retune.mix changes. When @ref LimiterConfig::enable_isp_limiter
  ///          is @c true, the final ISP limiter runs after the aligned mix and
  ///          adds @c IspLimiter::latency_samples: a 6-sample FIR group delay
  ///          plus @c ceil(5 * 0.1 ms * sample_rate) attack-settle samples
  ///          (31 samples at 48 kHz).
  ///          Other stages add <= 8 samples combined and are intentionally
  ///          omitted. Returns 0 before prepare() has been called.
  int latency_samples() const noexcept;

 private:
  struct ChannelState {
    StreamingRetune retune;
    StreamingFormant formant;
    rt::BiquadState hpf;
    rt::BiquadState body;
    rt::BiquadState presence;
    rt::BiquadState air;
    rt::BiquadState deess_band;
    float gate_env = 0.0f;
    float gate_gain = 1.0f;
    float comp_env = 0.0f;
    float comp_gain = 1.0f;
    float deess_env = 0.0f;
    float deess_gain = 1.0f;  // Smoothed deesser reduction gain.
    float limiter_gain = 1.0f;
    // Derived controls are refreshed at absolute sample positions 0, 32, ...
    // while every parameter smoother continues to advance for every sample.
    ControlCadence control_cadence;
    float gate_threshold_linear = 0.0f;
    float gate_range_linear = 1.0f;
    float gate_attack_alpha = 1.0f;
    float gate_release_alpha = 1.0f;
    float comp_attack_alpha = 1.0f;
    float comp_release_alpha = 1.0f;
    float limiter_ceiling_linear = 1.0f;
    float limiter_release_alpha = 1.0f;
    // Snapshot updates arrive between blocks, but level changes must remain
    // sample-continuous. Keep one equal smoother per channel for stereo.
    rt::ParamSmoother input_gain{1.0f, 10.0f, 48000.0};
    rt::ParamSmoother output_gain{1.0f, 10.0f, 48000.0};
    rt::ParamSmoother wet_mix{1.0f, 10.0f, 48000.0};
    // Every live control update is a target change. Filter coefficients are
    // rebuilt from these smooth values at a short fixed cadence below.
    rt::ParamSmoother eq_highpass_hz{80.0f, 12.0f, 48000.0};
    rt::ParamSmoother eq_body_db{0.0f, 12.0f, 48000.0};
    rt::ParamSmoother eq_presence_db{1.0f, 12.0f, 48000.0};
    rt::ParamSmoother eq_air_db{0.0f, 12.0f, 48000.0};
    rt::ParamSmoother gate_threshold_db{-55.0f, 12.0f, 48000.0};
    rt::ParamSmoother gate_attack_ms{2.0f, 12.0f, 48000.0};
    rt::ParamSmoother gate_release_ms{100.0f, 12.0f, 48000.0};
    rt::ParamSmoother gate_range_db{18.0f, 12.0f, 48000.0};
    rt::ParamSmoother comp_threshold_db{-22.0f, 12.0f, 48000.0};
    rt::ParamSmoother comp_ratio{2.5f, 12.0f, 48000.0};
    rt::ParamSmoother comp_attack_ms{6.0f, 12.0f, 48000.0};
    rt::ParamSmoother comp_release_ms{90.0f, 12.0f, 48000.0};
    rt::ParamSmoother comp_makeup_db{1.0f, 12.0f, 48000.0};
    rt::ParamSmoother deess_frequency_hz{7200.0f, 12.0f, 48000.0};
    rt::ParamSmoother deess_threshold_db{-28.0f, 12.0f, 48000.0};
    rt::ParamSmoother deess_ratio{4.0f, 12.0f, 48000.0};
    rt::ParamSmoother deess_range_db{8.0f, 12.0f, 48000.0};
    rt::ParamSmoother limiter_ceiling_db{-1.0f, 12.0f, 48000.0};
    rt::ParamSmoother limiter_release_ms{50.0f, 12.0f, 48000.0};
    float filter_highpass_hz = 80.0f;
    float filter_body_db = 0.0f;
    float filter_presence_db = 1.0f;
    float filter_air_db = 0.0f;
    float filter_deess_frequency_hz = 7200.0f;
    StreamingReverb reverb;
    /// Outer dry path aligned with the retune stage before the whole-chain
    /// wet/dry blend. Allocated only by prepare().
    std::vector<float> dry_delay;
    std::size_t dry_delay_pos = 0;
    /// Optional final-stage inter-sample-peak limiter; engaged when
    /// LimiterConfig::enable_isp_limiter is true. Always prepared so toggling
    /// the flag at runtime never re-allocates from the audio thread.
    IspLimiter isp_limiter;
  };

  /// @brief Recomputes sample-rate-derived detector state.
  /// @details Called from prepare() and — via
  ///          @ref maybe_adopt_snapshot — from the audio thread when a new
  ///          configuration snapshot is adopted between blocks. Never called
  ///          from @ref set_config directly.
  void update_derived();
  /// @brief Allocates per-channel buffers. MUST only be called from prepare().
  void allocate_channel(ChannelState& state);
  /// @brief Applies @p config to per-channel DSP coefficients / sub-stage
  ///        configs. Realtime-safe: never resizes any buffers, so it is safe
  ///        to call from the audio thread between blocks.
  /// @param channel_index Index of this channel; used to derive per-channel
  ///        seeds (notably for the reverb) so stereo channels are decorrelated.
  void apply_channel_config(ChannelState& state, int channel_index,
                            const RealtimeVoiceChangerConfig& config);
  /// @brief Rebuilds the input high-pass coefficient from its smoothed value.
  ///        Called at a bounded cadence from the audio thread.
  void update_input_filter(ChannelState& state) noexcept;
  /// @brief Rebuilds output EQ and de-esser coefficients from their smoothed
  ///        values. Called at a bounded cadence from the audio thread.
  void update_output_filters(ChannelState& state) noexcept;
  void reset_channel(ChannelState& state);
  /// @brief Mirrors the resolved retune grain size into @c config_ so config()
  ///        reports the effective (prepared) grain rather than the requested
  ///        one. No-op before prepare(). Control-thread only.
  void sync_effective_grain_size() noexcept;
  /// @brief Publishes the latency-report atomic mirrors from @c config_.
  ///        Control-thread only (constructor / prepare / set_config).
  void update_latency_mirrors() noexcept;
  float process_input_stage(ChannelState& state, float input, bool control_update) noexcept;
  float process_output_stage(ChannelState& state, float input, bool control_update) noexcept;
  void ensure_scratch(int num_samples) noexcept;
  /// @brief Audio-thread hand-off: adopts the latest published configuration
  ///        if it changed since the last block, and if so re-runs
  ///        derived-state and per-channel-coefficient updates. Returns @c
  ///        active_config_, valid for the whole block (the constructor always
  ///        stores an initial value before the instance is usable, so this is
  ///        never observed uninitialised).
  const RealtimeVoiceChangerConfig& adopt_snapshot_for_block() noexcept;

  RealtimeVoiceChangerConfig config_{};
  /// @brief Realtime-safe single-writer/single-reader hand-off cell (see @c
  ///        rt::SeqlockCell). Unlike @c rt::RtPublisher's shared_ptr
  ///        snapshots, storing into this cell never allocates on the writer
  ///        side either — see @ref set_config for why that matters on WASM.
  rt::SeqlockCell<RealtimeVoiceChangerConfig> config_cell_;
  /// @brief Monotonic version bumped (release ordering) every time @c
  ///        config_cell_ is stored into. The audio thread compares this
  ///        against @c applied_config_version_ to detect a pending
  ///        configuration change without re-deriving coefficients when
  ///        nothing changed. Coalescing is acceptable here: only the latest
  ///        published value must eventually be observed, matching @c
  ///        rt::RtPublisher's own "latest wins" semantics under a burst.
  std::atomic<std::uint32_t> config_version_{0};
  /// @brief Version the audio thread last applied to per-channel DSP
  ///        coefficients. Audio-thread only; compared against @c
  ///        config_version_ inside @ref adopt_snapshot_for_block.
  std::uint32_t applied_config_version_ = 0;
  /// @brief Audio thread's adopted working configuration, returned by @ref
  ///        adopt_snapshot_for_block. Written only on the audio thread, inside
  ///        @ref adopt_snapshot_for_block (and once, equivalently, by
  ///        prepare() before any audio thread runs against this instance).
  RealtimeVoiceChangerConfig active_config_{};
  // Whether the ISP limiter was active for the last adopted snapshot. Its
  // lookahead history must not survive an inactive interval.
  bool applied_isp_limiter_active_ = false;
  double sample_rate_ = 0.0;
  int max_block_size_ = 0;
  int num_channels_ = 1;
  std::vector<ChannelState> channels_;
  std::vector<float> scratch_;

  /// Latency-report mirror. latency_samples() may be polled by a host from a
  /// thread other than the one calling set_config(), so the ISP enable flag is
  /// mirrored atomically instead of reading the mutable config_ directly.
  std::atomic<bool> latency_isp_enabled_{false};

  /// Fast detector coefficient used by gate/comp to follow |x| with ~1 ms tau;
  /// user-controlled attack/release apply to the resulting *gain* transition,
  /// not the detector. This avoids the double-LP smearing that obscured
  /// user-configured A/R in earlier versions.
  float fast_det_alpha_ = 1.0f;
  /// Sub-millisecond limiter attack so transient bursts taper across a few
  /// samples instead of a single-sample step (which audibly clicks).
  float limiter_attack_ = 1.0f;
  float deess_alpha_ = 1.0f;
  float deess_gain_alpha_ = 1.0f;  // Smoothing of the deesser reduction gain.
};

RealtimeVoiceChangerConfig realtime_voice_changer_preset(VoiceCharacterPreset preset);
VoiceCharacterPreset realtime_voice_changer_preset_from_id(std::string_view id);
const char* realtime_voice_changer_preset_id(VoiceCharacterPreset preset) noexcept;
std::vector<std::string> realtime_voice_changer_preset_names();

RealtimeVoiceChangerConfig normalize_realtime_voice_changer_config(
    const RealtimeVoiceChangerConfig& config);
/// @brief Validates a configuration for finite values and structural ranges.
/// @details Returns false (with @p error populated) if any field is NaN, ±Inf,
///          or grossly out of range. Out-of-range but finite values are still
///          accepted and clamped via @ref normalize_realtime_voice_changer_config
///          into @p normalized so existing tolerant entry points keep working.
bool validate_realtime_voice_changer_config(const RealtimeVoiceChangerConfig& config,
                                            RealtimeVoiceChangerConfig* normalized,
                                            std::string* error);

RealtimeVoiceChangerConfig realtime_voice_changer_config_from_json(std::string_view json);
/// Parses a configuration accepted at public realtime construction/update
/// boundaries. Preset ids and complete flat PODs are supported for binding
/// compatibility; every other JSON object must satisfy the strict preset
/// schema, so partial nested configs cannot silently inherit defaults.
bool realtime_voice_changer_config_from_input(std::string_view input,
                                              RealtimeVoiceChangerConfig* config,
                                              std::string* error);
std::string realtime_voice_changer_config_to_json(const RealtimeVoiceChangerConfig& config);
std::string realtime_voice_changer_preset_json(VoiceCharacterPreset preset);
bool validate_realtime_voice_changer_preset_json(std::string_view json,
                                                 std::string* normalized_json, std::string* error);

}  // namespace sonare::editing::voice_changer
