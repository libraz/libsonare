#include "core/resample.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "CDSPResampler.h"
#include "util/exception.h"

namespace sonare {

std::vector<float> resample(const float* samples, size_t size, int src_sr, int target_sr) {
  SONARE_CHECK(src_sr > 0 && target_sr > 0, ErrorCode::InvalidParameter);
  // Reject null + size>0 before either the copy or resampler path can dereference it.
  // A null pointer with size==0 is treated as an empty input (valid).
  SONARE_CHECK_MSG(samples != nullptr || size == 0, ErrorCode::InvalidParameter,
                   "resample: samples is null but size > 0");

  if (size == 0) {
    return {};
  }

  // If sample rates are equal, just copy
  if (src_sr == target_sr) {
    return std::vector<float>(samples, samples + size);
  }

  // Calculate output size
  const double ratio = static_cast<double>(target_sr) / static_cast<double>(src_sr);
  const size_t expected_size = static_cast<size_t>(std::round(static_cast<double>(size) * ratio));

  std::vector<float> result;
  result.reserve(expected_size + 1);

  // Create resampler (24-bit quality for float precision)
  constexpr int kBlockSize = 1024;
  r8b::CDSPResampler24 resampler(static_cast<double>(src_sr), static_cast<double>(target_sr),
                                 kBlockSize);

  // The resampler has internal filter latency, so the first valid output sample
  // only emerges after several input samples. Ask r8brain exactly how many input
  // samples (real + flush zeros) are needed to produce the full expected output,
  // then flush precisely that many trailing zeros — no arbitrary pass cap that
  // could truncate the filter tail or over-pad with zeros.
  const size_t required_input =
      expected_size > 0 ? static_cast<size_t>(
                              resampler.getInputRequiredForOutput(static_cast<int>(expected_size)))
                        : 0;

  // Convert only the current block to r8brain's double input. Retaining whole-input and
  // whole-output double vectors here used roughly two extra copies of a long file.
  std::array<double, kBlockSize> input_block{};
  auto append_output = [&result](const double* output, int output_length) {
    if (output == nullptr || output_length <= 0) return;
    for (int index = 0; index < output_length; ++index) {
      result.push_back(static_cast<float>(output[index]));
    }
  };

  // Process in blocks.
  const float* input_ptr = samples;
  size_t remaining = size;

  while (remaining > 0) {
    int block_len = static_cast<int>(std::min(remaining, static_cast<size_t>(kBlockSize)));
    for (int index = 0; index < block_len; ++index) {
      input_block[static_cast<size_t>(index)] = static_cast<double>(input_ptr[index]);
    }

    double* output_ptr = nullptr;
    const int output_len = resampler.process(input_block.data(), block_len, output_ptr);
    append_output(output_ptr, output_len);

    input_ptr += block_len;
    remaining -= block_len;
  }

  // Flush exactly the required number of trailing zeros to drain the filter tail.
  std::array<double, kBlockSize> zeros{};
  size_t flush_remaining = (required_input > size) ? (required_input - size) : 0;
  while (flush_remaining > 0 && result.size() < expected_size) {
    int block_len = static_cast<int>(std::min(flush_remaining, static_cast<size_t>(kBlockSize)));

    double* output_ptr = nullptr;
    const int output_len = resampler.process(zeros.data(), block_len, output_ptr);
    append_output(output_ptr, output_len);
    flush_remaining -= static_cast<size_t>(block_len);
  }

  // Trim to the analytic expected size (drops any extra latency-induced samples).
  if (result.size() > expected_size) {
    result.resize(expected_size);
  }

  return result;
}

Audio resample(const Audio& audio, int target_sr) {
  if (audio.empty()) {
    return Audio::from_buffer(nullptr, 0, target_sr);
  }

  if (audio.sample_rate() == target_sr) {
    return audio.to_mono();  // Return a copy
  }

  std::vector<float> resampled =
      resample(audio.data(), audio.size(), audio.sample_rate(), target_sr);
  return Audio::from_vector(std::move(resampled), target_sr);
}

}  // namespace sonare
