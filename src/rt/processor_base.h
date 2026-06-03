#pragma once

/// @file processor_base.h
/// @brief Base interface for stateful mastering processors.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "util/exception.h"

namespace sonare::rt {

class ProcessorBase {
 public:
  ProcessorBase() = default;
  ProcessorBase(const ProcessorBase& other)
      : bypassed_(other.bypassed_.load(std::memory_order_acquire)) {}
  ProcessorBase& operator=(const ProcessorBase& other) {
    if (this != &other) {
      bypassed_.store(other.bypassed_.load(std::memory_order_acquire), std::memory_order_release);
    }
    return *this;
  }
  ProcessorBase(ProcessorBase&& other) noexcept
      : bypassed_(other.bypassed_.load(std::memory_order_acquire)) {}
  ProcessorBase& operator=(ProcessorBase&& other) noexcept {
    if (this != &other) {
      bypassed_.store(other.bypassed_.load(std::memory_order_acquire), std::memory_order_release);
    }
    return *this;
  }
  virtual ~ProcessorBase() = default;

  /// @brief Throws @c SonareException (InvalidState) if @p prepared is false.
  ///        Centralises the "X must be prepared before processing" check that
  ///        every derived processor performs at the top of @c process().
  /// @param prepared       Value of the derived processor's @c prepared_ flag.
  /// @param processor_name Class name used in the exception message
  ///                       (e.g. "Compressor"). Must be a string literal or
  ///                       otherwise outlive the throw.
  static void ensure_prepared(bool prepared, const char* processor_name) {
    if (!prepared) {
      throw SonareException(ErrorCode::InvalidState,
                            std::string(processor_name) + " must be prepared before processing");
    }
  }

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
  // Optional decay length after input becomes silent, in samples. Hosts use
  // this to keep offline bounces from truncating reverb/delay/plugin tails.
  virtual int tail_samples() const noexcept { return 0; }
  virtual float last_gain_reduction_db() const { return 0.0f; }

  // Generic host bypass state. Containers such as graph nodes and insert
  // chains consume this flag by leaving the dry buffer untouched and skipping
  // process(); processors may still override for plugin-specific bypass.
  virtual bool set_bypassed(bool bypassed, bool reset_on_bypass = false) {
    const bool was = bypassed_.exchange(bypassed, std::memory_order_acq_rel);
    if (bypassed && reset_on_bypass && !was) {
      reset();
    }
    return true;
  }
  virtual bool bypassed() const noexcept { return bypassed_.load(std::memory_order_acquire); }

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
  //
  // WARNING: the default returns true and assumes set_parameter performs only
  // in-place scalar updates. Any processor whose set_parameter allocates, locks,
  // or rebuilds state (kernels, buffers, FFT plans, etc.) MUST override this to
  // return false for the affected param_ids — otherwise an allocating parameter
  // is silently reported RT-safe and may be applied from the audio callback.
  virtual bool parameter_is_realtime_safe(unsigned int param_id) const noexcept {
    (void)param_id;
    return true;
  }

  // Opaque instance-state persistence for host session save/restore. save_state
  // appends the processor's full restorable state (preset + automatable
  // parameter values, voice config, etc.) to @p out as an OPAQUE byte span and
  // returns true; load_state rehydrates from a span previously produced by the
  // same processor build and returns true on success. Invariant 6: the blob is
  // an opaque byte sequence — NO plugin-SDK type (VST3 / AU / CLAP) crosses this
  // seam; the out-of-tree bridge owns the encoding. Default: stateless processor
  // (save_state returns false / appends nothing, load_state ignores the span).
  //
  // Threading: both run on the CONTROL thread (save before a process block,
  // load before the instance is wired into the audio graph). They MAY allocate.
  virtual bool save_state(std::vector<uint8_t>& out) const {
    (void)out;
    return false;
  }
  virtual bool load_state(const uint8_t* data, size_t len) {
    (void)data;
    (void)len;
    return false;
  }

 private:
  std::atomic<bool> bypassed_{false};
};

}  // namespace sonare::rt
