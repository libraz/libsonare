#pragma once

/// @file processor_base.h
/// @brief Base interface for stateful mastering processors.

namespace sonare::rt {

class ProcessorBase {
 public:
  virtual ~ProcessorBase() = default;

  virtual void prepare(double sample_rate, int max_block_size) = 0;
  virtual void process(float* const* channels, int num_channels, int num_samples) = 0;
  virtual void reset() = 0;
  virtual int latency_samples() const noexcept { return 0; }
  // Q8 fixed-point latency, in samples. The integer API remains the legacy
  // floor value; graph/mixing PDC uses Q8 so fractional processor latency is
  // not rounded away.
  virtual int latency_samples_q8() const noexcept { return latency_samples() << 8; }
  virtual int output_latency_samples_q8(int output_port) const noexcept {
    (void)output_port;
    return latency_samples_q8();
  }
  virtual float last_gain_reduction_db() const { return 0.0f; }

  // Optional external detector/key input for processors that support
  // sidechain operation. Buffers are borrowed until the processor consumes or
  // clears them; default implementation is a no-op for processors without a
  // sidechain detector.
  virtual void set_sidechain(const float* const* channels, int num_channels, int num_samples) {
    (void)channels;
    (void)num_channels;
    (void)num_samples;
  }
  virtual void clear_sidechain() {}

  // Set a processor-specific scalar parameter by id. Returns false if the id is
  // not recognized. Default: no automatable parameters. Implementations must be
  // RT-safe (no allocation, no blocking).
  virtual bool set_parameter(unsigned int param_id, float value) {
    (void)param_id;
    (void)value;
    return false;
  }

  // Returns whether set_parameter(param_id, ...) is safe to call from an audio
  // processing callback. Most processors either implement set_parameter as an
  // in-place scalar/coefficient update or do not implement it at all; processors
  // that rebuild kernels, resize buffers, or reset audio state must override.
  virtual bool parameter_is_realtime_safe(unsigned int param_id) const noexcept {
    (void)param_id;
    return true;
  }
};

}  // namespace sonare::rt
