#pragma once

#include <vector>

#include "rt/oversampler.h"
#include "rt/processor_base.h"

namespace sonare::mastering::saturation {

struct TubeConfig {
  float drive_db = 6.0f;
  float bias = 0.15f;
  float mix = 1.0f;
  int oversample_factor = 4;
  float bias_v = -1.6f;
  float harmonic_drive = 1.0f;
};

class Tube : public rt::ProcessorBase {
 public:
  explicit Tube(TubeConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  void set_config(const TubeConfig& config);
  const TubeConfig& tube_config() const { return tube_config_; }

  // Automatable parameters (RT-safe, no allocation, no state reset). All are
  // read per sample in process_model()/apply_miller_filter() with no
  // precomputed coefficients (the Miller-filter cutoff is derived from drive_db
  // each sample):
  //   0 = drive_db
  //   1 = bias
  //   2 = mix (clamped to [0, 1])
  //   3 = bias_v (must stay finite)
  //   4 = harmonic_drive (clamped to [0, 1])
  // oversample_factor is a discrete mode and is not exposed.
  bool set_parameter(unsigned int param_id, float value) override;

 private:
  static void validate_config(const TubeConfig& config);
  static float process_model(float sample, const TubeConfig& config);
  void allocate_scratch();
  void ensure_state(int num_channels);
  float apply_miller_filter(int channel, float sample);

  TubeConfig tube_config_{};
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  sonare::rt::Oversampler oversampler_{4};
  // Preallocated oversampling scratch (sized max_block_size_*oversample_factor
  // in prepare()) so the audio-thread process() path never allocates.
  std::vector<float> up_scratch_;
  std::vector<float> down_scratch_;
  std::vector<float> miller_state_;
};

}  // namespace sonare::mastering::saturation
