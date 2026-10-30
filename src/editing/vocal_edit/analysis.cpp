// SONARE_WASM_EXCEPTION_UNWIND: release analysis and segmentation buffers when validation throws.
#include "editing/vocal_edit/analysis.h"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <string>

#include "editing/pitch_editor/f0_provider.h"
#include "editing/pitch_editor/note_segmenter.h"
#include "editing/vocal_edit/session_internal.h"
#include "editing/vocal_edit/source_digest.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/sha256.h"

namespace sonare::editing::vocal_edit {

using detail::frame_ceil;
using detail::frame_raw_position;
using detail::frame_sample_position;
using detail::invalid;
using sonare::constants::kA4Hz;
using sonare::constants::kCentsPerOctave;
using sonare::constants::kCentsPerSemitone;
using sonare::constants::kMidiA4;
using sonare::constants::kSemitonesPerOctave;

namespace {

class DigestBuilder {
 public:
  void update(const uint8_t* data, size_t size) { hash_.update(data, size); }
  void u8(uint8_t value) { update(&value, 1); }
  void u32(uint32_t value) {
    uint8_t bytes[4];
    for (unsigned int shift = 0; shift < 32; shift += 8)
      bytes[shift / 8] = static_cast<uint8_t>(value >> shift);
    update(bytes, sizeof(bytes));
  }
  void u64(uint64_t value) {
    uint8_t bytes[8];
    for (unsigned int shift = 0; shift < 64; shift += 8)
      bytes[shift / 8] = static_cast<uint8_t>(value >> shift);
    update(bytes, sizeof(bytes));
  }
  void f32(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    u32(bits);
  }
  void f64(double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    u64(bits);
  }
  Sha256Digest finalize() { return hash_.finalize(); }

 private:
  util::Sha256 hash_;
};

int64_t rounded_sample(double value) {
  const double int64_lower = -std::ldexp(1.0, 63);
  const double int64_upper_exclusive = std::ldexp(1.0, 63);
  if (!std::isfinite(value) || value < int64_lower || value >= int64_upper_exclusive) {
    invalid("analysis.grid", "analysis frame position is outside int64 sample range");
  }
  const double rounded = std::round(value);
  if (rounded < int64_lower || rounded >= int64_upper_exclusive) {
    invalid("analysis.grid", "analysis frame position cannot be represented as a sample");
  }
  return static_cast<int64_t>(rounded);
}

bool usable_frame(const VocalAnalysisData& analysis, size_t frame) {
  return analysis.voiced[frame] != 0 && analysis.f0_hz[frame] > 0.0f &&
         std::isfinite(analysis.f0_hz[frame]);
}

double median_of_sorted(const std::vector<double>& sorted) {
  const size_t n = sorted.size();
  return n % 2 == 1 ? sorted[n / 2] : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
}

// The analysis F0 is a pYIN bin centre (10-cent grid anchored at fmin), and the Viterbi path
// has been seen one bin past the nearest; the next difference-function minimum is an octave away.
constexpr double kRefineWindowCents = 25.0;

/// @brief Sub-bin F0 for one analysis frame, read off the source audio.
///
/// Minimises the exact fixed-window difference function d(tau) = sum (x[n] - x[n + tau])^2 over
/// the lags within kRefineWindowCents of the analysis period, with a parabolic sub-sample
/// minimum. The window is fixed across lags so its length cannot bias the minimum. Returns the
/// analysis value when the minimum sits on the search edge or the frame carries no dip.
float refine_frame_f0(const Audio& source, const AnalysisGrid& grid, uint32_t frame, float f0_hz,
                      std::vector<float>& frame_samples, std::vector<double>& difference) {
  const double sample_rate = source.sample_rate();
  const double period = sample_rate / static_cast<double>(f0_hz);
  const double ratio = std::pow(2.0, kRefineWindowCents / kCentsPerOctave);
  const int frame_length = static_cast<int>(grid.frame_length_samples);
  const double lag_low_raw = std::floor(period / ratio);
  const double lag_high_raw = std::ceil(period * ratio);
  if (!(lag_low_raw >= 1.0) || !(lag_high_raw + 1.0 < static_cast<double>(frame_length))) {
    return f0_hz;
  }
  const int lag_low = static_cast<int>(lag_low_raw);
  const int lag_high = static_cast<int>(lag_high_raw);
  const int window = frame_length - lag_high;

  const double centre = frame_sample_position(grid, frame);
  const int64_t begin = static_cast<int64_t>(std::floor(centre - 0.5 * frame_length));
  frame_samples.assign(static_cast<size_t>(frame_length), 0.0f);
  const int64_t source_size = static_cast<int64_t>(source.size());
  for (int n = 0; n < frame_length; ++n) {
    const int64_t sample = begin + n;
    if (sample >= 0 && sample < source_size) {
      frame_samples[static_cast<size_t>(n)] = source[static_cast<size_t>(sample)];
    }
  }

  difference.assign(static_cast<size_t>(lag_high - lag_low + 1), 0.0);
  size_t best = 0;
  for (int lag = lag_low; lag <= lag_high; ++lag) {
    double sum = 0.0;
    for (int n = 0; n < window; ++n) {
      const double delta = static_cast<double>(frame_samples[static_cast<size_t>(n)]) -
                           static_cast<double>(frame_samples[static_cast<size_t>(n + lag)]);
      sum += delta * delta;
    }
    const size_t index = static_cast<size_t>(lag - lag_low);
    difference[index] = sum;
    if (sum < difference[best]) best = index;
  }
  if (best == 0 || best + 1 == difference.size()) return f0_hz;
  const double left = difference[best - 1];
  const double middle = difference[best];
  const double right = difference[best + 1];
  const double denominator = left - 2.0 * middle + right;
  if (!(denominator > 0.0)) return f0_hz;
  const double offset = 0.5 * (left - right) / denominator;
  const double refined_period = static_cast<double>(lag_low) + static_cast<double>(best) + offset;
  const double refined = sample_rate / refined_period;
  if (!std::isfinite(refined) || refined < f0_hz / ratio || refined > f0_hz * ratio) return f0_hz;
  return static_cast<float>(refined);
}

/// @brief Refined F0 for the frames [frame_start, frame_end); unusable frames keep their value.
std::vector<float> refine_f0_range(const Audio& source, const VocalAnalysisData& analysis,
                                   uint32_t frame_start, uint32_t frame_end) {
  std::vector<float> refined(analysis.f0_hz.begin() + frame_start,
                             analysis.f0_hz.begin() + frame_end);
  std::vector<float> frame_samples;
  std::vector<double> difference;
  for (uint32_t frame = frame_start; frame < frame_end; ++frame) {
    const size_t i = static_cast<size_t>(frame);
    if (!usable_frame(analysis, i)) continue;
    refined[static_cast<size_t>(frame - frame_start)] =
        refine_frame_f0(source, analysis.grid, frame, analysis.f0_hz[i], frame_samples, difference);
  }
  return refined;
}

/// @brief Cuts the refined contour into notes with the shared segmenter.
std::vector<pitch_editor::NoteRegion> segment_contour(const VocalAnalysisData& analysis,
                                                      const std::vector<float>& f0_hz,
                                                      int sample_rate) {
  pitch_editor::F0Track track;
  track.f0_hz = f0_hz;
  track.voiced.resize(analysis.voiced.size());
  for (size_t i = 0; i < analysis.voiced.size(); ++i) track.voiced[i] = analysis.voiced[i] != 0;
  track.sample_rate = sample_rate;
  track.frame_rate_hz = static_cast<float>(sample_rate / analysis.grid.samples_per_frame);
  track.hop_length = std::max(1, static_cast<int>(std::llround(analysis.grid.samples_per_frame)));
  const pitch_editor::NoteSegmenter segmenter(
      {static_cast<float>(analysis.settings.segmentation_threshold_cents),
       static_cast<float>(analysis.settings.minimum_note_ms),
       static_cast<float>(analysis.settings.reference_hz)});
  return segmenter.segment(track);
}

// A vibrato peak well below the others is the fade-in's first hump, whose position is noise;
// on a flat note every noise peak is alike and all of them count.
constexpr double kProminentPeakFraction = 0.5;

// Vibrato rates the fallback fit searches, and its grid step.
constexpr double kVibratoRateMinHz = 3.5;
constexpr double kVibratoRateMaxHz = 8.5;
constexpr double kVibratoRateToleranceHz = 1e-9;
constexpr double kVibratoRateStepHz = 0.05;
// Below two cycles a growing amplitude is not separable from the centre (fit outliers of 80 cents).
constexpr double kGrowthFitMinCycles = 2.0;

/// @brief Constant term of the least-squares fit c + a sin(wt) + b cos(wt), plus t-scaled
/// sine and cosine when @p growth, over the rates the body spans at least one cycle of.
/// Returns false when no rate fits or there are too few frames for the model.
bool fit_vibrato(const std::vector<double>& seconds, const std::vector<double>& values, bool growth,
                 double* centre, double* rate_hz) {
  const size_t columns = growth ? 5 : 3;
  const size_t n = values.size();
  const double span = seconds.back() - seconds.front();
  if (n <= columns + 1 || !(span > 0.0)) return false;
  const double lowest = std::max(kVibratoRateMinHz, 1.0 / span);
  double best_rss = std::numeric_limits<double>::infinity();
  Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic> design(n, columns);
  const Eigen::Map<const Eigen::VectorXd> target(values.data(), static_cast<Eigen::Index>(n));
  for (int step = 0;; ++step) {
    const double rate = lowest + step * kVibratoRateStepHz;
    if (rate > kVibratoRateMaxHz + kVibratoRateToleranceHz) break;
    const double omega = sonare::constants::kTwoPiD * rate;
    for (size_t k = 0; k < n; ++k) {
      const double t = seconds[k];
      const auto row = static_cast<Eigen::Index>(k);
      design(row, 0) = 1.0;
      design(row, 1) = std::sin(omega * t);
      design(row, 2) = std::cos(omega * t);
      if (growth) {
        design(row, 3) = t * design(row, 1);
        design(row, 4) = t * design(row, 2);
      }
    }
    const Eigen::VectorXd coefficients = design.colPivHouseholderQr().solve(target);
    const double rss = (design * coefficients - target).squaredNorm();
    if (rss < best_rss) {
      best_rss = rss;
      *centre = coefficients(0);
      *rate_hz = rate;
    }
  }
  return std::isfinite(best_rss);
}

/// @brief Pitch centre of a note from its refined per-frame MIDI contour.
///
/// @p frames holds each value's analysis frame, ascending. Body: the frames within the threshold
/// of the median, from the first to the last turning point, so the glides at either end are left
/// out; a turning point needs both neighbours on adjacent frames, since a gap left by the band is
/// no turn. Centre: the piecewise-linear body integrated from its first to its last vibrato peak,
/// each the parabolic maximum of one excursion above the body mean. A peak sits at the same phase
/// whatever the amplitude, so a vibrato still growing in integrates to zero over a cycle; a level
/// crossing does not (asin(level / amplitude)), and a frame median is off by the partial cycle at
/// either end (measured 2.6 cents at 5.5 Hz, +-25 cents). A peak on the body's edge is left out,
/// since a glide cutting the vibrato short makes one there. With fewer than two peaks the centre
/// is the constant term of a vibrato fit to the body (fit_vibrato), its amplitude growing linearly
/// once the body spans kGrowthFitMinCycles; the body mean stands in when nothing fits.
double contour_centre(const std::vector<double>& midi, const std::vector<uint32_t>& frames,
                      double threshold_semitones, double seconds_per_frame) {
  std::vector<double> sorted(midi);
  std::sort(sorted.begin(), sorted.end());
  const double median = median_of_sorted(sorted);
  std::vector<double> in_band;
  std::vector<uint32_t> frame_of;
  in_band.reserve(midi.size());
  frame_of.reserve(midi.size());
  for (size_t i = 0; i < midi.size(); ++i) {
    if (std::abs(midi[i] - median) > threshold_semitones) continue;
    in_band.push_back(midi[i]);
    frame_of.push_back(frames[i]);
  }
  const auto mean_of = [](const std::vector<double>& values) {
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
  };
  if (in_band.size() < 2) return median;
  // Keep the frames from the first to the last turning point; the runs outside are the glides.
  size_t first_turn = in_band.size();
  size_t last_turn = 0;
  for (size_t i = 1; i + 1 < in_band.size(); ++i) {
    if (frame_of[i] != frame_of[i - 1] + 1 || frame_of[i + 1] != frame_of[i] + 1) continue;
    if ((in_band[i] - in_band[i - 1]) * (in_band[i + 1] - in_band[i]) > 0.0) continue;
    if (first_turn == in_band.size()) first_turn = i;
    last_turn = i;
  }
  if (first_turn == in_band.size() || last_turn < first_turn + 1) return mean_of(in_band);
  const std::vector<double> body(in_band.begin() + static_cast<std::ptrdiff_t>(first_turn),
                                 in_band.begin() + static_cast<std::ptrdiff_t>(last_turn + 1));

  const double level = mean_of(body);

  const auto fitted_centre = [&]() {
    std::vector<double> seconds(body.size());
    const double middle_frame = 0.5 * (static_cast<double>(frame_of[first_turn]) +
                                       static_cast<double>(frame_of[last_turn]));
    for (size_t k = 0; k < body.size(); ++k) {
      seconds[k] =
          (static_cast<double>(frame_of[first_turn + k]) - middle_frame) * seconds_per_frame;
    }
    double centre = level;
    double rate_hz = 0.0;
    if (!fit_vibrato(seconds, body, false, &centre, &rate_hz)) return level;
    if (rate_hz * (seconds.back() - seconds.front()) >= kGrowthFitMinCycles) {
      double grown = centre;
      if (fit_vibrato(seconds, body, true, &grown, &rate_hz)) centre = grown;
    }
    return centre;
  };

  struct Peak {
    double position;
    double height;
  };
  std::vector<Peak> peaks;
  for (size_t i = 0; i < body.size();) {
    if (!(body[i] > level)) {
      ++i;
      continue;
    }
    size_t end = i;
    size_t top = i;
    while (end < body.size() && body[end] > level) {
      if (body[end] > body[top]) top = end;
      ++end;
    }
    if (top > 0 && top + 1 < body.size()) {
      const double left = body[top - 1];
      const double middle = body[top];
      const double right = body[top + 1];
      const double denominator = left - 2.0 * middle + right;
      const double offset =
          denominator < 0.0 ? std::clamp(0.5 * (left - right) / denominator, -0.5, 0.5) : 0.0;
      peaks.push_back({static_cast<double>(top) + offset, middle - level});
    }
    i = end;
  }
  if (peaks.size() < 2) return fitted_centre();
  std::vector<double> heights;
  heights.reserve(peaks.size());
  for (const auto& peak : peaks) heights.push_back(peak.height);
  std::sort(heights.begin(), heights.end());
  const double prominent = kProminentPeakFraction * median_of_sorted(heights);
  double first = -1.0;
  double last = -1.0;
  for (const auto& peak : peaks) {
    if (peak.height < prominent) continue;
    if (first < 0.0) first = peak.position;
    last = peak.position;
  }
  if (first < 0.0 || !(last > first)) return fitted_centre();
  // Trapezoid integral of the piecewise-linear body between the two peaks.
  const auto value_at = [&](double x) {
    const size_t k = static_cast<size_t>(std::floor(x));
    if (k + 1 >= body.size()) return body.back();
    return body[k] + (body[k + 1] - body[k]) * (x - static_cast<double>(k));
  };
  double area = 0.0;
  double x = first;
  double y = value_at(first);
  for (size_t k = static_cast<size_t>(std::floor(first)) + 1; static_cast<double>(k) < last; ++k) {
    area += 0.5 * (y + body[k]) * (static_cast<double>(k) - x);
    x = static_cast<double>(k);
    y = body[k];
  }
  area += 0.5 * (y + value_at(last)) * (last - x);
  return area / (last - first);
}

/// @brief Measures a note whose refined contour for [frame_start, frame_end) is already known.
VocalNote measure_from_contour(const VocalAnalysisData& analysis, int source_rate, VocalNoteId id,
                               SampleRange source_range, uint32_t frame_start, uint32_t frame_end,
                               const std::vector<float>& refined_f0) {
  std::vector<double> midi;
  std::vector<uint32_t> frames;
  for (uint32_t frame = frame_start; frame < frame_end; ++frame) {
    if (usable_frame(analysis, static_cast<size_t>(frame))) {
      midi.push_back(midi_from_hz(refined_f0[static_cast<size_t>(frame - frame_start)]));
      frames.push_back(frame);
    }
  }
  VocalNote note;
  note.id = id;
  note.source_range = source_range;
  note.analysis_frame_start = frame_start;
  note.analysis_frame_end = frame_end;
  note.has_pitch = !midi.empty();
  if (midi.empty()) {
    note.centre_midi = 0.0;
    note.median_hz = 0.0;
    note.f0_stability = 0.0;
  } else {
    const double threshold_semitones =
        analysis.settings.segmentation_threshold_cents / kCentsPerSemitone;
    note.centre_midi =
        contour_centre(midi, frames, threshold_semitones,
                       analysis.grid.samples_per_frame / static_cast<double>(source_rate));
    note.median_hz = kA4Hz * std::pow(2.0, (note.centre_midi - kMidiA4) / kSemitonesPerOctave);
    double mad = 0.0;
    for (const double value : midi) mad += std::abs(value - note.centre_midi);
    mad /= static_cast<double>(midi.size());
    note.f0_stability = std::max(0.0, std::min(1.0, 1.0 - mad / threshold_semitones));
  }
  note.edit = VocalNoteEdit::identity_for(source_range);
  return note;
}

}  // namespace

int64_t SampleRange::length() const noexcept {
  if (end >= start) {
    const uint64_t difference = static_cast<uint64_t>(end) - static_cast<uint64_t>(start);
    return difference > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
               ? std::numeric_limits<int64_t>::max()
               : static_cast<int64_t>(difference);
  }
  const uint64_t difference = static_cast<uint64_t>(start) - static_cast<uint64_t>(end);
  return difference > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
             ? std::numeric_limits<int64_t>::min()
             : -static_cast<int64_t>(difference);
}

SourceDescriptor describe_source(const Audio& source) {
  if (source.empty() || source.sample_rate() <= 0) {
    invalid("source", "source audio must be non-empty and have a positive sample rate");
  }
  for (const float sample : source) {
    if (!std::isfinite(sample)) invalid("source", "source audio contains a non-finite sample");
  }

  SourceDescriptor descriptor;
  descriptor.sample_rate = static_cast<uint32_t>(source.sample_rate());
  if (source.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    invalid("source.sample_count", "source sample count is too large");
  }
  descriptor.sample_count = static_cast<int64_t>(source.size());
  descriptor.digest = digest_source_pcm(source.data(), source.size());
  return descriptor;
}

Sha256Digest digest_analysis(const VocalAnalysisData& analysis) {
  DigestBuilder digest;
  static constexpr char kTag[] = "SVE1-ANALYSIS\0";
  digest.update(reinterpret_cast<const uint8_t*>(kTag), sizeof(kTag) - 1);
  digest.f64(analysis.grid.frame_origin_sample);
  digest.f64(analysis.grid.samples_per_frame);
  digest.u32(analysis.grid.frame_length_samples);
  digest.f64(analysis.settings.fmin_hz);
  digest.f64(analysis.settings.fmax_hz);
  digest.f64(analysis.settings.yin_threshold);
  digest.f64(analysis.settings.voiced_threshold);
  digest.u8(static_cast<uint8_t>(analysis.settings.centered ? 1 : 0));
  digest.f64(analysis.settings.segmentation_threshold_cents);
  digest.f64(analysis.settings.minimum_note_ms);
  digest.f64(analysis.settings.reference_hz);
  digest.u32(static_cast<uint32_t>(analysis.algorithm_id.size()));
  digest.update(reinterpret_cast<const uint8_t*>(analysis.algorithm_id.data()),
                analysis.algorithm_id.size());
  digest.u32(analysis.algorithm_version);
  digest.u64(static_cast<uint64_t>(analysis.f0_hz.size()));
  for (const float value : analysis.f0_hz) digest.f32(value);
  for (const uint8_t value : analysis.voiced) digest.u8(value);
  return digest.finalize();
}

VocalAnalysisData validate_analysis(const Audio& source, VocalAnalysisData analysis) {
  static_cast<void>(describe_source(source));
  const auto& grid = analysis.grid;
  if (!std::isfinite(grid.frame_origin_sample))
    invalid("analysis.grid.frame_origin_sample", "must be finite");
  if (!std::isfinite(grid.samples_per_frame) || grid.samples_per_frame < 1.0) {
    invalid("analysis.grid.samples_per_frame", "must be finite and >= 1");
  }
  if (grid.frame_length_samples == 0) invalid("analysis.grid.frame_length_samples", "must be > 0");
  if (analysis.f0_hz.empty()) invalid("analysis.f0_hz", "must not be empty");
  if (analysis.f0_hz.size() > std::numeric_limits<uint32_t>::max()) {
    invalid("analysis.f0_hz", "frame count is too large");
  }
  if (analysis.voiced.size() != analysis.f0_hz.size()) {
    invalid("analysis.voiced", "must have the same length as f0_hz");
  }
  if (!analysis.amplitude.empty() && analysis.amplitude.size() != analysis.f0_hz.size()) {
    invalid("analysis.amplitude", "must be empty or have the same length as f0_hz");
  }
  if (analysis.algorithm_id.empty()) invalid("analysis.algorithm_id", "must not be empty");
  if (analysis.algorithm_id != "host" && analysis.algorithm_id != "libsonare.pyin") {
    invalid("analysis.algorithm_id", "algorithm is not supported");
  }
  if (analysis.algorithm_id.size() > std::numeric_limits<uint32_t>::max()) {
    invalid("analysis.algorithm_id", "algorithm identifier is too long");
  }
  if (analysis.algorithm_version != 1)
    invalid("analysis.algorithm_version", "algorithm version is unsupported");

  const auto& settings = analysis.settings;
  if (!std::isfinite(settings.fmin_hz) || !std::isfinite(settings.fmax_hz) ||
      !std::isfinite(settings.yin_threshold) || !std::isfinite(settings.voiced_threshold) ||
      !std::isfinite(settings.segmentation_threshold_cents) ||
      !std::isfinite(settings.minimum_note_ms) || !std::isfinite(settings.reference_hz) ||
      settings.fmin_hz <= 0.0 || settings.fmax_hz <= settings.fmin_hz ||
      settings.yin_threshold <= 0.0 || settings.yin_threshold > 1.0 ||
      settings.voiced_threshold < 0.0 || settings.voiced_threshold > 1.0 ||
      settings.segmentation_threshold_cents <= 0.0 || settings.minimum_note_ms <= 0.0 ||
      settings.reference_hz <= 0.0) {
    invalid("analysis.settings", "analysis settings are malformed");
  }

  const double nyquist = static_cast<double>(source.sample_rate()) * 0.5;
  for (size_t i = 0; i < analysis.f0_hz.size(); ++i) {
    if (analysis.voiced[i] != 0 && analysis.voiced[i] != 1) {
      invalid("analysis.voiced", "voiced values must be exactly 0 or 1");
    }
    float& f0 = analysis.f0_hz[i];
    if (analysis.voiced[i] == 0) {
      f0 = 0.0f;
    } else {
      if (!std::isfinite(f0) || f0 <= 0.0f)
        invalid("analysis.f0_hz", "voiced frames need a positive finite F0");
      if (static_cast<double>(f0) > nyquist)
        invalid("analysis.f0_hz", "voiced F0 must not exceed Nyquist");
    }
  }
  for (const float value : analysis.amplitude) {
    if (!std::isfinite(value) || value < 0.0f)
      invalid("analysis.amplitude", "must be finite and non-negative");
  }
  // Frame positions are later converted to checked int64 sample coordinates
  // during amplitude synthesis and note extraction. Reject a finite but
  // unrepresentable origin/cadence before any floating-to-integer cast; a
  // direct static_cast from a huge double is undefined behaviour.
  const long double first_frame = static_cast<long double>(grid.frame_origin_sample);
  const long double last_frame = first_frame + static_cast<long double>(analysis.f0_hz.size() - 1) *
                                                   static_cast<long double>(grid.samples_per_frame);
  const long double min_sample = -std::ldexp(1.0L, 63);
  const long double max_sample_exclusive = std::ldexp(1.0L, 63);
  if (!std::isfinite(first_frame) || !std::isfinite(last_frame) || first_frame < min_sample ||
      last_frame >= max_sample_exclusive) {
    invalid("analysis.grid", "analysis frame positions are outside the int64 sample domain");
  }
  if (analysis.amplitude.empty()) {
    analysis.amplitude.assign(analysis.f0_hz.size(), 0.0f);
    for (size_t frame = 0; frame < analysis.amplitude.size(); ++frame) {
      const double centre = frame_sample_position(analysis.grid, static_cast<uint32_t>(frame));
      const long double begin_position =
          std::floor(static_cast<long double>(centre) -
                     static_cast<long double>(analysis.grid.frame_length_samples) * 0.5L);
      const long double end_position =
          begin_position + static_cast<long double>(analysis.grid.frame_length_samples);
      if (!std::isfinite(begin_position) || !std::isfinite(end_position) ||
          begin_position < min_sample || end_position >= max_sample_exclusive) {
        invalid("analysis.grid", "analysis frame window is outside the int64 sample domain");
      }
      const int64_t begin = static_cast<int64_t>(begin_position);
      const int64_t end = begin + static_cast<int64_t>(analysis.grid.frame_length_samples);
      double sum = 0.0;
      size_t count = 0;
      for (int64_t sample = std::max<int64_t>(0, begin);
           sample < std::min<int64_t>(static_cast<int64_t>(source.size()), end); ++sample) {
        const double value = source[static_cast<size_t>(sample)];
        sum += value * value;
        ++count;
      }
      analysis.amplitude[frame] = count == 0 ? 0.0f : static_cast<float>(std::sqrt(sum / count));
    }
  }
  analysis.digest = digest_analysis(analysis);
  return analysis;
}

VocalAnalysisData analyze_vocal(const Audio& source, const VocalAnalysisOptions& options) {
  if (!std::isfinite(options.voiced_threshold) || options.voiced_threshold < 0.0 ||
      options.voiced_threshold > 1.0 || !std::isfinite(options.segmentation_threshold_cents) ||
      options.segmentation_threshold_cents <= 0.0 || !std::isfinite(options.minimum_note_ms) ||
      options.minimum_note_ms <= 0.0 || !std::isfinite(options.reference_hz) ||
      options.reference_hz <= 0.0) {
    invalid("analysis.voiced_threshold", "must be in [0, 1]");
  }
  if (options.pitch.frame_length <= 0 || options.pitch.hop_length <= 0) {
    invalid("analysis.pitch", "frame and hop lengths must be positive");
  }
  editing::pitch_editor::PyinF0Provider provider(options.pitch);
  const auto track = provider.detect(source);
  VocalAnalysisData result;
  result.grid.frame_origin_sample = options.pitch.center ? 0.0 : options.pitch.frame_length * 0.5;
  result.grid.samples_per_frame = static_cast<double>(options.pitch.hop_length);
  result.grid.frame_length_samples = static_cast<uint32_t>(options.pitch.frame_length);
  result.settings.fmin_hz = options.pitch.fmin;
  result.settings.fmax_hz = options.pitch.fmax;
  result.settings.yin_threshold = options.pitch.threshold;
  result.settings.voiced_threshold = options.voiced_threshold;
  result.settings.centered = options.pitch.center;
  result.settings.segmentation_threshold_cents = options.segmentation_threshold_cents;
  result.settings.minimum_note_ms = options.minimum_note_ms;
  result.settings.reference_hz = options.reference_hz;
  result.f0_hz = track.f0_hz;
  result.voiced.resize(track.voiced.size());
  for (size_t i = 0; i < result.voiced.size(); ++i) result.voiced[i] = track.voiced[i] ? 1 : 0;
  // Amplitude is derived by validate_analysis from the saved grid. The pYIN
  // voiced bool mask is authoritative; voiced_prob is intentionally ignored.
  result.algorithm_id = options.algorithm_id;
  result.algorithm_version = options.algorithm_version;
  return validate_analysis(source, std::move(result));
}

std::vector<VocalNote> extract_vocal_notes(const Audio& source, const VocalAnalysisData& analysis) {
  const std::vector<float> refined =
      refine_f0_range(source, analysis, 0, static_cast<uint32_t>(analysis.f0_hz.size()));
  const auto regions = segment_contour(analysis, refined, source.sample_rate());
  std::vector<VocalNote> notes;
  notes.reserve(regions.size());
  for (const auto& region : regions) {
    const int64_t start = std::max<int64_t>(
        0, std::min<int64_t>(
               static_cast<int64_t>(source.size()),
               rounded_sample(frame_sample_position(analysis.grid, region.frame_start))));
    int64_t end = std::max<int64_t>(
        start,
        std::min<int64_t>(static_cast<int64_t>(source.size()),
                          rounded_sample(frame_sample_position(analysis.grid, region.frame_end))));
    if (end <= start) end = std::min<int64_t>(static_cast<int64_t>(source.size()), start + 1);
    if (end <= start) continue;
    if (notes.size() >= static_cast<size_t>(std::numeric_limits<VocalNoteId>::max())) {
      invalid("note_id", "note ID counter exhausted");
    }
    const auto frame_start = static_cast<uint32_t>(region.frame_start);
    const auto frame_end = static_cast<uint32_t>(region.frame_end);
    const std::vector<float> region_f0(refined.begin() + frame_start, refined.begin() + frame_end);
    notes.push_back(measure_from_contour(analysis, source.sample_rate(),
                                         static_cast<VocalNoteId>(notes.size() + 1), {start, end},
                                         frame_start, frame_end, region_f0));
  }
  return notes;
}

VocalNote measure_vocal_note(const Audio& source, const VocalAnalysisData& analysis, VocalNoteId id,
                             SampleRange source_range) {
  if (id == kInvalidVocalNoteId) invalid("note_id", "must be non-zero");
  if (source_range.start < 0 || source_range.end <= source_range.start ||
      source_range.end > static_cast<int64_t>(source.size())) {
    invalid("source_range", "note source range is malformed");
  }
  if (!std::isfinite(analysis.grid.frame_origin_sample) ||
      !std::isfinite(analysis.grid.samples_per_frame) || analysis.grid.samples_per_frame < 1.0 ||
      analysis.f0_hz.empty() || analysis.f0_hz.size() != analysis.voiced.size() ||
      analysis.f0_hz.size() > std::numeric_limits<uint32_t>::max()) {
    invalid("analysis.grid", "analysis grid and arrays are malformed");
  }
  const auto frame_boundary = [&](int64_t sample) -> uint32_t {
    const double rounded = frame_ceil(analysis.grid, sample);
    if (!std::isfinite(rounded)) invalid("source_range", "source frame position is not finite");
    const double frame_count = static_cast<double>(analysis.f0_hz.size());
    if (rounded <= 0.0) return 0;
    if (rounded >= frame_count) return static_cast<uint32_t>(analysis.f0_hz.size());
    return static_cast<uint32_t>(rounded);
  };
  const double first_raw = frame_raw_position(analysis.grid, source_range.start);
  const double last_raw = frame_raw_position(analysis.grid, source_range.end);
  if (!std::isfinite(first_raw) || !std::isfinite(last_raw)) {
    invalid("source_range", "source frame position is not finite");
  }
  const uint32_t frame_start = frame_boundary(source_range.start);
  const uint32_t frame_end = std::max(frame_start, frame_boundary(source_range.end));
  if (frame_end <= frame_start) invalid("source_range", "source span contains no analysis frame");
  return measure_from_contour(analysis, source.sample_rate(), id, source_range, frame_start,
                              frame_end, refine_f0_range(source, analysis, frame_start, frame_end));
}

}  // namespace sonare::editing::vocal_edit
