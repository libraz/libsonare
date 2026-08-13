#include "mixing/bus.h"

#include <algorithm>
#include <utility>

#include "mixing/tail_utils.h"
#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::mixing {

namespace {

// Widest bed a phase-1 bus can be handed, and the planes behind its front pair.
// Derived from the layout table rather than written as a literal so a new
// layout cannot leave a compensation bank silently short.
constexpr int kMaxBusPlanes = channel_count(ChannelLayout::SevenPointOne);
constexpr int kMaxBusSurroundPlanes = kMaxBusPlanes - 2;

}  // namespace

BusProcessor::BusProcessor(BusRole role, int max_inputs) : role_(role), max_inputs_(max_inputs) {
  // Pre-reserve so add_insert (control thread) never reallocates inserts_ /
  // insert_sidechains_ while process() (audio thread) iterates them; a
  // reallocation would invalidate the in-flight pointers/iterators (C++ UB).
  inserts_.reserve(kMaxInserts);
  insert_spo_.reserve(kMaxInserts);
  insert_sidechains_.reserve(kMaxInserts);
}

void BusProcessor::prepare(double sample_rate, int max_block_size) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  max_block_size_ = max_block_size;
  for (size_t index = 0; index < inserts_.size(); ++index) {
    inserts_[index]->prepare(sample_rate_, max_block_size_);
    prepare_insert_alignment_delays(index);
  }
  meter_.prepare(sample_rate_, max_block_size_);
}

void BusProcessor::process(float* const* channels, int num_channels, int num_samples) {
  // IIR-based inserts (EQ, compressor, limiter) can accumulate denormals during
  // silence, which causes 10-100x CPU spikes on x86 without DAZ/FTZ. Mirror the
  // mastering processors and voice changer guard at the process-block boundary.
  rt::ScopedNoDenormals no_denormals;
  // Single-segment chain: no first-insert offset, no sidechain shift. The bus
  // forwards the full sidechain width (up to kMaxSidechainChannels).
  std::array<const float*, kMaxSidechainChannels> shifted{};
  run_insert_chain(inserts_, insert_spo_, insert_sidechains_, channels, num_channels, num_samples,
                   /*first_insert_index=*/0, /*sidechain_offset=*/0, shifted.data(),
                   kMaxSidechainChannels, lfe_index(layout_), stereo_pair_alignment_delays_.data(),
                   bypass_alignment_delays_.data());
  meter_.process(channels, num_channels, num_samples);
}

void BusProcessor::reset() {
  for (auto& insert : inserts_) {
    insert->reset();
  }
  for (auto& delay : stereo_pair_alignment_delays_) {
    delay.reset();
  }
  for (auto& delay : bypass_alignment_delays_) {
    delay.reset();
  }
  meter_.reset();
}

int BusProcessor::latency_samples() const noexcept { return latency_samples_q8() >> 8; }

int BusProcessor::latency_samples_q8() const noexcept {
  int total = 0;
  for (const auto& insert : inserts_) {
    total += insert->latency_samples_q8();
  }
  return total;
}

int BusProcessor::tail_samples() const noexcept { return processor_chain_tail_samples(inserts_); }

void BusProcessor::add_insert(std::unique_ptr<rt::ProcessorBase> processor, bool stereo_pair_only) {
  if (!processor) {
    throw SonareException(ErrorCode::InvalidParameter, "insert processor must not be null");
  }
  if (inserts_.size() >= kMaxInserts) {
    throw SonareException(ErrorCode::InvalidState, "BusProcessor insert cap exceeded");
  }
  if (max_block_size_ > 0) {
    processor->prepare(sample_rate_, max_block_size_);
  }
  inserts_.push_back(std::move(processor));
  insert_spo_.push_back(stereo_pair_only ? 1 : 0);
  insert_sidechains_.resize(inserts_.size());
  if (max_block_size_ > 0) {
    prepare_insert_alignment_delays(inserts_.size() - 1);
  }
}

void BusProcessor::prepare_insert_alignment_delays(size_t insert_index) {
  if (insert_index >= inserts_.size() || insert_index >= stereo_pair_alignment_delays_.size()) {
    return;
  }

  const bool stereo_pair_only = insert_index < insert_spo_.size() && insert_spo_[insert_index] != 0;
  const int latency_q8 = inserts_[insert_index]->latency_samples_q8();

  // Phase-1 layouts top out at 7.1, so the six planes after L/R cover every
  // possible untouched surround plane. This is deliberately independent from
  // the sidechain width: it is program-audio state, not detector state.
  configure_insert_alignment_delay(stereo_pair_alignment_delays_[insert_index],
                                   kMaxBusSurroundPlanes, stereo_pair_only ? latency_q8 : 0);
  // The bypass substitute replaces the whole insert, so it spans every plane a
  // 7.1 bus can present rather than only the ones behind the front pair.
  configure_insert_alignment_delay(bypass_alignment_delays_[insert_index], kMaxBusPlanes,
                                   latency_q8);
}

bool BusProcessor::apply_insert_parameter(unsigned int insert_index, unsigned int param_id,
                                          float value) noexcept {
  const size_t index = insert_index;
  if (index >= inserts_.size()) {
    return false;
  }
  rt::ProcessorBase* insert = inserts_[index].get();
  if (insert == nullptr || !insert->parameter_is_realtime_safe(param_id)) {
    return false;
  }
  return insert->set_parameter(param_id, value);
}

bool BusProcessor::set_insert_bypassed(unsigned int insert_index, bool bypassed,
                                       bool reset_on_bypass) noexcept {
  const size_t index = insert_index;
  if (index >= inserts_.size()) {
    return false;
  }
  rt::ProcessorBase* insert = inserts_[index].get();
  return insert != nullptr && insert->set_bypassed(bypassed, reset_on_bypass);
}

int BusProcessor::insert_parameter_id_for_key(unsigned int insert_index,
                                              const std::string& key) const noexcept {
  const size_t index = insert_index;
  if (index >= inserts_.size()) {
    return -1;
  }
  const rt::ProcessorBase* insert = inserts_[index].get();
  if (insert == nullptr) {
    return -1;
  }
  for (const auto& desc : insert->parameter_descriptors()) {
    if (desc.key == key) {
      return static_cast<int>(desc.id);
    }
  }
  return -1;
}

void BusProcessor::set_insert_sidechain(unsigned int insert_index, const float* const* channels,
                                        int num_channels, int num_samples) {
  const size_t index = insert_index;
  // insert_sidechains_ is sized by add_insert (control thread). Never resize
  // here: process() reads it on the audio thread, so a resize could grow or
  // reallocate the vector under the reader. Out-of-range keys are no-ops,
  // matching ChannelStrip::set_insert_sidechain().
  if (index >= insert_sidechains_.size()) {
    return;
  }
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
    insert_sidechains_[index] = {{}, 0, 0, true};
    return;
  }
  const int n = std::min(num_channels, kMaxSidechainChannels);
  InsertSidechain entry;
  entry.channels = {};
  for (int ch = 0; ch < n; ++ch) {
    entry.channels[static_cast<size_t>(ch)] = channels[ch];
  }
  entry.num_channels = n;
  entry.num_samples = num_samples;
  entry.managed = true;
  insert_sidechains_[index] = entry;
}

void BusProcessor::clear_insert_sidechains() noexcept {
  for (auto& sidechain : insert_sidechains_) {
    sidechain = {};
  }
}

}  // namespace sonare::mixing
