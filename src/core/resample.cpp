#include "core/resample.h"

#include <algorithm>
#include <cmath>

#include "CDSPResampler.h"
#include "util/exception.h"

namespace sonare {

std::vector<float> resample(const float* samples, size_t size, int src_sr, int target_sr) {
  SONARE_CHECK(src_sr > 0 && target_sr > 0, ErrorCode::InvalidParameter);

  if (size == 0) {
    return {};
  }

  // If sample rates are equal, just copy
  if (src_sr == target_sr) {
    return std::vector<float>(samples, samples + size);
  }

  // Convert input to double for r8brain
  std::vector<double> input_double(size);
  for (size_t i = 0; i < size; ++i) {
    input_double[i] = static_cast<double>(samples[i]);
  }

  // Calculate output size
  const double ratio = static_cast<double>(target_sr) / static_cast<double>(src_sr);
  const size_t expected_size = static_cast<size_t>(std::round(static_cast<double>(size) * ratio));

  std::vector<double> output_double;
  output_double.reserve(expected_size + 1);

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

  // Process in blocks
  double* input_ptr = input_double.data();
  size_t remaining = size;

  while (remaining > 0) {
    int block_len = static_cast<int>(std::min(remaining, static_cast<size_t>(kBlockSize)));

    double* output_ptr = nullptr;
    int output_len = resampler.process(input_ptr, block_len, output_ptr);

    if (output_len > 0 && output_ptr != nullptr) {
      output_double.insert(output_double.end(), output_ptr, output_ptr + output_len);
    }

    input_ptr += block_len;
    remaining -= block_len;
  }

  // Flush exactly the required number of trailing zeros to drain the filter tail.
  std::vector<double> zeros(kBlockSize, 0.0);
  size_t flush_remaining = (required_input > size) ? (required_input - size) : 0;
  while (flush_remaining > 0 && output_double.size() < expected_size) {
    int block_len = static_cast<int>(std::min(flush_remaining, static_cast<size_t>(kBlockSize)));

    double* output_ptr = nullptr;
    int output_len = resampler.process(zeros.data(), block_len, output_ptr);
    if (output_len > 0 && output_ptr != nullptr) {
      output_double.insert(output_double.end(), output_ptr, output_ptr + output_len);
    }
    flush_remaining -= static_cast<size_t>(block_len);
  }

  // Trim to the analytic expected size (drops any extra latency-induced samples).
  if (output_double.size() > expected_size) {
    output_double.resize(expected_size);
  }

  // Convert back to float
  std::vector<float> result(output_double.size());
  for (size_t i = 0; i < output_double.size(); ++i) {
    result[i] = static_cast<float>(output_double[i]);
  }

  return result;
}

Audio resample(const Audio& audio, int target_sr) {
  if (audio.empty()) {
    return Audio();
  }

  if (audio.sample_rate() == target_sr) {
    return audio.to_mono();  // Return a copy
  }

  std::vector<float> resampled =
      resample(audio.data(), audio.size(), audio.sample_rate(), target_sr);
  return Audio::from_vector(std::move(resampled), target_sr);
}

}  // namespace sonare
