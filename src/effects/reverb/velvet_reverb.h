#pragma once

/// @file velvet_reverb.h
/// @brief Velvet-noise sparse-FIR reverb (Valimaki & Prawda 2021).

#include <cstdint>
#include <vector>

#include "effects/common/dc_blocker.h"
#include "rt/processor_base.h"

namespace sonare::effects::reverb {

struct VelvetReverbConfig {
  /// @brief Scales reverb_time_s: effective T60 = reverb_time_s * (0.5 + decay).
  float decay = 0.45f;
  float dry_wet = 0.3f;
  float reverb_time_s = 1.5f;  ///< Base T60 in seconds.
  float density_hz = 2000.0f;  ///< Velvet pulse density, clamped to [1000, 3000].
  bool enable_shelf = true;    ///< Post one-pole high-shelf HF damping at 6 kHz.
};

class VelvetReverb : public rt::ProcessorBase {
 public:
  explicit VelvetReverb(VelvetReverbConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  // Shape parameters (index order matches sibling reverbs):
  //   0 = decay
  //   1 = reverb_time_s
  //   2 = dry_wet (clamped to [0, 1] in process())
  //   3 = density_hz
  // Note: decay, reverb_time_s and density_hz are NOT lock-free / RT-safe;
  // changing any of them rebuilds the velvet-noise tap tables and ring buffers
  // (offline reconfiguration). dry_wet is RT-safe and only read in process().
  bool set_parameter(unsigned int param_id, float value) override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;

 private:
  struct Tap {
    int offset = 0;
    float gain = 0.0f;
  };

  /// @brief Ring buffer with single-write / multi-tap read.
  struct Ring {
    std::vector<float> buf;
    int size = 1;
    int index = 0;

    void prepare(int length);
    void reset();
    void write(float in);             ///< Writes then advances the head.
    float read_at(int offset) const;  ///< Reads offset samples behind the last write.
  };

  void build_table(std::vector<Tap>& taps, std::uint32_t seed_offset, int grid_ls, int n_seg,
                   int num_pulses, float decay_rate, double sr) const;

  VelvetReverbConfig config_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  bool prepared_ = false;
  Ring ring_l_;
  Ring ring_r_;
  std::vector<Tap> taps_l_;
  std::vector<Tap> taps_r_;

  // Post high-shelf damping state. The one-pole state tracks the low band;
  // process() recombines it with an attenuated high band.
  float shelf_b0_ = 0.0f;
  float shelf_pole_ = 0.0f;
  float shelf_state_l_ = 0.0f;
  float shelf_state_r_ = 0.0f;

  effects::common::DcBlocker dc_blocker_;
};

}  // namespace sonare::effects::reverb
