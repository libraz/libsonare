#include "effects/decompose.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

namespace sonare {

DecomposeResult decompose(const float* S, int n_features, int n_frames, int n_components,
                          int n_iter, const std::string& solver) {
  if (S == nullptr) throw std::invalid_argument("decompose: S is null");
  if (n_features <= 0 || n_frames <= 0 || n_components <= 0) {
    throw std::invalid_argument("decompose: dimensions must be positive");
  }
  if (solver != "mu") {
    throw std::invalid_argument("decompose: only solver=\"mu\" is supported");
  }

  std::mt19937 rng(42);
  std::uniform_real_distribution<float> dist(0.01f, 1.0f);

  DecomposeResult out;
  out.W.assign(static_cast<size_t>(n_features) * n_components, 0.0f);
  out.H.assign(static_cast<size_t>(n_components) * n_frames, 0.0f);
  for (auto& v : out.W) v = dist(rng);
  for (auto& v : out.H) v = dist(rng);
  const float kEps = 1e-9f;

  // Iterative multiplicative updates (Lee-Seung Frobenius cost).
  for (int it = 0; it < n_iter; ++it) {
    // Update H: H *= (W^T S) / (W^T W H + eps)
    std::vector<float> WtS(static_cast<size_t>(n_components) * n_frames, 0.0f);
    for (int c = 0; c < n_components; ++c) {
      for (int t = 0; t < n_frames; ++t) {
        float s = 0.0f;
        for (int f = 0; f < n_features; ++f) {
          s += out.W[f * n_components + c] * S[f * n_frames + t];
        }
        WtS[c * n_frames + t] = s;
      }
    }
    std::vector<float> WtWH(static_cast<size_t>(n_components) * n_frames, 0.0f);
    for (int c = 0; c < n_components; ++c) {
      for (int t = 0; t < n_frames; ++t) {
        float s = 0.0f;
        for (int cc = 0; cc < n_components; ++cc) {
          float WtW = 0.0f;
          for (int f = 0; f < n_features; ++f) {
            WtW += out.W[f * n_components + c] * out.W[f * n_components + cc];
          }
          s += WtW * out.H[cc * n_frames + t];
        }
        WtWH[c * n_frames + t] = s;
      }
    }
    for (size_t i = 0; i < out.H.size(); ++i) {
      out.H[i] *= WtS[i] / (WtWH[i] + kEps);
    }

    // Update W: W *= (S H^T) / (W H H^T + eps)
    std::vector<float> SHt(static_cast<size_t>(n_features) * n_components, 0.0f);
    for (int f = 0; f < n_features; ++f) {
      for (int c = 0; c < n_components; ++c) {
        float s = 0.0f;
        for (int t = 0; t < n_frames; ++t) {
          s += S[f * n_frames + t] * out.H[c * n_frames + t];
        }
        SHt[f * n_components + c] = s;
      }
    }
    std::vector<float> WHHt(static_cast<size_t>(n_features) * n_components, 0.0f);
    for (int f = 0; f < n_features; ++f) {
      for (int c = 0; c < n_components; ++c) {
        float s = 0.0f;
        for (int cc = 0; cc < n_components; ++cc) {
          float HHt = 0.0f;
          for (int t = 0; t < n_frames; ++t) {
            HHt += out.H[c * n_frames + t] * out.H[cc * n_frames + t];
          }
          s += out.W[f * n_components + cc] * HHt;
        }
        WHHt[f * n_components + c] = s;
      }
    }
    for (size_t i = 0; i < out.W.size(); ++i) {
      out.W[i] *= SHt[i] / (WHHt[i] + kEps);
    }
  }
  return out;
}

std::vector<float> nn_filter(const float* S, int n_features, int n_frames,
                             const std::string& aggregate, int k, int width) {
  if (S == nullptr) throw std::invalid_argument("nn_filter: S is null");
  if (n_features <= 0 || n_frames <= 0) return {};
  if (k <= 0) k = std::min(5, n_frames);

  // Pre-compute column norms for cosine similarity.
  std::vector<float> norms(n_frames, 0.0f);
  for (int t = 0; t < n_frames; ++t) {
    float s = 0.0f;
    for (int f = 0; f < n_features; ++f) {
      float v = S[f * n_frames + t];
      s += v * v;
    }
    norms[t] = std::sqrt(s);
  }

  std::vector<float> out(static_cast<size_t>(n_features) * n_frames, 0.0f);
  for (int t = 0; t < n_frames; ++t) {
    // Compute cosine similarity to every other frame.
    std::vector<std::pair<float, int>> sims;
    sims.reserve(n_frames);
    for (int u = 0; u < n_frames; ++u) {
      if (std::abs(u - t) < width) continue;
      float dot = 0.0f;
      for (int f = 0; f < n_features; ++f) {
        dot += S[f * n_frames + t] * S[f * n_frames + u];
      }
      float sim = (norms[t] > 0.0f && norms[u] > 0.0f) ? dot / (norms[t] * norms[u]) : 0.0f;
      sims.push_back({sim, u});
    }
    int kk = std::min<int>(k, static_cast<int>(sims.size()));
    if (kk == 0) {
      // No usable neighbours -> fall back to the frame itself.
      for (int f = 0; f < n_features; ++f) {
        out[f * n_frames + t] = S[f * n_frames + t];
      }
      continue;
    }
    std::partial_sort(sims.begin(), sims.begin() + kk, sims.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
    // Aggregate.
    if (aggregate == "median") {
      for (int f = 0; f < n_features; ++f) {
        std::vector<float> vals(kk);
        for (int q = 0; q < kk; ++q) {
          vals[q] = S[f * n_frames + sims[q].second];
        }
        std::nth_element(vals.begin(), vals.begin() + kk / 2, vals.end());
        out[f * n_frames + t] = vals[kk / 2];
      }
    } else {
      for (int f = 0; f < n_features; ++f) {
        float s = 0.0f;
        for (int q = 0; q < kk; ++q) {
          s += S[f * n_frames + sims[q].second];
        }
        out[f * n_frames + t] = s / kk;
      }
    }
  }
  return out;
}

}  // namespace sonare
