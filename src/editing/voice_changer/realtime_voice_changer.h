#pragma once

/// @file realtime_voice_changer.h
/// @brief Integrated realtime DSP chain for character voice changing.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "editing/voice_changer/isp_limiter.h"
#include "editing/voice_changer/streaming_formant.h"
#include "editing/voice_changer/streaming_retune.h"
#include "editing/voice_changer/streaming_reverb.h"
#include "rt/biquad_design.h"
#include "rt/rt_publisher.h"

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
inline constexpr std::uint32_t kVoiceChangerAbiVersion = 1u;

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

struct GateConfig {
  float threshold_db = -55.0f;
  float attack_ms = 2.0f;
  float release_ms = 100.0f;
  float range_db = 18.0f;
};

struct CompressorConfig {
  float threshold_db = -22.0f;
  float ratio = 2.5f;
  float attack_ms = 6.0f;
  float release_ms = 90.0f;
  float makeup_gain_db = 1.0f;
};

struct DeesserConfig {
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
  ///          @ref ceiling_db. Adds @c IspLimiter::latency_samples (6 samples
  ///          at any sample rate; see RealtimeVoiceChanger::latency_samples()
  ///          comment for the breakdown) to the chain latency.
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
  GateConfig gate;
  CompressorConfig compressor;
  DeesserConfig deesser;
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
  void reset();
  /// @brief Publishes a new configuration to the realtime processing chain.
  /// @details Safe to call concurrently with @ref process_block on the same
  ///          instance: the configuration is normalized and stored in a
  ///          lock-free snapshot (see @c rt::RtPublisher), and the audio
  ///          thread atomically adopts it at the start of the next block via
  ///          @ref process_block. Derived coefficients and per-channel DSP
  ///          state are re-applied on the audio thread when the snapshot is
  ///          adopted, so no @c BiquadState / sub-stage member is ever written
  ///          concurrently with sample processing. May allocate (the snapshot
  ///          @c shared_ptr) and is therefore NOT realtime-safe itself; call
  ///          from the configuration thread only. Two threads MUST NOT call
  ///          @ref set_config concurrently with each other (single-producer
  ///          hand-off).
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
  /// @brief Reports the dominant processing latency in samples.
  /// @details Equal to the retune grain size, which dominates the chain
  ///          (typically 256-1024 samples). Other stages (formant LP, EQ
  ///          biquads, reverb predelay) add <= 8 samples combined, and the
  ///          optional ISP limiter adds another 6 samples (the BS.1770-style
  ///          4x upsampler's group delay) when enabled — both are well under
  ///          the retune grain and DAW hosts typically tolerate the
  ///          difference without explicit compensation. Returns 0 before
  ///          prepare() has been called.
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
    StreamingReverb reverb;
    /// Optional final-stage inter-sample-peak limiter; engaged when
    /// LimiterConfig::enable_isp_limiter is true. Always prepared so toggling
    /// the flag at runtime never re-allocates from the audio thread.
    IspLimiter isp_limiter;
  };

  /// @brief Recomputes scalar derived state (gains, alphas) from @p config.
  /// @details Writes to non-snapshot member fields (@c input_gain_,
  ///          @c gate_attack_, ...). Called from prepare() and — via
  ///          @ref maybe_adopt_snapshot — from the audio thread when a new
  ///          configuration snapshot is adopted between blocks. Never called
  ///          from @ref set_config directly.
  void update_derived(const RealtimeVoiceChangerConfig& config);
  /// @brief Allocates per-channel buffers. MUST only be called from prepare().
  void allocate_channel(ChannelState& state);
  /// @brief Applies @p config to per-channel DSP coefficients / sub-stage
  ///        configs. Realtime-safe: never resizes any buffers, so it is safe
  ///        to call from the audio thread between blocks.
  /// @param channel_index Index of this channel; used to derive per-channel
  ///        seeds (notably for the reverb) so stereo channels are decorrelated.
  void apply_channel_config(ChannelState& state, int channel_index,
                            const RealtimeVoiceChangerConfig& config);
  void reset_channel(ChannelState& state);
  float process_input_stage(ChannelState& state, const RealtimeVoiceChangerConfig& config,
                            float input) noexcept;
  float process_output_stage(ChannelState& state, const RealtimeVoiceChangerConfig& config,
                             float input) noexcept;
  void ensure_scratch(int num_samples) noexcept;
  /// @brief Audio-thread hand-off: adopts any pending snapshot and, if a new
  ///        one was adopted, re-runs derived-state and per-channel-coefficient
  ///        updates. Returns the snapshot the block should use; falls back to
  ///        the control-thread @c config_ mirror only when no snapshot has been
  ///        published yet (which is unreachable in normal operation because
  ///        the constructor always publishes an initial snapshot).
  const RealtimeVoiceChangerConfig& adopt_snapshot_for_block() noexcept;

  RealtimeVoiceChangerConfig config_{};
  /// @brief Lock-free single-producer (config thread) / single-consumer (audio
  ///        thread) snapshot publisher. The audio thread reads
  ///        @c config_publisher_.current() through a stable pointer that only
  ///        changes inside @c acquire(), so the per-sample loop sees a
  ///        consistent config for the whole block.
  rt::RtPublisher<RealtimeVoiceChangerConfig> config_publisher_;
  /// @brief Tracks which snapshot pointer the audio thread last applied to
  ///        per-channel DSP coefficients. When @c config_publisher_.current()
  ///        differs from this, the audio thread re-runs
  ///        @ref update_derived / @ref apply_channel_config before processing.
  const RealtimeVoiceChangerConfig* applied_snapshot_ = nullptr;
  double sample_rate_ = 0.0;
  int max_block_size_ = 0;
  int num_channels_ = 1;
  std::vector<ChannelState> channels_;
  std::vector<float> scratch_;

  float input_gain_ = 1.0f;
  float output_gain_ = 1.0f;
  float wet_mix_ = 1.0f;
  /// Fast detector coefficient used by gate/comp to follow |x| with ~1 ms tau;
  /// user-controlled attack/release apply to the resulting *gain* transition,
  /// not the detector. This avoids the double-LP smearing that obscured
  /// user-configured A/R in earlier versions.
  float fast_det_alpha_ = 1.0f;
  float gate_attack_ = 1.0f;
  float gate_release_ = 1.0f;
  float comp_attack_ = 1.0f;
  float comp_release_ = 1.0f;
  /// Sub-millisecond limiter attack so transient bursts taper across a few
  /// samples instead of a single-sample step (which audibly clicks).
  float limiter_attack_ = 1.0f;
  float limiter_release_ = 1.0f;
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
std::string realtime_voice_changer_config_to_json(const RealtimeVoiceChangerConfig& config);
std::string realtime_voice_changer_preset_json(VoiceCharacterPreset preset);
bool validate_realtime_voice_changer_preset_json(std::string_view json,
                                                 std::string* normalized_json, std::string* error);

}  // namespace sonare::editing::voice_changer
