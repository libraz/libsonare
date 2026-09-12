#include "editing/polyphony/masked_renderer.h"

#include <cstddef>
#include <utility>
#include <vector>

#include "util/exception.h"

namespace sonare::editing::polyphony {

Audio render_masked_notes(const Spectrogram& spec, const NoteMaskSet& masks,
                          const std::vector<note_model::NoteObject>& notes, int length,
                          const note_model::NoteRenderConfig& config) {
  SONARE_CHECK(!spec.empty(), ErrorCode::InvalidParameter);
  SONARE_CHECK(notes.size() == masks.notes.size(), ErrorCode::InvalidParameter);
  SONARE_CHECK(length >= 0, ErrorCode::InvalidParameter);
  note_model::validate_render_config(config);
  for (const note_model::NoteObject& note : notes) {
    note_model::validate_note_for_render(note);
  }

  const Audio residual = residual_spectrum(spec, masks).to_audio(length);
  // A framing that reconstructs to nothing is rejected here, not once per note.
  SONARE_CHECK(!residual.empty(), ErrorCode::InvalidParameter);
  std::vector<float> accumulator(residual.begin(), residual.end());

  for (std::size_t i = 0; i < notes.size(); ++i) {
    // The masked spectrogram is a temporary, so it dies with the statement that
    // inverts it and only its inverse outlives the step.
    const Audio note_audio = apply_note_mask(spec, masks.notes[i]).to_audio(length);
    const Audio edited = note_model::render_notes(note_audio, {notes[i]}, config);
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
