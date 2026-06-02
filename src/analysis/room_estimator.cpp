#include "analysis/room_estimator.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "acoustic/late_reverb.h"

namespace sonare {
namespace {

// Consume the SAME Sabine/Eyring proportionality constant the forward
// `sabine_rt60`/`eyring_rt60` synthesis path uses (promoted to double here), so
// the geometry estimate inverts the synthesis with identical bit-for-bit
// coefficients rather than an independently-rounded literal.
constexpr double kSabineCoeff = static_cast<double>(sonare::acoustic::kSabineCoeff);

// Direct-to-reverberant ratio (dB): energy within +/- 2.5 ms of the strongest
// peak versus all energy outside that direct window (both the tail after it and
// any build-up before it, per the ISO 3382 split). Meaningful for impulse-like
// inputs; for sustained signals the analyzer flags the input blind and the
// caller should weight `drr_db` by the returned confidence.
float estimate_drr_db(const Audio& x) {
  const size_t n = x.size();
  if (n == 0) return 0.0f;
  const float* d = x.data();
  size_t peak = 0;
  float best = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    const float a = std::abs(d[i]);
    if (a > best) {
      best = a;
      peak = i;
    }
  }
  if (best <= 0.0f) return 0.0f;

  const int half = std::max(1, static_cast<int>(std::lround(0.0025 * x.sample_rate())));
  const size_t lo = peak > static_cast<size_t>(half) ? peak - static_cast<size_t>(half) : 0;
  const size_t hi = std::min(n, peak + static_cast<size_t>(half) + 1);
  double direct = 0.0;
  double reverb = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double e = static_cast<double>(d[i]) * static_cast<double>(d[i]);
    if (i >= lo && i < hi) {
      direct += e;
    } else {
      reverb += e;  // both pre-direct build-up and the post-direct tail
    }
  }
  constexpr double kEps = 1e-12;
  return static_cast<float>(10.0 * std::log10(std::max(direct, kEps) / std::max(reverb, kEps)));
}

}  // namespace

RoomEstimate estimate_room(const Audio& recording, const RoomEstimateConfig& config) {
  RoomEstimate est;

  const AcousticParameters ap = detect_acoustic(recording, config.acoustic);
  est.rt60_bands = ap.rt60_bands;
  est.drr_db = estimate_drr_db(recording);

  const float rt60_bb = ap.rt60;
  if (!(rt60_bb > 0.0f) || !std::isfinite(rt60_bb)) {
    est.confidence = 0.0f;
    return est;  // unanalyzable input -> zeroed geometry, honest zero confidence
  }

  // Room-shape prior: length L, width L/r_lw, height L/r_lh, so the volume and
  // surface scale as V = kV * L^3 and S = 2 * kS * L^2.
  const double r_lw = std::max(1e-3, static_cast<double>(config.aspect_hint_lw));
  const double r_lh = std::max(1e-3, static_cast<double>(config.aspect_hint_lh));
  const double kV = 1.0 / (r_lw * r_lh);
  const double kS = 1.0 / r_lw + 1.0 / r_lh + 1.0 / (r_lw * r_lh);

  // The inverse Sabine/Eyring problem fixes only V * alpha, so anchor the volume
  // scale with the absorption prior. Use the same statistical model the forward
  // synthesis path uses (Eyring by default) so the round-trip is consistent.
  const bool use_eyring = config.prefer_eyring;
  const double a0 = std::clamp(static_cast<double>(config.reference_absorption), 0.01, 0.99);
  const double absn0 = use_eyring ? -std::log(1.0 - a0) : a0;
  // Sabine:  RT = kSab*kV*L / (2*kS*a0)
  // Eyring:  RT = kSab*kV*L / (2*kS*(-ln(1-a0)))
  //   => L = RT * 2*kS*absn0 / (kSab*kV)
  const double length = static_cast<double>(rt60_bb) * 2.0 * kS * absn0 / (kSabineCoeff * kV);
  const double volume = kV * length * length * length;
  const double surface = 2.0 * kS * length * length;

  est.volume = static_cast<float>(volume);
  est.dims.length = static_cast<float>(length);
  est.dims.width = static_cast<float>(length / r_lw);
  est.dims.height = static_cast<float>(length / r_lh);

  // Per-band mean absorption solved at the fixed (V, S). Sabine gives A/S
  // directly; Eyring corrects the diffuse-field reflection statistics.
  const size_t n_bands = est.rt60_bands.size();
  est.absorption_bands.assign(n_bands, 0.0f);
  int valid = 0;
  int clamped = 0;
  double sum_valid = 0.0;
  std::vector<bool> have(n_bands, false);
  for (size_t b = 0; b < n_bands; ++b) {
    const float rt = est.rt60_bands[b];
    if (!(rt > 0.0f) || !std::isfinite(rt)) continue;
    const double sab = kSabineCoeff * volume / (surface * static_cast<double>(rt));  // A/S
    const double a = use_eyring ? (1.0 - std::exp(-sab)) : sab;
    const double a_cl = std::clamp(a, 0.0, 0.99);
    if (a_cl != a) ++clamped;
    est.absorption_bands[b] = static_cast<float>(a_cl);
    have[b] = true;
    sum_valid += a_cl;
    ++valid;
  }
  // Fill bands the analyzer could not resolve with the valid-band mean so the
  // vector stays usable for material assignment; missing bands cost confidence.
  if (valid > 0) {
    const float mean_a = static_cast<float>(sum_valid / valid);
    for (size_t b = 0; b < n_bands; ++b) {
      if (!have[b]) est.absorption_bands[b] = mean_a;
    }
  }

  // Confidence: the analyzer's own support, scaled by band coverage and by the
  // physical plausibility of the recovered absorptions. Heavy clamping means the
  // priors are fighting the data, so the estimate is not trustworthy.
  float c = std::clamp(ap.confidence, 0.0f, 1.0f);
  if (n_bands > 0) {
    c *= static_cast<float>(valid) / static_cast<float>(n_bands);
    if (valid > 0) {
      c *= 1.0f - static_cast<float>(clamped) / static_cast<float>(valid);
    }
  }
  est.confidence = std::clamp(c, 0.0f, 1.0f);
  return est;
}

}  // namespace sonare
