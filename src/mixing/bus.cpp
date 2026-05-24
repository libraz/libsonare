#include "mixing/bus.h"

#include <algorithm>

namespace sonare::mixing {

BusProcessor::BusProcessor(BusRole role, int max_inputs) : role_(role), max_inputs_(max_inputs) {}

void BusProcessor::prepare(double, int) {}

void BusProcessor::process(float* const*, int, int) {}

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

}  // namespace sonare::mixing
