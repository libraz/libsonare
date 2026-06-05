#include "mixing/bus.h"

#include <algorithm>
#include <utility>

#include "rt/scoped_no_denormals.h"
#include "util/exception.h"

namespace sonare::mixing {

namespace {

int total_tail_samples(const std::vector<std::unique_ptr<rt::ProcessorBase>>& inserts) noexcept {
  int total = 0;
  for (const auto& insert : inserts) {
    total += std::max(0, insert->tail_samples());
  }
  return total;
}

}  // namespace

BusProcessor::BusProcessor(BusRole role, int max_inputs) : role_(role), max_inputs_(max_inputs) {
  // Pre-reserve so add_insert (control thread) never reallocates inserts_ /
  // insert_sidechains_ while process() (audio thread) iterates them; a
  // reallocation would invalidate the in-flight pointers/iterators (C++ UB).
  inserts_.reserve(kMaxInserts);
  insert_sidechains_.reserve(kMaxInserts);
}

void BusProcessor::prepare(double sample_rate, int max_block_size) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  max_block_size_ = max_block_size;
  for (auto& insert : inserts_) {
    insert->prepare(sample_rate_, max_block_size_);
  }
  meter_.prepare(sample_rate_, max_block_size_);
}

void BusProcessor::process(float* const* channels, int num_channels, int num_samples) {
  // IIR-based inserts (EQ, compressor, limiter) can accumulate denormals during
  // silence, which causes 10-100x CPU spikes on x86 without DAZ/FTZ. Mirror the
  // mastering processors and voice changer guard at the process-block boundary.
  rt::ScopedNoDenormals no_denormals;
  for (size_t index = 0; index < inserts_.size(); ++index) {
    const InsertSidechain* key =
        index < insert_sidechains_.size() ? &insert_sidechains_[index] : nullptr;
    if (key != nullptr && key->num_channels > 0 && key->num_samples > 0) {
      // Clip the key length to whatever is available rather than discarding a
      // short block; dropping the key would make a sidechain compressor lose
      // its detector input for the block and click. Mirrors ChannelStrip.
      inserts_[index]->set_sidechain(key->channels.data(), key->num_channels,
                                     std::min(key->num_samples, num_samples));
    } else if (key != nullptr && key->managed) {
      inserts_[index]->clear_sidechain();
    } else {
      // Leave directly configured processor sidechains intact.
    }
    if (inserts_[index]->bypassed()) {
      continue;
    }
    inserts_[index]->process(channels, num_channels, num_samples);
  }
  meter_.process(channels, num_channels, num_samples);
}

void BusProcessor::reset() {
  for (auto& insert : inserts_) {
    insert->reset();
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

int BusProcessor::tail_samples() const noexcept { return total_tail_samples(inserts_); }

void BusProcessor::sum_inputs(const std::vector<float* const*>& inputs, float* const* output,
                              int num_channels, int num_samples) const {
  if (output == nullptr || num_channels <= 0 || num_samples <= 0) {
    return;
  }

  for (int ch = 0; ch < num_channels; ++ch) {
    if (output[ch] != nullptr) {
      std::fill(output[ch], output[ch] + num_samples, 0.0f);
    }
  }

  const int limit = max_inputs_ > 0 ? std::min(static_cast<int>(inputs.size()), max_inputs_)
                                    : static_cast<int>(inputs.size());
  for (int input_index = 0; input_index < limit; ++input_index) {
    float* const* input = inputs[static_cast<size_t>(input_index)];
    if (input == nullptr) {
      continue;
    }
    for (int ch = 0; ch < num_channels; ++ch) {
      if (input[ch] == nullptr || output[ch] == nullptr) {
        continue;
      }
      for (int i = 0; i < num_samples; ++i) {
        output[ch][i] += input[ch][i];
      }
    }
  }
}

void BusProcessor::add_insert(std::unique_ptr<rt::ProcessorBase> processor) {
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
  insert_sidechains_.resize(inserts_.size());
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
