/// @file polyphony.cpp
/// @brief Embind binding for polyphonic editing: one analysis held as a handle,
///        the notes it found, and a render back to audio.
///
/// The shape is sonare_c_polyphony.h's -- an opaque analysis and the operations
/// that header names -- and the call path is not: that C translation unit is not
/// linked into WASM, so every method reaches editing::polyphony directly and
/// resolves its own config and indices the way the C entry points do.
///
/// What stays inside the handle is the reason the handle exists. The analysis
/// holds the source's complex spectrogram and, per note, the complex weight of
/// every bin it claimed; neither crosses, and nothing here reports a per-bin
/// figure. What crosses is what a host acts on: the notes, each note's pending
/// edit, the per-frame voice count, and per note a pitch, a level and a salience
/// curve.
///
/// Nothing here catches -- a refusal is a SonareException, which the module wrapper
/// turns into a SonareError -- so there is no catch arm for emscripten's default
/// DISABLE_EXCEPTION_CATCHING to elide.

#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "editing/note_model/note_renderer.h"
#include "editing/polyphony/polyphonic_edit.h"
#include "wasm/bindings/common/common.h"
#include "wasm/bindings/common/note_val.h"

namespace {

using sonare_wasm_editing::noteEditFromVal;
using sonare_wasm_editing::noteObjectsToVal;

// Mirrors sonare_c_polyphony.cpp: an absent key keeps the core default, which is
// what sonare_polyphonic_config_init sets, and a present one is read literally. Every
// range is the stage's own, refused where it is read.
editing::polyphony::PolyphonicEditConfig configFromVal(const val& config) {
  editing::polyphony::PolyphonicEditConfig out;
  // Absence is this function's own; the conversion is the shared reader's, and a
  // value that cannot be a setting at all is refused here.
  const auto set_float = [&config](const char* key, float& field) {
    const float value = floatProperty(config, key, field);
    if (!std::isfinite(value)) {
      throw WasmRangeError(std::string(key) + " must be a finite number");
    }
    field = value;
  };
  const auto set_int = [&config](const char* key, int& field) {
    field = intProperty(config, key, field);
  };

  auto& stft = out.extraction.stft;
  set_int("nFft", stft.n_fft);
  set_int("hopLength", stft.hop_length);
  set_int("winLength", stft.win_length);

  auto& spectrum = out.extraction.spectrum;
  set_float("centRefHz", spectrum.ref_hz);
  set_float("centsPerBin", spectrum.cents_per_bin);
  set_float("centMaxHz", spectrum.max_hz);
  spectrum.use_tonality = boolProperty(config, "tonality", spectrum.use_tonality);

  auto& salience = out.extraction.estimation.salience;
  set_int("salienceHarmonics", salience.n_harmonics);
  set_float("f0MinHz", salience.f0_min_hz);
  set_float("f0MaxHz", salience.f0_max_hz);
  set_float("salienceAlphaHz", salience.alpha_hz);
  set_float("salienceBetaHz", salience.beta_hz);
  set_float("salienceInharmonicity", salience.inharmonicity);

  auto& estimation = out.extraction.estimation;
  set_int("maxPolyphony", estimation.max_polyphony);
  set_float("minFramePeakRatio", estimation.min_frame_peak_ratio);
  set_float("minSeparationCents", estimation.min_separation_cents);
  set_float("subtractionFactor", estimation.subtraction_factor);

  auto& ridges = out.extraction.ridges;
  set_float("maxJumpCents", ridges.max_jump_cents);
  set_float("minRidgePeakRatio", ridges.min_ridge_peak_ratio);
  set_float("minRidgeDurationMs", ridges.min_duration_ms);
  set_float("reattackRatio", ridges.reattack_ratio);

  set_int("maskHarmonics", out.masks.n_harmonics);
  set_float("claimLobes", out.masks.claim_lobes);
  set_float("inharmonicity", out.masks.inharmonicity);

  out.estimate_inharmonicity =
      boolProperty(config, "estimateInharmonicity", out.estimate_inharmonicity);
  set_int("inharmonicityMinPartials", out.inharmonicity.min_partials);
  set_float("inharmonicityMaxResidualBins", out.inharmonicity.max_residual_bins);
  set_float("inharmonicityMaxStretch", out.inharmonicity.max_inharmonicity);

  set_int("windowFrames", out.shared_bins.window_frames);
  set_float("minPartialSeparation", out.shared_bins.min_partial_separation);
  set_float("maxFitResidual", out.shared_bins.max_fit_residual);
  set_float("maxWeightModulus", out.shared_bins.max_weight_modulus);
  set_float("maxRefineHz", out.shared_bins.max_refine_hz);
  set_float("f0ToleranceCents", out.shared_bins.f0_tolerance_cents);

  set_float("segmentationThresholdCents", out.notes.segmenter.segmentation_threshold_cents);
  set_float("minNoteMs", out.notes.segmenter.min_note_ms);
  set_float("referenceHz", out.notes.segmenter.reference_hz);
  return out;
}

// The config is resolved before the samples are copied, the way the C entry point
// resolves it before run_offline.
editing::polyphony::PolyphonicAnalysis analyzeFromVal(const char* fn, const char* name, val samples,
                                                      int sample_rate, val config) {
  const editing::polyphony::PolyphonicEditConfig resolved = configFromVal(config);
  return editing::polyphony::analyze_polyphonic(loadValidatedAudio(fn, name, samples, sample_rate),
                                                resolved);
}

}  // namespace

class PolyphonicAnalysisWasm {
 public:
  PolyphonicAnalysisWasm(val samples, int sample_rate, val config)
      : analysis_(analyzeFromVal("analyze_polyphonic", "samples", samples, sample_rate, config)) {}

  // A count crosses as a double: every other count on this surface does, and a
  // host sizes nothing from it -- the accessors below return whole arrays.
  double noteCount() const { return static_cast<double>(analysis_.notes.size()); }

  int frameCount() const { return analysis_.spectrum.n_frames(); }

  // The same note shape the by-value door returns, through the same converter: an
  // edit means one thing whichever door applies it, so a note has to read the same
  // way through both.
  val notes() const { return noteObjectsToVal(analysis_.notes); }

  void setNoteEdit(double note, val edit) {
    const std::size_t index = noteIndex(note);
    std::size_t cumulative_count = 0;
    // Whatever the render rejects about an edit it rejects on the way out, so a
    // field is not checked twice here and a refusal names one place.
    analysis_.notes[index].edit =
        noteEditFromVal(edit, "setNoteEdit edit", "setNoteEdit input", &cumulative_count);
  }

  val polyphony() const { return vectorToInt32Array(analysis_.track.polyphony); }

  val noteF0(double note) const {
    return vectorToFloat32Array(analysis_.notes[noteIndex(note)].f0_hz.values);
  }

  val noteAmplitude(double note) const {
    return vectorToFloat32Array(analysis_.notes[noteIndex(note)].amplitude.values);
  }

  // The one curve here that is not a measurement: the points a caller last handed
  // setNoteEdit, indexed from 0 rather than over the note's span. The same array a
  // note object carries inline; it is here because the C ABI's note struct reports
  // an envelope count and a count whose points cannot be fetched promises what it
  // cannot deliver.
  val noteEnvelope(double note) const {
    return vectorToFloat32Array(analysis_.notes[noteIndex(note)].edit.amplitude_envelope);
  }

  val noteSalience(double note) const {
    const std::size_t index = noteIndex(note);
    const editing::note_model::NoteObject& object = analysis_.notes[index];
    const std::vector<editing::polyphony::NoteMask>& masks = analysis_.masks.notes;
    if (index >= masks.size()) {
      throw SonareException(ErrorCode::InvalidState, "noteSalience: the note has no claim set");
    }
    const int ridge_index = masks[index].ridge_index;
    const std::vector<editing::polyphony::F0Ridge>& ridges = analysis_.track.ridges;
    if (ridge_index < 0 || static_cast<std::size_t>(ridge_index) >= ridges.size()) {
      throw SonareException(ErrorCode::InvalidState, "noteSalience: the claim set has no ridge");
    }

    // The ridge's curve, not the note's, so it is read through the ridge's own
    // start and a frame the ridge does not reach is 0 rather than the neighbour's
    // value.
    const editing::polyphony::F0Ridge& ridge = ridges[static_cast<std::size_t>(ridge_index)];
    std::vector<float> span(
        static_cast<std::size_t>(std::max(0, object.frame_end - object.frame_start)), 0.0f);
    for (std::size_t i = 0; i < span.size(); ++i) {
      const long long frame = static_cast<long long>(object.frame_start) +
                              static_cast<long long>(i) - static_cast<long long>(ridge.frame_start);
      if (frame < 0 || static_cast<std::size_t>(frame) >= ridge.salience.size()) continue;
      span[i] = ridge.salience[static_cast<std::size_t>(frame)];
    }
    return vectorToFloat32Array(span);
  }

  // Empty is the fit not having been asked for. Any other length would break the
  // per-note indexing this reports under, which an analysis built here cannot do.
  val noteInharmonicity() const {
    const std::vector<float>& fitted = analysis_.inharmonicity_fit;
    if (!fitted.empty() && fitted.size() != analysis_.notes.size()) {
      throw SonareException(ErrorCode::InvalidState,
                            "noteInharmonicity: the fit does not cover every note");
    }
    return vectorToFloat32Array(fitted);
  }

  val render(val options) const {
    // Resolved the way renderNotes resolves the same two fields, so one config
    // means one thing whichever door applies it.
    editing::note_model::NoteRenderConfig config;
    const float fade_ms = floatProperty(options, "fadeMs", 0.0f);
    const float vibrato_cutoff_hz = floatProperty(options, "vibratoCutoffHz", 0.0f);
    if (!std::isfinite(fade_ms) || fade_ms < 0.0f || !std::isfinite(vibrato_cutoff_hz) ||
        vibrato_cutoff_hz < 0.0f) {
      throw WasmRangeError("render: fadeMs and vibratoCutoffHz must be finite and non-negative");
    }
    if (fade_ms > 0.0f) config.fade_ms = fade_ms;
    if (vibrato_cutoff_hz > 0.0f) config.decomposition.vibrato_cutoff_hz = vibrato_cutoff_hz;

    const Audio rendered = editing::polyphony::render_polyphonic(analysis_, config);
    return vectorToFloat32Array(rendered.data(), rendered.size());
  }

 private:
  // A note index arrives as a bare JS number, so it is checked before it indexes
  // anything: embind casts a JS number to size_t, which wraps a negative one into
  // a huge in-range-looking value.
  std::size_t noteIndex(double note) const {
    const std::size_t index = wasmIndexArg(note, "note index");
    if (index >= analysis_.notes.size()) {
      throw WasmRangeError("note index is out of range");
    }
    return index;
  }

  editing::polyphony::PolyphonicAnalysis analysis_;
};

PolyphonicAnalysisWasm* createPolyphonicAnalysis(val samples, const val& sample_rate, val config) {
  return new PolyphonicAnalysisWasm(samples, checkedIntFromVal(sample_rate, "sampleRate"), config);
}

void registerPolyphonyBindings() {
  class_<PolyphonicAnalysisWasm>("PolyphonicAnalysis")
      .property("noteCount", &PolyphonicAnalysisWasm::noteCount)
      .property("frameCount", &PolyphonicAnalysisWasm::frameCount)
      .function("notes", &PolyphonicAnalysisWasm::notes)
      .function("setNoteEdit", &PolyphonicAnalysisWasm::setNoteEdit)
      .function("polyphony", &PolyphonicAnalysisWasm::polyphony)
      .function("noteF0", &PolyphonicAnalysisWasm::noteF0)
      .function("noteAmplitude", &PolyphonicAnalysisWasm::noteAmplitude)
      .function("noteSalience", &PolyphonicAnalysisWasm::noteSalience)
      .function("noteInharmonicity", &PolyphonicAnalysisWasm::noteInharmonicity)
      .function("noteEnvelope", &PolyphonicAnalysisWasm::noteEnvelope)
      .function("render", &PolyphonicAnalysisWasm::render);
  function("createPolyphonicAnalysis", &createPolyphonicAnalysis, allow_raw_pointers());
}

#endif  // __EMSCRIPTEN__
