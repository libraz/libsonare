#include "analysis/room_estimator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "acoustic/late_reverb.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare {
namespace {

// Consume the SAME Sabine/Eyring proportionality constant the forward
// `sabine_rt60`/`eyring_rt60` synthesis path uses (promoted to double here), so
// the geometry estimate inverts the synthesis with identical bit-for-bit
// coefficients rather than an independently-rounded literal.
constexpr double kSabineCoeff = static_cast<double>(sonare::acoustic::kSabineCoeff);

// Octave bands (125 Hz for band 0) averaged into ISO 3382's T_mid: 500 Hz and 1 kHz.
constexpr size_t kTmidLowBand = 2;
constexpr size_t kTmidHighBand = 3;

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
  return power_to_db_scalar(std::max(direct, kEps) / std::max(reverb, kEps));
}

}  // namespace

void validate_room_estimate_config(const RoomEstimateConfig& config) {
  SONARE_CHECK_MSG(numeric::finite_positive(config.aspect_hint_lw) &&
                       numeric::finite_positive(config.aspect_hint_lh),
                   ErrorCode::InvalidParameter, "room aspect priors must be finite and positive");
  SONARE_CHECK_MSG(numeric::finite(config.reference_absorption), ErrorCode::InvalidParameter,
                   "room absorption prior must be finite");
  SONARE_CHECK_MSG(config.acoustic.n_octave_bands >= 0 &&
                       numeric::finite_positive(config.acoustic.min_decay_db) &&
                       numeric::finite_non_negative(config.acoustic.noise_floor_margin_db),
                   ErrorCode::InvalidParameter, "room acoustic analysis settings are invalid");
}

RoomEstimate estimate_room(const Audio& recording, const RoomEstimateConfig& config) {
  validate_room_estimate_config(config);
  RoomEstimate est;

  const AcousticParameters ap = detect_acoustic(recording, config.acoustic);
  est.rt60_bands = ap.rt60_bands;
  est.drr_db = estimate_drr_db(recording);

  const float rt60_bb = ap.rt60;
  if (!(rt60_bb > 0.0f) || !std::isfinite(rt60_bb)) {
    // No measurable decay: geometry is NaN, the acoustic family's "not measurable".
    const float nan = std::numeric_limits<float>::quiet_NaN();
    est.volume = nan;
    est.dims.length = nan;
    est.dims.width = nan;
    est.dims.height = nan;
    est.confidence = 0.0f;
    return est;
  }

  // Room-shape prior: length L, width L/r_lw, height L/r_lh, so the volume and
  // surface scale as V = kV * L^3 and S = 2 * kS * L^2.
  const double r_lw = std::max(1e-3, static_cast<double>(config.aspect_hint_lw));
  const double r_lh = std::max(1e-3, static_cast<double>(config.aspect_hint_lh));
  const double kV = 1.0 / (r_lw * r_lh);
  const double kS = 1.0 / r_lw + 1.0 / r_lh + 1.0 / (r_lw * r_lh);

  // The inverse Sabine/Eyring problem fixes only V / A (L / alpha_eff here), so anchor the volume
  // scale with the absorption prior. Use the same statistical model the forward
  // synthesis path uses (Eyring by default) so the round-trip is consistent.
  // Clamped silently; the interval's width is a budget. See RoomEstimateConfig.
  const bool use_eyring = config.prefer_eyring;
  const double a0 = std::clamp(static_cast<double>(config.reference_absorption), 0.01, 0.99);
  const double absn0 = use_eyring ? -std::log(1.0 - a0) : a0;
  // The air term the synthesis adds by default (ISO reference climate) is part of every decay.
  const acoustic::AirAbsorption reference_air{};
  const auto air_m = [&](size_t band) {
    return static_cast<double>(acoustic::air_absorption_m_per_meter(
        acoustic::octave_center_hz(static_cast<int>(band)), reference_air.temperature_c,
        reference_air.humidity_percent));
  };
  // With the air term: RT = kSab*kV*L / (2*kS*absn0 + 4*m*kV*L)
  //   => L = RT * 2*kS*absn0 / (kV * (kSab - 4*m*RT))
  // The anchor is T_mid (the 500 Hz and 1 kHz octaves), each inverted with its own air term;
  // the broadband RT60 without an air term stands in when neither band is measurable.
  double length_sum = 0.0;
  int anchors = 0;
  for (const size_t band : {kTmidLowBand, kTmidHighBand}) {
    if (band >= est.rt60_bands.size()) continue;
    const double rt = static_cast<double>(est.rt60_bands[band]);
    const double denominator = kV * (kSabineCoeff - 4.0 * air_m(band) * rt);
    if (!(rt > 0.0) || !std::isfinite(rt) || !(denominator > 0.0)) continue;
    length_sum += rt * 2.0 * kS * absn0 / denominator;
    ++anchors;
  }
  const double length = anchors > 0
                            ? length_sum / anchors
                            : static_cast<double>(rt60_bb) * 2.0 * kS * absn0 / (kSabineCoeff * kV);
  const double volume = kV * length * length * length;
  const double surface = 2.0 * kS * length * length;

  est.volume = static_cast<float>(volume);
  est.dims.length = static_cast<float>(length);
  est.dims.width = static_cast<float>(length / r_lw);
  est.dims.height = static_cast<float>(length / r_lh);

  // Per-band mean wall absorption solved at the fixed (V, S). Sabine gives A/S
  // directly; Eyring corrects the diffuse-field reflection statistics. The air
  // term the synthesis adds by default (ISO reference climate) is taken out first,
  // so resynthesizing the estimate does not count the air twice.
  const size_t n_bands = est.rt60_bands.size();
  est.absorption_bands.assign(n_bands, 0.0f);
  int valid = 0;
  int clamped = 0;
  double sum_valid = 0.0;
  std::vector<bool> have(n_bands, false);
  for (size_t b = 0; b < n_bands; ++b) {
    const float rt = est.rt60_bands[b];
    if (!(rt > 0.0f) || !std::isfinite(rt)) continue;
    const double air_term = 4.0 * volume * air_m(b);
    const double sab =
        (kSabineCoeff * volume / static_cast<double>(rt) - air_term) / surface;  // A_wall/S
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
