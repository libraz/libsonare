#include "editing/polyphony/note_mask.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/spectrum.h"
#include "editing/polyphony/multi_f0.h"
#include "editing/polyphony/private_helpers.h"
#include "util/exception.h"
#include "util/parameter_checks.h"

namespace sonare::editing::polyphony {
namespace {

namespace check = sonare::util::check;

/// Past this a claim spans the whole spectrum at every framing, so the width is
/// sizing a loop rather than describing a window.
constexpr float kMaxClaimLobes = 64.0f;

/// Half a claim's width, in bins. The Hann main lobe is 4 * n_fft / win_length
/// bins wide, so one lobe reaches half of that either side; counting in lobes
/// carries the width across zero padding instead of tying it to one framing.
double claim_half_width(const StftGeometry& geometry, const NoteMaskConfig& config) {
  return static_cast<double>(config.claim_lobes) * 2.0 * geometry.n_fft / geometry.win_length;
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
void check_geometry(const StftGeometry& geometry, const NoteMaskConfig& config,
                    const std::string& geometry_subject) {
  if (geometry.empty()) {
    check::refuse(geometry_subject.c_str(), "must not be empty");
  }
  // A partial is a frequency read at one bin spacing over one lobe, so a
  // spectrogram carrying neither is not an input this can read.
  check::integer_at_least(geometry.n_fft, 1, (geometry_subject + ".n_fft").c_str());
  check::integer_at_least(geometry.win_length, 1, (geometry_subject + ".win_length").c_str());
  check::integer_at_least(geometry.hop_length, 1, (geometry_subject + ".hop_length").c_str());
  check::integer_at_least(geometry.sample_rate, 1, (geometry_subject + ".sample_rate").c_str());
  check::integer_in_range(config.n_harmonics, 1, kMaxNoteMaskHarmonics, "config.n_harmonics");
  check::in_interval(config.claim_lobes, 0.0, false, kMaxClaimLobes, true, "config.claim_lobes");
  check::at_least(config.inharmonicity, 0.0, "config.inharmonicity");
}

/// Every bin and frame a mask names lands inside a @p n_bins x @p n_frames
/// spectrum, and its own sparse indexing is consistent.
void check_mask_shape(const NoteMask& mask, int n_bins, int n_frames, const std::string& subject,
                      const std::string& frame_count_subject) {
  detail::check_mask_shape(mask, n_bins, n_frames, subject, frame_count_subject);
  // Zero or non-finite is not a shape error and would leave the total and the
  // residual wrong with no call having failed, which is worse than a rejection.
  // An estimated weight carries a phase and is not bounded above by one.
  for (size_t i = 0; i < mask.weights.size(); ++i) {
    const std::complex<float>& weight = mask.weights[i];
    if (!(std::isfinite(weight.real()) && std::isfinite(weight.imag()) &&
          weight != std::complex<float>(0.0f, 0.0f))) {
      check::refuse((subject + ".weights[" + std::to_string(i) + "]").c_str(),
                    "must be finite and nonzero");
    }
  }
}

void check_set_shape(const NoteMaskSet& masks) {
  check::integer_at_least(masks.n_bins, 1, "masks.n_bins");
  check::integer_at_least(masks.n_frames, 1, "masks.n_frames");
  for (size_t i = 0; i < masks.notes.size(); ++i) {
    const std::string subject = "masks.notes[" + std::to_string(i) + "]";
    check_mask_shape(masks.notes[i], masks.n_bins, masks.n_frames, subject, "masks.n_frames");
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
  return Spectrogram::from_complex(std::move(data), spec.n_bins(), spec.n_frames(), spec.n_fft(),
                                   spec.hop_length(), spec.sample_rate(), spec.window(),
                                   spec.center(), spec.win_length());
}

}  // namespace

std::vector<PartialClaim> partial_claims(const Spectrogram& spec, float f0_hz,
                                         const NoteMaskConfig& config) {
  const StftGeometry geometry = geometry_of(spec);
  if (geometry.empty()) check::refuse("spec", "must not be empty");
  return partial_claims(geometry, f0_hz, config);
}

std::vector<PartialClaim> partial_claims(const StftGeometry& geometry, float f0_hz,
                                         const NoteMaskConfig& config) {
  check_geometry(geometry, config, "geometry");
  check::greater_than(f0_hz, 0.0, "f0_hz");

  std::vector<PartialClaim> claims;
  place_claims(f0_hz, config, static_cast<double>(geometry.n_fft) / geometry.sample_rate,
               claim_half_width(geometry, config), geometry.n_bins, claims);
  return claims;
}

NoteMaskSet build_note_masks(const Spectrogram& spec, const MultiF0Track& track,
                             const NoteMaskConfig& config) {
  return build_note_masks(spec, track, config, std::vector<float>{});
}

NoteMaskSet build_note_masks(const Spectrogram& spec, const MultiF0Track& track,
                             const NoteMaskConfig& config,
                             const std::vector<float>& per_ridge_stretch) {
  const StftGeometry geometry = geometry_of(spec);
  if (geometry.empty()) check::refuse("spec", "must not be empty");
  try {
    return build_note_masks(geometry, track, config, per_ridge_stretch);
  } catch (const SonareParameterRefusal& e) {
    throw e.respelled(
        e.code(),
        [](const std::string& subject) {
          return subject.compare(0, 9, "geometry.") == 0 ? "spec" + subject.substr(8) : subject;
        },
        e.caller_spelled());
  }
}

NoteMaskSet build_note_masks(const StftGeometry& geometry, const MultiF0Track& track,
                             const NoteMaskConfig& config) {
  return build_note_masks(geometry, track, config, std::vector<float>{});
}

NoteMaskSet build_note_masks(const StftGeometry& geometry, const MultiF0Track& track,
                             const NoteMaskConfig& config,
                             const std::vector<float>& per_ridge_stretch) {
  check_geometry(geometry, config, "geometry");
  // The track and its framing are caller inputs; each mismatch names the two
  // fields whose values disagree.
  if (track.n_frames != geometry.n_frames) {
    check::refuse_relation({"track.n_frames", "geometry.n_frames"}, "{0} must equal {1}");
  }
  if (track.hop_length != geometry.hop_length) {
    check::refuse_relation({"track.hop_length", "geometry.hop_length"}, "{0} must equal {1}");
  }
  if (track.sample_rate != geometry.sample_rate) {
    check::refuse_relation({"track.sample_rate", "geometry.sample_rate"}, "{0} must equal {1}");
  }
  for (size_t i = 0; i < track.ridges.size(); ++i) {
    const F0Ridge& ridge = track.ridges[i];
    const std::string ridge_path = "track.ridges[" + std::to_string(i) + "]";
    check::integer_at_least(ridge.frame_start, 0, (ridge_path + ".frame_start").c_str());
    if (ridge.frame_end() > geometry.n_frames) {
      check::refuse_relation(
          {ridge_path + ".frame_start", ridge_path + ".f0_hz", "geometry.n_frames"},
          "{0} plus the length of {1} must be at most {2}");
    }
    // Rejected here as track_f0_ridges rejects it: a value one entry point
    // refuses and another absorbs is where a validation gap hides.
    for (size_t j = 0; j < ridge.f0_hz.size(); ++j) {
      const float hz = ridge.f0_hz[j];
      if (!(std::isfinite(hz) && hz > 0.0f)) {
        check::refuse_outside(check::real_domain_above(0.0, false),
                              (ridge_path + ".f0_hz[" + std::to_string(j) + "]").c_str());
      }
    }
  }
  SONARE_CHECK_MSG(per_ridge_stretch.empty() || per_ridge_stretch.size() == track.ridges.size(),
                   ErrorCode::InvalidParameter,
                   "perRidgeStretch must be empty or one value per ridge");
  for (const float stretch : per_ridge_stretch) {
    // Only a negative number carries the refusal; a NaN passes every comparison
    // that would have caught it and reaches the geometry as a stretch.
    SONARE_CHECK_MSG(std::isfinite(stretch), ErrorCode::InvalidParameter,
                     "perRidgeStretch must be finite");
  }

  const int n_bins = geometry.n_bins;
  const int n_frames = geometry.n_frames;
  const double bins_per_hz = static_cast<double>(geometry.n_fft) / geometry.sample_rate;
  const double half_width = claim_half_width(geometry, config);

  NoteMaskSet set;
  set.n_bins = n_bins;
  set.n_frames = n_frames;
  set.hop_length = geometry.hop_length;
  set.sample_rate = geometry.sample_rate;
  // Carried so a later stage can tell which partial stands on a bin without
  // guessing a harmonic number back out of the bin's frequency.
  set.config = config;
  set.notes.resize(track.ridges.size());
  // One config per ridge, differing only in the stretch, so place_claims stays the
  // single derivation. The effective value is stored rather than the argument: a
  // negative entry is the caller's refusal and the geometry used the declared
  // value, which is the number a replay has to read back.
  std::vector<NoteMaskConfig> per_ridge(track.ridges.size(), config);
  if (!per_ridge_stretch.empty()) {
    set.inharmonicity.resize(track.ridges.size());
    for (size_t i = 0; i < track.ridges.size(); ++i) {
      if (per_ridge_stretch[i] >= 0.0f) per_ridge[i].inharmonicity = per_ridge_stretch[i];
      set.inharmonicity[i] = per_ridge[i].inharmonicity;
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
  detail::ActiveMasks active(set.notes);
  for (int frame = 0; frame < n_frames; ++frame) {
    const std::vector<size_t>& live = active.advance(frame);
    // Every note names its bins and counts itself on each before any share is read.
    for (const size_t i : live) {
      NoteMask& mask = set.notes[i];
      const int index = frame - mask.frame_start;
      place_claims(track.ridges[i].f0_hz[static_cast<size_t>(index)], per_ridge[i], bins_per_hz,
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
    for (const size_t i : live) {
      NoteMask& mask = set.notes[i];
      const int index = frame - mask.frame_start;
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
  if (spec.empty()) check::refuse("spec", "must not be empty");
  check_mask_shape(mask, spec.n_bins(), spec.n_frames(), "mask", "spec.n_frames");

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

namespace {

/// The inverse over the frames a mask reaches; @p cell_at(k, bin, frame) is the
/// spectrogram at mask entry @p k, which sits at (@p bin, @p frame).
template <typename CellAt>
NoteSegment invert_cells(const StftGeometry& geometry, const NoteMask& mask, int length,
                         const std::string& geometry_subject, const std::string& mask_subject,
                         const std::string& frame_count_subject, const CellAt& cell_at) {
  if (geometry.empty()) {
    check::refuse(geometry_subject.c_str(), "must not be empty");
  }
  check_mask_shape(mask, geometry.n_bins, geometry.n_frames, mask_subject, frame_count_subject);
  check::integer_at_least(length, 0, "length");

  const int n_bins = geometry.n_bins;
  const int n_frames = geometry.n_frames;
  const int n_fft = geometry.n_fft;
  const int hop = geometry.hop_length;
  // Every frame whose window overlaps a masked frame's, so each sample the note
  // reaches sums the same frames the full-length inverse does.
  const int reach = (n_fft + hop - 1) / hop;
  const int first = std::max(0, mask.frame_start - reach);
  const int last = std::min(n_frames, mask.frame_start + mask.n_frames + reach);
  const int span = last - first;

  std::vector<std::complex<float>> masked(static_cast<size_t>(n_bins) * span);
  for (int index = 0; index < mask.n_frames; ++index) {
    const int frame = mask.frame_start + index;
    for (int32_t k = mask.frame_offset[static_cast<size_t>(index)];
         k < mask.frame_offset[static_cast<size_t>(index) + 1]; ++k) {
      const size_t bin = static_cast<size_t>(mask.bins[static_cast<size_t>(k)]);
      masked[bin * span + static_cast<size_t>(frame - first)] =
          cell_at(static_cast<size_t>(k), bin, static_cast<size_t>(frame)) *
          mask.weights[static_cast<size_t>(k)];
    }
  }
  const Spectrogram part =
      Spectrogram::from_complex(std::move(masked), n_bins, span, n_fft, hop, geometry.sample_rate,
                                geometry.window, geometry.center, geometry.win_length);

  // Centred or not, output position j of the part is input position first*hop + j.
  const int64_t first_sample = static_cast<int64_t>(first) * hop;
  const int64_t trim = geometry.center ? n_fft / 2 : 0;
  const int64_t whole =
      length > 0 ? length : static_cast<int64_t>(n_frames - 1) * hop + n_fft - 2 * trim;
  // Short of the last frame the part ends where its own frames do; reaching it,
  // the part runs to the length as the zero-padded full inverse does.
  const int64_t part_natural = static_cast<int64_t>(span - 1) * hop + n_fft - trim;
  const int64_t available = last < n_frames ? part_natural : whole - first_sample;
  // A length ending before the part leaves `available` negative; that is an empty count.
  const int64_t count =
      std::clamp<int64_t>(whole - first_sample, 0, std::max<int64_t>(available, 0));
  if (count == 0) {
    return {Audio::from_vector({}, geometry.sample_rate), first_sample};
  }
  return {part.to_audio(static_cast<int>(count)), first_sample};
}

}  // namespace

NoteSegment invert_note_mask(const Spectrogram& spec, const NoteMask& mask, int length) {
  const StftGeometry geometry = geometry_of(spec);
  if (geometry.empty()) check::refuse("spec", "must not be empty");
  const std::complex<float>* data = spec.complex_data();
  const auto n_frames = static_cast<size_t>(spec.n_frames());
  return invert_cells(
      geometry, mask, length, "spec", "mask", "spec.n_frames",
      [&](size_t, size_t bin, size_t frame) { return data[bin * n_frames + frame]; });
}

NoteSegment invert_note_mask(const MaskedCells& cells, const NoteMaskSet& masks, std::size_t note,
                             int length) {
  if (!(note < masks.notes.size() && note < cells.notes.size())) {
    check::refuse("note", "must name a note in both masks.notes and cells.notes");
  }
  const NoteMask& mask = masks.notes[note];
  const std::vector<std::complex<float>>& values = cells.notes[note];
  if (values.size() != mask.bins.size()) {
    check::refuse_relation({"cells.notes[" + std::to_string(note) + "]",
                            "masks.notes[" + std::to_string(note) + "].bins"},
                           "{0} must have the same number of entries as {1}");
  }
  const std::string mask_subject = "masks.notes[" + std::to_string(note) + "]";
  return invert_cells(cells.geometry, mask, length, "cells.geometry", mask_subject,
                      "cells.geometry.n_frames",
                      [&](size_t k, size_t, size_t) { return values[k]; });
}

MaskedCells read_masked_cells(const Audio& audio, const StftConfig& stft,
                              const NoteMaskSet& masks) {
  MaskedCells cells;
  cells.geometry = stft_geometry(stft, audio.sample_rate(), audio.size());
  if (masks.n_bins != cells.geometry.n_bins) {
    check::refuse_relation({"masks.n_bins", "stft.n_fft"}, "{0} must equal {1} / 2 + 1");
  }
  if (masks.n_frames != cells.geometry.n_frames) {
    check::refuse_relation({"masks.n_frames", "audio", "stft"},
                           "{0} must describe the frame count from {1} with {2}");
  }
  if (masks.hop_length != cells.geometry.hop_length) {
    check::refuse_relation({"masks.hop_length", "stft.hop_length"}, "{0} must equal {1}");
  }
  if (masks.sample_rate != cells.geometry.sample_rate) {
    check::refuse_relation({"masks.sample_rate", "audio.sample_rate"}, "{0} must equal {1}");
  }
  check_set_shape(masks);

  cells.notes.resize(masks.notes.size());
  for (size_t i = 0; i < masks.notes.size(); ++i) {
    cells.notes[i].resize(masks.notes[i].bins.size());
  }
  for_each_stft_tile(audio, stft, 0, [&](const Spectrogram& tile, int first_frame, int) {
    const int tile_frames = tile.n_frames();
    const int tile_end = first_frame + tile_frames;
    const std::complex<float>* data = tile.complex_data();
    for (size_t i = 0; i < masks.notes.size(); ++i) {
      const NoteMask& mask = masks.notes[i];
      const int from = std::max(mask.frame_start, first_frame);
      const int to = std::min(mask.frame_end(), tile_end);
      for (int frame = from; frame < to; ++frame) {
        const auto index = static_cast<size_t>(frame - mask.frame_start);
        for (int32_t k = mask.frame_offset[index]; k < mask.frame_offset[index + 1]; ++k) {
          const auto bin = static_cast<size_t>(mask.bins[static_cast<size_t>(k)]);
          cells.notes[i][static_cast<size_t>(k)] = data[bin * static_cast<size_t>(tile_frames) +
                                                        static_cast<size_t>(frame - first_frame)];
        }
      }
    }
  });
  return cells;
}

Spectrogram residual_spectrum(const Spectrogram& spec, const NoteMaskSet& masks) {
  if (spec.empty()) check::refuse("spec", "must not be empty");
  // The hop and the rate as well as the sizes: a set from another framing indexes
  // the same array while meaning different times.
  if (masks.n_bins != spec.n_bins()) {
    check::refuse_relation({"masks.n_bins", "spec.n_bins"}, "{0} must equal {1}");
  }
  if (masks.n_frames != spec.n_frames()) {
    check::refuse_relation({"masks.n_frames", "spec.n_frames"}, "{0} must equal {1}");
  }
  if (masks.hop_length != spec.hop_length()) {
    check::refuse_relation({"masks.hop_length", "spec.hop_length"}, "{0} must equal {1}");
  }
  if (masks.sample_rate != spec.sample_rate()) {
    check::refuse_relation({"masks.sample_rate", "spec.sample_rate"}, "{0} must equal {1}");
  }
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
