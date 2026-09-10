#include "editing/note_model/note_split_merge.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "util/exception.h"

namespace sonare::editing::note_model {
namespace {

/// Cuts an envelope at @p position of its span, in the same endpoint-anchored
/// linear reading the renderer resamples it under. Both halves carry the value
/// at the cut, so the two together describe the shape the whole one did.
void split_envelope(const std::vector<float>& envelope, double position, std::vector<float>& head,
                    std::vector<float>& tail) {
  head.clear();
  tail.clear();
  if (envelope.empty()) return;
  if (envelope.size() == 1) {
    head = envelope;
    tail = envelope;
    return;
  }

  const double cut = position * static_cast<double>(envelope.size() - 1);
  const size_t lo = std::min(static_cast<size_t>(cut), envelope.size() - 1);
  const size_t hi = std::min(lo + 1, envelope.size() - 1);
  const float frac = static_cast<float>(cut - static_cast<double>(lo));

  head.assign(envelope.begin(), envelope.begin() + static_cast<std::ptrdiff_t>(lo) + 1);
  if (frac > 0.0f) {
    const float value = envelope[lo] * (1.0f - frac) + envelope[hi] * frac;
    head.push_back(value);
    tail.push_back(value);
  }
  tail.insert(tail.end(), envelope.begin() + static_cast<std::ptrdiff_t>(frac > 0.0f ? hi : lo),
              envelope.end());
}

}  // namespace

std::vector<NoteObject> split_note(const Audio& audio, const pitch_editor::F0Track& track,
                                   const std::vector<NoteObject>& notes, size_t index, int frame,
                                   const NoteExtractorConfig& config) {
  SONARE_CHECK(index < notes.size(), ErrorCode::InvalidParameter);
  const NoteObject& source = notes[index];
  SONARE_CHECK(frame > source.frame_start && frame < source.frame_end, ErrorCode::InvalidParameter);

  NoteObject head = make_note(audio, track, source.frame_start, frame, config);
  NoteObject tail = make_note(audio, track, frame, source.frame_end, config);
  head.edit = source.edit;
  tail.edit = source.edit;
  const double position = static_cast<double>(frame - source.frame_start) /
                          static_cast<double>(source.frame_end - source.frame_start);
  split_envelope(source.edit.amplitude_envelope, position, head.edit.amplitude_envelope,
                 tail.edit.amplitude_envelope);

  std::vector<NoteObject> result;
  result.reserve(notes.size() + 1);
  result.insert(result.end(), notes.begin(), notes.begin() + static_cast<std::ptrdiff_t>(index));
  result.push_back(std::move(head));
  result.push_back(std::move(tail));
  result.insert(result.end(), notes.begin() + static_cast<std::ptrdiff_t>(index) + 1, notes.end());
  return result;
}

std::vector<NoteObject> merge_notes(const Audio& audio, const pitch_editor::F0Track& track,
                                    const std::vector<NoteObject>& notes, size_t first, size_t last,
                                    const NoteExtractorConfig& config) {
  SONARE_CHECK(first < last && last < notes.size(), ErrorCode::InvalidParameter);

  NoteObject merged =
      make_note(audio, track, notes[first].frame_start, notes[last].frame_end, config);
  merged.edit = notes[first].edit;

  std::vector<NoteObject> result;
  result.reserve(notes.size() - (last - first));
  result.insert(result.end(), notes.begin(), notes.begin() + static_cast<std::ptrdiff_t>(first));
  result.push_back(std::move(merged));
  result.insert(result.end(), notes.begin() + static_cast<std::ptrdiff_t>(last) + 1, notes.end());
  return result;
}

}  // namespace sonare::editing::note_model
