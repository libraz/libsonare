#include "mixing/fx_bus.h"

#include <stdexcept>
#include <utility>

namespace sonare::mixing {

FxBus::FxBus(int max_inputs) : bus_(BusRole::Aux, max_inputs) {}

void FxBus::prepare(double sample_rate, int max_block_size) {
  sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
  max_block_size_ = max_block_size;
  bus_.prepare(sample_rate_, max_block_size_);
  for (auto& insert : inserts_) {
    insert->prepare(sample_rate_, max_block_size_);
  }
}

void FxBus::process(float* const* channels, int num_channels, int num_samples) {
  for (auto& insert : inserts_) {
    insert->process(channels, num_channels, num_samples);
  }
}

void FxBus::reset() {
  bus_.reset();
  for (auto& insert : inserts_) {
    insert->reset();
  }
}

int FxBus::latency_samples() const noexcept {
  int total = bus_.latency_samples();
  for (const auto& insert : inserts_) {
    total += insert->latency_samples();
  }
  return total;
}

void FxBus::add_insert(std::unique_ptr<rt::ProcessorBase> processor) {
  if (!processor) {
    throw std::invalid_argument("insert processor must not be null");
  }
  if (max_block_size_ > 0) {
    processor->prepare(sample_rate_, max_block_size_);
  }
  inserts_.push_back(std::move(processor));
}

}  // namespace sonare::mixing
