#include "effects/decompose.h"

#include <Eigen/Dense>
#include <Eigen/SVD>
#include <algorithm>
#include <cmath>
#include <random>

#include "core/audio.h"
#include "core/spectrum.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/numeric_validation.h"
#include "util/validated.h"

namespace sonare {

namespace {

// Divide-by-amplitude / near-zero guard (== sonare::constants::kAmpEpsilon, 1e-9f).
constexpr float kEps = sonare::constants::kAmpEpsilon;

/// @brief True when every element of a rows x n_frames matrix is finite.
/// @details Callers validate the dimensions as positive first, so the product
///          is well defined here.
bool all_finite(const float* S, int rows, int n_frames) noexcept {
  const size_t total = static_cast<size_t>(rows) * static_cast<size_t>(n_frames);
  for (size_t index = 0; index < total; ++index) {
    if (!std::isfinite(S[index])) return false;
  }
  return true;
}

/// @brief Random non-negative initialisation, seeded for determinism.
void init_random(std::vector<float>& W, std::vector<float>& H, int n_features, int n_components,
                 int n_frames) {
  std::mt19937 rng(42);
  std::uniform_real_distribution<float> dist(0.01f, 1.0f);
  W.assign(static_cast<size_t>(n_features) * n_components, 0.0f);
  H.assign(static_cast<size_t>(n_components) * n_frames, 0.0f);
  for (auto& v : W) v = dist(rng);
  for (auto& v : H) v = dist(rng);
}

/// @brief NNDSVD initialisation (Boutsidis & Gallopoulos 2008).
/// @details Uses the leading singular vectors of S and splits each into its
///          positive and negative parts to seed (W, H). Deterministic.
void init_nndsvd(const float* S, std::vector<float>& W, std::vector<float>& H, int n_features,
                 int n_components, int n_frames) {
  Eigen::MatrixXf X(n_features, n_frames);
  for (int f = 0; f < n_features; ++f) {
    for (int t = 0; t < n_frames; ++t) {
      X(f, t) = S[f * n_frames + t];
    }
  }
  Eigen::JacobiSVD<Eigen::MatrixXf> svd(X, Eigen::ComputeThinU | Eigen::ComputeThinV);
  const Eigen::MatrixXf& U = svd.matrixU();
  const Eigen::MatrixXf& V = svd.matrixV();
  const Eigen::VectorXf sv = svd.singularValues();

  const int rank = std::min({n_components, static_cast<int>(sv.size())});
  W.assign(static_cast<size_t>(n_features) * n_components, 0.0f);
  H.assign(static_cast<size_t>(n_components) * n_frames, 0.0f);

  if (rank == 0) return;

  // Component 0: leading singular vector (take absolute values).
  const float s0 = std::sqrt(std::max(sv[0], 0.0f));
  for (int f = 0; f < n_features; ++f) W[f * n_components + 0] = s0 * std::abs(U(f, 0));
  for (int t = 0; t < n_frames; ++t) H[0 * n_frames + t] = s0 * std::abs(V(t, 0));

  for (int k = 1; k < rank; ++k) {
    Eigen::VectorXf x = U.col(k);
    Eigen::VectorXf y = V.col(k);
    Eigen::VectorXf xp = x.cwiseMax(0.0f);
    Eigen::VectorXf xn = (-x).cwiseMax(0.0f);
    Eigen::VectorXf yp = y.cwiseMax(0.0f);
    Eigen::VectorXf yn = (-y).cwiseMax(0.0f);
    const float xp_n = xp.norm();
    const float xn_n = xn.norm();
    const float yp_n = yp.norm();
    const float yn_n = yn.norm();
    const float mp = xp_n * yp_n;
    const float mn = xn_n * yn_n;
    Eigen::VectorXf u, v;
    float sigma;
    if (mp >= mn) {
      sigma = mp;
      u = (xp_n > 0.0f) ? Eigen::VectorXf(xp / xp_n) : Eigen::VectorXf::Zero(n_features);
      v = (yp_n > 0.0f) ? Eigen::VectorXf(yp / yp_n) : Eigen::VectorXf::Zero(n_frames);
    } else {
      sigma = mn;
      u = (xn_n > 0.0f) ? Eigen::VectorXf(xn / xn_n) : Eigen::VectorXf::Zero(n_features);
      v = (yn_n > 0.0f) ? Eigen::VectorXf(yn / yn_n) : Eigen::VectorXf::Zero(n_frames);
    }
    const float scale = std::sqrt(std::max(sv[k] * sigma, 0.0f));
    for (int f = 0; f < n_features; ++f) W[f * n_components + k] = scale * u[f];
    for (int t = 0; t < n_frames; ++t) H[k * n_frames + t] = scale * v[t];
  }

  // Avoid hard zeros (which the MU updates cannot escape from).
  const float floor_val = kEps;
  for (float& v : W) v = std::max(v, floor_val);
  for (float& v : H) v = std::max(v, floor_val);
}

/// @brief Computes WH = W * H into `out` [n_features x n_frames].
void multiply_WH(const std::vector<float>& W, const std::vector<float>& H, int n_features,
                 int n_components, int n_frames, std::vector<float>& out) {
  out.assign(static_cast<size_t>(n_features) * n_frames, 0.0f);
  for (int f = 0; f < n_features; ++f) {
    for (int t = 0; t < n_frames; ++t) {
      float s = 0.0f;
      for (int c = 0; c < n_components; ++c) {
        s += W[f * n_components + c] * H[c * n_frames + t];
      }
      out[f * n_frames + t] = s;
    }
  }
}

}  // namespace

DecomposeResult decompose(const float* S, int n_features, int n_frames, int n_components,
                          int n_iter, const std::string& solver, float beta,
                          const std::string& init) {
  if (S == nullptr) throw SonareException(ErrorCode::InvalidParameter, "decompose: S is null");
  if (n_features <= 0 || n_frames <= 0 || n_components <= 0) {
    throw SonareException(ErrorCode::InvalidParameter, "decompose: dimensions must be positive");
  }
  if (solver != "mu") {
    throw SonareException(ErrorCode::InvalidParameter,
                          "decompose: only solver=\"mu\" is supported");
  }
  if (!std::isfinite(beta)) {
    throw SonareException(ErrorCode::InvalidParameter, "decompose: beta must be finite");
  }
  if (n_iter < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "decompose: n_iter must be non-negative");
  }
  // A non-finite element factorises into all-NaN W/H that no caller can tell
  // apart from a valid result by shape. Rejected here rather than at each
  // binding so the C ABI, the WASM bindings (which call this directly) and the
  // in-process callers all inherit one rule.
  if (!all_finite(S, n_features, n_frames)) {
    throw SonareException(ErrorCode::InvalidParameter, "decompose: S contains a non-finite value");
  }

  DecomposeResult out;
  if (init == "random") {
    init_random(out.W, out.H, n_features, n_components, n_frames);
  } else if (init == "nndsvd") {
    init_nndsvd(S, out.W, out.H, n_features, n_components, n_frames);
  } else {
    throw SonareException(ErrorCode::InvalidParameter,
                          "decompose: init must be \"random\" or \"nndsvd\"");
  }

  // Generalized beta-divergence multiplicative updates (Fevotte-Idier 2011).
  //   H <- H * (W^T (X (WH)^{beta-2})) / (W^T (WH)^{beta-1})
  //   W <- W * ((X (WH)^{beta-2}) H^T) / ((WH)^{beta-1} H^T)
  // For beta = 2 these reduce to the familiar Frobenius MU updates.
  const float exp_num = beta - 2.0f;  // exponent on WH in the numerator's (X * ...)
  const float exp_den = beta - 1.0f;  // exponent on WH in the denominator

  std::vector<float> WH(static_cast<size_t>(n_features) * n_frames, 0.0f);
  std::vector<float> num_feat(static_cast<size_t>(n_features) * n_frames, 0.0f);  // X * WH^(b-2)
  std::vector<float> den_feat(static_cast<size_t>(n_features) * n_frames, 0.0f);  // WH^(b-1)

  for (int it = 0; it < n_iter; ++it) {
    multiply_WH(out.W, out.H, n_features, n_components, n_frames, WH);

    // Build feature-space numerator/denominator factors.
    for (size_t i = 0; i < WH.size(); ++i) {
      const float wh = WH[i] + kEps;
      const float pow_num = (exp_num == 0.0f) ? 1.0f : std::pow(wh, exp_num);
      const float pow_den = (exp_den == 0.0f) ? 1.0f : std::pow(wh, exp_den);
      num_feat[i] = S[i] * pow_num;
      den_feat[i] = pow_den;
    }

    // H <- H * (W^T num_feat) / (W^T den_feat)
    for (int c = 0; c < n_components; ++c) {
      for (int t = 0; t < n_frames; ++t) {
        float num = 0.0f;
        float den = 0.0f;
        for (int f = 0; f < n_features; ++f) {
          const float w = out.W[f * n_components + c];
          num += w * num_feat[f * n_frames + t];
          den += w * den_feat[f * n_frames + t];
        }
        out.H[c * n_frames + t] *= num / (den + kEps);
      }
    }

    // Rebuild WH and feature-space factors with the updated H.
    multiply_WH(out.W, out.H, n_features, n_components, n_frames, WH);
    for (size_t i = 0; i < WH.size(); ++i) {
      const float wh = WH[i] + kEps;
      const float pow_num = (exp_num == 0.0f) ? 1.0f : std::pow(wh, exp_num);
      const float pow_den = (exp_den == 0.0f) ? 1.0f : std::pow(wh, exp_den);
      num_feat[i] = S[i] * pow_num;
      den_feat[i] = pow_den;
    }

    // W <- W * (num_feat H^T) / (den_feat H^T)
    for (int f = 0; f < n_features; ++f) {
      for (int c = 0; c < n_components; ++c) {
        float num = 0.0f;
        float den = 0.0f;
        for (int t = 0; t < n_frames; ++t) {
          const float h = out.H[c * n_frames + t];
          num += num_feat[f * n_frames + t] * h;
          den += den_feat[f * n_frames + t] * h;
        }
        out.W[f * n_components + c] *= num / (den + kEps);
      }
    }
  }
  return out;
}

std::vector<float> nn_filter(const float* S, int n_features, int n_frames,
                             const std::string& aggregate, int k, int width) {
  if (S == nullptr) throw SonareException(ErrorCode::InvalidParameter, "nn_filter: S is null");
  if (n_features <= 0 || n_frames <= 0) return {};
  if (aggregate != "mean" && aggregate != "median" && aggregate != "min" && aggregate != "max") {
    throw SonareException(ErrorCode::InvalidParameter,
                          "nn_filter: aggregate must be mean/median/min/max");
  }
  // librosa rejects a negative exclusion width (it would silently disable the
  // |i-j| < width time-exclusion band and yield a degenerate self-including
  // filter). Mirror that rejection instead of treating width<0 as width=0.
  if (width < 0) {
    throw SonareException(ErrorCode::InvalidParameter, "nn_filter: width must be non-negative");
  }
  // The most dangerous non-finite case in this file: a NaN poisons the cosine
  // similarities, then disappears in the aggregation, so the caller gets an
  // entirely finite spectrogram whose values have silently shifted, with
  // nothing to detect it by. Rejected here so every surface inherits the rule.
  if (!all_finite(S, n_features, n_frames)) {
    throw SonareException(ErrorCode::InvalidParameter, "nn_filter: S contains a non-finite value");
  }
  if (k <= 0) k = std::min(5, n_frames);

  // Pre-compute column norms for cosine similarity.
  std::vector<float> norms(n_frames, 0.0f);
  for (int t = 0; t < n_frames; ++t) {
    float s = 0.0f;
    for (int f = 0; f < n_features; ++f) {
      const float v = S[f * n_frames + t];
      s += v * v;
    }
    norms[t] = std::sqrt(s);
  }

  // Replicate librosa.segment.recurrence_matrix in mode="connectivity":
  //   1. For each row i, get the top (k + 2*width) cosine neighbours,
  //      including self.
  //   2. Zero the |i-j| < width diagonal band (drops self and width-1
  //      neighbours on each side).
  //   3. Among the remaining neighbours, keep the `k` entries with the
  //      smallest *column indices* (librosa uses a stable argsort of an all-
  //      ones connectivity row, which boils down to column-index ordering).
  //   4. Transpose the matrix so column t lists the points that selected t.
  // librosa's nn_filter then reads this column-major view to perform non-local
  // means: for output column t, aggregate every column i for which R[i, t] is
  // set. We accumulate `selectors_for[t] = {i : R[i, t] != 0}` directly.
  std::vector<std::vector<int>> selectors_for(n_frames);
  std::vector<std::pair<float, int>> sims;
  sims.reserve(static_cast<size_t>(n_frames));
  const int n_neighbors = std::min(n_frames - 1, k + 2 * width);
  for (int i = 0; i < n_frames; ++i) {
    sims.clear();
    for (int j = 0; j < n_frames; ++j) {
      if (j == i) continue;  // sklearn excludes self automatically
      float dot = 0.0f;
      for (int f = 0; f < n_features; ++f) {
        dot += S[f * n_frames + i] * S[f * n_frames + j];
      }
      const float sim = (norms[i] > 0.0f && norms[j] > 0.0f) ? dot / (norms[i] * norms[j]) : 0.0f;
      sims.push_back({sim, j});
    }
    if (sims.empty()) continue;
    // (1) Top (k+2*width) by cosine similarity (largest similarity = smallest
    // cosine distance).
    const int nn = std::min(n_neighbors, static_cast<int>(sims.size()));
    std::partial_sort(sims.begin(), sims.begin() + nn, sims.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
    // (2) Drop those falling inside |i-j| < width (self already excluded).
    std::vector<int> kept;
    kept.reserve(static_cast<size_t>(nn));
    for (int q = 0; q < nn; ++q) {
      const int j = sims[q].second;
      if (std::abs(i - j) < width) continue;
      kept.push_back(j);
    }
    // (3) Among the survivors, keep the `k` smallest column indices —
    // librosa's stable argsort of an all-ones connectivity row picks the
    // lowest-index columns first.
    std::sort(kept.begin(), kept.end());
    const int kk = std::min<int>(k, static_cast<int>(kept.size()));
    // After the final `.T`, column i of the recurrence matrix lists i's k-NN
    // — exactly what librosa's `nn_filter` aggregates when emitting output
    // column i.
    auto& dst = selectors_for[i];
    for (int q = 0; q < kk; ++q) dst.push_back(kept[q]);
  }

  std::vector<float> out(static_cast<size_t>(n_features) * n_frames, 0.0f);
  for (int t = 0; t < n_frames; ++t) {
    const auto& selectors = selectors_for[t];
    if (selectors.empty()) {
      for (int f = 0; f < n_features; ++f) out[f * n_frames + t] = S[f * n_frames + t];
      continue;
    }
    if (aggregate == "median") {
      for (int f = 0; f < n_features; ++f) {
        std::vector<float> vals(selectors.size());
        for (size_t q = 0; q < selectors.size(); ++q) vals[q] = S[f * n_frames + selectors[q]];
        const auto mid = vals.begin() + vals.size() / 2;
        std::nth_element(vals.begin(), mid, vals.end());
        float median = *mid;
        // numpy.median (used by librosa.decompose.nn_filter for aggregate=median)
        // averages the two central elements for an even count; nth_element alone
        // returns only the upper one.
        if ((vals.size() % 2) == 0) {
          const float lower = *std::max_element(vals.begin(), mid);
          median = 0.5f * (lower + median);
        }
        out[f * n_frames + t] = median;
      }
    } else if (aggregate == "min") {
      for (int f = 0; f < n_features; ++f) {
        float m = std::numeric_limits<float>::infinity();
        for (int i : selectors) m = std::min(m, S[f * n_frames + i]);
        out[f * n_frames + t] = m;
      }
    } else if (aggregate == "max") {
      for (int f = 0; f < n_features; ++f) {
        float m = -std::numeric_limits<float>::infinity();
        for (int i : selectors) m = std::max(m, S[f * n_frames + i]);
        out[f * n_frames + t] = m;
      }
    } else {  // "mean"
      const float inv_count = 1.0f / static_cast<float>(selectors.size());
      for (int f = 0; f < n_features; ++f) {
        float s = 0.0f;
        for (int i : selectors) s += S[f * n_frames + i];
        out[f * n_frames + t] = s * inv_count;
      }
    }
  }
  return out;
}

void validate_config(const DecomposeStemsConfig& config) {
  SONARE_CHECK_MSG(config.n_components > 0, ErrorCode::InvalidParameter,
                   "DecomposeStemsConfig: nComponents must be positive");
  SONARE_CHECK_MSG(config.n_fft > 0, ErrorCode::InvalidParameter,
                   "DecomposeStemsConfig: nFft must be positive");
  SONARE_CHECK_MSG(config.hop_length > 0, ErrorCode::InvalidParameter,
                   "DecomposeStemsConfig: hopLength must be positive");
  SONARE_CHECK_MSG(config.n_iter > 0, ErrorCode::InvalidParameter,
                   "DecomposeStemsConfig: nIter must be positive");
  SONARE_CHECK_MSG(numeric::finite(config.beta), ErrorCode::InvalidParameter,
                   "DecomposeStemsConfig: beta must be finite");
  SONARE_CHECK_MSG(numeric::finite(config.mask_power) && config.mask_power >= 1.0f,
                   ErrorCode::InvalidParameter,
                   "DecomposeStemsConfig: maskPower must be finite and at least 1");
}

DecomposeStemsResult decompose_stems(const float* samples, std::size_t n, int sample_rate,
                                     const DecomposeStemsConfig& config) {
  const DecomposeStemsConfig checked = Validated<DecomposeStemsConfig>::make(config).get();
  SONARE_CHECK(samples != nullptr && n > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(sample_rate > 0, ErrorCode::InvalidParameter);

  const Audio audio = Audio::from_buffer(samples, n, sample_rate);
  const Spectrogram spectrum =
      Spectrogram::compute(audio, make_stft_config(checked.n_fft, checked.hop_length));
  const int n_bins = spectrum.n_bins();
  const int n_frames = spectrum.n_frames();
  SONARE_CHECK(n_bins > 0 && n_frames > 0, ErrorCode::InvalidParameter);

  const std::vector<float>& magnitude = spectrum.magnitude();
  DecomposeResult factors = decompose(magnitude.data(), n_bins, n_frames, checked.n_components,
                                      checked.n_iter, "mu", checked.beta, checked.init);

  const int k = checked.n_components;
  const std::complex<float>* source = spectrum.complex_data();
  const std::size_t cells = static_cast<std::size_t>(n_bins) * static_cast<std::size_t>(n_frames);

  // Model magnitude per component, then the shared denominator. Computed once
  // for the whole spectrogram rather than per component so the k reconstructions
  // cost one pass over W*H instead of k.
  std::vector<float> model(cells * static_cast<std::size_t>(k), 0.0f);
  std::vector<float> denominator(cells, 0.0f);
  for (int component = 0; component < k; ++component) {
    float* plane = model.data() + static_cast<std::size_t>(component) * cells;
    for (int bin = 0; bin < n_bins; ++bin) {
      const float w = factors.W[static_cast<std::size_t>(bin) * static_cast<std::size_t>(k) +
                                static_cast<std::size_t>(component)];
      for (int frame = 0; frame < n_frames; ++frame) {
        const float h =
            factors.H[static_cast<std::size_t>(component) * static_cast<std::size_t>(n_frames) +
                      static_cast<std::size_t>(frame)];
        const float value = std::max(w * h, 0.0f);
        const float weighted =
            checked.mask_power == 1.0f ? value : std::pow(value, checked.mask_power);
        const std::size_t cell =
            static_cast<std::size_t>(bin) * static_cast<std::size_t>(n_frames) +
            static_cast<std::size_t>(frame);
        plane[cell] = weighted;
        denominator[cell] += weighted;
      }
    }
  }

  DecomposeStemsResult out;
  out.W = std::move(factors.W);
  out.H = std::move(factors.H);
  out.components.reserve(static_cast<std::size_t>(k));
  std::vector<std::complex<float>> masked(cells);
  for (int component = 0; component < k; ++component) {
    const float* plane = model.data() + static_cast<std::size_t>(component) * cells;
    for (std::size_t cell = 0; cell < cells; ++cell) {
      // A cell the model gives no energy to is dropped from every component
      // rather than split evenly, so the masks never manufacture signal where
      // the factorisation has none.
      const float total = denominator[cell];
      const float mask = total > kEps ? plane[cell] / total : 0.0f;
      masked[cell] = source[cell] * mask;
    }
    const Spectrogram component_spectrum =
        Spectrogram::from_complex(masked.data(), n_bins, n_frames, checked.n_fft,
                                  checked.hop_length, sample_rate, spectrum.window());
    const Audio rendered = component_spectrum.to_audio(static_cast<int>(n));
    out.components.emplace_back(rendered.data(), rendered.data() + rendered.size());
  }
  return out;
}

}  // namespace sonare
