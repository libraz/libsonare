#include "editing/polyphony/polyphonic_edit.h"

#include <cstddef>
#include <limits>

#include "editing/polyphony/inharmonicity.h"
#include "editing/polyphony/masked_notes.h"
#include "editing/polyphony/masked_renderer.h"
#include "editing/polyphony/shared_bins.h"
#include "util/exception.h"

namespace sonare::editing::polyphony {

PolyphonicAnalysis analyze_polyphonic(const Audio& audio, const PolyphonicEditConfig& config) {
  // Truncating here would describe a prefix of the source as the whole of it.
  SONARE_CHECK(audio.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()),
               ErrorCode::InvalidParameter);

  PolyphonicAnalysis analysis;
  analysis.length = static_cast<int>(audio.size());
  analysis.spectrum = Spectrogram::compute(audio, config.extraction.stft);
  analysis.track = extract_multi_f0(audio, analysis.spectrum, config.extraction);
  analysis.masks = build_note_masks(analysis.spectrum, analysis.track, config.masks);
  if (config.estimate_inharmonicity) {
    // The fit reads the declared geometry to tell a contested partial from a clear
    // one, and then places its own, so the claims are built twice rather than once.
    analysis.inharmonicity_fit = estimate_track_inharmonicity(analysis.spectrum, analysis.track,
                                                              analysis.masks, config.inharmonicity);
    analysis.masks = build_note_masks(analysis.spectrum, analysis.track, config.masks,
                                      analysis.inharmonicity_fit);
  }
  // An equal split is what build_note_masks can decide without reading the data;
  // this is the stage that reads it.
  analysis.masks =
      solve_shared_bins(analysis.spectrum, analysis.masks, analysis.track, config.shared_bins);
  analysis.notes = make_masked_notes(analysis.spectrum, analysis.track, analysis.masks,
                                     analysis.length, config.notes);
  return analysis;
}

Audio render_polyphonic(const PolyphonicAnalysis& analysis,
                        const note_model::NoteRenderConfig& config) {
  return render_masked_notes(analysis.spectrum, analysis.masks, analysis.notes, analysis.length,
                             config);
}

}  // namespace sonare::editing::polyphony
