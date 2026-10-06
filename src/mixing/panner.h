#pragma once

/// @file panner.h
/// @brief Stereo panner with selectable pan laws.

#include <algorithm>
#include <atomic>

#include "mixing/pan_law.h"
#include "rt/param_smoother.h"
#include "rt/processor_base.h"

namespace sonare::mixing {

/// @brief The pan position the panner will actually store for @p pan.
/// @details Every writer that also caches the value it passed - the C ABI
///          setters and the scene walker, which both echo their cache back to
///          the caller - resolves it through here, so a read-back can never
///          report a position the processor is not using.
inline float clamp_pan(float pan) noexcept { return std::clamp(pan, -1.0f, 1.0f); }

enum class PanMode {
  Balance,
  StereoPan,
  DualPan,
};

struct PannerConfig {
  float pan = 0.0f;
  PanLaw pan_law = PanLaw::Const3dB;
  float smoothing_ms = 5.0f;
  PanMode mode = PanMode::Balance;
};

class PannerProcessor : public rt::ProcessorBase {
 public:
  explicit PannerProcessor(PannerConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void set_pan(float pan) noexcept;
  float pan() const noexcept { return pan_.load(std::memory_order_relaxed); }

  void set_pan_law(PanLaw law) noexcept { pan_law_.store(law, std::memory_order_relaxed); }
  PanLaw pan_law() const noexcept { return pan_law_.load(std::memory_order_relaxed); }

  void set_pan_mode(PanMode mode) noexcept { pan_mode_.store(mode, std::memory_order_relaxed); }
  PanMode pan_mode() const noexcept { return pan_mode_.load(std::memory_order_relaxed); }

  void set_dual_pan(float left_pan, float right_pan) noexcept;
  float dual_pan_left() const noexcept { return dual_pan_left_.load(std::memory_order_relaxed); }
  float dual_pan_right() const noexcept { return dual_pan_right_.load(std::memory_order_relaxed); }

  /// @brief Copies the in-flight DSP state from another panner.
  /// @details The source and destination must be control-quiescent: neither may
  ///          be processed or mutated concurrently. This is an allocation-free
  ///          state handoff for a retained bus slot and deliberately excludes
  ///          generic @c ProcessorBase metadata such as bypass and detector state.
  void copy_state_from(const PannerProcessor& other) noexcept;

  /// @brief True when Balance mode, pan 0, and the position smoother already sits
  ///        at centre -- a no-op the caller can skip.
  /// @details Skipping here is exact where running the panner is not: Const3dB's
  ///          NearUnity gain at dead centre is 0.99999994f, not 1.0f (see
  ///          pan_law.h), so this checks configuration and settle state rather
  ///          than the computed gain.
  bool at_rest_identity() const noexcept;

 private:
  /// Last gain pair evaluated for one position, keyed by everything it depends on, so a
  /// settled position costs a compare per sample instead of a law evaluation.
  struct GainCache {
    float position = 0.0f;
    PanLaw law = PanLaw::Const3dB;
    PanNormalization normalization = PanNormalization::Raw;
    PanGains gains{};
    bool valid = false;

    PanGains get(float pos, PanLaw pan_law, PanNormalization norm) noexcept {
      if (!valid || pos != position || pan_law != law || norm != normalization) {
        gains = compute_pan_gains(pos, pan_law, norm);
        position = pos;
        law = pan_law;
        normalization = norm;
        valid = true;
      }
      return gains;
    }
  };

  double sample_rate_ = 48000.0;
  float smoothing_ms_ = 5.0f;
  // Glides act on the pan positions and the law is evaluated per sample from the
  // smoothed position, so every instant of a glide is a valid static placement.
  rt::ParamSmoother pan_smoother_{0.0f, 5.0f, 48000.0};
  rt::ParamSmoother dual_left_smoother_{-1.0f, 5.0f, 48000.0};
  rt::ParamSmoother dual_right_smoother_{1.0f, 5.0f, 48000.0};
  GainCache pan_cache_;
  GainCache dual_left_cache_;
  GainCache dual_right_cache_;
  std::atomic<float> pan_{0.0f};
  std::atomic<float> dual_pan_left_{-1.0f};
  std::atomic<float> dual_pan_right_{1.0f};
  std::atomic<PanLaw> pan_law_{PanLaw::Const3dB};
  std::atomic<PanMode> pan_mode_{PanMode::Balance};
};

}  // namespace sonare::mixing
