#pragma once

/// @file bus.h
/// @brief Summing bus primitive for subgroup, aux and master buses.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

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

  void sum_inputs(const std::vector<float* const*>& inputs, float* const* output, int num_channels,
                  int num_samples) const;
  /// Appends an insert to the chain. When @p stereo_pair_only is true the insert
  /// is a StereoPairOnly processor (catalog channelPolicy): on a surround bus
  /// (num_channels > 2) it is handed only the front L/R pair so the surround
  /// planes pass through dry and width-sensitive inserts (e.g. eq.midSide, which
  /// aborts on a non-stereo width) get their required 2-plane view. On a
  /// stereo/mono bus the flag is inert and the call is the legacy full-buffer
  /// path. Mirrors ChannelStrip::add_pre/post_insert.
  void add_insert(std::unique_ptr<rt::ProcessorBase> processor, bool stereo_pair_only = false);
  size_t num_inserts() const noexcept { return inserts_.size(); }
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
  static constexpr int kMaxSidechainChannels = 8;

  struct InsertSidechain {
    std::array<const float*, kMaxSidechainChannels> channels{};
    int num_channels = 0;
    int num_samples = 0;
    bool managed = false;
  };

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
