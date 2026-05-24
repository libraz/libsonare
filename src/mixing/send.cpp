#include "mixing/send.h"

namespace sonare::mixing {

SendProcessor::SendProcessor(SendConfig config)
    : gain_({config.send_db, config.smoothing_ms}), timing_(config.timing) {}

void SendProcessor::prepare(double sample_rate, int max_block_size) {
  gain_.prepare(sample_rate, max_block_size);
}

void SendProcessor::process(float* const* channels, int num_channels, int num_samples) {
  gain_.process(channels, num_channels, num_samples);
}

void SendProcessor::reset() { gain_.reset(); }

void SendProcessor::set_send_db(float send_db) noexcept { gain_.set_gain_db(send_db); }

}  // namespace sonare::mixing
