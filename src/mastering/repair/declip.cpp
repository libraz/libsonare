#include "mastering/repair/declip.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

#include "util/lpc.h"

namespace sonare::mastering::repair {
namespace {

float cubic_hermite(float y0, float y1, float y2, float y3, float t) {
  const float m1 = 0.5f * (y2 - y0);
  const float m2 = 0.5f * (y3 - y1);
  const float t2 = t * t;
  const float t3 = t2 * t;
  return (2.0f * t3 - 3.0f * t2 + 1.0f) * y1 + (t3 - 2.0f * t2 + t) * m1 +
         (-2.0f * t3 + 3.0f * t2) * y2 + (t3 - t2) * m2;
}

bool has_cubic_context(size_t start, size_t end, size_t size) {
  return start >= 2 && end + 1 < size;
}

bool can_use_lpc(size_t size, int order) {
  return order > 0 && size > static_cast<size_t>(order + 2);
}

float interpolate_fallback(const std::vector<float>& samples, size_t start, size_t end, size_t j) {
  const float left = start > 0 ? samples[start - 1] : (end < samples.size() ? samples[end] : 0.0f);
  const float right = end < samples.size() ? samples[end] : left;
  const size_t length = end - start;
  const float t = static_cast<float>(j - start + 1) / static_cast<float>(length + 1);
  return has_cubic_context(start, end, samples.size())
             ? cubic_hermite(samples[start - 2], left, right, samples[end + 1], t)
             : left + (right - left) * t;
}

/// @brief LPC-based reconstruction of a single clipped region.
///
/// Minimal Janssen-style improvement over the previous implementation: Burg
/// AR(p) estimation is fed **only the known (unclipped) samples** rather than
/// the whole signal (which used to include the in-progress interpolated values
/// and steadily contaminated the spectral model across iterations). Burg sees
/// the concatenation of every non-clipped run in the signal; the small
/// discontinuities at the join points are far less harmful to the spectral
/// envelope than the run-away contamination of the original loop.
///
/// The forward AR prediction used to fill the gap, the cubic/linear fallback,
/// and the `lpc_blend` weighting all behave exactly as before.
///
/// TODO(declip): replace the open-loop forward AR predictor with a full
/// Janssen linear-system solver (minimise ||A x||^2 over the unknown samples
/// via the closed-form normal equations). The current code is the conservative
/// minimal fix for the spectral-pollution bug; the full solver requires more
/// careful boundary handling and is tracked as a follow-up task.
void reconstruct_region_lpc(std::vector<float>& samples, size_t start, size_t end,
                            const DeclipConfig& config) {
  for (size_t j = start; j < end; ++j) {
    samples[j] = interpolate_fallback(samples, start, end, j);
  }

  if (!can_use_lpc(samples.size(), config.lpc_order)) return;
  const int order =
      std::min(config.lpc_order, static_cast<int>(std::max<size_t>(1, samples.size() / 4)));
  if (!can_use_lpc(samples.size(), order)) return;

  // Build Burg's training set from samples *outside* every clipped run, using
  // the same threshold the outer loop applies. This keeps the model anchored
  // to clean data and stops the original "train on filled-in values" bug.
  std::vector<float> known;
  known.reserve(samples.size());
  for (size_t i = 0; i < samples.size(); ++i) {
    if (std::abs(samples[i]) < config.clip_threshold) {
      known.push_back(samples[i]);
    }
  }
  if (!can_use_lpc(known.size(), order)) return;

  for (int iteration = 0; iteration < std::max(config.iterations, 1); ++iteration) {
    const auto model = sonare::lpc_burg(known.data(), known.size(), order);
    for (size_t j = start; j < end; ++j) {
      double predicted = 0.0;
      const size_t max_k = std::min(model.ar.size() - 1, j);
      for (size_t k = 1; k <= max_k; ++k) {
        predicted -= static_cast<double>(model.ar[k]) * samples[j - k];
      }
      const float fallback = interpolate_fallback(samples, start, end, j);
      samples[j] =
          config.lpc_blend * static_cast<float>(predicted) + (1.0f - config.lpc_blend) * fallback;
    }
  }
}

}  // namespace

Audio declip(const Audio& audio, const DeclipConfig& config) {
  if (audio.empty()) throw std::invalid_argument("audio must not be empty");
  if (!(config.clip_threshold > 0.0f && config.clip_threshold <= 1.0f) || config.lpc_order < 0 ||
      config.iterations < 0) {
    throw std::invalid_argument("invalid declip threshold");
  }
  std::vector<float> samples(audio.data(), audio.data() + audio.size());
  size_t i = 0;
  while (i < samples.size()) {
    if (std::abs(samples[i]) < config.clip_threshold) {
      ++i;
      continue;
    }

    const size_t start = i;
    while (i < samples.size() && std::abs(samples[i]) >= config.clip_threshold) ++i;
    const size_t end = i;
    reconstruct_region_lpc(samples, start, end, config);
  }
  return Audio::from_vector(std::move(samples), audio.sample_rate());
}

}  // namespace sonare::mastering::repair
