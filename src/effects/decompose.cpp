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

using sonare::numeric::all_finite;

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
///          positive and negative parts to seed (W, H). No RNG, and solved in double
///          so the seed does not depend on the target's summation order: in single
///          precision the trailing vectors sit at the noise floor and one input seeds
///          differently on wasm32 and arm64. Only the seed is double; (W, H) and the
///          multiplicative updates stay float. Components past the input's effective
///          rank are degenerate at any precision and reproduce on neither.
void init_nndsvd(const float* S, std::vector<float>& W, std::vector<float>& H, int n_features,
                 int n_components, int n_frames) {
  Eigen::MatrixXd X(n_features, n_frames);
  for (int f = 0; f < n_features; ++f) {
    for (int t = 0; t < n_frames; ++t) {
      X(f, t) = S[f * n_frames + t];
    }
  }
  Eigen::JacobiSVD<Eigen::MatrixXd> svd(X, Eigen::ComputeThinU | Eigen::ComputeThinV);
  const Eigen::MatrixXd& U = svd.matrixU();
  const Eigen::MatrixXd& V = svd.matrixV();
  const Eigen::VectorXd sv = svd.singularValues();

  const int rank = std::min({n_components, static_cast<int>(sv.size())});
  W.assign(static_cast<size_t>(n_features) * n_components, 0.0f);
  H.assign(static_cast<size_t>(n_components) * n_frames, 0.0f);

  if (rank == 0) return;

  // Component 0: leading singular vector (take absolute values).
  const double s0 = std::sqrt(std::max(sv[0], 0.0));
  for (int f = 0; f < n_features; ++f) {
    W[f * n_components + 0] = static_cast<float>(s0 * std::abs(U(f, 0)));
  }
  for (int t = 0; t < n_frames; ++t) {
    H[0 * n_frames + t] = static_cast<float>(s0 * std::abs(V(t, 0)));
  }

  for (int k = 1; k < rank; ++k) {
    Eigen::VectorXd x = U.col(k);
    Eigen::VectorXd y = V.col(k);
    Eigen::VectorXd xp = x.cwiseMax(0.0);
    Eigen::VectorXd xn = (-x).cwiseMax(0.0);
    Eigen::VectorXd yp = y.cwiseMax(0.0);
    Eigen::VectorXd yn = (-y).cwiseMax(0.0);
    const double xp_n = xp.norm();
    const double xn_n = xn.norm();
    const double yp_n = yp.norm();
    const double yn_n = yn.norm();
    const double mp = xp_n * yp_n;
    const double mn = xn_n * yn_n;
    Eigen::VectorXd u, v;
    double sigma;
    if (mp >= mn) {
      sigma = mp;
      u = (xp_n > 0.0) ? Eigen::VectorXd(xp / xp_n) : Eigen::VectorXd::Zero(n_features);
      v = (yp_n > 0.0) ? Eigen::VectorXd(yp / yp_n) : Eigen::VectorXd::Zero(n_frames);
    } else {
      sigma = mn;
      u = (xn_n > 0.0) ? Eigen::VectorXd(xn / xn_n) : Eigen::VectorXd::Zero(n_features);
      v = (yn_n > 0.0) ? Eigen::VectorXd(yn / yn_n) : Eigen::VectorXd::Zero(n_frames);
    }
    const double scale = std::sqrt(std::max(sv[k] * sigma, 0.0));
    for (int f = 0; f < n_features; ++f) {
      W[f * n_components + k] = static_cast<float>(scale * u[f]);
    }
    for (int t = 0; t < n_frames; ++t) {
      H[k * n_frames + t] = static_cast<float>(scale * v[t]);
    }
  }

  // Avoid hard zeros (which the MU updates cannot escape from).
  const float floor_val = kEps;
  for (float& v : W) v = std::max(v, floor_val);
  for (float& v : H) v = std::max(v, floor_val);
}

/// @brief Computes WH = W * H into `out` [n_features x n_frames].
/// @details Component-major inner pass: a component is one contiguous row of H and the
///          output row is written contiguously. Each output cell takes its terms in
///          component order, which the multiplicative updates need -- a last-bit difference
///          here compounds over n_iter.
void multiply_WH(const std::vector<float>& W, const std::vector<float>& H, int n_features,
                 int n_components, int n_frames, std::vector<float>& out) {
  out.assign(static_cast<size_t>(n_features) * n_frames, 0.0f);
  for (int f = 0; f < n_features; ++f) {
    float* out_row = out.data() + static_cast<size_t>(f) * n_frames;
    for (int c = 0; c < n_components; ++c) {
      const float w = W[f * n_components + c];
      const float* h_row = H.data() + static_cast<size_t>(c) * n_frames;
      for (int t = 0; t < n_frames; ++t) {
        out_row[t] += w * h_row[t];
      }
    }
  }
}

/// @brief Model magnitude per component, then the shared denominator.
/// @details Shared by @ref decompose_stems_linked's every caller (mono goes
///          through it with @p channel_count == 1): one pass over W*H per
///          component, computed once for the whole spectrogram rather than
///          per channel, so C channels cost the same model build as one.
void build_component_model(const DecomposeResult& factors, int n_bins, int n_frames, int k,
                           float mask_power, std::vector<float>& model,
                           std::vector<float>& denominator) {
  const std::size_t cells = static_cast<std::size_t>(n_bins) * static_cast<std::size_t>(n_frames);
  model.assign(cells * static_cast<std::size_t>(k), 0.0f);
  denominator.assign(cells, 0.0f);
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
        const float weighted = mask_power == 1.0f ? value : std::pow(value, mask_power);
        const std::size_t cell =
            static_cast<std::size_t>(bin) * static_cast<std::size_t>(n_frames) +
            static_cast<std::size_t>(frame);
        plane[cell] = weighted;
        denominator[cell] += weighted;
      }
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

  // Per-frame accumulators for the H update, held across iterations rather than per component.
  std::vector<float> h_num(static_cast<size_t>(n_frames), 0.0f);
  std::vector<float> h_den(static_cast<size_t>(n_frames), 0.0f);

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
    // Feature-major with per-frame accumulators: num_feat/den_feat are row-major
    // [n_features x n_frames], so a feature is one contiguous row. Each (c, t) accumulator
    // still takes its terms in feature order, so H is unchanged bit for bit -- which the
    // multiplicative updates need, since a last-bit difference here compounds over n_iter.
    for (int c = 0; c < n_components; ++c) {
      std::fill(h_num.begin(), h_num.end(), 0.0f);
      std::fill(h_den.begin(), h_den.end(), 0.0f);
      for (int f = 0; f < n_features; ++f) {
        const float w = out.W[f * n_components + c];
        const float* num_row = num_feat.data() + static_cast<size_t>(f) * n_frames;
        const float* den_row = den_feat.data() + static_cast<size_t>(f) * n_frames;
        for (int t = 0; t < n_frames; ++t) {
          h_num[t] += w * num_row[t];
          h_den[t] += w * den_row[t];
        }
      }
      for (int t = 0; t < n_frames; ++t) {
        out.H[c * n_frames + t] *= h_num[t] / (h_den[t] + kEps);
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

  // Pre-compute column norms for cosine similarity. Feature-major with per-frame
  // accumulators: S is row-major [n_features x n_frames], so a feature is one contiguous
  // row and each frame still sums features in order, leaving the norm bit for bit the same.
  std::vector<float> norms(n_frames, 0.0f);
  for (int f = 0; f < n_features; ++f) {
    const float* row = S + static_cast<size_t>(f) * n_frames;
    for (int t = 0; t < n_frames; ++t) {
      norms[t] += row[t] * row[t];
    }
  }
  for (int t = 0; t < n_frames; ++t) norms[t] = std::sqrt(norms[t]);

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
  std::vector<float> dots(static_cast<size_t>(n_frames), 0.0f);
  const int n_neighbors = std::min(n_frames - 1, k + 2 * width);
  for (int i = 0; i < n_frames; ++i) {
    sims.clear();
    // One feature-major pass builds the whole row of dot products instead of walking two
    // strided columns per pair. Each dots[j] still accumulates over features in order, so the
    // similarities -- and the neighbour ranking they drive -- are unchanged bit for bit.
    // dots[i] is computed and never read; self is excluded below as before.
    std::fill(dots.begin(), dots.end(), 0.0f);
    for (int f = 0; f < n_features; ++f) {
      const float* row = S + static_cast<size_t>(f) * n_frames;
      const float si = row[i];
      for (int j = 0; j < n_frames; ++j) {
        dots[j] += si * row[j];
      }
    }
    const float norm_i = norms[i];
    for (int j = 0; j < n_frames; ++j) {
      if (j == i) continue;  // sklearn excludes self automatically
      const float sim = (norm_i > 0.0f && norms[j] > 0.0f) ? dots[j] / (norm_i * norms[j]) : 0.0f;
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
  // Reused across cells; the gather below overwrites every element.
  std::vector<float> vals;
  // Feature-major: out is written contiguously and every gather stays inside one row of S.
  // Each cell still reads its selectors in ascending order, so the aggregates are unchanged.
  const auto emit = [&](auto cell) {
    for (int f = 0; f < n_features; ++f) {
      const size_t base = static_cast<size_t>(f) * n_frames;
      const float* row = S + base;
      for (int t = 0; t < n_frames; ++t) {
        out[base + t] = selectors_for[t].empty() ? row[t] : cell(row, t);
      }
    }
  };
  // Dispatched once: inside the nest the comparison would run per output cell.
  if (aggregate == "median") {
    emit([&](const float* row, int t) {
      const std::vector<int>& selectors = selectors_for[t];
      vals.resize(selectors.size());
      for (size_t q = 0; q < selectors.size(); ++q) vals[q] = row[selectors[q]];
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
      return median;
    });
  } else if (aggregate == "min") {
    emit([&](const float* row, int t) {
      float m = std::numeric_limits<float>::infinity();
      for (int i : selectors_for[t]) m = std::min(m, row[i]);
      return m;
    });
  } else if (aggregate == "max") {
    emit([&](const float* row, int t) {
      float m = -std::numeric_limits<float>::infinity();
      for (int i : selectors_for[t]) m = std::max(m, row[i]);
      return m;
    });
  } else {  // "mean"
    // One reciprocal per frame; taken inside the nest it would be one division per cell.
    std::vector<float> inv_counts(static_cast<size_t>(n_frames), 0.0f);
    for (int t = 0; t < n_frames; ++t) {
      const size_t count = selectors_for[t].size();
      if (count != 0) inv_counts[static_cast<size_t>(t)] = 1.0f / static_cast<float>(count);
    }
    emit([&](const float* row, int t) {
      float s = 0.0f;
      for (int i : selectors_for[t]) s += row[i];
      return s * inv_counts[static_cast<size_t>(t)];
    });
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

DecomposeStemsLinkedResult decompose_stems_linked(const float* const* channels,
                                                  std::size_t channel_count, std::size_t n,
                                                  int sample_rate,
                                                  const DecomposeStemsConfig& config) {
  const DecomposeStemsConfig checked = Validated<DecomposeStemsConfig>::make(config).get();
  SONARE_CHECK(channels != nullptr && channel_count > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(channel_count <= kMaxDecomposeStemsLinkedChannels, ErrorCode::InvalidParameter);
  for (std::size_t c = 0; c < channel_count; ++c) {
    SONARE_CHECK(channels[c] != nullptr, ErrorCode::InvalidParameter);
  }
  SONARE_CHECK(n > 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(sample_rate > 0, ErrorCode::InvalidParameter);

  const StftConfig stft_config = make_stft_config(checked.n_fft, checked.hop_length);
  std::vector<Audio> audio;
  std::vector<Spectrogram> spectra;
  audio.reserve(channel_count);
  spectra.reserve(channel_count);
  for (std::size_t c = 0; c < channel_count; ++c) {
    audio.push_back(Audio::from_buffer(channels[c], n, sample_rate));
    spectra.push_back(Spectrogram::compute(audio.back(), stft_config));
  }
  const int n_bins = spectra.front().n_bins();
  const int n_frames = spectra.front().n_frames();
  SONARE_CHECK(n_bins > 0 && n_frames > 0, ErrorCode::InvalidParameter);
  const std::size_t cells = static_cast<std::size_t>(n_bins) * static_cast<std::size_t>(n_frames);

  // M = (1/C) * sum_c |X_c|. For channel_count == 1 the sum is a single term
  // (adding 0 changes nothing) and the divide is by 1 (exact in IEEE754), so
  // this reproduces the mono magnitude bit for bit rather than merely
  // approximating it.
  std::vector<float> mean_magnitude(cells, 0.0f);
  for (std::size_t c = 0; c < channel_count; ++c) {
    const std::complex<float>* spectrum = spectra[c].complex_data();
    for (std::size_t cell = 0; cell < cells; ++cell) {
      mean_magnitude[cell] += std::abs(spectrum[cell]);
    }
  }
  const float channel_count_f = static_cast<float>(channel_count);
  for (float& value : mean_magnitude) value /= channel_count_f;

  DecomposeResult factors = decompose(mean_magnitude.data(), n_bins, n_frames, checked.n_components,
                                      checked.n_iter, "mu", checked.beta, checked.init);

  const int k = checked.n_components;
  std::vector<float> model;
  std::vector<float> denominator;
  build_component_model(factors, n_bins, n_frames, k, checked.mask_power, model, denominator);

  DecomposeStemsLinkedResult out;
  out.W = std::move(factors.W);
  out.H = std::move(factors.H);
  out.components.assign(static_cast<std::size_t>(k),
                        std::vector<std::vector<float>>(channel_count));
  std::vector<std::complex<float>> masked(cells);
  for (int component = 0; component < k; ++component) {
    const float* plane = model.data() + static_cast<std::size_t>(component) * cells;
    for (std::size_t c = 0; c < channel_count; ++c) {
      const std::complex<float>* source = spectra[c].complex_data();
      for (std::size_t cell = 0; cell < cells; ++cell) {
        // A cell the model gives no energy to is dropped from every component
        // rather than split evenly, so the masks never manufacture signal
        // where the factorisation has none. One mask per cell, applied
        // unchanged to every channel's own spectrum.
        const float total = denominator[cell];
        const float mask = total > kEps ? plane[cell] / total : 0.0f;
        masked[cell] = source[cell] * mask;
      }
      const Spectrogram component_spectrum =
          Spectrogram::from_complex(masked.data(), n_bins, n_frames, checked.n_fft,
                                    checked.hop_length, sample_rate, spectra[c].window());
      const Audio rendered = component_spectrum.to_audio(static_cast<int>(n));
      out.components[static_cast<std::size_t>(component)][c] =
          std::vector<float>(rendered.data(), rendered.data() + rendered.size());
    }
  }
  return out;
}

DecomposeStemsResult decompose_stems(const float* samples, std::size_t n, int sample_rate,
                                     const DecomposeStemsConfig& config) {
  const float* channels[1] = {samples};
  DecomposeStemsLinkedResult linked = decompose_stems_linked(channels, 1, n, sample_rate, config);

  DecomposeStemsResult out;
  out.W = std::move(linked.W);
  out.H = std::move(linked.H);
  out.components.reserve(linked.components.size());
  for (std::vector<std::vector<float>>& component : linked.components) {
    out.components.push_back(std::move(component[0]));
  }
  return out;
}

}  // namespace sonare
