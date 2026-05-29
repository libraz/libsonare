#pragma once

/// @file streaming_reverb.h
/// @brief Per-channel Schroeder reverb (2 combs + 1 series allpass) used by
///        the realtime voice changer chain. Exposed as a standalone module so
///        other DSP code can reuse the same speech-friendly tail shape.

#include <array>
#include <cstddef>
#include <vector>

namespace sonare::editing::voice_changer {

/// @brief Configuration for @ref StreamingReverb.
/// @details Field-compatible with the @c ReverbConfig type embedded in
///          @ref RealtimeVoiceChangerConfig (`using ReverbConfig =
///          StreamingReverbConfig`), so existing JSON / POD APIs stay
///          unchanged after this struct was extracted out of the voice
///          changer header.
struct StreamingReverbConfig {
  /// Dry/wet ratio in `[0, 1]`. Voice changer further caps this to 0.45 to
  /// keep speech intelligible.
  float mix = 0.04f;

  /// Approximate RT60-style decay in milliseconds. Drives the comb delay-line
  /// length and feedback gain (matched so each comb decays by ~60 dB in
  /// roughly @c time_ms).
  float time_ms = 320.0f;

  /// Damping factor in `[0, 1]`. `0` = bright reverb (~12 kHz low-pass in
  /// feedback); `1` = dark reverb (~1 kHz low-pass).
  float damping = 0.55f;

  /// Deterministic phase/jitter seed. Same `time_ms + damping + seed` always
  /// produces the same impulse response.
  int seed = 1;
};

/// @brief Per-channel Schroeder reverb.
/// @details Allocates its delay-line buffers in @ref prepare based on the
///          maximum supported decay time (@ref kMaxTimeMs); subsequent
///          @ref set_config calls only update the read distance / feedback /
///          damping coefficients, so they remain realtime-safe (no
///          reallocation on the audio thread).
class StreamingReverb {
 public:
  /// Upper bound on @ref StreamingReverbConfig::time_ms used to size the
  /// internal delay buffers at @ref prepare time. Must agree with the schema
  /// validator's upper bound on the realtime voice changer's `time_ms` field.
  static constexpr float kMaxTimeMs = 1800.0f;

  StreamingReverb() = default;
  explicit StreamingReverb(StreamingReverbConfig config) : config_(config) {}

  /// @brief Allocates per-channel delay-line buffers sized for @ref kMaxTimeMs.
  /// @details NOT realtime-safe. Call once during initialization with the
  ///          maximum block size and channel count you intend to use.
  void prepare(double sample_rate, int max_block_size);

  /// @brief Zeros all delay-line state but keeps the buffer footprint.
  void reset() noexcept;

  /// @brief Applies @p config. Realtime-safe — never resizes buffers.
  /// @param channel_index Per-channel seed salt; stereo channels must use
  ///        different indices so their reverb tails decorrelate (a shared
  ///        seed produces a fully mono L=R tail from a single mono source).
  void set_config(const StreamingReverbConfig& config, int channel_index = 0);

  const StreamingReverbConfig& config() const noexcept { return config_; }

  /// @brief Processes one sample and returns the wet/dry-mixed output.
  /// @details If `config.mix <= 0` or @ref prepare has not run yet, returns
  ///          @p input unchanged.
  float process_sample(float input) noexcept;

  /// @brief Processes a contiguous block sample-by-sample.
  void process_block(const float* input, float* output, int num_samples) noexcept;

 private:
  static constexpr int kNumCombs = 2;

  StreamingReverbConfig config_{};
  double sample_rate_ = 0.0;

  std::array<std::vector<float>, kNumCombs> comb_buf_{};
  std::array<std::size_t, kNumCombs> comb_pos_{0u, 0u};
  std::array<std::size_t, kNumCombs> comb_delay_{1u, 1u};
  std::array<float, kNumCombs> comb_fb_{0.5f, 0.5f};
  std::array<float, kNumCombs> comb_lp_{0.0f, 0.0f};
  float damping_alpha_ = 1.0f;  // 1 = no damping (bright), 0 = full LP (dark).

  std::vector<float> allpass_buf_;
  std::size_t allpass_pos_ = 0;
  std::size_t allpass_delay_ = 1;  // ~5 ms fixed diffusion.
  float allpass_g_ = 0.7f;         // Diffusion feedback, sign flipped by seed.
};

}  // namespace sonare::editing::voice_changer
