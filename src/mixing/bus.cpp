#include "mixing/bus.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace sonare::mixing {

BusProcessor::BusProcessor(BusRole role, int max_inputs) : role_(role), max_inputs_(max_inputs) {}

void BusProcessor::prepare(double sample_rate, int max_block_size) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  max_block_size_ = max_block_size;
  for (auto& insert : inserts_) {
    insert->prepare(sample_rate_, max_block_size_);
  }
  meter_.prepare(sample_rate_, max_block_size_);
}

void BusProcessor::process(float* const* channels, int num_channels, int num_samples) {
  for (size_t index = 0; index < inserts_.size(); ++index) {
    const InsertSidechain* key =
        index < insert_sidechains_.size() ? &insert_sidechains_[index] : nullptr;
    if (key != nullptr && key->num_channels > 0 && key->num_samples >= num_samples) {
      inserts_[index]->set_sidechain(key->channels.data(), key->num_channels, num_samples);
    } else if (key != nullptr && key->managed) {
      inserts_[index]->clear_sidechain();
    } else {
      // Leave directly configured processor sidechains intact.
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
    throw std::invalid_argument("insert processor must not be null");
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
  if (index >= inserts_.size()) {
    return;
  }
  if (insert_sidechains_.size() < inserts_.size()) {
    insert_sidechains_.resize(inserts_.size());
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
