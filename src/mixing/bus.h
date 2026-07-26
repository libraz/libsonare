#pragma once

/// @file bus.h
/// @brief Summing bus primitive for subgroup, aux and master buses.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mixing/insert_chain.h"
#include "mixing/meter.h"
#include "rt/processor_base.h"

namespace sonare::mixing {

enum class BusRole {
  Subgroup,
  Aux,
  Master,
};

class BusProcessor : public rt::ProcessorBase {
 public:
  explicit BusProcessor(BusRole role = BusRole::Subgroup, int max_inputs = 0);

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  int latency_samples() const noexcept override;
  int latency_samples_q8() const noexcept override;
  int tail_samples() const noexcept override;

  /// Appends an insert to the chain. When @p stereo_pair_only is true the insert
  /// is a StereoPairOnly processor (catalog channelPolicy): on a surround bus
  /// (num_channels > 2) it is handed only the front L/R pair so the surround
  /// planes pass through dry and width-sensitive inserts (e.g. eq.midSide, which
  /// aborts on a non-stereo width) get their required 2-plane view. On a
  /// stereo/mono bus the flag is inert and the call is the legacy full-buffer
  /// path. Mirrors ChannelStrip::add_pre/post_insert.
  void add_insert(std::unique_ptr<rt::ProcessorBase> processor, bool stereo_pair_only = false);
  size_t num_inserts() const noexcept { return inserts_.size(); }
  // Applies an insert parameter immediately (no automation lane, no allocation).
  // AUDIO-THREAD ONLY: mutates processor coefficients that process() reads, so it
  // must run from the audio callback, never concurrently with process(). Returns
  // false for an out-of-range insert or a param the processor reports non-RT-safe.
  // Mirrors ChannelStrip::apply_insert_parameter.
  bool apply_insert_parameter(unsigned int insert_index, unsigned int param_id,
                              float value) noexcept;
  // Toggles bypass for the insert at @p insert_index. When @p reset_on_bypass is
  // true the processor is reset as it is bypassed. Returns false for an
  // out-of-range insert. Mirrors ChannelStrip::set_insert_bypassed.
  bool set_insert_bypassed(unsigned int insert_index, bool bypassed,
                           bool reset_on_bypass = false) noexcept;
  // Resolves a processor JSON-key parameter name to its integer param_id for the
  // insert at @p insert_index, or -1 if unknown. Control-thread API: reads the
  // processor's static descriptor table, touching no mutable audio state. Mirrors
  // ChannelStrip::insert_parameter_id_for_key.
  int insert_parameter_id_for_key(unsigned int insert_index, const std::string& key) const noexcept;
  void set_insert_sidechain(unsigned int insert_index, const float* const* channels,
                            int num_channels, int num_samples);
  void clear_insert_sidechains() noexcept;
  MeterSnapshot meter_snapshot() const noexcept { return meter_.snapshot(); }
  size_t insert_sidechain_slot_count() const noexcept { return insert_sidechains_.size(); }
  size_t insert_sidechains_capacity() const noexcept { return insert_sidechains_.capacity(); }

  BusRole role() const noexcept { return role_; }
  int max_inputs() const noexcept { return max_inputs_; }

  // Upper bound on inserts per bus. Reserved at construction so add_insert
  // never reallocates inserts_ / insert_sidechains_ while the audio thread
  // iterates them in process(). Exceeding the cap throws SonareException
  // (InvalidState), mirroring ChannelStrip::add_pre/post_insert.
  static constexpr size_t kMaxInserts = 64;

 private:
  BusRole role_ = BusRole::Subgroup;
  int max_inputs_ = 0;
  std::vector<std::unique_ptr<rt::ProcessorBase>> inserts_;
  // Parallel to inserts_: 1 marks a StereoPairOnly insert (front-pair-only on a
  // surround bus). Reserved at construction alongside inserts_ so add_insert
  // never reallocates it while process() iterates.
  std::vector<uint8_t> insert_spo_;
  std::vector<InsertSidechain> insert_sidechains_;
  MeterProcessor meter_{};
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
};

}  // namespace sonare::mixing
