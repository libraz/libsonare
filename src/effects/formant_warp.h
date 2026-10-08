#pragma once

/// @file formant_warp.h
/// @brief Lightweight formant-colour warp using LPC analysis context.
///
/// Core-side rather than voice-changer-side: note editing warps formants too,
/// and its build option is the one the voice changer already requires.

#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/audio.h"

namespace sonare {

class FFT;

/// Factor range the warp is defined over. It clamps its own factor to these, so
/// they belong to the warp rather than to any one caller's parameter policy.
inline constexpr float kFormantFactorMin = 0.55f;
inline constexpr float kFormantFactorMax = 1.65f;

/// Analysis frame of the warp, in samples at 48 kHz (about 21.3 ms). The hop is a
/// quarter of the frame and the FFT the next power of two of at least twice the frame.
inline constexpr int kFormantWarpFrameAt48k = 1024;

struct FormantWarpConfig {
  float factor = 1.0f;
  int lpc_order = 12;
  float amount = 1.0f;
  /// False keeps the frame at kFormantWarpFrameAt48k samples at every rate. True
  /// defines it in time instead: that many samples at 48 kHz, rescaled to the
  /// input rate and rounded to a multiple of four.
  bool frame_in_time = false;
};

/// @brief Factor the warp actually applies, after the range clamp and the
///        dry/wet fold.
/// @details Published so a caller that echoes the factor it was given reports the
///          one that was used. A NaN comes back a NaN, because std::clamp does not
///          launder one; the warp refuses it rather than resolving it.
float effective_formant_factor(float factor, float amount) noexcept;

/// @brief Analysis frame, in samples, at @p sample_rate: kFormantWarpFrameAt48k rescaled to the
///        rate and rounded to a multiple of four.
int formant_warp_frame_size(int sample_rate) noexcept;

/// @brief LPC order that resolves formants at @p sample_rate: one per kHz plus two.
/// @details A fixed order of 12 only resolves them near 22 kHz.
int formant_warp_lpc_order(int sample_rate) noexcept;

/// @brief Push-streaming formant warp: LPC residual re-coloured with the frequency-warped
///        all-pole envelope, windowed overlap-add at a quarter-frame hop.
/// @details The one frame routine behind both @ref FormantWarp and the realtime voice changer.
///          Output lags input by exactly @ref latency_samples. At a factor of 1 the output is
///          the input through the same delay, bit for bit (a short crossfade joins the two
///          states, so automation crossing unity does not click). All allocation happens in
///          @ref prepare; @ref process never allocates or throws.
class FormantWarpStream {
 public:
  FormantWarpStream();
  ~FormantWarpStream();
  FormantWarpStream(FormantWarpStream&&) noexcept;
  FormantWarpStream& operator=(FormantWarpStream&&) noexcept;

  /// @param frame_size Analysis frame in samples; a multiple of four, at least 16.
  /// @param lpc_order Requested order; limited to [2, frame_size - 1].
  /// @throws SonareException InvalidParameter for a frame that is not a multiple of four.
  void prepare(int frame_size, int lpc_order);
  /// @brief Clears the delay and overlap state; the factor is kept.
  void reset() noexcept;
  /// @brief Sets the warp applied from the next analysis frame on, clamped to the factor range.
  void set_factor(float factor) noexcept;
  float factor() const noexcept { return factor_; }
  /// @brief Samples the output lags the input by; 0 before prepare().
  int latency_samples() const noexcept { return frame_size_; }
  /// @brief Processes @p num_samples samples; @p input and @p output may be the same buffer.
  void process(const float* input, float* output, int num_samples) noexcept;

 private:
  void process_frame() noexcept;
  void accumulate_unwarped() noexcept;
  std::size_t frame_start_slot() const noexcept;

  int frame_size_ = 0;
  int hop_size_ = 0;
  int order_ = 0;
  float factor_ = 1.0f;
  float bypass_ = 0.0f;         ///< Crossfade position: 0 = overlap-add output, 1 = delayed input.
  float bypass_target_ = 0.0f;  ///< 1 while the factor is unity.
  float bypass_step_ = 0.0f;
  int hop_count_ = 0;
  std::int64_t pushed_ = 0;
  std::size_t ring_pos_ = 0;
  std::size_t emit_slot_ = 0;
  bool first_frame_ = true;

  std::unique_ptr<FFT> fft_;
  std::vector<float> hann_;
  std::vector<float> ring_;  ///< Last frame_size input samples, which is also the dry delay.
  std::vector<float> acc_;   ///< Overlap-add signal accumulator, 2 * frame_size slots.
  std::vector<float> norm_;  ///< Overlap-add window-power accumulator.
  std::vector<float> windowed_;
  std::vector<float> ar_;
  std::vector<double> lags_;
  std::vector<double> levinson_a_;
  std::vector<double> levinson_next_;
  std::vector<float> padded_;
  std::vector<float> time_frame_;
  std::vector<float> envelope_;
  std::vector<float> warped_;
  std::vector<std::complex<float>> spectrum_;
  std::vector<std::complex<float>> predictor_spectrum_;
};

class FormantWarp {
 public:
  explicit FormantWarp(FormantWarpConfig config = {});

  Audio process(const Audio& audio) const;
  const FormantWarpConfig& config() const noexcept { return config_; }

 private:
  FormantWarpConfig config_{};
};

}  // namespace sonare
