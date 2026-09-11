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

/// Appends the bins the partials of @p f0_hz claim, ascending and each once.
/// Counting a bin twice for one note would divide the note's share with itself.
/// @p f0_hz is positive and finite, which the caller checks.
void append_claimed_bins(float f0_hz, const NoteMaskConfig& config, double bins_per_hz,
                         double half_width, int n_bins, std::vector<int32_t>& out) {
  int32_t next = 0;
  for (int harmonic = 1; harmonic <= config.n_harmonics; ++harmonic) {
    const double stretch =
        std::sqrt(1.0 + static_cast<double>(config.inharmonicity) * harmonic * harmonic);
    const double centre = harmonic * static_cast<double>(f0_hz) * stretch * bins_per_hz;
    const double lo = std::ceil(centre - half_width);
    const double hi = std::floor(centre + half_width);
    // Partials ascend, so one past the top claims nothing and neither does any above it.
    if (!(lo < static_cast<double>(n_bins))) break;
    if (!(hi >= 0.0)) continue;
    const int32_t first = std::max(next, static_cast<int32_t>(std::max(lo, 0.0)));
    const int32_t last = static_cast<int32_t>(std::min(hi, static_cast<double>(n_bins - 1)));
    for (int32_t bin = first; bin <= last; ++bin) out.push_back(bin);
    next = std::max(next, last + 1);
  }
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
}

void check_set_shape(const NoteMaskSet& masks) {
  SONARE_CHECK(masks.n_bins > 0 && masks.n_frames > 0, ErrorCode::InvalidParameter);
  for (const NoteMask& mask : masks.notes) {
    check_mask_shape(mask, masks.n_bins, masks.n_frames);
  }
}

/// Sum of every note's weights in the spectrogram's layout; @p masks is checked.
std::vector<float> accumulate_total(const NoteMaskSet& masks) {
  std::vector<float> total(static_cast<size_t>(masks.n_bins) * masks.n_frames, 0.0f);
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

NoteMaskSet build_note_masks(const Spectrogram& spec, const MultiF0Track& track,
                             const NoteMaskConfig& config) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);
  // A partial is a frequency read at one bin spacing over one lobe, so a
  // spectrogram carrying neither is not an input this can read.
  SONARE_CHECK(
      spec.n_fft() > 0 && spec.win_length() > 0 && spec.hop_length() > 0 && spec.sample_rate() > 0,
      ErrorCode::InvalidParameter);
  SONARE_CHECK(track.n_frames == spec.n_frames() && track.hop_length == spec.hop_length() &&
                   track.sample_rate == spec.sample_rate(),
               ErrorCode::InvalidParameter);
  SONARE_CHECK(config.n_harmonics >= 1 && config.n_harmonics <= kMaxHarmonics,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.claim_lobes) && config.claim_lobes > 0.0f &&
                   config.claim_lobes <= kMaxClaimLobes,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(std::isfinite(config.inharmonicity) && config.inharmonicity >= 0.0f,
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

  const int n_bins = spec.n_bins();
  const int n_frames = spec.n_frames();
  const double bins_per_hz = static_cast<double>(spec.n_fft()) / spec.sample_rate();
  // The Hann main lobe is 4 * n_fft / win_length bins wide, so one lobe reaches
  // half of that either side; counting in lobes carries the width across zero
  // padding instead of tying it to one framing.
  const double half_width =
      static_cast<double>(config.claim_lobes) * 2.0 * spec.n_fft() / spec.win_length();

  NoteMaskSet set;
  set.n_bins = n_bins;
  set.n_frames = n_frames;
  set.hop_length = spec.hop_length();
  set.sample_rate = spec.sample_rate();
  set.notes.resize(track.ridges.size());
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
  for (int frame = 0; frame < n_frames; ++frame) {
    // Every note names its bins and counts itself on each before any share is read.
    for (size_t i = 0; i < set.notes.size(); ++i) {
      NoteMask& mask = set.notes[i];
      const int index = frame - mask.frame_start;
      if (index < 0 || index >= mask.n_frames) continue;
      append_claimed_bins(track.ridges[i].f0_hz[static_cast<size_t>(index)], config, bins_per_hz,
                          half_width, n_bins, mask.bins);
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
  SONARE_CHECK(masks.n_bins == spec.n_bins() && masks.n_frames == spec.n_frames(),
               ErrorCode::InvalidParameter);
  check_set_shape(masks);

  const std::vector<float> total = accumulate_total(masks);
  const std::complex<float>* data = spec.complex_data();
  std::vector<std::complex<float>> residual(total.size());
  // Unclamped: the notes and this add back to the input, and a clamp here would
  // hide a total over one rather than let the identity fail on it.
  for (size_t at = 0; at < total.size(); ++at) residual[at] = data[at] * (1.0f - total[at]);
  return scaled_like(spec, std::move(residual));
}

std::vector<float> mask_total(const NoteMaskSet& masks) {
  check_set_shape(masks);
  return accumulate_total(masks);
}

}  // namespace sonare::editing::polyphony
