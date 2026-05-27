#include "effects/reverb/convolution_reverb.h"

#include <algorithm>
#include <stdexcept>

#include "rt/scoped_no_denormals.h"

namespace sonare::effects::reverb {

namespace {
// The library targets mono/stereo only; preallocate engines for two channels.
constexpr int kMaxChannels = 2;
// FFT partition size used by the per-channel convolvers. A power of two keeps
// the underlying real FFT efficient while bounding the block buffering latency.
constexpr int kPartitionSize = 256;
}  // namespace

void ConvolutionReverb::prepare(double, int) {
  partition_size_ = kPartitionSize;
  convolvers_.resize(static_cast<size_t>(kMaxChannels));
  block_input_.assign(static_cast<size_t>(kMaxChannels),
                      std::vector<float>(static_cast<size_t>(partition_size_), 0.0f));
  block_output_.assign(static_cast<size_t>(kMaxChannels),
                       std::vector<float>(static_cast<size_t>(partition_size_), 0.0f));
  fill_count_.assign(static_cast<size_t>(kMaxChannels), 0);
  rebuild_convolvers();
  reset();
}

void ConvolutionReverb::rebuild_convolvers() {
  if (partition_size_ <= 0) {
    return;
  }
  for (auto& convolver : convolvers_) {
    if (!convolver) {
      convolver = std::make_unique<rt::PartitionedConvolver>(
          rt::PartitionedConvolverConfig{partition_size_});
    }
    convolver->set_impulse_response(ir_);
  }
}

void ConvolutionReverb::process(float* const* channels, int num_channels, int num_samples) {
  rt::ScopedNoDenormals no_denormals;
  if (channels == nullptr || num_channels <= 0 || num_samples <= 0 || ir_.empty() ||
      partition_size_ <= 0) {
    return;
  }
  // Convolvers/buffers are preallocated for the maximum supported channel count;
  // clamp here so the audio thread never allocates.
  const int channels_to_process = std::min(num_channels, static_cast<int>(convolvers_.size()));
  for (int ch = 0; ch < channels_to_process; ++ch) {
    auto& convolver = convolvers_[static_cast<size_t>(ch)];
    if (channels[ch] == nullptr || !convolver) {
      continue;
    }
    float* data = channels[ch];
    auto& in_block = block_input_[static_cast<size_t>(ch)];
    auto& out_block = block_output_[static_cast<size_t>(ch)];
    int fill = fill_count_[static_cast<size_t>(ch)];
    for (int i = 0; i < num_samples; ++i) {
      // Emit the convolution output produced one partition ago, then stage the
      // incoming sample. This introduces partition_size_ samples of latency.
      const float input_sample = data[i];
      data[i] = out_block[static_cast<size_t>(fill)];
      in_block[static_cast<size_t>(fill)] = input_sample;
      if (++fill == partition_size_) {
        convolver->process_block(in_block.data(), out_block.data());
        fill = 0;
      }
    }
    fill_count_[static_cast<size_t>(ch)] = fill;
  }
}

void ConvolutionReverb::reset() {
  for (auto& convolver : convolvers_) {
    if (convolver) {
      convolver->reset();
    }
  }
  for (auto& block : block_input_) {
    std::fill(block.begin(), block.end(), 0.0f);
  }
  for (auto& block : block_output_) {
    std::fill(block.begin(), block.end(), 0.0f);
  }
  std::fill(fill_count_.begin(), fill_count_.end(), 0);
}

void ConvolutionReverb::load_ir(const float* impulse_response, int num_samples) {
  if (num_samples < 0 || (num_samples > 0 && impulse_response == nullptr)) {
    throw std::invalid_argument("invalid impulse response");
  }
  ir_.assign(impulse_response, impulse_response + num_samples);
  // Feeding the IR into the convolvers (re)allocates FFT partitions; this is a
  // non-RT operation, so it is safe to run here outside the audio thread.
  rebuild_convolvers();
}

void ConvolutionReverb::load_ir(const std::vector<float>& impulse_response) {
  load_ir(impulse_response.data(), static_cast<int>(impulse_response.size()));
}

}  // namespace sonare::effects::reverb
