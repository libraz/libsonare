#include "mixing/fx_bus.h"

#include <utility>

namespace sonare::mixing {

FxBus::FxBus(int max_inputs) : bus_(BusRole::Aux, max_inputs) {}

void FxBus::prepare(double sample_rate, int max_block_size) {
  bus_.prepare(sample_rate, max_block_size);
}

void FxBus::process(float* const* channels, int num_channels, int num_samples) {
  bus_.process(channels, num_channels, num_samples);
}

void FxBus::reset() { bus_.reset(); }

int FxBus::latency_samples() const noexcept { return bus_.latency_samples(); }

int FxBus::latency_samples_q8() const noexcept { return bus_.latency_samples_q8(); }

void FxBus::add_insert(std::unique_ptr<rt::ProcessorBase> processor) {
  bus_.add_insert(std::move(processor));
}

void FxBus::set_insert_sidechain(unsigned int insert_index, const float* const* channels,
                                 int num_channels, int num_samples) {
  bus_.set_insert_sidechain(insert_index, channels, num_channels, num_samples);
}

void FxBus::clear_insert_sidechains() noexcept { bus_.clear_insert_sidechains(); }

}  // namespace sonare::mixing
