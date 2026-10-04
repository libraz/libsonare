// SONARE_WASM_EXCEPTION_UNWIND: release analysis and segmentation buffers when validation throws.
#include "editing/vocal_edit/analysis.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <string>

#include "editing/pitch_editor/f0_provider.h"
#include "editing/pitch_editor/note_segmenter.h"
#include "editing/vocal_edit/source_digest.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/sha256.h"

namespace sonare::editing::vocal_edit {

using sonare::constants::kA4Hz;
using sonare::constants::kCentsPerSemitone;
using sonare::constants::kMidiA4;
using sonare::constants::kSemitonesPerOctave;

namespace {

[[noreturn]] void invalid(const std::string& field, const std::string& message) {
  throw VocalEditException(VocalReason::kInvalidInput, message, field);
}

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

double sample_at(const AnalysisGrid& grid, uint32_t frame) {
  return grid.frame_origin_sample + static_cast<double>(frame) * grid.samples_per_frame;
}

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
      const double centre = sample_at(analysis.grid, static_cast<uint32_t>(frame));
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
  pitch_editor::F0Track track;
  track.f0_hz = analysis.f0_hz;
  track.voiced.resize(analysis.voiced.size());
  for (size_t i = 0; i < analysis.voiced.size(); ++i) track.voiced[i] = analysis.voiced[i] != 0;
  track.sample_rate = source.sample_rate();
  track.frame_rate_hz = static_cast<float>(source.sample_rate() / analysis.grid.samples_per_frame);
  track.hop_length = std::max(1, static_cast<int>(std::llround(analysis.grid.samples_per_frame)));
  pitch_editor::NoteSegmenter segmenter(
      {static_cast<float>(analysis.settings.segmentation_threshold_cents),
       static_cast<float>(analysis.settings.minimum_note_ms),
       static_cast<float>(analysis.settings.reference_hz)});
  const auto regions = segmenter.segment(track);
  std::vector<VocalNote> notes;
  notes.reserve(regions.size());
  for (const auto& region : regions) {
    const int64_t start = std::max<int64_t>(
        0, std::min<int64_t>(static_cast<int64_t>(source.size()),
                             rounded_sample(sample_at(analysis.grid, region.frame_start))));
    int64_t end = std::max<int64_t>(
        start, std::min<int64_t>(static_cast<int64_t>(source.size()),
                                 rounded_sample(sample_at(analysis.grid, region.frame_end))));
    if (end <= start) end = std::min<int64_t>(static_cast<int64_t>(source.size()), start + 1);
    if (end <= start) continue;
    if (notes.size() >= static_cast<size_t>(std::numeric_limits<VocalNoteId>::max())) {
      invalid("note_id", "note ID counter exhausted");
    }
    VocalNote note = measure_vocal_note(source, analysis,
                                        static_cast<VocalNoteId>(notes.size() + 1), {start, end});
    note.analysis_frame_start = static_cast<uint32_t>(region.frame_start);
    note.analysis_frame_end = static_cast<uint32_t>(region.frame_end);
    notes.push_back(std::move(note));
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
    const double raw = (static_cast<double>(sample) - analysis.grid.frame_origin_sample) /
                       analysis.grid.samples_per_frame;
    const double rounded = std::ceil(raw - 1.0e-12);
    if (!std::isfinite(rounded)) invalid("source_range", "source frame position is not finite");
    const double frame_count = static_cast<double>(analysis.f0_hz.size());
    if (rounded <= 0.0) return 0;
    if (rounded >= frame_count) return static_cast<uint32_t>(analysis.f0_hz.size());
    return static_cast<uint32_t>(rounded);
  };
  const double first_raw =
      (static_cast<double>(source_range.start) - analysis.grid.frame_origin_sample) /
      analysis.grid.samples_per_frame;
  const double last_raw =
      (static_cast<double>(source_range.end) - analysis.grid.frame_origin_sample) /
      analysis.grid.samples_per_frame;
  if (!std::isfinite(first_raw) || !std::isfinite(last_raw)) {
    invalid("source_range", "source frame position is not finite");
  }
  const uint32_t frame_start = frame_boundary(source_range.start);
  const uint32_t frame_end = std::max(frame_start, frame_boundary(source_range.end));
  if (frame_end <= frame_start) invalid("source_range", "source span contains no analysis frame");
  std::vector<double> midi;
  for (uint32_t frame = frame_start; frame < frame_end; ++frame) {
    const size_t i = static_cast<size_t>(frame);
    if (analysis.voiced[i] != 0 && analysis.f0_hz[i] > 0.0f) {
      midi.push_back(kMidiA4 + kSemitonesPerOctave *
                                   std::log2(static_cast<double>(analysis.f0_hz[i]) / kA4Hz));
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
    std::sort(midi.begin(), midi.end());
    note.centre_midi = midi[midi.size() / 2];
    note.median_hz = kA4Hz * std::pow(2.0, (note.centre_midi - kMidiA4) / kSemitonesPerOctave);
    double mad = 0.0;
    for (const double value : midi) mad += std::abs(value - note.centre_midi);
    mad /= static_cast<double>(midi.size());
    note.f0_stability =
        std::max(0.0, std::min(1.0, 1.0 - mad / (analysis.settings.segmentation_threshold_cents /
                                                 kCentsPerSemitone)));
  }
  note.edit = VocalNoteEdit::identity_for(source_range);
  static_cast<void>(source);
  return note;
}

}  // namespace sonare::editing::vocal_edit
