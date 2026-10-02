#include "editing/note_model/note_split_merge.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

#include "core/audio.h"
#include "editing/note_model/note_renderer.h"
#include "util/exception.h"
#include "util/numeric_validation.h"

namespace sonare::editing::note_model {
namespace {

/// Evaluates the envelope at a source-sample coordinate. The renderer reads an
/// envelope over the [0, sample_count - 1] grid, so this uses that same grid
/// rather than the frame boundary a split happened to be requested at.
float envelope_at_sample(const std::vector<float>& envelope, long double sample,
                         int64_t source_length) noexcept {
  if (envelope.empty()) return 0.0f;
  if (envelope.size() == 1 || source_length <= 1) return envelope.front();

  const long double source_span = static_cast<long double>(source_length - 1);
  const long double position = std::clamp(sample, 0.0L, source_span) *
                               static_cast<long double>(envelope.size() - 1) / source_span;
  const size_t lo = std::min(static_cast<size_t>(position), envelope.size() - 1);
  const size_t hi = std::min(lo + 1, envelope.size() - 1);
  const float fraction = static_cast<float>(position - static_cast<long double>(lo));
  return envelope[lo] * (1.0f - fraction) + envelope[hi] * fraction;
}

bool checked_multiply(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t* out) noexcept {
  if (out == nullptr || (rhs != 0 && lhs > std::numeric_limits<std::uint64_t>::max() / rhs)) {
    return false;
  }
  *out = lhs * rhs;
  return true;
}

bool checked_lcm(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t limit,
                 std::uint64_t* out) noexcept {
  if (out == nullptr || lhs == 0 || rhs == 0) return false;
  const std::uint64_t divisor = std::gcd(lhs, rhs);
  const std::uint64_t reduced = lhs / divisor;
  if (reduced > limit / rhs) return false;
  *out = reduced * rhs;
  return true;
}

/// Resamples one child over a rational sample grid that contains every source
/// envelope knot in that child. A piecewise-linear function is reproduced
/// exactly by linear interpolation once all of its knots are grid points. The
/// gcd/lcm calculation keeps the grid as small as possible; the shared audio
/// resource ceiling prevents a pathological edit from allocating an unbounded
/// envelope while the checked products keep the arithmetic defined.
std::vector<float> resample_envelope_piece(const std::vector<float>& envelope,
                                           int64_t source_length, int64_t begin_sample,
                                           int64_t end_sample) {
  if (envelope.empty()) return {};
  if (envelope.size() == 1) return envelope;
  SONARE_CHECK(source_length > 0 && begin_sample >= 0 && end_sample >= begin_sample &&
                   end_sample < source_length,
               ErrorCode::InvalidParameter);
  SONARE_CHECK(envelope.size() <= kMaxAudioBufferSize, ErrorCode::InvalidParameter);

  const std::uint64_t source_span = static_cast<std::uint64_t>(source_length - 1);
  const std::uint64_t piece_span = static_cast<std::uint64_t>(end_sample - begin_sample);
  const std::uint64_t knot_denominator = static_cast<std::uint64_t>(envelope.size() - 1);
  if (piece_span == 0 || source_span == 0) {
    return {envelope_at_sample(envelope, static_cast<long double>(begin_sample), source_length)};
  }

  std::uint64_t denominator = 0;
  SONARE_CHECK(checked_multiply(knot_denominator, piece_span, &denominator),
               ErrorCode::InvalidParameter);
  std::uint64_t grid_intervals = 1;
  bool use_sample_grid = false;
  for (std::uint64_t knot = 1; knot < knot_denominator; ++knot) {
    std::uint64_t knot_position = 0;
    std::uint64_t begin_term = 0;
    SONARE_CHECK(checked_multiply(knot, source_span, &knot_position) &&
                     checked_multiply(static_cast<std::uint64_t>(begin_sample), knot_denominator,
                                      &begin_term),
                 ErrorCode::InvalidParameter);
    if (knot_position <= begin_term) continue;
    const std::uint64_t numerator = knot_position - begin_term;
    if (numerator >= denominator) continue;

    // The knot is at numerator / denominator in the child interval. The
    // child grid needs a multiple of denominator / gcd(numerator, denominator)
    // intervals for that point to be one of its nodes.
    const std::uint64_t needed = denominator / std::gcd(numerator, denominator);
    if (!checked_lcm(grid_intervals, needed, piece_span, &grid_intervals)) {
      // The renderer only observes the integer source-sample grid. Falling
      // back to one value per child sample is both exact and smaller than the
      // rational knot grid that would otherwise grow as an LCM.
      use_sample_grid = true;
      break;
    }
  }

  if (use_sample_grid) grid_intervals = piece_span;
  std::vector<float> result(static_cast<size_t>(grid_intervals + 1));
  for (std::uint64_t grid = 0; grid <= grid_intervals; ++grid) {
    const long double sample =
        static_cast<long double>(begin_sample) + static_cast<long double>(grid) *
                                                     static_cast<long double>(piece_span) /
                                                     static_cast<long double>(grid_intervals);
    result[static_cast<size_t>(grid)] = envelope_at_sample(envelope, sample, source_length);
  }
  return result;
}

/// Cuts an envelope at the source-sample boundary represented by the two
/// resulting note spans. The old two-point representation is retained for
/// compatibility with callers that inspect the compact ramp values directly;
/// longer envelopes use the rational grid above so every original knot remains
/// a node of the child interpolation.
void split_envelope(const std::vector<float>& envelope, double position, int64_t source_length,
                    int64_t head_end_sample, int64_t tail_begin_sample, int64_t tail_end_sample,
                    std::vector<float>& head, std::vector<float>& tail) {
  head.clear();
  tail.clear();
  if (envelope.empty()) return;
  if (envelope.size() == 1) {
    head = envelope;
    tail = envelope;
    return;
  }

  if (envelope.size() == 2) {
    const double cut = position;
    const float value =
        envelope[0] * static_cast<float>(1.0 - cut) + envelope[1] * static_cast<float>(cut);
    head = {envelope[0], value};
    tail = {value, envelope[1]};
    return;
  }

  head = resample_envelope_piece(envelope, source_length, 0, head_end_sample);
  tail = resample_envelope_piece(envelope, source_length, tail_begin_sample, tail_end_sample);
}

int64_t saturating_add(int64_t lhs, int64_t rhs) noexcept {
  if (rhs > 0 && lhs > std::numeric_limits<int64_t>::max() - rhs) {
    return std::numeric_limits<int64_t>::max();
  }
  if (rhs < 0 && lhs < std::numeric_limits<int64_t>::min() - rhs) {
    return std::numeric_limits<int64_t>::min();
  }
  return lhs + rhs;
}

int64_t stretch_growth(int64_t source_length, float ratio) noexcept {
  if (source_length <= 0 || !std::isfinite(ratio) || ratio <= 0.0f || ratio == 1.0f) {
    return 0;
  }

  // Match time_stretch's checked projection exactly: it receives the rate
  // reciprocal and rounds ceil(input_count / rate), which can differ from
  // ceil(input_count * ratio) at a float boundary such as 5 * 0.8f.
  std::size_t projected = 0;
  constexpr std::size_t kProjectionLimit =
      std::min<std::size_t>(kMaxAudioBufferSize, static_cast<std::size_t>(INT_MAX));
  const float rate = 1.0f / ratio;
  if (!numeric::checked_projected_count(static_cast<std::size_t>(source_length), rate,
                                        kProjectionLimit, &projected)) {
    // The renderer rejects this same projection as over budget. Keep the
    // split arithmetic defined in case the caller inspects the edits before
    // rendering them.
    return std::numeric_limits<int64_t>::max() - source_length;
  }
  return static_cast<int64_t>(projected) - source_length;
}

/// Keep two child note spans inside the source and disjoint after make_note's
/// terminal-frame nudge. The bounds are repaired before envelope coordinates
/// and stretch placement are derived, so both calculations describe the same
/// physical source span.
void repair_split_boundary(const NoteObject& source, NoteObject& head, NoteObject& tail) noexcept {
  const int64_t lower = source.onset_sample;
  const int64_t upper = std::max(source.offset_sample, lower);
  head.onset_sample = std::clamp(head.onset_sample, lower, upper);
  head.offset_sample = std::clamp(head.offset_sample, lower, upper);
  if (head.offset_sample < head.onset_sample) head.offset_sample = head.onset_sample;
  tail.onset_sample = std::clamp(tail.onset_sample, lower, upper);
  tail.offset_sample = std::clamp(tail.offset_sample, lower, upper);
  if (tail.offset_sample < tail.onset_sample) tail.offset_sample = tail.onset_sample;

  if (head.offset_sample > tail.onset_sample) {
    if (tail.onset_sample > head.onset_sample) {
      head.offset_sample = tail.onset_sample;
    } else {
      // No positive disjoint head exists at this sample resolution. Mark it
      // empty; split_note omits it below and keeps the physical tail.
      head.offset_sample = head.onset_sample;
    }
  }
}

}  // namespace

std::vector<NoteObject> split_note(const Audio& audio, const pitch_editor::F0Track& track,
                                   const std::vector<NoteObject>& notes, size_t index, int frame,
                                   const NoteExtractorConfig& config) {
  SONARE_CHECK(index < notes.size(), ErrorCode::InvalidParameter);
  for (const NoteObject& note : notes) {
    SONARE_CHECK(is_valid_note_span(note, static_cast<int64_t>(audio.size())),
                 ErrorCode::InvalidParameter);
    SONARE_CHECK(is_valid_note_edit(note.edit), ErrorCode::InvalidParameter);
  }
  const NoteObject& source = notes[index];
  SONARE_CHECK(frame > source.frame_start && frame < source.frame_end, ErrorCode::InvalidParameter);

  NoteObject head = make_note(audio, track, source.frame_start, frame, config);
  NoteObject tail = make_note(audio, track, frame, source.frame_end, config);
  repair_split_boundary(source, head, tail);
  head.edit = source.edit;
  tail.edit = source.edit;
  const bool head_present = head.length_samples() > 0;
  const bool tail_present = tail.length_samples() > 0;
  if (head_present && tail_present) {
    tail.edit.time_offset_samples =
        saturating_add(tail.edit.time_offset_samples,
                       stretch_growth(head.length_samples(), source.edit.time_stretch_ratio));
    const double position = static_cast<double>(frame - source.frame_start) /
                            static_cast<double>(source.frame_end - source.frame_start);
    const int64_t source_length = source.length_samples();
    const int64_t head_end_sample = head.offset_sample - source.onset_sample - 1;
    const int64_t tail_begin_sample = tail.onset_sample - source.onset_sample;
    const int64_t tail_end_sample = tail.offset_sample - source.onset_sample - 1;
    split_envelope(source.edit.amplitude_envelope, position, source_length, head_end_sample,
                   tail_begin_sample, tail_end_sample, head.edit.amplitude_envelope,
                   tail.edit.amplitude_envelope);
  }

  std::vector<NoteObject> result;
  result.reserve(notes.size() + 1);
  result.insert(result.end(), notes.begin(), notes.begin() + static_cast<std::ptrdiff_t>(index));
  if (head_present) result.push_back(std::move(head));
  if (tail_present) result.push_back(std::move(tail));
  result.insert(result.end(), notes.begin() + static_cast<std::ptrdiff_t>(index) + 1, notes.end());
  return result;
}

std::vector<NoteObject> merge_notes(const Audio& audio, const pitch_editor::F0Track& track,
                                    const std::vector<NoteObject>& notes, size_t first, size_t last,
                                    const NoteExtractorConfig& config) {
  SONARE_CHECK(first < last && last < notes.size(), ErrorCode::InvalidParameter);
  for (const NoteObject& note : notes) {
    SONARE_CHECK(is_valid_note_span(note, static_cast<int64_t>(audio.size())),
                 ErrorCode::InvalidParameter);
    SONARE_CHECK(is_valid_note_edit(note.edit), ErrorCode::InvalidParameter);
  }

  NoteObject merged =
      make_note(audio, track, notes[first].frame_start, notes[last].frame_end, config);
  const int64_t physical_onset = notes[first].onset_sample;
  const int64_t physical_offset = notes[last].offset_sample;
  merged.onset_sample = std::clamp(merged.onset_sample, physical_onset, physical_offset);
  merged.offset_sample = std::clamp(merged.offset_sample, physical_onset, physical_offset);
  if (merged.offset_sample < merged.onset_sample) merged.offset_sample = merged.onset_sample;
  merged.edit = notes[first].edit;

  std::vector<NoteObject> result;
  result.reserve(notes.size() - (last - first));
  result.insert(result.end(), notes.begin(), notes.begin() + static_cast<std::ptrdiff_t>(first));
  result.push_back(std::move(merged));
  result.insert(result.end(), notes.begin() + static_cast<std::ptrdiff_t>(last) + 1, notes.end());
  return result;
}

}  // namespace sonare::editing::note_model
