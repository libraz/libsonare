#include "editing/note_model/note_state_decoder.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "util/constants.h"
#include "util/exception.h"
#include "util/peak.h"

namespace sonare::editing::note_model {

using constants::kCentsPerOctave;
using constants::kCentsPerSemitone;
using constants::kEpsilon;
using constants::kPiD;
using constants::kSilenceDb;

namespace {

/// Pitch bin width: a third of a semitone.
constexpr double kBinCents = static_cast<double>(kCentsPerSemitone) / 3.0;
/// Frame period every per-frame cost is stated at: a 512-sample hop at 44.1 kHz.
constexpr double kReferenceFramePeriodSec = 512.0 / 44100.0;
/// Score of a state no path has reached.
constexpr double kUnreachable = -1e18;

// Held fixed while the cross-validated grid below was searched on development recordings.
/// Cauchy width of the Attack pitch density (cents).
constexpr double kAttackSigmaCents = 300.0;
/// Mean dwell of the Silent state (s).
constexpr double kSilenceDwellSec = 1.0;
/// Mean dwell of an Attack state (s).
constexpr double kAttackDwellSec = 0.03;
/// Share of a Stable exit that goes straight to another note rather than to silence.
constexpr double kNoteChangeShare = 0.3;
/// Log cost of entering a note, the same for every target bin.
constexpr double kLogEntry = -2.0;
/// Extra log cost of re-entering the bin a note is leaving.
constexpr double kLogSameBin = -6.0;
/// Log emission of an Attack or Stable state on an unvoiced frame, per reference period.
constexpr double kLogUnvoicedNote = -9.0;
/// Half-width of the window in which a valley supports a frame (s).
constexpr double kCueWindowSec = 0.025;
/// A frame this far below its local level reference counts as unvoiced (dB).
constexpr double kLevelFloorDb = 40.0;
/// Radius of the sliding level reference (s).
constexpr double kLevelRadiusSec = 1.0;
/// A level event under this fraction of the next-highest does not set the reference.
constexpr float kLevelLoneRatio = 0.3f;
/// Minimum spacing between RMS events of the level reference (s).
constexpr double kLevelEventSpacingSec = 0.05;

// Frozen by a 5-fold cross-validated grid on development recordings, restricted to values
// that also segment every synthetic regression take correctly; the silence emission sits at
// the edge of that feasible region.
/// Stable pitch width per cent of segmentation threshold (70 cents at the default 50).
constexpr double kStableSigmaPerThreshold = 1.4;
/// Width the Stable density is normalised at, whatever width the threshold gives (cents).
constexpr double kStableNormSigmaCents = 90.0;
/// Mean dwell of a Stable state (s).
constexpr double kStableDwellSec = 0.6;
/// Log emission of the Silent state on a voiced frame, per reference period.
constexpr double kLogVoicedSilence = -9.0;
/// Bonus on a same-bin re-entry at a frame a valley supports.
constexpr double kCueBonus = 16.0;

// Narrow-valley detector: these split repeated same-pitch strikes in the synthetic regression
// takes at every gain tried, while an 8 Hz amplitude-modulation trough fails the width test.
/// Spacing of the RMS grid (ms).
constexpr double kValleyGridMs = 1.0;
/// RMS window length in periods of the run's median F0.
constexpr double kValleyWindowPeriods = 2.0;
/// Span on either side a valley's crest is taken over (ms).
constexpr double kValleyContextMs = 20.0;
/// A valley lies under this fraction of the lower crest.
constexpr double kValleyDepthRatio = 0.5;
/// The low stretch around a valley is under this fraction of each side's crest.
constexpr double kValleyLowRatio = 0.7;
/// Widest low stretch still counted as a valley (ms).
constexpr double kValleyMaxWidthMs = 15.0;
/// Lowest crest a valley is measured against.
constexpr double kValleyCrestFloor = 1e-6;

enum class Kind : std::int8_t { kSilent, kAttack, kStable };

struct PathState {
  Kind kind;
  int bin;
};

/// Index of the first maximum of @p values.
std::size_t first_max(const std::vector<double>& values) {
  std::size_t best = 0;
  for (std::size_t i = 1; i < values.size(); ++i) {
    if (values[i] > values[best]) best = i;
  }
  return best;
}

double self_loop(double dwell_sec, double h) { return -h / dwell_sec; }

double leave(double dwell_sec) {
  return std::log1p(-std::exp(-kReferenceFramePeriodSec / dwell_sec));
}

/// Voicing per frame: a positive finite F0 at a level near the local reference.
std::vector<bool> usable_voicing(const pitch_editor::F0Track& track,
                                 const std::vector<float>& rms_in, double h) {
  const std::size_t n = rms_in.size();
  std::vector<float> rms(n);
  for (std::size_t i = 0; i < n; ++i) rms[i] = std::isfinite(rms_in[i]) ? rms_in[i] : 0.0f;

  std::vector<int> peaks;
  for (std::size_t i = 1; i + 1 < n; ++i) {
    if (rms[i] > rms[i - 1] && rms[i] > rms[i + 1]) peaks.push_back(static_cast<int>(i));
  }
  // Half-to-even rounding, as the reference model rounds.
  const int spacing = std::max(1, static_cast<int>(std::nearbyint(kLevelEventSpacingSec / h)));
  const auto radius = static_cast<std::size_t>(std::max(1.0, std::nearbyint(kLevelRadiusSec / h)));
  const std::vector<int> events = select_peaks_min_distance(peaks, rms.data(), spacing);
  const std::vector<float> reference =
      sliding_max_without_lone_peaks(rms.data(), n, radius, events, kLevelLoneRatio);

  const double epsilon = static_cast<double>(kEpsilon);
  std::vector<bool> voiced(n, false);
  for (std::size_t t = 0; t < n; ++t) {
    const float hz = track.f0_hz[t];
    if (!track.voiced[t] || !std::isfinite(hz) || hz <= 0.0f) continue;
    const double level = 20.0 * std::log10(std::max(static_cast<double>(rms[t]), epsilon));
    const double ref_db = 20.0 * std::log10(std::max(static_cast<double>(reference[t]), epsilon));
    voiced[t] = level >= ref_db - kLevelFloorDb && level >= static_cast<double>(kSilenceDb);
  }
  return voiced;
}

/// Frames within the cue window of a valley frame.
std::vector<bool> valley_cues(const std::vector<int>& valley_frames, std::size_t n, double h) {
  std::vector<bool> cue(n, false);
  for (const int frame : valley_frames) {
    const double valley_sec = static_cast<double>(frame) * h;
    const double lo = std::floor((valley_sec - kCueWindowSec) / h) - 1.0;
    const double hi = std::ceil((valley_sec + kCueWindowSec) / h) + 1.0;
    const double last = static_cast<double>(n) - 1.0;
    if (hi < 0.0 || lo > last) continue;
    const auto begin = static_cast<std::size_t>(std::max(0.0, lo));
    const auto end = static_cast<std::size_t>(std::min(last, hi));
    for (std::size_t t = begin; t <= end; ++t) {
      if (std::abs(static_cast<double>(t) * h - valley_sec) <= kCueWindowSec) cue[t] = true;
    }
  }
  return cue;
}

/// Python-style rounding (half to even) of a non-negative quantity to an index.
std::ptrdiff_t round_index(double value) {
  return static_cast<std::ptrdiff_t>(std::nearbyint(value));
}

/// Absolute samples of the narrow valleys in `samples[begin, end)`.
void append_valley_samples(const float* samples, std::ptrdiff_t begin, std::ptrdiff_t end,
                           int sample_rate, double median_hz, std::vector<std::ptrdiff_t>& out) {
  const std::ptrdiff_t n = end - begin;
  const double sr = static_cast<double>(sample_rate);
  const std::ptrdiff_t window =
      std::max<std::ptrdiff_t>(2, round_index(kValleyWindowPeriods * sr / median_hz));
  if (window > n) return;
  const std::ptrdiff_t left = window / 2;
  const std::ptrdiff_t right = window - left;
  const std::ptrdiff_t step = std::max<std::ptrdiff_t>(1, round_index(sr * kValleyGridMs / 1000.0));
  const std::ptrdiff_t context =
      std::max<std::ptrdiff_t>(1, round_index(sr * kValleyContextMs / 1000.0));
  const std::ptrdiff_t cp = std::max<std::ptrdiff_t>(
      1, round_index(static_cast<double>(context) / static_cast<double>(step)));
  const std::ptrdiff_t max_width = round_index(sr * kValleyMaxWidthMs / 1000.0);

  std::vector<double> prefix(static_cast<std::size_t>(n) + 1, 0.0);
  for (std::ptrdiff_t k = 0; k < n; ++k) {
    const double v = static_cast<double>(samples[begin + k]);
    prefix[static_cast<std::size_t>(k) + 1] = prefix[static_cast<std::size_t>(k)] + v * v;
  }
  std::vector<std::ptrdiff_t> centers;
  for (std::ptrdiff_t c = left; c <= n - right; c += step) centers.push_back(c);
  const auto count = static_cast<std::ptrdiff_t>(centers.size());
  if (count < 2 * cp + 1) return;
  std::vector<double> rms(centers.size());
  for (std::size_t j = 0; j < centers.size(); ++j) {
    const auto lo = static_cast<std::size_t>(centers[j] - left);
    const double energy = std::max(0.0, prefix[lo + static_cast<std::size_t>(window)] - prefix[lo]);
    rms[j] = std::sqrt(energy / static_cast<double>(window));
  }

  const auto at = [&](std::ptrdiff_t i) { return rms[static_cast<std::size_t>(i)]; };
  for (std::ptrdiff_t i = cp; i < count - cp; ++i) {
    if (at(i) > at(i - 1) || at(i) >= at(i + 1)) continue;
    double lmax = at(i - cp);
    for (std::ptrdiff_t k = i - cp + 1; k < i; ++k) lmax = std::max(lmax, at(k));
    double rmax = at(i + 1);
    for (std::ptrdiff_t k = i + 2; k <= i + cp; ++k) rmax = std::max(rmax, at(k));
    const double crest = std::min(lmax, rmax);
    if (!std::isfinite(crest) || crest < kValleyCrestFloor ||
        !(at(i) < kValleyDepthRatio * crest)) {
      continue;
    }
    std::ptrdiff_t lo = i;
    while (lo > i - cp && at(lo) < kValleyLowRatio * lmax) --lo;
    std::ptrdiff_t hi = i;
    while (hi < i + cp && at(hi) < kValleyLowRatio * rmax) ++hi;
    if (centers[static_cast<std::size_t>(hi)] - centers[static_cast<std::size_t>(lo)] > max_width) {
      continue;
    }
    out.push_back(begin + centers[static_cast<std::size_t>(i)]);
  }
}

/// Notes from the decoded path; a same-bin re-entry never leaves a piece under the minimum.
std::vector<DecodedSpan> path_spans(const std::vector<PathState>& path, double h,
                                    double minimum_sec) {
  struct Piece {
    int start;
    int end;
    bool rejoin;
  };
  std::vector<Piece> pieces;
  int start = -1;
  bool rejoin = false;
  PathState prev{Kind::kSilent, 0};
  const int n = static_cast<int>(path.size());
  for (int t = 0; t < n; ++t) {
    const PathState state = path[static_cast<std::size_t>(t)];
    const bool entering = state.kind == Kind::kAttack && prev.kind != Kind::kAttack;
    if ((entering || state.kind == Kind::kSilent) && start >= 0) {
      pieces.push_back({start, t, rejoin});
      start = -1;
    }
    if (entering) {
      start = t;
      rejoin = prev.kind == Kind::kStable && prev.bin == state.bin && !pieces.empty() &&
               pieces.back().end == t;
    }
    prev = state;
  }
  if (start >= 0) pieces.push_back({start, n, rejoin});

  const auto too_short = [&](const Piece& piece) {
    return static_cast<double>(piece.end - piece.start) * h < minimum_sec;
  };
  std::vector<Piece> merged;
  for (const Piece& piece : pieces) {
    if (piece.rejoin && !merged.empty() && (too_short(merged.back()) || too_short(piece))) {
      merged.back().end = piece.end;
    } else {
      merged.push_back(piece);
    }
  }
  std::vector<DecodedSpan> out;
  for (const Piece& piece : merged) {
    if (!too_short(piece)) out.push_back({piece.start, piece.end});
  }
  return out;
}

}  // namespace

std::vector<DecodedSpan> decode_note_states(const NoteDecoderInput& input, float fmin_hz,
                                            float fmax_hz, float reference_hz,
                                            float segmentation_threshold_cents, float min_note_ms) {
  SONARE_CHECK_MSG(input.track != nullptr, ErrorCode::InvalidParameter,
                   "decode_note_states: track is required");
  const pitch_editor::F0Track& track = *input.track;
  const std::size_t n = track.f0_hz.size();
  SONARE_CHECK_MSG(track.voiced.size() == n && input.frame_rms.size() == n,
                   ErrorCode::InvalidParameter,
                   "decode_note_states: voiced and frame_rms need one entry per frame");
  if (n == 0) return {};
  SONARE_CHECK_MSG(track.frame_rate() > 0.0f, ErrorCode::InvalidParameter,
                   "decode_note_states: track needs a frame cadence");
  SONARE_CHECK_MSG(fmin_hz > 0.0f && fmax_hz > fmin_hz && reference_hz > 0.0f,
                   ErrorCode::InvalidParameter, "decode_note_states: invalid pitch range");

  const double h = 1.0 / static_cast<double>(track.frame_rate());
  const double reference = static_cast<double>(reference_hz);
  const double cents_per_octave = static_cast<double>(kCentsPerOctave);
  const auto to_cents = [&](double hz) { return cents_per_octave * std::log2(hz / reference); };
  const auto low = static_cast<int>(std::floor(to_cents(fmin_hz) / kBinCents));
  const auto high = static_cast<int>(std::ceil(to_cents(fmax_hz) / kBinCents));
  SONARE_CHECK_MSG(high > low, ErrorCode::InvalidParameter,
                   "decode_note_states: pitch range spans fewer than two bins");
  const std::size_t nb = static_cast<std::size_t>(high - low + 1);
  std::vector<double> centres(nb);
  for (std::size_t b = 0; b < nb; ++b) {
    centres[b] = static_cast<double>(low + static_cast<int>(b)) * kBinCents;
  }

  const std::vector<bool> voiced = usable_voicing(track, input.frame_rms, h);
  const std::vector<bool> cue = valley_cues(input.valley_frames, n, h);

  const double scale = h / kReferenceFramePeriodSec;
  const double sigma_stable =
      kStableSigmaPerThreshold * static_cast<double>(segmentation_threshold_cents);
  const double lp_q = self_loop(kSilenceDwellSec, h);
  const double lp_a = self_loop(kAttackDwellSec, h);
  const double lp_a_exit = leave(kAttackDwellSec);
  const double lp_s = self_loop(kStableDwellSec, h);
  const double lp_s_exit = leave(kStableDwellSec);
  const double s_to_q = lp_s_exit + std::log1p(-kNoteChangeShare);
  const double s_to_a = lp_s_exit + std::log(kNoteChangeShare) + kLogEntry;
  const double q_to_a = leave(kSilenceDwellSec) + kLogEntry;
  const double norm_s = -std::log(kPiD * kStableNormSigmaCents);
  const double norm_a = -std::log(kPiD * kAttackSigmaCents);

  // Backpointers. Silent: 0 from Silent, 1 from Stable top1. Attack: 0 Silent, 1 Attack,
  // 2 Stable of another bin, 3 Stable of the same bin. Stable: 0 Stable, 1 Attack.
  std::vector<std::int8_t> back_q(n, 0);
  std::vector<std::int8_t> back_a(n * nb, 0);
  std::vector<std::int8_t> back_s(n * nb, 0);
  std::vector<std::int32_t> top1_at(n, 0);
  std::vector<std::int32_t> top2_at(n, 0);

  double q = 0.0;
  std::vector<double> a(nb, kUnreachable);
  std::vector<double> s(nb, kUnreachable);
  std::vector<double> next_a(nb);
  std::vector<double> next_s(nb);
  std::vector<double> e_a(nb);
  std::vector<double> e_s(nb);

  for (std::size_t t = 0; t < n; ++t) {
    double e_q = 0.0;
    if (voiced[t]) {
      const double x = to_cents(static_cast<double>(track.f0_hz[t]));
      for (std::size_t b = 0; b < nb; ++b) {
        const double d = x - centres[b];
        const double rs = d / sigma_stable;
        const double ra = d / kAttackSigmaCents;
        e_s[b] = scale * (norm_s - std::log1p(rs * rs));
        e_a[b] = scale * (norm_a - std::log1p(ra * ra));
      }
      e_q = scale * kLogVoicedSilence;
    } else {
      std::fill(e_s.begin(), e_s.end(), scale * kLogUnvoicedNote);
      std::fill(e_a.begin(), e_a.end(), scale * kLogUnvoicedNote);
    }

    // Ties go to the lower bin.
    const std::size_t top1 = first_max(s);
    std::size_t top2 = top1 == 0 ? 1 : 0;
    for (std::size_t b = 0; b < nb; ++b) {
      if (b != top1 && s[b] > s[top2]) top2 = b;
    }
    top1_at[t] = static_cast<std::int32_t>(top1);
    top2_at[t] = static_cast<std::int32_t>(top2);

    // Silent takes the Stable exit only when it is strictly better.
    const double from_s = s[top1] + s_to_q;
    const double from_q = q + lp_q;
    double next_q = from_q;
    if (from_s > from_q) {
      next_q = from_s;
      back_q[t] = 1;
    }

    const double cue_bonus = cue[t] ? kCueBonus : 0.0;
    const double enter_from_q = q + q_to_a;
    for (std::size_t b = 0; b < nb; ++b) {
      // Candidates in fixed order; a later one replaces an earlier only when strictly better.
      double best = enter_from_q;
      std::int8_t code = 0;
      const double stay = a[b] + lp_a;
      if (stay > best) {
        best = stay;
        code = 1;
      }
      const double other = (b == top1 ? s[top2] : s[top1]) + s_to_a;
      if (other > best) {
        best = other;
        code = 2;
      }
      const double same = ((s[b] + s_to_a) + kLogSameBin) + cue_bonus;
      if (same > best) {
        best = same;
        code = 3;
      }
      next_a[b] = best + e_a[b];
      back_a[t * nb + b] = code;

      const double hold = s[b] + lp_s;
      const double settle = a[b] + lp_a_exit;
      if (settle > hold) {
        next_s[b] = settle + e_s[b];
        back_s[t * nb + b] = 1;
      } else {
        next_s[b] = hold + e_s[b];
      }
    }
    q = next_q + e_q;
    a.swap(next_a);
    s.swap(next_s);
  }

  // Termination: Silent, then the best Attack, then the best Stable; later only if strictly better.
  PathState state{Kind::kSilent, 0};
  double best = q;
  const std::size_t a_best = first_max(a);
  if (a[a_best] > best) {
    best = a[a_best];
    state = {Kind::kAttack, static_cast<int>(a_best)};
  }
  const std::size_t s_best = first_max(s);
  if (s[s_best] > best) state = {Kind::kStable, static_cast<int>(s_best)};

  std::vector<PathState> path(n);
  for (std::size_t t = n; t-- > 0;) {
    path[t] = state;
    if (t == 0) break;
    const auto bin = static_cast<std::size_t>(state.bin);
    switch (state.kind) {
      case Kind::kSilent:
        state = back_q[t] == 1 ? PathState{Kind::kStable, top1_at[t]} : PathState{Kind::kSilent, 0};
        break;
      case Kind::kAttack:
        switch (back_a[t * nb + bin]) {
          case 0:
            state = {Kind::kSilent, 0};
            break;
          case 1:
            break;
          case 2:
            state = {Kind::kStable,
                     bin == static_cast<std::size_t>(top1_at[t]) ? top2_at[t] : top1_at[t]};
            break;
          default:
            state.kind = Kind::kStable;
            break;
        }
        break;
      case Kind::kStable:
        if (back_s[t * nb + bin] == 1) state.kind = Kind::kAttack;
        break;
    }
  }

  const double minimum_sec = std::max(static_cast<double>(min_note_ms) / 1000.0, 2.0 * h);
  return path_spans(path, h, minimum_sec);
}

std::vector<float> frame_rms_for_track(const Audio& audio, const pitch_editor::F0Track& track) {
  const int frames = track.n_frames();
  std::vector<float> rms(static_cast<std::size_t>(std::max(0, frames)), 0.0f);
  const double spf = track.samples_per_frame();
  const auto total = static_cast<double>(audio.size());
  const float* samples = audio.data();
  for (int i = 0; i < frames; ++i) {
    const double begin = std::min(std::floor(static_cast<double>(i) * spf), total);
    const double end = std::min(std::floor(static_cast<double>(i + 1) * spf), total);
    if (!(end > begin)) continue;
    const auto first = static_cast<std::size_t>(begin);
    const auto last = static_cast<std::size_t>(end);
    double sum = 0.0;
    for (std::size_t k = first; k < last; ++k) {
      const double v = static_cast<double>(samples[k]);
      sum += v * v;
    }
    const auto value = static_cast<float>(std::sqrt(sum / static_cast<double>(last - first)));
    if (std::isfinite(value)) rms[static_cast<std::size_t>(i)] = value;
  }
  return rms;
}

std::vector<int> narrow_valley_frames(const Audio& audio, const pitch_editor::F0Track& track) {
  const double spf = track.samples_per_frame();
  SONARE_CHECK_MSG(spf > 0.0, ErrorCode::InvalidParameter,
                   "narrow_valley_frames: track needs a frame cadence");
  SONARE_CHECK_MSG(audio.sample_rate() > 0, ErrorCode::InvalidParameter,
                   "narrow_valley_frames: audio needs a sample rate");
  const auto total = static_cast<std::ptrdiff_t>(audio.size());
  const auto frames = static_cast<std::size_t>(std::max(0, track.n_frames()));
  const auto voiced = [&](std::size_t t) {
    const float hz = track.f0_hz[t];
    return std::isfinite(hz) && hz > 0.0f;
  };

  std::vector<std::ptrdiff_t> samples;
  std::vector<double> run_hz;
  for (std::size_t a = 0; a < frames;) {
    if (!voiced(a)) {
      ++a;
      continue;
    }
    std::size_t b = a;
    run_hz.clear();
    while (b < frames && voiced(b)) run_hz.push_back(static_cast<double>(track.f0_hz[b++]));
    std::sort(run_hz.begin(), run_hz.end());
    const std::size_t mid = run_hz.size() / 2;
    const double median =
        run_hz.size() % 2 == 1 ? run_hz[mid] : (run_hz[mid - 1] + run_hz[mid]) / 2.0;
    const auto begin = static_cast<std::ptrdiff_t>(static_cast<double>(a) * spf);
    const auto end = std::min(total, static_cast<std::ptrdiff_t>(static_cast<double>(b) * spf));
    append_valley_samples(audio.data(), begin, end, audio.sample_rate(), median, samples);
    a = b;
  }

  std::vector<int> out;
  out.reserve(samples.size());
  for (const std::ptrdiff_t sample : samples) {
    out.push_back(static_cast<int>(std::nearbyint(static_cast<double>(sample) / spf)));
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}  // namespace sonare::editing::note_model
