#include "editing/polyphony/masked_renderer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "util/exception.h"
#include "util/parameter_checks.h"

namespace sonare::editing::polyphony {

namespace check = sonare::util::check;

Audio render_masked_notes(const Spectrogram& spec, const NoteMaskSet& masks,
                          const std::vector<note_model::NoteObject>& notes, int length,
                          const note_model::NoteRenderConfig& config) {
  if (spec.empty()) check::refuse("spec", "must not be empty");
  if (notes.size() != masks.notes.size()) {
    check::refuse_relation({"notes", "masks.notes"},
                           "{0} must have the same number of entries as {1}");
  }
  constexpr check::NumericDomain kOutputLengthDomain =
      check::integer_domain_of<decltype(length)>(0);
  check::integer_within(length, kOutputLengthDomain, "length");
  note_model::validate_render_config(config);
  // Length 0 takes the framing's own count, known only once an inverse has run.
  const int64_t span_bound = length > 0 ? length : -1;
  const auto validate_note_at = [&](std::size_t i, int64_t bound) {
    try {
      note_model::validate_note_for_render(notes[i], bound);
    } catch (const SonareParameterRefusal& e) {
      const std::string parent = "notes[" + std::to_string(i) + "]";
      throw e.respelled(
          e.code(),
          [&parent](const std::string& subject) {
            if (subject.compare(0, 5, "note.") == 0) return parent + subject.substr(4);
            return parent + "." + subject;
          },
          e.caller_spelled());
    }
  };
  for (std::size_t i = 0; i < notes.size(); ++i) {
    validate_note_at(i, span_bound);
  }

  const Audio residual = residual_spectrum(spec, masks).to_audio(length);
  // A framing that reconstructs to nothing is rejected here, not once per note.
  SONARE_CHECK(!residual.empty(), ErrorCode::InvalidParameter);
  if (length == 0) {
    for (std::size_t i = 0; i < notes.size(); ++i) {
      validate_note_at(i, static_cast<int64_t>(residual.size()));
    }
  }
  std::vector<float> accumulator(residual.begin(), residual.end());

  for (std::size_t i = 0; i < notes.size(); ++i) {
    // Only the frames the note reaches are inverted; the rest of its audio is silence.
    const NoteSegment segment = invert_note_mask(spec, masks.notes[i], length);
    SONARE_CHECK(segment.first_sample >= 0 &&
                     static_cast<uint64_t>(segment.first_sample) + segment.audio.size() <=
                         accumulator.size(),
                 ErrorCode::InvalidParameter);
    std::vector<float> samples(accumulator.size(), 0.0f);
    std::copy(segment.audio.begin(), segment.audio.end(),
              samples.begin() + static_cast<std::ptrdiff_t>(segment.first_sample));
    const Audio note_audio = Audio::from_vector(std::move(samples), residual.sample_rate());
    Audio edited;
    try {
      edited = note_model::render_notes(note_audio, {notes[i]}, config);
    } catch (const SonareParameterRefusal& e) {
      const std::string nested = "notes[0].";
      if (e.subject().compare(0, nested.size(), nested) != 0) throw;
      const std::string parent = "notes[" + std::to_string(i) + "].";
      throw e.respelled(
          e.code(),
          [&nested, &parent](const std::string& subject) {
            return subject.compare(0, nested.size(), nested) == 0
                       ? parent + subject.substr(nested.size())
                       : subject;
          },
          e.caller_spelled());
    }
    // render_notes returns its input's length, so every inverse in the call was
    // given the same one; an error rather than a clamp if that ever parts.
    SONARE_CHECK(edited.size() == accumulator.size(), ErrorCode::InvalidParameter);
    for (std::size_t j = 0; j < accumulator.size(); ++j) {
      accumulator[j] += edited[j];
    }
  }

  return Audio::from_vector(std::move(accumulator), residual.sample_rate());
}

}  // namespace sonare::editing::polyphony
