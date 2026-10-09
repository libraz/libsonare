#pragma once

/// @file bus.h
/// @brief Summing bus primitive for subgroup, aux and master buses.

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/channel_layout.h"
#include "mixing/alignment_delay.h"
#include "mixing/insert_chain.h"
#include "mixing/meter.h"
#include "rt/processor_base.h"
#include "util/constants.h"

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
  /// Resets the inserts and alignment delays, keeping the meter; reset() is
  /// this plus the meter.
  void reset_processing() noexcept;
  int latency_samples() const noexcept override;
  int latency_samples_q8() const noexcept override;
  int tail_samples() const noexcept override;
  /// Latency from the bus input to the detector tap of an insert. Inserts are
  /// ordered exactly as the runtime chain; an out-of-range index has no value.
  std::optional<int> insert_input_latency_samples_q8(unsigned int insert_index) const noexcept;

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
  bool constructed_insert_parameter_value(unsigned int insert_index, unsigned int param_id,
                                          float* out) const noexcept {
    return insert_index < inserts_.size() && inserts_[insert_index] != nullptr &&
           inserts_[insert_index]->constructed_parameter_value(param_id, out);
  }
  bool last_applied_insert_parameter_value(unsigned int insert_index, unsigned int param_id,
                                           float* out) const noexcept {
    return insert_index < inserts_.size() && inserts_[insert_index] != nullptr &&
           inserts_[insert_index]->last_applied_parameter_value(param_id, out);
  }
  // Toggles bypass for the insert at @p insert_index. When @p reset_on_bypass is
  // true the processor is reset as it is bypassed. Returns false for an
  // out-of-range insert. Mirrors ChannelStrip::set_insert_bypassed.
  bool set_insert_bypassed(unsigned int insert_index, bool bypassed,
                           bool reset_on_bypass = false) noexcept;
  // Resolves a processor JSON-key parameter name to its integer param_id for the
  // insert at @p insert_index, or -1 if unknown/non-realtime-safe. Control-thread
  // API: reads the processor's static descriptor table, touching no mutable audio
  // state. Mirrors ChannelStrip::insert_parameter_id_for_key.
  int insert_parameter_id_for_key(unsigned int insert_index, const std::string& key) const noexcept;
  /// The processor at @p insert_index, or nullptr. Allocation free.
  const rt::ProcessorBase* insert_processor(unsigned int insert_index) const noexcept {
    return insert_index < inserts_.size() ? inserts_[insert_index].get() : nullptr;
  }
  void set_insert_sidechain(unsigned int insert_index, const float* const* channels,
                            int num_channels, int num_samples);
  // Drops every key for the next block. Slots once keyed through
  // set_insert_sidechain() clear their processor's sidechain; others are left alone.
  void clear_insert_sidechains() noexcept;
  MeterSnapshot meter_snapshot() const noexcept { return meter_.snapshot(); }
  /// @brief Ends the signal for the bus meter (see MeterProcessor::flush_true_peak).
  void flush_meters() noexcept { meter_.flush_true_peak(); }
  /// Per-insert audible gain reduction (dB <= 0) of the last block. Copies
  /// min(@p capacity, count) entries and returns the count. Audio-thread read only.
  size_t insert_gain_reduction_db(float* out, size_t capacity) const noexcept {
    const size_t n = std::min(capacity, insert_gain_reduction_count_);
    for (size_t i = 0; i < n; ++i) out[i] = insert_gain_reduction_db_[i];
    return insert_gain_reduction_count_;
  }
  size_t insert_sidechain_slot_count() const noexcept { return insert_sidechains_.size(); }
  size_t insert_sidechains_capacity() const noexcept { return insert_sidechains_.capacity(); }

  BusRole role() const noexcept { return role_; }
  int max_inputs() const noexcept { return max_inputs_; }
  /// Declares the bus speaker layout for linked detector routing. Direct bus
  /// users keep the stereo default; the realtime mixer sets this from its
  /// TrackBusConfig before rendering a surround group bus.
  void set_channel_layout(ChannelLayout layout) noexcept { layout_ = layout; }
  ChannelLayout channel_layout() const noexcept { return layout_; }

  // Upper bound on inserts per bus. Reserved at construction so add_insert
  // never reallocates inserts_ / insert_sidechains_ while the audio thread
  // iterates them in process(). Exceeding the cap throws SonareException
  // (InvalidState), mirroring ChannelStrip::add_pre/post_insert.
  static constexpr size_t kMaxInserts = 64;

 private:
  static constexpr int kMaxBusScratchChannels = 8;

  void prepare_insert_alignment_delays(size_t insert_index);

  /// @brief Every owned processor's discard count added together -- the inserts
  ///        and the meter -- for the block delta in process(). RT-safe: relaxed
  ///        atomic loads only.
  /// @details These are owned here and reachable from outside only as a count,
  ///   so a discard inside one is observable nowhere unless the bus records it.
  ///   The sum answers "did any of them move", which is the question the bus's
  ///   own per-block count asks; it is never published as a count of its own,
  ///   and summing is safe only because of that -- a member may be driven
  ///   several times per the bus's block. Mirrors ChannelStrip.
  uint64_t member_discard_sum() const noexcept {
    uint64_t total = meter_.non_finite_discard_count();
    for (const auto& insert : inserts_) {
      if (insert) total += insert->non_finite_discard_count();
    }
    return total;
  }

  BusRole role_ = BusRole::Subgroup;
  int max_inputs_ = 0;
  ChannelLayout layout_ = ChannelLayout::Stereo;
  std::vector<std::unique_ptr<rt::ProcessorBase>> inserts_;
  std::array<float, kMaxInserts> insert_gain_reduction_db_{};
  size_t insert_gain_reduction_count_ = 0;
  // Parallel to inserts_: 1 marks a StereoPairOnly insert (front-pair-only on a
  // surround bus). Reserved at construction alongside inserts_ so add_insert
  // never reallocates it while process() iterates.
  std::vector<uint8_t> insert_spo_;
  std::vector<InsertSidechain> insert_sidechains_;
  // One preallocated delay bank per insert. A latent StereoPairOnly insert
  // processes only L/R on a surround bus, so its corresponding bank delays the
  // otherwise untouched planes (C/LFE/surrounds) by exactly the same Q8 amount.
  std::array<AlignmentDelay, kMaxInserts> stereo_pair_alignment_delays_;
  // Second bank, one slot per insert, standing in for a BYPASSED insert's own
  // latency across every plane. latency_samples_q8() deliberately still counts a
  // bypassed insert -- host PDC is a control-thread quantity and bypass flips on
  // the audio thread -- so the chain has to keep delivering what it advertises.
  // Both banks stay primed while unused so a toggle is continuous. Mirrors
  // ChannelStrip's soft-bypass contract.
  std::array<AlignmentDelay, kMaxInserts> bypass_alignment_delays_;
  // Reusable planar copy for warming bypassed inserts. Bus layouts are capped
  // at eight planes here, and the rows are allocated once in prepare().
  std::array<std::vector<float>, kMaxBusScratchChannels> bypass_scratch_;
  std::array<float*, kMaxBusScratchChannels> bypass_scratch_channels_{};
  MeterProcessor meter_{};
  double sample_rate_ = sonare::constants::kDefaultDawSampleRate;
  int max_block_size_ = 0;
};

}  // namespace sonare::mixing
