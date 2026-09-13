#include "editing/polyphony/inharmonicity.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "core/spectrum.h"
#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/note_mask.h"
#include "editing/polyphony/shared_bins.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare::editing::polyphony {
namespace {

using sonare::constants::kCentsPerOctave;
using sonare::constants::kSpectrumEpsilon;

/// Two points define the line, so a fit on fewer is not a fit however low a
/// caller sets its own floor.
constexpr int kMinFitPartials = 2;

/// @ref partial_claims' own ceiling on a harmonic count.
constexpr int kMaxHarmonics = 128;

/// Half-widths of the fitted stretch's own uncertainty the next partial is searched
/// over. Every measured fixture is identical from 0.5 to 6; at 7 a 55 Hz note's
/// window reaches its neighbour before the stretch is pinned and it is refused.
constexpr double kBracketSigmas = 3.0;

/// Floor under the peak reading's own error, in bins, which the search window and
/// the negative-fit test scale from. The interpolation's worst error over pitch,
/// framing, rolloff, decay and noise to -20 dB is 0.016 bins; this is three times it.
constexpr double kPeakErrorBins = 0.05;

/// How far a peak must stand over the median magnitude around it to count as a
/// partial. A present partial clears 4.7x at the worst pitch measured and 152x
/// elsewhere; a window over a noise floor reaches 1.5x.
constexpr double kMinProminence = 2.5;

/// How far below the note's strongest partial a peak may stand and still be one.
/// Float rounding above a tone's highest partial has a median as small as its
/// peaks, so the ratio above reads 6x there: -123 dB against -79 dB rendered.
constexpr double kMinPartialLevel = 1e-5;

/// Line through @c (h^2, (f_h/h)^2), whose intercept is @c f0^2 and whose slope
/// over that intercept is B.
struct StretchFit {
  double f0_sq = 0.0;
  double slope = 0.0;
  bool valid = false;
};

void check_config(const InharmonicityConfig& config) {
  // Named rather than bare: these are the caller's own values, and a refusal is the
  // only place a caller learns where their range ends. The spelling is the one a
  // binding will expose rather than the core's.
  SONARE_CHECK_MSG(config.min_partials >= kMinFitPartials && config.min_partials <= kMaxHarmonics,
                   ErrorCode::InvalidParameter,
                   "InharmonicityConfig: minPartials must be in [2, 128]");
  // One-sided ranges admit both NaN and infinity, so finiteness is checked rather
  // than left to the comparison.
  SONARE_CHECK_MSG(std::isfinite(config.max_residual_bins) && config.max_residual_bins > 0.0f,
                   ErrorCode::InvalidParameter,
                   "InharmonicityConfig: maxResidualBins must be finite and positive");
  SONARE_CHECK_MSG(std::isfinite(config.max_inharmonicity) && config.max_inharmonicity > 0.0f,
                   ErrorCode::InvalidParameter,
                   "InharmonicityConfig: maxInharmonicity must be finite and positive");
}

/// The ridge's f0 at @p frame, falling back to its median off the span.
double ridge_f0_at(const F0Ridge& ridge, int frame) {
  const int index = frame - ridge.frame_start;
  if (index >= 0 && index < static_cast<int>(ridge.f0_hz.size())) {
    return ridge.f0_hz[static_cast<size_t>(index)];
  }
  return sonare::median(ridge.f0_hz.data(), ridge.f0_hz.size());
}

StretchFit fit_stretch(const std::vector<double>& x, const std::vector<double>& y) {
  StretchFit fit;
  if (x.size() < static_cast<size_t>(kMinFitPartials)) return fit;
  const double n = static_cast<double>(x.size());
  double sum_x = 0.0;
  double sum_y = 0.0;
  double sum_xx = 0.0;
  double sum_xy = 0.0;
  for (size_t i = 0; i < x.size(); ++i) {
    sum_x += x[i];
    sum_y += y[i];
    sum_xx += x[i] * x[i];
    sum_xy += x[i] * y[i];
  }
  const double denominator = n * sum_xx - sum_x * sum_x;
  if (!(denominator > 0.0)) return fit;
  fit.slope = (n * sum_xy - sum_x * sum_y) / denominator;
  fit.f0_sq = (sum_y - fit.slope * sum_x) / n;
  fit.valid = std::isfinite(fit.slope) && std::isfinite(fit.f0_sq) && fit.f0_sq > 0.0;
  return fit;
}

/// Largest distance in Hz between a measured partial and where @p fit puts it.
/// @details The worst and not the median: one partial outside its claim is the
///          whole defect this fit exists to prevent, and a median hides it.
double worst_residual_hz(const std::vector<double>& x, const std::vector<double>& y,
                         const StretchFit& fit) {
  double worst = 0.0;
  for (size_t i = 0; i < x.size(); ++i) {
    const double modelled = fit.f0_sq + fit.slope * x[i];
    if (!(modelled > 0.0)) return std::numeric_limits<double>::infinity();
    const double harmonic = std::sqrt(x[i]);
    worst = std::max(worst, harmonic * std::abs(std::sqrt(y[i]) - std::sqrt(modelled)));
  }
  return worst;
}

/// How well the partials are placed, in Hz, floored at what one reading can do.
double stretch_sigma_hz(const std::vector<double>& x, const std::vector<double>& y,
                        const StretchFit& fit, double bin_hz) {
  return std::max(worst_residual_hz(x, y, fit), kPeakErrorBins * bin_hz);
}

/// Mean magnitude over the frames @p ridge spans, one value per bin.
void mean_magnitude(const Spectrogram& spec, const F0Ridge& ridge, std::vector<float>& mean) {
  const int n_frames = spec.n_frames();
  const int first = std::max(ridge.frame_start, 0);
  const int last = std::min(ridge.frame_end(), n_frames);
  const std::vector<float>& magnitude = spec.magnitude();
  mean.assign(static_cast<size_t>(spec.n_bins()), 0.0f);
  if (last <= first) return;
  for (int bin = 0; bin < spec.n_bins(); ++bin) {
    double sum = 0.0;
    for (int frame = first; frame < last; ++frame) {
      sum += magnitude[static_cast<size_t>(bin) * n_frames + frame];
    }
    mean[static_cast<size_t>(bin)] = static_cast<float>(sum / (last - first));
  }
}

/// One position another note's partial may stand at.
struct RivalPartial {
  double centre_hz = 0.0;
  /// How far from @ref centre_hz that partial may actually be.
  double reach_hz = 0.0;
};

/// Every partial position the ridges sounding beside @p note may stand at, which
/// is what says a partial of @p note's is contested.
/// @details @p rival_stretch holds each ridge's own fitted stretch, and a negative
///          entry falls back to the declared geometry @p masks was built with. The
///          fallback is not equivalent, and that is why this is run twice: a
///          rival's own stretch moves its high partials off the declared positions,
///          and on a fifth of stretched tones it puts the upper note's eleventh
///          partial 52 Hz up, onto the lower note's sixteenth and more than a main
///          lobe from where the declared geometry looks for it. The lower note then
///          fits its neighbour's partial as its own, which is the one outcome the
///          contest test exists to prevent.
void rival_partials(const Spectrogram& spec, const MultiF0Track& track, const NoteMaskSet& masks,
                    const std::vector<float>& refined, const std::vector<float>& rival_stretch,
                    size_t note, int first_frame, int last_frame, double lobe_hz, double error_rel,
                    std::vector<RivalPartial>& rivals) {
  const double nyquist_hz = 0.5 * spec.sample_rate();
  const int n_harmonics = std::min(masks.config.n_harmonics, kMaxHarmonics);
  rivals.clear();
  for (size_t other = 0; other < track.ridges.size(); ++other) {
    if (other == note) continue;
    const F0Ridge& ridge = track.ridges[other];
    // A ridge sounding nowhere in the span puts nothing in the window.
    if (ridge.frame_end() <= first_frame || ridge.frame_start >= last_frame) continue;
    const double built = ridge_f0_at(ridge, first_frame);
    if (!(built > 0.0)) continue;
    // A refined rival is placed to a fraction of a cent, and only an unrefined one
    // is widened by the track's declared error. Widening both puts every partial
    // of a two-octave dyad in contest: 50 cents at the lower note's fifth partial
    // already exceeds half the spacing its partials leave.
    const bool placed = refined[other] > 0.0f;
    const double f0 = placed ? refined[other] : built;
    const auto push = [&](double centre) {
      rivals.push_back(RivalPartial{centre, placed ? lobe_hz : lobe_hz + error_rel * centre});
    };
    const float fitted = rival_stretch[other];
    if (fitted >= 0.0f) {
      for (int k = 1; k <= n_harmonics; ++k) {
        const double kd = k;
        const double centre = kd * f0 * std::sqrt(1.0 + static_cast<double>(fitted) * kd * kd);
        if (centre > nyquist_hz) break;
        push(centre);
      }
      continue;
    }
    NoteMaskConfig declared = masks.config;
    // The stretch that rival's own claims were placed with, which is the scalar
    // only where the set does not carry one per note.
    declared.inharmonicity = masks.stretch_of(other);
    for (const PartialClaim& claim : partial_claims(spec, static_cast<float>(built), declared)) {
      // The centre is linear in the f0 at a fixed harmonic and stretch, so scaling
      // moves the partial to the refined f0 exactly.
      push(claim.centre_hz * f0 / built);
    }
  }
}

/// Frequency of the magnitude peak inside [@p lo_bin, @p hi_bin], or 0 where the
/// window holds no peak that can be read as a partial.
/// @details Quadratic interpolation over log magnitude, which is what takes the
///          reading below the half bin where a flat claim over a four-bin main
///          lobe starts leaving 6 dB behind. @p f0_hz sets the neighbourhood the
///          prominence is measured against: half a harmonic spacing is the region
///          one partial is the only feature of.
/// @param strongest Largest peak this note has already yielded, 0 before the
///        first. Read only as a level reference, so the first peak sets it.
/// @param level Out: the peak's own magnitude, whatever the verdict.
double peak_hz(const std::vector<float>& mean, int lo_bin, int hi_bin, double bin_hz, double f0_hz,
               double strongest, double& level, std::vector<float>& scratch) {
  int peak = lo_bin;
  for (int bin = lo_bin + 1; bin <= hi_bin; ++bin) {
    if (mean[static_cast<size_t>(bin)] > mean[static_cast<size_t>(peak)]) peak = bin;
  }
  level = mean[static_cast<size_t>(peak)];
  if (!(level > kMinPartialLevel * strongest)) return 0.0;
  // A maximum of the spectrum and not merely of the window: a skirt reaching in
  // from outside is monotone across it, and rejecting the window's own edges
  // instead would drop a partial displaced by more than the window's half-width
  // from its prediction -- which is the one the misfit gate exists to judge.
  if (mean[static_cast<size_t>(peak)] < mean[static_cast<size_t>(peak) - 1] ||
      mean[static_cast<size_t>(peak)] < mean[static_cast<size_t>(peak) + 1]) {
    return 0.0;
  }

  const int n_bins = static_cast<int>(mean.size());
  const int radius = std::max(2, static_cast<int>(std::lround(f0_hz / 2.0 / bin_hz)));
  const int floor_lo = std::max(0, peak - radius);
  const int floor_hi = std::min(n_bins - 1, peak + radius);
  scratch.assign(mean.begin() + floor_lo, mean.begin() + floor_hi + 1);
  const double local_floor = sonare::median(scratch.data(), scratch.size());
  if (!(mean[static_cast<size_t>(peak)] > kMinProminence * local_floor)) return 0.0;

  const double left = std::log(mean[static_cast<size_t>(peak) - 1] + kSpectrumEpsilon);
  const double centre = std::log(mean[static_cast<size_t>(peak)] + kSpectrumEpsilon);
  const double right = std::log(mean[static_cast<size_t>(peak) + 1] + kSpectrumEpsilon);
  const double curvature = left - 2.0 * centre + right;
  double offset = 0.0;
  if (curvature < 0.0) offset = std::clamp(0.5 * (left - right) / curvature, -0.5, 0.5);
  return (peak + offset) * bin_hz;
}

/// @ref estimate_track_inharmonicity for one ridge, @c -1 where it is refused.
float estimate_one(const Spectrogram& spec, const MultiF0Track& track, const NoteMaskSet& masks,
                   const InharmonicityConfig& config, const std::vector<float>& refined,
                   const std::vector<float>& rival_stretch, size_t note, double margin_hz,
                   double lobe_hz, double error_rel, std::vector<float>& mean,
                   std::vector<RivalPartial>& rivals, std::vector<float>& scratch) {
  const F0Ridge& ridge = track.ridges[note];
  const int first_frame = std::max(ridge.frame_start, 0);
  const int last_frame = std::min(ridge.frame_end(), spec.n_frames());
  mean_magnitude(spec, ridge, mean);
  rival_partials(spec, track, masks, refined, rival_stretch, note, first_frame, last_frame, lobe_hz,
                 error_rel, rivals);

  const int n_bins = spec.n_bins();
  const double bin_hz = static_cast<double>(spec.sample_rate()) / spec.n_fft();
  const double nyquist_hz = 0.5 * spec.sample_rate();
  const int n_harmonics = std::min(masks.config.n_harmonics, kMaxHarmonics);

  // The widest stretch the caller believes, narrowed by every partial that lands,
  // and the f0 the windows are predicted from, which starts refined and moves to
  // the fit's own once the fit is realisable.
  double bracket_lo = 0.0;
  double bracket_hi = config.max_inharmonicity;
  double f0_hz = refined[note];

  double strongest = 0.0;
  std::vector<double> x;
  std::vector<double> y;
  for (int h = 1; h <= n_harmonics; ++h) {
    const double hd = h;
    const double lo_hz = hd * f0_hz * std::sqrt(1.0 + bracket_lo * hd * hd) - margin_hz;
    const double hi_hz = hd * f0_hz * std::sqrt(1.0 + bracket_hi * hd * hd) + margin_hz;
    const double above = hd + 1.0;
    const double below = hd - 1.0;
    // Where the neighbouring partials may be under the same bracket. Both sides
    // are checked: the window above is the one that looks binding, but a bracket
    // wide enough stretches the partial below past this one's own position, and
    // that is the side that fires first on a low note.
    const double above_lo_hz =
        above * f0_hz * std::sqrt(1.0 + bracket_lo * above * above) - margin_hz;
    const double below_hi_hz =
        below > 0.0 ? below * f0_hz * std::sqrt(1.0 + bracket_hi * below * below) + margin_hz : 0.0;
    // Once the window holds a position a neighbour may stand at, that neighbour
    // can be read as this partial, and the windows only widen from here.
    if (!(hi_hz < above_lo_hz) || !(lo_hz > below_hi_hz) || !(hi_hz < nyquist_hz)) break;

    // Closed on both sides: a bin whose centre falls exactly on an edge is searched.
    // Every term here is double -- the float claim_lobes widens exactly and no float
    // literal enters -- so a bin on the edge is decided by the rule rather than by
    // which way a literal rounded. The first and last bins are left out because the
    // interpolation reads a neighbour either side.
    const int lo_bin = std::max(1, static_cast<int>(std::ceil(lo_hz / bin_hz)));
    const int hi_bin = std::min(n_bins - 2, static_cast<int>(std::floor(hi_hz / bin_hz)));
    if (hi_bin - lo_bin < 2) continue;

    double level = 0.0;
    const double measured_hz =
        peak_hz(mean, lo_bin, hi_bin, bin_hz, f0_hz, strongest, level, scratch);
    if (!(measured_hz > 0.0)) continue;

    // The reading and not the window is what has to stand clear of the other
    // notes. Refusing a window a rival reaches into refuses the whole upper note
    // of a two-octave dyad, whose every window is wide until the stretch is
    // pinned; refusing the reading drops only the partials that really coincide,
    // and a window where a rival's partial is the louder one yields that rival's
    // position and is dropped by the same test.
    bool contested = false;
    for (const RivalPartial& rival : rivals) {
      if (std::abs(measured_hz - rival.centre_hz) < rival.reach_hz) {
        contested = true;
        break;
      }
    }
    if (contested) continue;

    // Only an admitted peak sets the reference: a contested one may be a rival's.
    strongest = std::max(strongest, level);
    x.push_back(hd * hd);
    y.push_back((measured_hz / hd) * (measured_hz / hd));
    const StretchFit fit = fit_stretch(x, y);
    if (!fit.valid) continue;
    const double fitted = fit.slope / fit.f0_sq;
    // A displacement of B*h^3*f0/2 read to sigma pins B to 2*sigma/(h^3*f0).
    const double spread =
        kBracketSigmas * 2.0 * stretch_sigma_hz(x, y, fit, bin_hz) / (hd * hd * hd * f0_hz);
    // The f0 moves only for a fit a real series could have produced. A fit whose
    // slope is negative put its intercept where it did to absorb the slope, so
    // taking that f0 beside a stretch clamped back into range mixes two
    // incompatible halves: two partials perturbed in opposite directions fit a
    // compression of 5e-3 and an f0 3% high, and that f0 with a stretch of zero
    // moves every later window off its partial. The refined f0 is the better
    // prediction until the fit is realisable.
    if (fitted >= 0.0 && fitted <= static_cast<double>(config.max_inharmonicity)) {
      f0_hz = std::sqrt(fit.f0_sq);
    }
    // The bracket narrows whatever the sign, because it is a range of B and not a
    // value: refusing to narrow it on a fit a hair below zero, which is what a
    // note with no stretch gives, leaves the window wide enough to break the walk.
    const double believed = std::clamp(fitted, 0.0, static_cast<double>(config.max_inharmonicity));
    bracket_lo = std::max(0.0, believed - spread);
    bracket_hi = std::min(static_cast<double>(config.max_inharmonicity), believed + spread);
  }

  if (x.size() < static_cast<size_t>(config.min_partials)) return -1.0f;
  const StretchFit fit = fit_stretch(x, y);
  if (!fit.valid) return -1.0f;
  // Over the gate is refused and equal to it is accepted, compared in double, and
  // the division by bin_hz happens here and nowhere else: half a bin is a designed
  // threshold so inputs land exactly on it, a second expression that multiplied the
  // threshold back into Hz would disagree in the last bit, and unlike a window edge
  // -- where a bit decides whether a partial is in or out, which is visible -- this
  // one flips the verdict with nothing to see.
  const double residual_bins = worst_residual_hz(x, y, fit) / bin_hz;
  if (!(residual_bins <= static_cast<double>(config.max_residual_bins))) return -1.0f;

  const double fitted = fit.slope / fit.f0_sq;
  const double top = std::sqrt(x.back());
  const double precision = kBracketSigmas * 2.0 * stretch_sigma_hz(x, y, fit, bin_hz) /
                           (top * top * top * std::sqrt(fit.f0_sq));
  // A negative fit past its own precision is refused and not rounded up: no partial
  // series is compressed, so the fit found something other than this note's
  // partials. Inside that precision it is the harmonic series, which is a fitted
  // result and has to be reachable -- a true stretch of zero reads negative half
  // the time.
  //
  // Above the ceiling is refused and equal to it is accepted, compared in double
  // against the widened float. The default is a power of two for this comparison's
  // sake: a value like 0.05f is not representable, so a stretch of exactly 0.05
  // would land under the ceiling by the float's own rounding rather than by a rule.
  if (fitted < -precision || fitted > static_cast<double>(config.max_inharmonicity)) return -1.0f;
  return static_cast<float>(std::max(0.0, fitted));
}

}  // namespace

std::vector<float> estimate_track_inharmonicity(const Spectrogram& spec, const MultiF0Track& track,
                                                const NoteMaskSet& masks,
                                                const InharmonicityConfig& config) {
  check_config(config);
  // Runs the same validation solve_shared_bins does, and returns the f0 the
  // windows are predicted from: a ridge it could not refine is refused here too.
  const SharedBinConfig refine_config;
  const std::vector<float> refined = refine_track_f0(spec, track, masks, refine_config);

  // The claim's own half-width in Hz, 2 * claim_lobes * sample_rate / win_length,
  // which is how far a prediction may be out and still hold its partial. Not
  // widened by max_residual_bins: a gate that widened the window would decide what
  // the fit sees rather than only whether the fit is believed, and a loose one
  // would walk the window onto a neighbour.
  const double margin_hz =
      2.0 * static_cast<double>(masks.config.claim_lobes) * spec.sample_rate() / spec.win_length();
  // One window main lobe, which is the distance inside which two partials are one
  // peak rather than two: a rival nearer than this pulls the reading instead of
  // standing beside it, and the claim half-width above is only half of it.
  const double lobe_hz = 4.0 * spec.sample_rate() / spec.win_length();
  const double error_rel =
      std::pow(2.0, static_cast<double>(refine_config.f0_tolerance_cents) / kCentsPerOctave) - 1.0;

  std::vector<float> mean;
  std::vector<float> scratch;
  std::vector<RivalPartial> rivals;
  // Twice over the ridges. The first pass has only the declared geometry to say
  // where the other notes are; the second places each rival's partials from the
  // stretch the first fitted for it, which is what keeps a neighbour's high
  // partial -- displaced off its declared position by its own stretch -- from
  // being fitted as this note's. A ridge the first pass refused keeps the declared
  // geometry in the second, so the second is never worse informed than the first.
  std::vector<float> stretch(track.ridges.size(), -1.0f);
  for (int pass = 0; pass < 2; ++pass) {
    const std::vector<float> rival_stretch = stretch;
    for (size_t note = 0; note < track.ridges.size(); ++note) {
      // 0 is refine_track_f0's refusal, and the windows cannot be placed without it.
      if (!(refined[note] > 0.0f)) continue;
      stretch[note] = estimate_one(spec, track, masks, config, refined, rival_stretch, note,
                                   margin_hz, lobe_hz, error_rel, mean, rivals, scratch);
    }
  }
  return stretch;
}

}  // namespace sonare::editing::polyphony
