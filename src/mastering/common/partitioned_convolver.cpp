#include "mastering/common/partitioned_convolver.h"

#include <algorithm>
#include <stdexcept>

namespace sonare::mastering::common {

PartitionedConvolver::PartitionedConvolver(PartitionedConvolverConfig config) : config_(config) {
  validate_config();
  rebuild_fft();
}

void PartitionedConvolver::set_impulse_response(const float* impulse_response, int num_samples) {
  if (num_samples < 0) throw std::invalid_argument("num_samples must be non-negative");
  if (num_samples > 0 && impulse_response == nullptr) {
    throw std::invalid_argument("impulse_response must not be null");
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

  input_spectra_.assign(static_cast<size_t>(partitions),
                        std::vector<std::complex<float>>(static_cast<size_t>(fft_->n_bins())));
  ola_buffer_.assign(static_cast<size_t>((partitions + 1) * partition_size), 0.0f);
  input_spectrum_index_ = 0;
  reset();
}

void PartitionedConvolver::set_impulse_response(const std::vector<float>& impulse_response) {
  set_impulse_response(impulse_response.data(), static_cast<int>(impulse_response.size()));
}

void PartitionedConvolver::reset() {
  std::fill(overlap_.begin(), overlap_.end(), 0.0f);
  for (auto& spectrum : input_spectra_) {
    std::fill(spectrum.begin(), spectrum.end(), std::complex<float>{0.0f, 0.0f});
  }
  std::fill(ola_buffer_.begin(), ola_buffer_.end(), 0.0f);
  input_spectrum_index_ = 0;
}

void PartitionedConvolver::process_block(const float* input, float* output) {
  if (input == nullptr || output == nullptr) throw std::invalid_argument("input/output is null");

  const int partition_size = config_.partition_size;
  if (ir_partitions_.empty()) {
    std::fill(output, output + partition_size, 0.0f);
    return;
  }

  std::copy(input, input + partition_size, fft_input_.begin());
  std::fill(fft_input_.begin() + partition_size, fft_input_.end(), 0.0f);

  fft_->forward(fft_input_.data(), current_spectrum_.data());

  const int partitions = num_partitions();
  for (int partition = 0; partition < partitions; ++partition) {
    const auto& ir_spectrum = ir_partitions_[static_cast<size_t>(partition)];
    for (size_t bin = 0; bin < accum_spectrum_.size(); ++bin) {
      accum_spectrum_[bin] = current_spectrum_[bin] * ir_spectrum[bin];
    }
    fft_->inverse(accum_spectrum_.data(), fft_output_.data());
    const size_t offset = static_cast<size_t>(partition * partition_size);
    for (int i = 0; i < fft_size_; ++i) {
      ola_buffer_[offset + static_cast<size_t>(i)] += fft_output_[static_cast<size_t>(i)];
    }
  }

  std::copy(ola_buffer_.begin(), ola_buffer_.begin() + partition_size, output);
  std::move(ola_buffer_.begin() + partition_size, ola_buffer_.end(), ola_buffer_.begin());
  std::fill(ola_buffer_.end() - partition_size, ola_buffer_.end(), 0.0f);
}

void PartitionedConvolver::validate_config() const {
  if (config_.partition_size <= 0) {
    throw std::invalid_argument("partition_size must be positive");
  }
}

void PartitionedConvolver::rebuild_fft() {
  fft_size_ = config_.partition_size * 2;
  fft_ = std::make_unique<sonare::FFT>(fft_size_);
  overlap_.assign(static_cast<size_t>(config_.partition_size), 0.0f);
  fft_input_.assign(static_cast<size_t>(fft_size_), 0.0f);
  fft_output_.assign(static_cast<size_t>(fft_size_), 0.0f);
  current_spectrum_.assign(static_cast<size_t>(fft_->n_bins()), {0.0f, 0.0f});
  accum_spectrum_.assign(static_cast<size_t>(fft_->n_bins()), {0.0f, 0.0f});
}

}  // namespace sonare::mastering::common
