#include "editing/polyphony/note_mask.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/spectrum.h"
#include "editing/polyphony/multi_f0.h"
#include "util/exception.h"

namespace sonare::editing::polyphony {
namespace {

/// The salience kernel's own bound: partial 128 of any F0 an STFT resolves is
/// already past audio.
constexpr int kMaxHarmonics = 128;

/// Past this a claim spans the whole spectrum at every framing, so the width is
/// sizing a loop rather than describing a window.
constexpr float kMaxClaimLobes = 64.0f;

/// Half a claim's width, in bins. The Hann main lobe is 4 * n_fft / win_length
/// bins wide, so one lobe reaches half of that either side; counting in lobes
/// carries the width across zero padding instead of tying it to one framing.
double claim_half_width(const Spectrogram& spec, const NoteMaskConfig& config) {
  return static_cast<double>(config.claim_lobes) * 2.0 * spec.n_fft() / spec.win_length();
}

/// The claim geometry, and the only place it is derived. @p f0_hz is positive
/// and finite and @p config is in range, both of which the caller checks.
/// @details Ranges come out ascending and disjoint: a bin counted twice for one
///          note would divide that note's share with itself, so where two
///          partials overlap the lower one keeps the shared bins and the upper
///          starts after them. A partial left with nothing is not returned.
void place_claims(float f0_hz, const NoteMaskConfig& config, double bins_per_hz, double half_width,
                  int n_bins, std::vector<PartialClaim>& out) {
  out.clear();
  // A real signal has nothing above Nyquist, so a partial centred past it would
  // claim bins that cannot hold this note's content. Partial frequency is monotone
  // in the harmonic, so the first one past it takes the rest of the series with it.
  const double nyquist_bin = n_bins - 1;
  int32_t next = 0;
  for (int harmonic = 1; harmonic <= config.n_harmonics; ++harmonic) {
    const double stretch =
        std::sqrt(1.0 + static_cast<double>(config.inharmonicity) * harmonic * harmonic);
    const double centre_hz = harmonic * static_cast<double>(f0_hz) * stretch;
    const double centre = centre_hz * bins_per_hz;
    if (!(centre <= nyquist_bin)) break;
    // The width is clamped where the centre is not: a partial at or under Nyquist
    // keeps the part of its claim that fits.
    const double lo = std::ceil(centre - half_width);
    const double hi = std::floor(centre + half_width);
    const int32_t first = std::max(next, static_cast<int32_t>(std::max(lo, 0.0)));
    const int32_t last = static_cast<int32_t>(std::min(hi, nyquist_bin));
    next = std::max(next, last + 1);
    if (first > last) continue;
    out.push_back(PartialClaim{harmonic, static_cast<float>(centre_hz), static_cast<int>(first),
                               static_cast<int>(last)});
  }
}

/// The framing and geometry checks @ref build_note_masks and @ref partial_claims
/// share, so one cannot drift into accepting what the other refuses.
void check_geometry(const Spectrogram& spec, const NoteMaskConfig& config) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);
  // A partial is a frequency read at one bin spacing over one lobe, so a
  // spectrogram carrying neither is not an input this can read.
  SONARE_CHECK(
      spec.n_fft() > 0 && spec.win_length() > 0 && spec.hop_length() > 0 && spec.sample_rate() > 0,
      ErrorCode::InvalidParameter);
  SONARE_CHECK(config.n_harmonics >= 1 && config.n_harmonics <= kMaxHarmonics,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.claim_lobes) && config.claim_lobes > 0.0f &&
                   config.claim_lobes <= kMaxClaimLobes,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.inharmonicity) && config.inharmonicity >= 0.0f,
               ErrorCode::InvalidParameter);
}

/// Every bin and frame a mask names lands inside a @p n_bins x @p n_frames
/// spectrum, and its own sparse indexing is consistent.
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
  // Zero or non-finite is not a shape error and would leave the total and the
  // residual wrong with no call having failed, which is worse than a rejection.
  // An estimated weight carries a phase and is not bounded above by one.
  for (const std::complex<float>& weight : mask.weights) {
    SONARE_CHECK(std::isfinite(weight.real()) && std::isfinite(weight.imag()) &&
                     weight != std::complex<float>(0.0f, 0.0f),
                 ErrorCode::InvalidParameter);
  }
}

void check_set_shape(const NoteMaskSet& masks) {
  SONARE_CHECK(masks.n_bins > 0 && masks.n_frames > 0, ErrorCode::InvalidParameter);
  for (const NoteMask& mask : masks.notes) {
    check_mask_shape(mask, masks.n_bins, masks.n_frames);
  }
}

/// Sum of every note's weights in the spectrogram's layout; @p masks is checked.
std::vector<std::complex<float>> accumulate_total(const NoteMaskSet& masks) {
  std::vector<std::complex<float>> total(static_cast<size_t>(masks.n_bins) * masks.n_frames,
                                         std::complex<float>(0.0f, 0.0f));
  for (const NoteMask& mask : masks.notes) {
    for (int index = 0; index < mask.n_frames; ++index) {
      const int frame = mask.frame_start + index;
      for (int32_t k = mask.frame_offset[static_cast<size_t>(index)];
           k < mask.frame_offset[static_cast<size_t>(index) + 1]; ++k) {
        const size_t at = static_cast<size_t>(mask.bins[static_cast<size_t>(k)]) * masks.n_frames +
                          static_cast<size_t>(frame);
        total[at] += mask.weights[static_cast<size_t>(k)];
      }
    }
  }
  return total;
}

/// @p spec scaled bin by bin, carrying its framing so the result reconstructs
/// with the same normalization as its source.
Spectrogram scaled_like(const Spectrogram& spec, std::vector<std::complex<float>> data) {
  return Spectrogram::from_complex(data.data(), spec.n_bins(), spec.n_frames(), spec.n_fft(),
                                   spec.hop_length(), spec.sample_rate(), spec.window(),
                                   spec.center(), spec.win_length());
}

}  // namespace

std::vector<PartialClaim> partial_claims(const Spectrogram& spec, float f0_hz,
                                         const NoteMaskConfig& config) {
  check_geometry(spec, config);
  SONARE_CHECK(std::isfinite(f0_hz) && f0_hz > 0.0f, ErrorCode::InvalidParameter);

  std::vector<PartialClaim> claims;
  place_claims(f0_hz, config, static_cast<double>(spec.n_fft()) / spec.sample_rate(),
               claim_half_width(spec, config), spec.n_bins(), claims);
  return claims;
}

NoteMaskSet build_note_masks(const Spectrogram& spec, const MultiF0Track& track,
                             const NoteMaskConfig& config) {
  return build_note_masks(spec, track, config, std::vector<float>{});
}

NoteMaskSet build_note_masks(const Spectrogram& spec, const MultiF0Track& track,
                             const NoteMaskConfig& config,
                             const std::vector<float>& per_ridge_stretch) {
  check_geometry(spec, config);
  SONARE_CHECK(track.n_frames == spec.n_frames() && track.hop_length == spec.hop_length() &&
                   track.sample_rate == spec.sample_rate(),
               ErrorCode::InvalidParameter);
  for (const F0Ridge& ridge : track.ridges) {
    SONARE_CHECK(ridge.frame_start >= 0 && ridge.frame_end() <= spec.n_frames(),
                 ErrorCode::InvalidParameter);
    // Rejected here as track_f0_ridges rejects it: a value one entry point
    // refuses and another absorbs is where a validation gap hides.
    for (const float f0_hz : ridge.f0_hz) {
      SONARE_CHECK(std::isfinite(f0_hz) && f0_hz > 0.0f, ErrorCode::InvalidParameter);
    }
  }
  SONARE_CHECK_MSG(per_ridge_stretch.empty() || per_ridge_stretch.size() == track.ridges.size(),
                   ErrorCode::InvalidParameter,
                   "build_note_masks: perRidgeStretch must be empty or one value per ridge");
  for (const float stretch : per_ridge_stretch) {
    // Only a negative number carries the refusal; a NaN passes every comparison
    // that would have caught it and reaches the geometry as a stretch.
    SONARE_CHECK_MSG(std::isfinite(stretch), ErrorCode::InvalidParameter,
                     "build_note_masks: perRidgeStretch must be finite");
  }

  const int n_bins = spec.n_bins();
  const int n_frames = spec.n_frames();
  const double bins_per_hz = static_cast<double>(spec.n_fft()) / spec.sample_rate();
  const double half_width = claim_half_width(spec, config);

  NoteMaskSet set;
  set.n_bins = n_bins;
  set.n_frames = n_frames;
  set.hop_length = spec.hop_length();
  set.sample_rate = spec.sample_rate();
  // Carried so a later stage can tell which partial stands on a bin without
  // guessing a harmonic number back out of the bin's frequency.
  set.config = config;
  set.notes.resize(track.ridges.size());
  // One config per ridge, differing only in the stretch, so place_claims stays the
  // single derivation. The effective value is stored rather than the argument: a
  // negative entry is the caller's refusal and the geometry used the declared
  // value, which is the number a replay has to read back.
  std::vector<NoteMaskConfig> geometry(track.ridges.size(), config);
  if (!per_ridge_stretch.empty()) {
    set.inharmonicity.resize(track.ridges.size());
    for (size_t i = 0; i < track.ridges.size(); ++i) {
      if (per_ridge_stretch[i] >= 0.0f) geometry[i].inharmonicity = per_ridge_stretch[i];
      set.inharmonicity[i] = geometry[i].inharmonicity;
    }
  }
  for (size_t i = 0; i < track.ridges.size(); ++i) {
    NoteMask& mask = set.notes[i];
    mask.ridge_index = static_cast<int>(i);
    mask.frame_start = track.ridges[i].frame_start;
    mask.n_frames = static_cast<int>(track.ridges[i].f0_hz.size());
    mask.frame_offset.assign(static_cast<size_t>(mask.n_frames) + 1, 0);
  }

  // One scratch for the run; the bins touched are listed so clearing it costs
  // the claims rather than the spectrum.
  std::vector<int> claims(static_cast<size_t>(n_bins), 0);
  std::vector<int32_t> touched;
  std::vector<PartialClaim> partials;
  for (int frame = 0; frame < n_frames; ++frame) {
    // Every note names its bins and counts itself on each before any share is read.
    for (size_t i = 0; i < set.notes.size(); ++i) {
      NoteMask& mask = set.notes[i];
      const int index = frame - mask.frame_start;
      if (index < 0 || index >= mask.n_frames) continue;
      place_claims(track.ridges[i].f0_hz[static_cast<size_t>(index)], geometry[i], bins_per_hz,
                   half_width, n_bins, partials);
      for (const PartialClaim& partial : partials) {
        for (int bin = partial.first_bin; bin <= partial.last_bin; ++bin) {
          mask.bins.push_back(static_cast<int32_t>(bin));
        }
      }
      mask.frame_offset[static_cast<size_t>(index) + 1] = static_cast<int32_t>(mask.bins.size());
      for (int32_t k = mask.frame_offset[static_cast<size_t>(index)];
           k < mask.frame_offset[static_cast<size_t>(index) + 1]; ++k) {
        const int32_t bin = mask.bins[static_cast<size_t>(k)];
        if (claims[static_cast<size_t>(bin)]++ == 0) touched.push_back(bin);
      }
    }
    // A bin claimed by n notes gives each of them 1/n of it.
    for (NoteMask& mask : set.notes) {
      const int index = frame - mask.frame_start;
      if (index < 0 || index >= mask.n_frames) continue;
      for (int32_t k = mask.frame_offset[static_cast<size_t>(index)];
           k < mask.frame_offset[static_cast<size_t>(index) + 1]; ++k) {
        const int32_t bin = mask.bins[static_cast<size_t>(k)];
        mask.weights.push_back(1.0f / static_cast<float>(claims[static_cast<size_t>(bin)]));
      }
    }
    for (const int32_t bin : touched) claims[static_cast<size_t>(bin)] = 0;
    touched.clear();
  }

  return set;
}

Spectrogram apply_note_mask(const Spectrogram& spec, const NoteMask& mask) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);
  check_mask_shape(mask, spec.n_bins(), spec.n_frames());

  const int n_frames = spec.n_frames();
  const std::complex<float>* data = spec.complex_data();
  std::vector<std::complex<float>> masked(static_cast<size_t>(spec.n_bins()) * n_frames);
  for (int index = 0; index < mask.n_frames; ++index) {
    const int frame = mask.frame_start + index;
    for (int32_t k = mask.frame_offset[static_cast<size_t>(index)];
         k < mask.frame_offset[static_cast<size_t>(index) + 1]; ++k) {
      const size_t at = static_cast<size_t>(mask.bins[static_cast<size_t>(k)]) * n_frames +
                        static_cast<size_t>(frame);
      masked[at] = data[at] * mask.weights[static_cast<size_t>(k)];
    }
  }
  return scaled_like(spec, std::move(masked));
}

Spectrogram residual_spectrum(const Spectrogram& spec, const NoteMaskSet& masks) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);
  // The hop and the rate as well as the sizes: a set from another framing indexes
  // the same array while meaning different times.
  SONARE_CHECK(masks.n_bins == spec.n_bins() && masks.n_frames == spec.n_frames() &&
                   masks.hop_length == spec.hop_length() && masks.sample_rate == spec.sample_rate(),
               ErrorCode::InvalidParameter);
  check_set_shape(masks);

  const std::vector<std::complex<float>> total = accumulate_total(masks);
  const std::complex<float>* data = spec.complex_data();
  std::vector<std::complex<float>> residual(total.size());
  // Unclamped: the notes and this add back to the input, and a clamp here would
  // hide a total over one rather than let the identity fail on it.
  for (size_t at = 0; at < total.size(); ++at) residual[at] = data[at] * (1.0f - total[at]);
  return scaled_like(spec, std::move(residual));
}

std::vector<std::complex<float>> mask_total(const NoteMaskSet& masks) {
  check_set_shape(masks);
  return accumulate_total(masks);
}

}  // namespace sonare::editing::polyphony
