#include "rt/partitioned_convolver.h"

#include <algorithm>

#include "util/exception.h"

namespace sonare::rt {

PartitionedConvolver::PartitionedConvolver(PartitionedConvolverConfig config) : config_(config) {
  validate_config();
  rebuild_fft();
}

void PartitionedConvolver::set_impulse_response(const float* impulse_response, int num_samples) {
  if (num_samples < 0)
    throw SonareException(ErrorCode::InvalidParameter, "num_samples must be non-negative");
  if (num_samples > 0 && impulse_response == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "impulse_response must not be null");
  }

  const int partition_size = config_.partition_size;
  const int partitions = num_samples == 0 ? 0 : (num_samples + partition_size - 1) / partition_size;
  ir_partitions_.assign(static_cast<size_t>(partitions),
                        std::vector<std::complex<float>>(static_cast<size_t>(fft_->n_bins())));

  std::vector<float> partition_time(static_cast<size_t>(fft_size_), 0.0f);
  for (int partition = 0; partition < partitions; ++partition) {
    std::fill(partition_time.begin(), partition_time.end(), 0.0f);
    const int offset = partition * partition_size;
    const int copy_count = std::min(partition_size, num_samples - offset);
    std::copy_n(impulse_response + offset, copy_count, partition_time.begin());
    fft_->forward(partition_time.data(), ir_partitions_[static_cast<size_t>(partition)].data());
  }

  // One input spectrum per partition forms the frequency-domain ring; the
  // overlap-add tail spans a single partition (fft_size_ - partition_size).
  const size_t bins = static_cast<size_t>(fft_->n_bins());
  input_spectrum_ring_.assign(static_cast<size_t>(partitions),
                              std::vector<std::complex<float>>(bins, {0.0f, 0.0f}));
  ola_buffer_.assign(static_cast<size_t>(fft_size_), 0.0f);
  reset();
}

void PartitionedConvolver::set_impulse_response(const std::vector<float>& impulse_response) {
  set_impulse_response(impulse_response.data(), static_cast<int>(impulse_response.size()));
}

void PartitionedConvolver::reset() {
  std::fill(ola_buffer_.begin(), ola_buffer_.end(), 0.0f);
  ring_pos_ = 0;
  for (auto& spectrum : input_spectrum_ring_) {
    std::fill(spectrum.begin(), spectrum.end(), std::complex<float>{0.0f, 0.0f});
  }
}

void PartitionedConvolver::process_block(const float* input, float* output) noexcept {
  const int partition_size = config_.partition_size;
  // The audio thread must never throw: tolerate null pointers by emitting
  // silence (when possible) and returning without touching internal state.
  if (input == nullptr || output == nullptr) {
    if (output != nullptr) std::fill(output, output + partition_size, 0.0f);
    return;
  }

  if (ir_partitions_.empty()) {
    std::fill(output, output + partition_size, 0.0f);
    return;
  }

  // Zero-padded forward transform of the current block ([block, zeros]).
  std::copy(input, input + partition_size, fft_input_.begin());
  std::fill(fft_input_.begin() + partition_size, fft_input_.end(), 0.0f);
  fft_->forward(fft_input_.data(), current_spectrum_.data());

  // Store this block's spectrum in the ring and accumulate the partitioned
  // products in the frequency domain: Y[bin] = sum_p X[m-p] * IR[p][bin].
  const int partitions = num_partitions();
  std::copy(current_spectrum_.begin(), current_spectrum_.end(),
            input_spectrum_ring_[static_cast<size_t>(ring_pos_)].begin());

  std::fill(accum_spectrum_.begin(), accum_spectrum_.end(), std::complex<float>{0.0f, 0.0f});
  for (int partition = 0; partition < partitions; ++partition) {
    int ring_index = ring_pos_ - partition;
    if (ring_index < 0) ring_index += partitions;
    const auto& x_spectrum = input_spectrum_ring_[static_cast<size_t>(ring_index)];
    const auto& ir_spectrum = ir_partitions_[static_cast<size_t>(partition)];
    for (size_t bin = 0; bin < accum_spectrum_.size(); ++bin) {
      accum_spectrum_[bin] += x_spectrum[bin] * ir_spectrum[bin];
    }
  }

  // Single inverse FFT, then overlap-add the 2N segment into the output stream.
  fft_->inverse(accum_spectrum_.data(), fft_output_.data());
  for (int i = 0; i < fft_size_; ++i) {
    ola_buffer_[static_cast<size_t>(i)] += fft_output_[static_cast<size_t>(i)];
  }
  std::copy(ola_buffer_.begin(), ola_buffer_.begin() + partition_size, output);

  // Shift the overlap tail down by one partition for the next block.
  std::move(ola_buffer_.begin() + partition_size, ola_buffer_.end(), ola_buffer_.begin());
  std::fill(ola_buffer_.end() - partition_size, ola_buffer_.end(), 0.0f);

  ring_pos_ = (ring_pos_ + 1 >= partitions) ? 0 : ring_pos_ + 1;
}

void PartitionedConvolver::validate_config() const {
  if (config_.partition_size <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "partition_size must be positive");
  }
}

void PartitionedConvolver::rebuild_fft() {
  fft_size_ = config_.partition_size * 2;
  fft_ = std::make_unique<sonare::FFT>(fft_size_);
  fft_input_.assign(static_cast<size_t>(fft_size_), 0.0f);
  fft_output_.assign(static_cast<size_t>(fft_size_), 0.0f);
  current_spectrum_.assign(static_cast<size_t>(fft_->n_bins()), {0.0f, 0.0f});
  accum_spectrum_.assign(static_cast<size_t>(fft_->n_bins()), {0.0f, 0.0f});
}

}  // namespace sonare::rt
