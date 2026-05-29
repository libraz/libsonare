#include "mastering/repair/declick.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

#include "util/lpc.h"

namespace sonare::mastering::repair {
namespace {

bool can_use_lpc(size_t size, int order) {
  return order > 0 && size > static_cast<size_t>(order + 2);
}

std::vector<bool> detect_clicks(const std::vector<float>& samples, const DeclickConfig& config) {
  std::vector<bool> mask(samples.size(), false);
  for (size_t i = 1; i + 1 < samples.size(); ++i) {
    mask[i] = std::abs(samples[i]) >= config.threshold;
  }

  if (!can_use_lpc(samples.size(), config.lpc_order)) return mask;

  const int order =
      std::min(config.lpc_order, static_cast<int>(std::max<size_t>(1, samples.size() / 4)));
  if (!can_use_lpc(samples.size(), order)) return mask;

  const auto model = sonare::lpc_burg(samples.data(), samples.size(), order);
  const auto residual = sonare::lpc_residual(samples.data(), samples.size(), model);
  constexpr size_t kRadius = 8;
  for (size_t i = 1; i + 1 < samples.size(); ++i) {
    const size_t begin = i > kRadius ? i - kRadius : 0;
    const size_t end = std::min(samples.size(), i + kRadius + 1);
    double residual_sum = 0.0;
    double sample_sum = 0.0;
    size_t count = 0;
    for (size_t j = begin; j < end; ++j) {
      if (j == i) continue;
      residual_sum += std::abs(residual[j]);
      sample_sum += std::abs(samples[j]);
      ++count;
    }
    const float local_residual =
        count == 0 ? 1.0e-6f : static_cast<float>(residual_sum / static_cast<double>(count));
    const float local_sample =
        count == 0 ? 1.0e-6f : static_cast<float>(sample_sum / static_cast<double>(count));
    if (std::abs(residual[i]) > local_residual * config.residual_ratio &&
        std::abs(samples[i]) > local_sample * 1.5f) {
      mask[i] = true;
    }
  }
  return mask;
}

void interpolate_region(std::vector<float>& output, const std::vector<float>& samples, size_t start,
                        size_t end, int lpc_order) {
  const size_t length = end - start;
  const float left = output[start - 1];
  const float right = samples[end];
  for (size_t j = start; j < end; ++j) {
    const float t = static_cast<float>(j - start + 1) / static_cast<float>(length + 1);
    output[j] = left + (right - left) * t;
  }

  if (!can_use_lpc(output.size(), lpc_order)) return;
  const int order = std::min(lpc_order, static_cast<int>(std::max<size_t>(1, output.size() / 4)));
  if (!can_use_lpc(output.size(), order)) return;

  const auto model = sonare::lpc_burg(output.data(), output.size(), order);
  for (size_t j = start; j < end; ++j) {
    double predicted = 0.0;
    const size_t max_k = std::min(model.ar.size() - 1, j);
    for (size_t k = 1; k <= max_k; ++k) {
      predicted -= static_cast<double>(model.ar[k]) * output[j - k];
    }
    output[j] = static_cast<float>(predicted);
  }
}

}  // namespace

Audio declick(const Audio& audio, const DeclickConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (!(config.threshold > 0.0f) || !(config.neighbor_ratio > 0.0f) ||
      config.max_click_samples == 0 || config.lpc_order < 0 || !(config.residual_ratio > 0.0f)) {
    throw std::invalid_argument("invalid declick configuration");
  }
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  std::vector<float> output = samples;
  const std::vector<bool> click_mask = detect_clicks(samples, config);
  size_t i = 1;
  while (i + 1 < samples.size()) {
    if (!click_mask[i]) {
      ++i;
      continue;
    }

    const size_t start = i;
    float peak = 0.0f;
    while (i + 1 < samples.size() && click_mask[i]) {
      peak = std::max(peak, std::abs(samples[i]));
      ++i;
    }
    const size_t end = i;
    const size_t length = end - start;
    const float local = std::max({std::abs(samples[start - 1]), std::abs(samples[end]), 1e-6f});
    if (length <= config.max_click_samples && peak > local * config.neighbor_ratio) {
      interpolate_region(output, samples, start, end, config.lpc_order);
    }
  }
  return Audio::from_vector(std::move(output), audio.sample_rate());
}

}  // namespace sonare::mastering::repair
