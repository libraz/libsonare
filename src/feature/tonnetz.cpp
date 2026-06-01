#include "feature/tonnetz.h"

#include <cmath>

#include "util/constants.h"
#include "util/exception.h"

namespace sonare {

namespace {

constexpr int kTonnetzDims = 6;
constexpr int kChromaBins = 12;

// Pre-computed Tonnetz transformation matrix [6 x 12].
// Matches librosa.feature.tonnetz: rows 0..1 = perfect fifth (period 12*7),
// rows 2..3 = minor third (period 12*3), rows 4..5 = major third (period 12*4).
// Scaling factors (r0, r1, r2) follow the original Harte 2006 paper.
std::vector<float> build_tonnetz_matrix() {
  using sonare::constants::kPi;
  const float r0 = 1.0f;
  const float r1 = 1.0f;
  const float r2 = 0.5f;
  const float kSeven = 7.0f / 6.0f;
  const float kThree = 3.0f / 2.0f;
  const float kTwo = 2.0f / 3.0f;

  std::vector<float> m(static_cast<size_t>(kTonnetzDims) * kChromaBins);
  for (int k = 0; k < kChromaBins; ++k) {
    float kf = static_cast<float>(k);
    m[0 * kChromaBins + k] = r0 * std::sin(kf * kSeven * kPi);
    m[1 * kChromaBins + k] = r0 * std::cos(kf * kSeven * kPi);
    m[2 * kChromaBins + k] = r1 * std::sin(kf * kThree * kPi);
    m[3 * kChromaBins + k] = r1 * std::cos(kf * kThree * kPi);
    m[4 * kChromaBins + k] = r2 * std::sin(kf * kTwo * kPi);
    m[5 * kChromaBins + k] = r2 * std::cos(kf * kTwo * kPi);
  }
  return m;
}

}  // namespace

std::vector<float> tonnetz(const Chroma& chroma) {
  return tonnetz(chroma.data(), chroma.n_chroma(), chroma.n_frames());
}

std::vector<float> tonnetz(const float* chromagram, int n_chroma, int n_frames) {
  if (chromagram == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "tonnetz: chromagram is null");
  }
  if (n_chroma != kChromaBins) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "tonnetz: only 12-bin chromagrams are supported");
  }
  if (n_frames <= 0) {
    return {};
  }

  static const std::vector<float> kMatrix = build_tonnetz_matrix();

  // L1-normalize each frame of the chromagram (matches librosa behavior).
  std::vector<float> norm(static_cast<size_t>(n_chroma) * n_frames);
  for (int t = 0; t < n_frames; ++t) {
    float sum = 0.0f;
    for (int k = 0; k < n_chroma; ++k) {
      sum += std::abs(chromagram[k * n_frames + t]);
    }
    if (sum > 0.0f) {
      for (int k = 0; k < n_chroma; ++k) {
        norm[k * n_frames + t] = chromagram[k * n_frames + t] / sum;
      }
    } else {
      for (int k = 0; k < n_chroma; ++k) {
        norm[k * n_frames + t] = 0.0f;
      }
    }
  }

  std::vector<float> out(static_cast<size_t>(kTonnetzDims) * n_frames, 0.0f);
  for (int d = 0; d < kTonnetzDims; ++d) {
    for (int t = 0; t < n_frames; ++t) {
      float acc = 0.0f;
      for (int k = 0; k < kChromaBins; ++k) {
        acc += kMatrix[d * kChromaBins + k] * norm[k * n_frames + t];
      }
      out[d * n_frames + t] = acc;
    }
  }
  return out;
}

}  // namespace sonare
