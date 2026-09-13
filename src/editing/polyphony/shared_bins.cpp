#include "editing/polyphony/shared_bins.h"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/spectrum.h"
#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/note_mask.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/math_utils.h"

namespace sonare::editing::polyphony {
namespace {

using sonare::constants::kCentsPerOctave;
using sonare::constants::kPi;
using sonare::constants::kPiD;
using sonare::constants::kTwoPiD;

using Complexd = std::complex<double>;

/// Below 4 frames an order-2 fit has no shift rows left to solve; past 64 a
/// note's own decay and drift have stopped reading as a fixed set of poles.
constexpr int kMinWindowFrames = 4;
constexpr int kMaxWindowFrames = 64;

/// The assignment enumerates the orders, so one claimant past this is a hang and
/// not a slower answer. The default window admits 4, so this is unreachable there.
constexpr int kMaxAssignmentOrder = 8;

double wrap_to_pi(double angle) {
  const double wrapped = std::fmod(angle + kPiD, kTwoPiD);
  return wrapped < 0.0 ? wrapped + kPiD : wrapped - kPiD;
}

/// The geometry @p masks placed note @p note's claims with.
/// @details The only geometry a replay may read. A set carries a stretch per note
///          where they differ, and reading the scalar instead identifies the wrong
///          partial from about the fourteenth up at an ordinary piano stretch --
///          which is the threshold @c NoteMaskSet::config's own note states.
NoteMaskConfig note_geometry(const NoteMaskSet& masks, size_t note) {
  NoteMaskConfig config = masks.config;
  config.inharmonicity = masks.stretch_of(note);
  return config;
}

/// The partial standing on @p bin, or null where the geometry places none there.
/// Claims are disjoint, so at most one can.
const PartialClaim* claim_over_bin(const std::vector<PartialClaim>& partials, int bin) {
  for (const PartialClaim& partial : partials) {
    if (bin >= partial.first_bin && bin <= partial.last_bin) return &partial;
  }
  return nullptr;
}

/// Highest partial an f0 refinement may read, in Hz.
double refine_ceiling_hz(const SharedBinConfig& config, double sample_rate, double hop) {
  if (config.max_refine_hz > 0.0f) return config.max_refine_hz;
  // Half the frame rate is how far a prediction may be wrong and still unwrap to
  // the right alias, so the tolerance turns it into a ceiling on the partial.
  const double spread =
      std::pow(2.0, static_cast<double>(config.f0_tolerance_cents) / kCentsPerOctave) - 1.0;
  if (!(spread > 0.0)) return 0.0;
  return (sample_rate / hop) / 2.0 / spread;
}

/// The ridge's f0 at @p frame, falling back to its median where the ridge and
/// the mask disagree on the span.
double ridge_f0_at(const F0Ridge& ridge, int frame) {
  const int index = frame - ridge.frame_start;
  if (index >= 0 && index < static_cast<int>(ridge.f0_hz.size())) {
    return ridge.f0_hz[static_cast<size_t>(index)];
  }
  return sonare::median(ridge.f0_hz.data(), ridge.f0_hz.size());
}

/// Every bin and frame a mask names lands inside the spectrum, and its own
/// sparse indexing is consistent, before anything is allocated against it.
void check_mask_shape(const NoteMask& mask, int n_bins, int n_frames) {
  SONARE_CHECK(mask.n_frames >= 0 && mask.frame_start >= 0 && mask.frame_end() <= n_frames,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(mask.frame_offset.size() == static_cast<size_t>(mask.n_frames) + 1,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(mask.weights.size() == mask.bins.size(), ErrorCode::InvalidParameter);
  SONARE_CHECK(mask.frame_offset.front() == 0, ErrorCode::InvalidParameter);
  SONARE_CHECK(static_cast<size_t>(mask.frame_offset.back()) == mask.bins.size(),
               ErrorCode::InvalidParameter);
  for (size_t i = 1; i < mask.frame_offset.size(); ++i) {
    SONARE_CHECK(mask.frame_offset[i] >= mask.frame_offset[i - 1], ErrorCode::InvalidParameter);
  }
  for (const int32_t bin : mask.bins) {
    SONARE_CHECK(bin >= 0 && bin < n_bins, ErrorCode::InvalidParameter);
  }
}

void check_config(const SharedBinConfig& config) {
  SONARE_CHECK(config.window_frames >= kMinWindowFrames && config.window_frames <= kMaxWindowFrames,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.min_partial_separation) &&
                   config.min_partial_separation > 0.0f && config.min_partial_separation <= kPi,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.max_fit_residual) && config.max_fit_residual > 0.0f &&
                   config.max_fit_residual <= 1.0f,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.max_weight_modulus) && config.max_weight_modulus >= 1.0f,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.max_refine_hz) && config.max_refine_hz >= 0.0f,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.f0_tolerance_cents) && config.f0_tolerance_cents > 0.0f &&
                   config.f0_tolerance_cents <= kCentsPerOctave,
               ErrorCode::InvalidParameter);
}

void check_inputs(const Spectrogram& spec, const NoteMaskSet& masks, const MultiF0Track& track,
                  const SharedBinConfig& config) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(spec.n_fft() > 0 && spec.hop_length() > 0 && spec.sample_rate() > 0,
               ErrorCode::InvalidParameter);
  // The hop and the rate as well as the sizes: a set from another framing indexes
  // the same array while meaning different times.
  SONARE_CHECK(masks.n_bins == spec.n_bins() && masks.n_frames == spec.n_frames() &&
                   masks.hop_length == spec.hop_length() && masks.sample_rate == spec.sample_rate(),
               ErrorCode::InvalidParameter);
  SONARE_CHECK(masks.notes.size() == track.ridges.size(), ErrorCode::InvalidParameter);
  // Read rather than carried here, so its shape is checked here: the set's own note
  // says a stage that interprets the claims validates the geometry for itself, and
  // a vector shorter than the notes would quietly read the scalar for the rest.
  // These are effective stretches, so a negative one is not a refusal but a value
  // partial_claims refuses, and a NaN passes every comparison that would catch it.
  SONARE_CHECK(masks.inharmonicity.empty() || masks.inharmonicity.size() == masks.notes.size(),
               ErrorCode::InvalidParameter);
  for (const float stretch : masks.inharmonicity) {
    SONARE_CHECK(std::isfinite(stretch) && stretch >= 0.0f, ErrorCode::InvalidParameter);
  }
  // The track carries the instants the ridges mean, so a hop or a rate of its own
  // indexes the same frames while describing different times.
  SONARE_CHECK(track.n_frames == spec.n_frames() && track.hop_length == spec.hop_length() &&
                   track.sample_rate == spec.sample_rate(),
               ErrorCode::InvalidParameter);
  for (const F0Ridge& ridge : track.ridges) {
    SONARE_CHECK(
        ridge.frame_start >= 0 && !ridge.f0_hz.empty() && ridge.frame_end() <= spec.n_frames(),
        ErrorCode::InvalidParameter);
    // The same standard build_note_masks holds the same struct to: a value one
    // entry point refuses and another absorbs is where a validation gap hides.
    for (const float f0_hz : ridge.f0_hz) {
      SONARE_CHECK(std::isfinite(f0_hz) && f0_hz > 0.0f, ErrorCode::InvalidParameter);
    }
  }
  for (const NoteMask& mask : masks.notes) {
    check_mask_shape(mask, masks.n_bins, masks.n_frames);
  }
  check_config(config);
}

/// One note's claim on one bin at one frame, pointing back at the mask entry it
/// came from so a solved weight can be written where it belongs.
struct BinClaim {
  int frame = 0;
  int note = 0;
  int32_t entry = 0;
};

/// A maximal run of consecutive frames on one bin claimed by exactly the same
/// notes. Its claims are a contiguous @c n_frames x @c n_notes block, so the
/// entry of note @c j at frame offset @c f is at
/// <tt>group_lo + f * n_notes + j</tt>.
struct Span {
  int frame_start = 0;
  int n_frames = 0;
  int n_notes = 0;
  size_t group_lo = 0;
};

/// Every mask entry re-indexed by bin, ascending in frame within a bin and in
/// note within a frame, so one bin's whole trajectory is one contiguous range.
void build_bin_claims(const NoteMaskSet& masks, std::vector<size_t>& bin_start,
                      std::vector<BinClaim>& claims) {
  bin_start.assign(static_cast<size_t>(masks.n_bins) + 1, 0);
  size_t total = 0;
  for (const NoteMask& mask : masks.notes) {
    total += mask.bins.size();
    for (const int32_t bin : mask.bins) ++bin_start[static_cast<size_t>(bin) + 1];
  }
  for (size_t bin = 1; bin < bin_start.size(); ++bin) bin_start[bin] += bin_start[bin - 1];

  std::vector<size_t> cursor(bin_start.begin(), bin_start.end() - 1);
  claims.assign(total, BinClaim{});
  // Frames outermost, so the claims of one bin come out sorted by frame.
  for (int frame = 0; frame < masks.n_frames; ++frame) {
    for (size_t i = 0; i < masks.notes.size(); ++i) {
      const NoteMask& mask = masks.notes[i];
      const int index = frame - mask.frame_start;
      if (index < 0 || index >= mask.n_frames) continue;
      for (int32_t k = mask.frame_offset[static_cast<size_t>(index)];
           k < mask.frame_offset[static_cast<size_t>(index) + 1]; ++k) {
        const size_t bin = static_cast<size_t>(mask.bins[static_cast<size_t>(k)]);
        claims[cursor[bin]++] = BinClaim{frame, static_cast<int>(i), k};
      }
    }
  }
}

bool same_notes(const std::vector<BinClaim>& claims, size_t a, size_t b, int count) {
  for (int i = 0; i < count; ++i) {
    if (claims[a + static_cast<size_t>(i)].note != claims[b + static_cast<size_t>(i)].note) {
      return false;
    }
  }
  return true;
}

void build_spans(const std::vector<BinClaim>& claims, size_t lo, size_t hi,
                 std::vector<Span>& spans) {
  spans.clear();
  size_t i = lo;
  while (i < hi) {
    const int frame = claims[i].frame;
    size_t j = i;
    while (j < hi && claims[j].frame == frame) ++j;
    const int width = static_cast<int>(j - i);
    if (!spans.empty()) {
      Span& prev = spans.back();
      const size_t prev_last = prev.group_lo + static_cast<size_t>(prev.n_frames - 1) *
                                                   static_cast<size_t>(prev.n_notes);
      if (prev.n_notes == width && prev.frame_start + prev.n_frames == frame &&
          same_notes(claims, prev_last, i, width)) {
        ++prev.n_frames;
        i = j;
        continue;
      }
    }
    spans.push_back(Span{frame, 1, width, i});
    i = j;
  }
}

/// Poles of @p x read as a sum of @p order damped phasors.
/// @return false when the decomposition did not converge or produced a pole
///         that is not finite.
bool esprit_poles(const std::vector<Complexd>& x, int order, std::vector<Complexd>& poles) {
  const int len = static_cast<int>(x.size());
  const int columns = std::max(order + 1, len / 2);
  const int rows = len - columns + 1;
  if (rows < 1 || order < 1) return false;

  Eigen::MatrixXcd hankel(rows, columns);
  for (int i = 0; i < rows; ++i) {
    for (int j = 0; j < columns; ++j) hankel(i, j) = x[static_cast<size_t>(i + j)];
  }

  Eigen::JacobiSVD<Eigen::MatrixXcd> svd(hankel, Eigen::ComputeThinU | Eigen::ComputeThinV);
  if (svd.matrixV().cols() < order) return false;
  // Eigen's V is already the conjugated, transposed right singular vectors, and
  // that conjugation is load-bearing: without it every rate comes out negated
  // and the pole-to-note match then silently picks the wrong partner.
  const Eigen::MatrixXcd signal = svd.matrixV().leftCols(order);

  const int shift = static_cast<int>(signal.rows()) - 1;
  if (shift < order) return false;
  const Eigen::MatrixXcd upper = signal.topRows(shift);
  const Eigen::MatrixXcd lower = signal.bottomRows(shift);
  const Eigen::MatrixXcd rotation = upper.colPivHouseholderQr().solve(lower);

  Eigen::ComplexEigenSolver<Eigen::MatrixXcd> solver(rotation, false);
  if (solver.info() != Eigen::Success) return false;

  poles.assign(static_cast<size_t>(order), Complexd(0.0, 0.0));
  for (int i = 0; i < order; ++i) {
    const Complexd pole = std::conj(solver.eigenvalues()(i));
    if (!std::isfinite(pole.real()) || !std::isfinite(pole.imag())) return false;
    poles[static_cast<size_t>(i)] = pole;
  }
  return true;
}

/// Smallest gap between any two of @p partial_hz, in radians per frame.
/// @details Wrapped per pair and not after taking the smallest gap in Hz: two
///          partials a frame rate apart turn identically and are invisible to the
///          fit, so an unwrapped gap presents the one case the gate cannot see as
///          the largest separation there is.
double closest_partial_separation(const std::vector<double>& partial_hz, double frame_rate_hz) {
  double closest = kPiD;
  for (size_t i = 0; i + 1 < partial_hz.size(); ++i) {
    for (size_t j = i + 1; j < partial_hz.size(); ++j) {
      const double gap = kTwoPiD * (partial_hz[i] - partial_hz[j]) / frame_rate_hz;
      closest = std::min(closest, std::abs(wrap_to_pi(gap)));
    }
  }
  return closest;
}

/// Least-squares @c sum_i c_i * z_i^n over @p x.
/// @param component @c poles.size() x @c x.size(), component @c i at
///        <tt>i * x.size() + n</tt>.
/// @param residual @c ||x - model|| / ||x||.
bool fit_components(const std::vector<Complexd>& x, const std::vector<Complexd>& poles,
                    std::vector<Complexd>& component, double& residual) {
  const int len = static_cast<int>(x.size());
  const int order = static_cast<int>(poles.size());
  residual = 1.0;

  Eigen::MatrixXcd basis(len, order);
  for (int i = 0; i < order; ++i) {
    // z^n and not exp(i*w*n): the modulus carries the partial's damping, and
    // fitting undamped phasors to a decaying one costs about 100 dB silently.
    Complexd power(1.0, 0.0);
    for (int n = 0; n < len; ++n) {
      basis(n, i) = power;
      power *= poles[static_cast<size_t>(i)];
    }
  }
  if (!basis.allFinite()) return false;

  Eigen::VectorXcd observed(len);
  for (int n = 0; n < len; ++n) observed(n) = x[static_cast<size_t>(n)];
  const double observed_norm = observed.norm();
  if (!(observed_norm > 0.0)) return false;

  const Eigen::VectorXcd coefficient = basis.colPivHouseholderQr().solve(observed);
  if (!coefficient.allFinite()) return false;

  const Eigen::VectorXcd model = basis * coefficient;
  if (!model.allFinite()) return false;
  residual = (observed - model).norm() / observed_norm;

  component.assign(static_cast<size_t>(order) * static_cast<size_t>(len), Complexd(0.0, 0.0));
  for (int i = 0; i < order; ++i) {
    for (int n = 0; n < len; ++n) {
      component[static_cast<size_t>(i) * static_cast<size_t>(len) + static_cast<size_t>(n)] =
          coefficient(i) * basis(n, i);
    }
  }
  return true;
}

/// The claimant-to-pole bijection of lowest total squared wrapped-angle distance.
/// @details Squared and not absolute because summed absolute distance ties
///          identically wherever both poles lie to one side of both predictions,
///          which decides nothing. There is one pole per claimant, so this is a
///          square assignment and every permutation is enumerated -- the bound
///          @c kMaxAssignmentOrder exists for that.
/// @param distance Scratch, @c order x @c order, note @c i against pole @c p at
///        <tt>i * order + p</tt>.
/// @param permutation Scratch for the candidate being summed.
/// @return false when two or more bijections reach the minimum total, which the
///         data does not decide between. Compared exactly: an order-independent
///         result cannot come from an order-dependent tiebreak, so a tie is
///         refused rather than settled.
bool assign_poles(const std::vector<Complexd>& poles, const std::vector<double>& predicted_rate,
                  std::vector<double>& distance, std::vector<int>& permutation,
                  std::vector<int>& pole_of_note) {
  const int order = static_cast<int>(poles.size());
  distance.assign(static_cast<size_t>(order) * static_cast<size_t>(order), 0.0);
  for (int note = 0; note < order; ++note) {
    const double rate = predicted_rate[static_cast<size_t>(note)];
    for (int p = 0; p < order; ++p) {
      const double gap = wrap_to_pi(std::arg(poles[static_cast<size_t>(p)]) - rate);
      distance[static_cast<size_t>(note) * static_cast<size_t>(order) + static_cast<size_t>(p)] =
          gap * gap;
    }
  }

  permutation.resize(static_cast<size_t>(order));
  for (int i = 0; i < order; ++i) permutation[static_cast<size_t>(i)] = i;
  pole_of_note.assign(static_cast<size_t>(order), 0);

  double best_total = 0.0;
  bool have_best = false;
  bool tied = false;
  do {
    double total = 0.0;
    for (int note = 0; note < order; ++note) {
      total += distance[static_cast<size_t>(note) * static_cast<size_t>(order) +
                        static_cast<size_t>(permutation[static_cast<size_t>(note)])];
    }
    if (!have_best || total < best_total) {
      have_best = true;
      // A strictly better total retires the tie with it: tied describes the
      // minimum as it now stands, not whether any two candidates ever agreed.
      tied = false;
      best_total = total;
      pole_of_note.assign(permutation.begin(), permutation.end());
    } else if (total == best_total) {
      tied = true;
    }
  } while (std::next_permutation(permutation.begin(), permutation.end()));
  return !tied;
}

std::complex<float> clamped_weight(Complexd weight, double limit) {
  const double modulus = std::abs(weight);
  if (modulus > limit) weight *= limit / modulus;
  return std::complex<float>(static_cast<float>(weight.real()), static_cast<float>(weight.imag()));
}

/// Whether @p centre_hz is the only predicted partial inside its own claim over
/// [@p frame_start, @p frame_end).
/// @details A bin no other note claims is not enough. Two notes a hertz and a
///          half apart have claims that overlap almost entirely, so the bins left
///          geometrically unshared are slivers at the claim edges holding
///          leakage, and an order-1 fit there returns the centroid of both.
/// @param error_rel Worst relative displacement the declared f0 error puts on a
///        predicted centre. Without widening by it the test is not fail-safe: the
///        centres compared carry that error undiluted, and at 50 cents it is twice
///        the half-width even at the alias ceiling.
bool partial_stands_alone(const Spectrogram& spec, const MultiF0Track& track,
                          const NoteMaskSet& masks, int note, int frame_start, int frame_end,
                          double centre_hz, double half_width_hz, double error_rel,
                          std::vector<PartialClaim>& scratch) {
  for (size_t other = 0; other < track.ridges.size(); ++other) {
    if (static_cast<int>(other) == note) continue;
    const F0Ridge& ridge = track.ridges[other];
    // A ridge sounding nowhere in the window puts nothing in the claim.
    if (ridge.frame_end() <= frame_start || ridge.frame_start >= frame_end) continue;
    const double f0 = ridge_f0_at(ridge, frame_start);
    if (!(f0 > 0.0)) continue;
    // The rival's own geometry, not this note's: the set places each note's claims
    // at its own stretch, so a rival's partials stand where its stretch put them.
    scratch = partial_claims(spec, static_cast<float>(f0), note_geometry(masks, other));
    for (const PartialClaim& claim : scratch) {
      const double rival_hz = claim.centre_hz;
      const double margin = half_width_hz + error_rel * (centre_hz + rival_hz);
      // Qualifying takes strict separation, so a partial exactly on the boundary
      // is disqualified rather than trusted.
      if (std::abs(rival_hz - centre_hz) <= margin) return false;
    }
  }
  return true;
}

/// @ref refine_track_f0 once the claim index is already built.
std::vector<float> refine_from_claims(const Spectrogram& spec, const MultiF0Track& track,
                                      const NoteMaskSet& masks, const SharedBinConfig& config,
                                      const std::vector<size_t>& bin_start,
                                      const std::vector<BinClaim>& claims) {
  const int n_frames = spec.n_frames();
  const int window = config.window_frames;
  const double sample_rate = spec.sample_rate();
  const double hop = spec.hop_length();
  const double hz_per_bin = sample_rate / spec.n_fft();
  const double ceiling = refine_ceiling_hz(config, sample_rate, hop);
  const double alias = sample_rate / hop;
  const std::complex<float>* data = spec.complex_data();

  // The claim's own half-width in Hz, which is what a rival partial has to clear
  // to leave this one alone: 2 * claim_lobes * sample_rate / win_length.
  const double half_width_hz =
      2.0 * static_cast<double>(masks.config.claim_lobes) * sample_rate / spec.win_length();
  // Read whether or not max_refine_hz was set: the ceiling is one use of the
  // tolerance and widening the qualifier is the other.
  const double error_rel =
      std::pow(2.0, static_cast<double>(config.f0_tolerance_cents) / kCentsPerOctave) - 1.0;

  std::vector<std::vector<float>> votes(masks.notes.size());
  std::vector<Span> spans;
  std::vector<PartialClaim> partials;
  std::vector<PartialClaim> rival_partials;
  std::vector<Complexd> trajectory(static_cast<size_t>(window));
  std::vector<Complexd> poles;

  for (int bin = 0; bin < masks.n_bins; ++bin) {
    const double bin_hz = bin * hz_per_bin;
    if (!(bin_hz > 0.0) || bin_hz > ceiling) continue;
    build_spans(claims, bin_start[static_cast<size_t>(bin)],
                bin_start[static_cast<size_t>(bin) + 1], spans);
    for (const Span& span : spans) {
      // A bin one note claims has no assignment problem, which is what makes the
      // order-1 rate it yields usable as an f0 vote.
      if (span.n_notes != 1 || span.n_frames < window) continue;
      const int note = claims[span.group_lo].note;
      const double f0 = ridge_f0_at(track.ridges[static_cast<size_t>(note)], span.frame_start);
      partials = partial_claims(spec, static_cast<float>(f0),
                                note_geometry(masks, static_cast<size_t>(note)));
      const PartialClaim* partial = claim_over_bin(partials, bin);
      if (partial == nullptr) continue;
      if (!partial_stands_alone(spec, track, masks, note, span.frame_start,
                                span.frame_start + window, partial->centre_hz, half_width_hz,
                                error_rel, rival_partials)) {
        continue;
      }

      for (int n = 0; n < window; ++n) {
        trajectory[static_cast<size_t>(n)] =
            Complexd(data[static_cast<size_t>(bin) * n_frames + span.frame_start + n]);
      }
      if (!esprit_poles(trajectory, 1, poles)) continue;

      const double predicted = partial->centre_hz;
      const double base = std::arg(poles[0]) * sample_rate / (kTwoPiD * hop);
      const double measured = base + std::round((predicted - base) / alias) * alias;
      if (!std::isfinite(measured) || !(measured > 0.0) || !(predicted > 0.0)) continue;
      // f0 = measured / (h * sqrt(1 + B*h^2)), and that divisor is predicted / f0,
      // so inverting the stretch this way leaves it derived in one place only.
      votes[static_cast<size_t>(note)].push_back(static_cast<float>(measured * f0 / predicted));
    }
  }

  std::vector<float> refined(masks.notes.size(), 0.0f);
  for (size_t i = 0; i < refined.size(); ++i) {
    // 0 rather than the incoming f0: a returned input cannot be told from a
    // refinement that agreed with it, and the gate needs that difference.
    if (votes[i].empty()) continue;
    refined[i] = sonare::median(votes[i].data(), votes[i].size());
  }
  return refined;
}

}  // namespace

std::vector<float> refine_track_f0(const Spectrogram& spec, const MultiF0Track& track,
                                   const NoteMaskSet& masks, const SharedBinConfig& config) {
  check_inputs(spec, masks, track, config);

  std::vector<size_t> bin_start;
  std::vector<BinClaim> claims;
  build_bin_claims(masks, bin_start, claims);
  return refine_from_claims(spec, track, masks, config, bin_start, claims);
}

NoteMaskSet solve_shared_bins(const Spectrogram& spec, const NoteMaskSet& masks,
                              const MultiF0Track& track, const SharedBinConfig& config,
                              SharedBinReport* report) {
  check_inputs(spec, masks, track, config);

  std::vector<size_t> bin_start;
  std::vector<BinClaim> claims;
  build_bin_claims(masks, bin_start, claims);
  // The assignment is what f0 error breaks, so every predicted rate below is
  // read off the refinement rather than off the track's own f0.
  const std::vector<float> refined =
      refine_from_claims(spec, track, masks, config, bin_start, claims);

  NoteMaskSet result = masks;
  // The spectrogram's own layout, as mask_total uses. Every cell a span covers is
  // overwritten below, so the fill is what the cells no note reached keep.
  const size_t surface = static_cast<size_t>(masks.n_bins) * static_cast<size_t>(masks.n_frames);
  if (report != nullptr) {
    report->outcome.assign(surface, SharedBinOutcome::Unclaimed);
    report->partial_separation.assign(surface, 0.0f);
    report->fit_residual.assign(surface, 0.0f);
  }

  const int n_frames = spec.n_frames();
  const int window = config.window_frames;
  const int step = window / 2;
  // The lesser of the two ceilings: what the fit can hold poles for, and what the
  // assignment can enumerate.
  const int max_claimants = std::min(window / 2, kMaxAssignmentOrder);
  const double sample_rate = spec.sample_rate();
  const double hop = spec.hop_length();
  const std::complex<float>* data = spec.complex_data();

  std::vector<Span> spans;
  std::vector<int> starts;
  std::vector<Complexd> trajectory(static_cast<size_t>(window));
  std::vector<Complexd> poles;
  std::vector<Complexd> component;
  std::vector<Complexd> accumulated;
  std::vector<std::complex<float>> resolved;
  std::vector<int> covered;
  std::vector<SharedBinOutcome> reason;
  std::vector<float> residual_at;
  std::vector<char> decided;
  std::vector<PartialClaim> partials;
  std::vector<double> predicted_hz;
  std::vector<double> predicted_rate;
  std::vector<double> pole_distance;
  std::vector<int> permutation;
  std::vector<int> pole_of_note;

  for (int bin = 0; bin < masks.n_bins; ++bin) {
    build_spans(claims, bin_start[static_cast<size_t>(bin)],
                bin_start[static_cast<size_t>(bin) + 1], spans);
    for (const Span& span : spans) {
      const size_t entries = static_cast<size_t>(span.n_frames) * static_cast<size_t>(span.n_notes);
      const auto record = [&](int frame_offset, SharedBinOutcome outcome, float separation,
                              float residual) {
        if (report == nullptr) return;
        const size_t at = static_cast<size_t>(bin) * static_cast<size_t>(n_frames) +
                          static_cast<size_t>(span.frame_start + frame_offset);
        report->outcome[at] = outcome;
        report->partial_separation[at] = separation;
        report->fit_residual[at] = residual;
      };
      const auto record_span = [&](SharedBinOutcome outcome, float separation) {
        for (int f = 0; f < span.n_frames; ++f) record(f, outcome, separation, 0.0f);
      };

      if (span.n_notes == 1) {
        record_span(SharedBinOutcome::Unshared, 0.0f);
        continue;
      }
      if (span.n_frames < window) {
        record_span(SharedBinOutcome::TooFewFrames, 0.0f);
        continue;
      }
      if (span.n_notes > max_claimants) {
        record_span(SharedBinOutcome::TooManyClaimants, 0.0f);
        continue;
      }

      // One refined f0 per ridge and a fixed claimant set over the span, so the
      // partial positions and the gap between them are a span-wide constant.
      predicted_hz.assign(static_cast<size_t>(span.n_notes), 0.0);
      predicted_rate.assign(static_cast<size_t>(span.n_notes), 0.0);
      bool every_partial_placed = true;
      for (int j = 0; j < span.n_notes && every_partial_placed; ++j) {
        const int note = claims[span.group_lo + static_cast<size_t>(j)].note;
        const double f0_refined = refined[static_cast<size_t>(note)];
        if (!(f0_refined > 0.0)) {
          every_partial_placed = false;
          break;
        }
        // Which partial stands here comes from replaying the geometry the mask was
        // built with; guessing it back from the bin's frequency misses the stretch.
        const double f0_built =
            ridge_f0_at(track.ridges[static_cast<size_t>(note)], span.frame_start);
        partials = partial_claims(spec, static_cast<float>(f0_built),
                                  note_geometry(masks, static_cast<size_t>(note)));
        const PartialClaim* partial = claim_over_bin(partials, bin);
        every_partial_placed = partial != nullptr && partial->centre_hz > 0.0f;
        if (!every_partial_placed) break;
        // The centre is linear in the f0 at a fixed harmonic and stretch, so
        // scaling it moves the partial to the refined f0 exactly.
        predicted_hz[static_cast<size_t>(j)] = partial->centre_hz * f0_refined / f0_built;
        predicted_rate[static_cast<size_t>(j)] =
            wrap_to_pi(kTwoPiD * predicted_hz[static_cast<size_t>(j)] * hop / sample_rate);
      }
      if (!every_partial_placed) {
        // The gate's input is missing rather than out of range, so it reports no
        // separation at all instead of one computed from an unrefined f0.
        record_span(SharedBinOutcome::F0NotRefined, 0.0f);
        continue;
      }

      const float separation =
          static_cast<float>(closest_partial_separation(predicted_hz, sample_rate / hop));
      if (separation < config.min_partial_separation) {
        record_span(SharedBinOutcome::PartialsTooClose, separation);
        continue;
      }

      starts.clear();
      const int last_start = span.frame_start + span.n_frames - window;
      for (int s = span.frame_start; s <= last_start; s += step) starts.push_back(s);
      // The step can stop short of the span's end; one more window ending on it
      // keeps the tail from silently keeping the equal split.
      if (starts.back() != last_start) starts.push_back(last_start);

      accumulated.assign(entries, Complexd(0.0, 0.0));
      covered.assign(static_cast<size_t>(span.n_frames), 0);
      reason.assign(static_cast<size_t>(span.n_frames), SharedBinOutcome::FitDiverged);
      residual_at.assign(static_cast<size_t>(span.n_frames), 0.0f);
      decided.assign(static_cast<size_t>(span.n_frames), 0);

      for (const int start : starts) {
        for (int n = 0; n < window; ++n) {
          trajectory[static_cast<size_t>(n)] =
              Complexd(data[static_cast<size_t>(bin) * n_frames + start + n]);
        }

        // The residual stays zero until the fit that measures it has run, so a
        // refusal never reports a misfit it never computed.
        bool solved = false;
        double residual = 0.0;
        SharedBinOutcome outcome = SharedBinOutcome::PolesNotFound;
        if (esprit_poles(trajectory, span.n_notes, poles)) {
          outcome = SharedBinOutcome::FitDiverged;
          double measured = 0.0;
          if (fit_components(trajectory, poles, component, measured)) {
            residual = measured;
            solved = residual <= config.max_fit_residual;
            if (solved) outcome = SharedBinOutcome::Solved;
          }
        }

        // Refused after the fit rather than before it, so the misfit it did
        // measure is reported rather than the zero the earlier refusals carry.
        if (solved &&
            !assign_poles(poles, predicted_rate, pole_distance, permutation, pole_of_note)) {
          solved = false;
          outcome = SharedBinOutcome::AssignmentAmbiguous;
        }

        if (solved) {
          for (int j = 0; j < span.n_notes; ++j) {
            const size_t pole = static_cast<size_t>(pole_of_note[static_cast<size_t>(j)]);
            for (int n = 0; n < window; ++n) {
              const int f = start - span.frame_start + n;
              accumulated[static_cast<size_t>(f) * static_cast<size_t>(span.n_notes) +
                          static_cast<size_t>(j)] +=
                  component[pole * static_cast<size_t>(window) + static_cast<size_t>(n)];
            }
          }
        }

        // A frame takes the first window that solved it, and otherwise the first
        // reason it was refused.
        for (int n = 0; n < window; ++n) {
          const int f = start - span.frame_start + n;
          if (solved) ++covered[static_cast<size_t>(f)];
          if (decided[static_cast<size_t>(f)] == 2) continue;
          if (!solved && decided[static_cast<size_t>(f)] == 1) continue;
          decided[static_cast<size_t>(f)] = solved ? 2 : 1;
          reason[static_cast<size_t>(f)] = outcome;
          residual_at[static_cast<size_t>(f)] = static_cast<float>(residual);
        }
      }

      for (int f = 0; f < span.n_frames; ++f) {
        if (covered[static_cast<size_t>(f)] == 0) {
          record(f, reason[static_cast<size_t>(f)], separation,
                 residual_at[static_cast<size_t>(f)]);
          continue;
        }
        const Complexd observed(data[static_cast<size_t>(bin) * n_frames + span.frame_start + f]);
        // Every weight of the bin is resolved before any is stored: one the mask
        // cannot hold sends the whole bin back, since a bin divided partly by the
        // fit and partly equally is neither division.
        resolved.assign(static_cast<size_t>(span.n_notes), std::complex<float>(0.0f, 0.0f));
        bool storable = true;
        for (int j = 0; j < span.n_notes && storable; ++j) {
          // Components are averaged over the windows covering the frame and only
          // then divided, so an overlap averages the fit and not the ratio.
          const Complexd mean =
              accumulated[static_cast<size_t>(f) * static_cast<size_t>(span.n_notes) +
                          static_cast<size_t>(j)] /
              static_cast<double>(covered[static_cast<size_t>(f)]);
          const std::complex<float> weight =
              clamped_weight(mean / observed, config.max_weight_modulus);
          storable = std::isfinite(weight.real()) && std::isfinite(weight.imag()) &&
                     weight != std::complex<float>(0.0f, 0.0f);
          resolved[static_cast<size_t>(j)] = weight;
        }
        if (!storable) {
          // Both signals read healthy here, so they are reported as they stand.
          record(f, SharedBinOutcome::DegenerateWeight, separation,
                 residual_at[static_cast<size_t>(f)]);
          continue;
        }

        record(f, reason[static_cast<size_t>(f)], separation, residual_at[static_cast<size_t>(f)]);
        for (int j = 0; j < span.n_notes; ++j) {
          const BinClaim& claim =
              claims[span.group_lo + static_cast<size_t>(f) * static_cast<size_t>(span.n_notes) +
                     static_cast<size_t>(j)];
          result.notes[static_cast<size_t>(claim.note)].weights[static_cast<size_t>(claim.entry)] =
              resolved[static_cast<size_t>(j)];
        }
      }
    }
  }

  return result;
}

}  // namespace sonare::editing::polyphony
