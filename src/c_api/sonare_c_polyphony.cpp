#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#if defined(SONARE_WITH_PITCH_EDITOR)
#include "c_api/note_render_common.h"
#include "c_api/transcribe_input_domains.h"
#include "editing/polyphony/polyphonic_edit.h"
#endif
#include <sonare/sonare_c.h>

#include "core/audio.h"
#include "core/spectrum.h"
#include "sonare_c_internal.h"
#include "util/parameter_checks.h"

using namespace sonare;
using namespace sonare_c_detail;

#if defined(SONARE_WITH_PITCH_EDITOR)

/// The handle. Nothing beyond the analysis: a note's edit carries its own envelope
/// points, so there is no pool for the handle to own.
struct SonarePolyphonicAnalysis {
  sonare::editing::polyphony::PolyphonicAnalysis analysis;
};

namespace {

using sonare::editing::polyphony::PolyphonicEditConfig;

/// @brief Copies @p in onto the core config field by field; NULL keeps the defaults.
/// @details Nothing is resolved here: every range is the core's, checked where each
///          stage reads it, so one stage's bounds are stated in one place.
SonareError resolve_config(const SonarePolyphonicConfig* in, PolyphonicEditConfig& out) {
  if (in == nullptr) return SONARE_OK;
  if (const SonareError err = run_validation([&] {
        require_struct_size(in->struct_size, sizeof(SonarePolyphonicConfig), "config",
                            "sonare_polyphonic_config_init");
        if (in->win_length != 0) {
          sonare::util::check::integer_within(in->win_length, sonare::kColaWinLengthDomain,
                                              "config.win_length");
        }
        if (!std::isnan(in->max_refine_hz)) {
          sonare::util::check::within(in->max_refine_hz,
                                      sonare_c_detail::kPolyphonicMaxRefineHzDomain,
                                      "config.max_refine_hz");
        }
        if (!std::isnan(in->reattack_ratio)) {
          sonare::util::check::within(in->reattack_ratio, sonare_c_detail::kReattackRatioDomain,
                                      "config.reattack_ratio");
        }
      });
      err != SONARE_OK) {
    return err;
  }

  // The core spells these three absences as 0.
  auto& stft = out.extraction.stft;
  stft.n_fft = in->n_fft;
  // A hop past INT_MAX saturates there, so the core refuses it rather than a cast wrapping it.
  stft.hop_length = static_cast<int>(
      std::min<size_t>(in->hop_length, static_cast<size_t>(std::numeric_limits<int>::max())));
  stft.win_length = static_cast<int>(in->win_length);

  auto& spectrum = out.extraction.spectrum;
  spectrum.ref_hz = in->cent_ref_hz;
  spectrum.cents_per_bin = in->cents_per_bin;
  spectrum.max_hz = in->cent_max_hz;
  spectrum.use_tonality = in->tonality != 0;

  auto& salience = out.extraction.estimation.salience;
  salience.n_harmonics = in->salience_harmonics;
  salience.f0_min_hz = in->f0_min_hz;
  salience.f0_max_hz = in->f0_max_hz;
  salience.alpha_hz = in->salience_alpha_hz;
  salience.beta_hz = in->salience_beta_hz;
  salience.inharmonicity = in->salience_inharmonicity;

  auto& estimation = out.extraction.estimation;
  estimation.max_polyphony = in->max_polyphony;
  estimation.min_frame_peak_ratio = in->min_frame_peak_ratio;
  estimation.min_separation_cents = in->min_separation_cents;
  estimation.subtraction_factor = in->subtraction_factor;

  auto& ridges = out.extraction.ridges;
  ridges.max_jump_cents = in->max_jump_cents;
  ridges.min_ridge_peak_ratio = in->min_ridge_peak_ratio;
  ridges.min_duration_ms = in->min_ridge_duration_ms;
  ridges.reattack_ratio = std::isnan(in->reattack_ratio) ? 0.0f : in->reattack_ratio;

  out.masks.n_harmonics = in->mask_harmonics;
  out.masks.claim_lobes = in->claim_lobes;
  out.masks.inharmonicity = in->inharmonicity;

  out.estimate_inharmonicity = in->estimate_inharmonicity != 0;
  out.inharmonicity.min_partials = in->inharmonicity_min_partials;
  out.inharmonicity.max_residual_bins = in->inharmonicity_max_residual_bins;
  out.inharmonicity.max_inharmonicity = in->inharmonicity_max_stretch;

  out.shared_bins.window_frames = in->window_frames;
  out.shared_bins.min_partial_separation = in->min_partial_separation;
  out.shared_bins.max_fit_residual = in->max_fit_residual;
  out.shared_bins.max_weight_modulus = in->max_weight_modulus;
  out.shared_bins.max_refine_hz = std::isnan(in->max_refine_hz) ? 0.0f : in->max_refine_hz;
  out.shared_bins.f0_tolerance_cents = in->f0_tolerance_cents;

  out.notes.segmenter.segmentation_threshold_cents = in->segmentation_threshold_cents;
  out.notes.segmenter.min_note_ms = in->min_note_ms;
  out.notes.segmenter.reference_hz = in->reference_hz;
  return SONARE_OK;
}

void fill_note(const sonare::editing::note_model::NoteObject& from, SonareNoteObject& to) {
  to.onset_sample = from.onset_sample;
  to.offset_sample = from.offset_sample;
  // No pool to index: the curves have their own accessors and the envelope's points
  // are whatever the caller last set.
  to.amplitude_offset = 0;
  to.frame_start = static_cast<int32_t>(from.frame_start);
  to.frame_end = static_cast<int32_t>(from.frame_end);
  to.median_hz = from.median_hz;
  to.median_cents = from.median_cents;
  to.f0_stability = from.f0_stability;
  sonare_note_edit_init(&to.edit);
  to.edit.time_offset_samples = from.edit.time_offset_samples;
  to.edit.amplitude_envelope_offset = 0;
  to.edit.amplitude_envelope_count = from.edit.amplitude_envelope.size();
  to.edit.pitch_semitones = from.edit.pitch_shift_semitones;
  to.edit.gain_db = from.edit.gain_db;
  to.edit.time_stretch_ratio = from.edit.time_stretch_ratio;
  to.edit.formant_semitones = from.edit.formant_shift_semitones;
  to.edit.vibrato_depth_change = from.edit.vibrato_depth_change;
  to.edit.drift_change = from.edit.drift_change;
  to.edit.muted = from.edit.muted ? 1 : 0;
}

/// @brief Copies @p values into @p out, clamped to @p capacity.
SonareError copy_curve(const std::vector<float>& values, float* out, size_t capacity,
                       size_t* out_count) {
  const size_t written = std::min(values.size(), capacity);
  if (const SonareError err = run_validation(
          [&] { sonare::util::check::non_null_when_count(out, written, "out", "capacity"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_count);
  std::copy(values.begin(), values.begin() + static_cast<ptrdiff_t>(written), out);
  *out_count = written;
  return SONARE_OK;
}

/// @brief Common preamble of every per-note accessor.
SonareError check_note(const SonarePolyphonicAnalysis* analysis, size_t note) {
  SONARE_C_REQUIRE_NONNULL(analysis);
  if (note >= analysis->analysis.notes.size()) {
    return run_validation(
        [&] { sonare::util::check::refuse("note", "must be less than the number of notes"); });
  }
  return SONARE_OK;
}

}  // namespace

#else

/// Defined in the feature-off build too, so releasing a handle nothing can have
/// produced is a well-formed delete rather than one through an incomplete type.
struct SonarePolyphonicAnalysis {};

#endif

void sonare_polyphonic_config_init(SonarePolyphonicConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
#if defined(SONARE_WITH_PITCH_EDITOR)
  const PolyphonicEditConfig defaults;
  const auto& stft = defaults.extraction.stft;
  config->n_fft = stft.n_fft;
  config->hop_length = static_cast<size_t>(stft.hop_length);
  config->win_length = 0;

  const auto& spectrum = defaults.extraction.spectrum;
  config->cent_ref_hz = spectrum.ref_hz;
  config->cents_per_bin = spectrum.cents_per_bin;
  config->cent_max_hz = spectrum.max_hz;
  config->tonality = spectrum.use_tonality ? 1 : 0;

  const auto& salience = defaults.extraction.estimation.salience;
  config->salience_harmonics = salience.n_harmonics;
  config->f0_min_hz = salience.f0_min_hz;
  config->f0_max_hz = salience.f0_max_hz;
  config->salience_alpha_hz = salience.alpha_hz;
  config->salience_beta_hz = salience.beta_hz;
  config->salience_inharmonicity = salience.inharmonicity;

  const auto& estimation = defaults.extraction.estimation;
  config->max_polyphony = estimation.max_polyphony;
  config->min_frame_peak_ratio = estimation.min_frame_peak_ratio;
  config->min_separation_cents = estimation.min_separation_cents;
  config->subtraction_factor = estimation.subtraction_factor;

  const auto& ridges = defaults.extraction.ridges;
  config->max_jump_cents = ridges.max_jump_cents;
  config->min_ridge_peak_ratio = ridges.min_ridge_peak_ratio;
  config->min_ridge_duration_ms = ridges.min_duration_ms;
  config->reattack_ratio = std::numeric_limits<float>::quiet_NaN();

  config->mask_harmonics = defaults.masks.n_harmonics;
  config->claim_lobes = defaults.masks.claim_lobes;
  config->inharmonicity = defaults.masks.inharmonicity;

  config->estimate_inharmonicity = defaults.estimate_inharmonicity ? 1 : 0;
  config->inharmonicity_min_partials = defaults.inharmonicity.min_partials;
  config->inharmonicity_max_residual_bins = defaults.inharmonicity.max_residual_bins;
  config->inharmonicity_max_stretch = defaults.inharmonicity.max_inharmonicity;

  config->window_frames = defaults.shared_bins.window_frames;
  config->min_partial_separation = defaults.shared_bins.min_partial_separation;
  config->max_fit_residual = defaults.shared_bins.max_fit_residual;
  config->max_weight_modulus = defaults.shared_bins.max_weight_modulus;
  config->max_refine_hz = std::numeric_limits<float>::quiet_NaN();
  config->f0_tolerance_cents = defaults.shared_bins.f0_tolerance_cents;

  config->segmentation_threshold_cents = defaults.notes.segmenter.segmentation_threshold_cents;
  config->min_note_ms = defaults.notes.segmenter.min_note_ms;
  config->reference_hz = defaults.notes.segmenter.reference_hz;
#endif
}

SonareError sonare_polyphonic_analyze(const float* samples, size_t length, int sample_rate,
                                      const SonarePolyphonicConfig* config,
                                      SonarePolyphonicAnalysis** out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_TRY
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (const SonareError err = validate_audio_params("samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }

  PolyphonicEditConfig resolved;
  const SonareError config_error = resolve_config(config, resolved);
  if (config_error != SONARE_OK) return config_error;
  SONARE_C_REQUIRE_NONNULL(out);

  return run_prevalidated_offline(
      samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        auto handle = std::make_unique<SonarePolyphonicAnalysis>();
        handle->analysis = sonare::editing::polyphony::analyze_polyphonic(audio, resolved);
        *out = handle.release();
        return SONARE_OK;
      });
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, samples, length, sample_rate, config, out);
#endif
  SONARE_C_CATCH
}

void sonare_polyphonic_analysis_destroy(SonarePolyphonicAnalysis* analysis) { delete analysis; }

SonareError sonare_polyphonic_note_count(const SonarePolyphonicAnalysis* analysis,
                                         size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
#if defined(SONARE_WITH_PITCH_EDITOR)
  SONARE_C_REQUIRE_NONNULL(analysis);
  SONARE_C_REQUIRE_NONNULL(out_count);
  *out_count = analysis->analysis.notes.size();
  return SONARE_OK;
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, analysis, out_count);
#endif
}

SonareError sonare_polyphonic_frame_count(const SonarePolyphonicAnalysis* analysis,
                                          size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  SONARE_C_TRY
#if defined(SONARE_WITH_PITCH_EDITOR)
  SONARE_C_REQUIRE_NONNULL(analysis);
  SONARE_C_REQUIRE_NONNULL(out_count);
  *out_count = static_cast<size_t>(analysis->analysis.spectrum.n_frames());
  return SONARE_OK;
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, analysis, out_count);
#endif
  SONARE_C_CATCH
}

SonareError sonare_polyphonic_notes(const SonarePolyphonicAnalysis* analysis, SonareNoteObject* out,
                                    size_t capacity, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
#if defined(SONARE_WITH_PITCH_EDITOR)
  SONARE_C_REQUIRE_NONNULL(analysis);
  const auto& notes = analysis->analysis.notes;
  const size_t written = std::min(notes.size(), capacity);
  if (const SonareError err = run_validation(
          [&] { sonare::util::check::non_null_when_count(out, written, "out", "capacity"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_count);
  for (size_t i = 0; i < written; ++i) fill_note(notes[i], out[i]);
  *out_count = written;
  return SONARE_OK;
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, analysis, out, capacity, out_count);
#endif
}

SonareError sonare_polyphonic_set_note_edit(SonarePolyphonicAnalysis* analysis, size_t note,
                                            const SonareNoteEdit* edit,
                                            const float* amplitude_envelope,
                                            size_t amplitude_envelope_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  SONARE_C_REQUIRE_NONNULL(analysis);
  if (const SonareError err = check_note(analysis, note); err != SONARE_OK) return err;
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(amplitude_envelope, amplitude_envelope_count);

  SONARE_C_TRY
  sonare::editing::note_model::NoteEdit replacement;
  if (edit != nullptr) {
    if (const SonareError err = run_validation([&] {
          require_struct_size(edit->struct_size, sizeof(SonareNoteEdit), "edit",
                              "sonare_note_edit_init");
          sonare::util::check::greater_than(edit->time_stretch_ratio, 0.0,
                                            "edit.time_stretch_ratio");
        });
        err != SONARE_OK) {
      return err;
    }
    replacement.pitch_shift_semitones = edit->pitch_semitones;
    replacement.gain_db = edit->gain_db;
    replacement.time_offset_samples = edit->time_offset_samples;
    replacement.time_stretch_ratio = edit->time_stretch_ratio;
    replacement.muted = edit->muted != 0;
    replacement.formant_shift_semitones = edit->formant_semitones;
    replacement.vibrato_depth_change = edit->vibrato_depth_change;
    replacement.drift_change = edit->drift_change;
  }
  replacement.amplitude_envelope.assign(amplitude_envelope,
                                        amplitude_envelope + amplitude_envelope_count);
  // Whatever the render rejects about an edit it rejects on the way out, so a
  // field is not checked twice here and a refusal names one place.
  analysis->analysis.notes[note].edit = std::move(replacement);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, analysis, note, edit, amplitude_envelope,
                                amplitude_envelope_count);
#endif
}

SonareError sonare_polyphonic_polyphony(const SonarePolyphonicAnalysis* analysis, int32_t* out,
                                        size_t capacity, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
#if defined(SONARE_WITH_PITCH_EDITOR)
  SONARE_C_REQUIRE_NONNULL(analysis);
  const auto& counts = analysis->analysis.track.polyphony;
  const size_t written = std::min(counts.size(), capacity);
  if (const SonareError err = run_validation(
          [&] { sonare::util::check::non_null_when_count(out, written, "out", "capacity"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out_count);
  for (size_t i = 0; i < written; ++i) out[i] = static_cast<int32_t>(counts[i]);
  *out_count = written;
  return SONARE_OK;
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, analysis, out, capacity, out_count);
#endif
}

SonareError sonare_polyphonic_note_f0(const SonarePolyphonicAnalysis* analysis, size_t note,
                                      float* out, size_t capacity, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  SONARE_C_TRY
#if defined(SONARE_WITH_PITCH_EDITOR)
  const SonareError error = check_note(analysis, note);
  if (error != SONARE_OK) return error;
  return copy_curve(analysis->analysis.notes[note].f0_hz.values, out, capacity, out_count);
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, analysis, note, out, capacity, out_count);
#endif
  SONARE_C_CATCH
}

SonareError sonare_polyphonic_note_amplitude(const SonarePolyphonicAnalysis* analysis, size_t note,
                                             float* out, size_t capacity, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  SONARE_C_TRY
#if defined(SONARE_WITH_PITCH_EDITOR)
  const SonareError error = check_note(analysis, note);
  if (error != SONARE_OK) return error;
  return copy_curve(analysis->analysis.notes[note].amplitude.values, out, capacity, out_count);
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, analysis, note, out, capacity, out_count);
#endif
  SONARE_C_CATCH
}

SonareError sonare_polyphonic_note_salience(const SonarePolyphonicAnalysis* analysis, size_t note,
                                            float* out, size_t capacity, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  SONARE_C_TRY
#if defined(SONARE_WITH_PITCH_EDITOR)
  const SonareError error = check_note(analysis, note);
  if (error != SONARE_OK) return error;

  const auto& object = analysis->analysis.notes[note];
  const auto& masks = analysis->analysis.masks.notes;
  if (note >= masks.size()) {
    set_last_error_static(SONARE_ERROR_INVALID_STATE,
                          "analysis has no mask for the requested note");
    return SONARE_ERROR_INVALID_STATE;
  }
  const int ridge_index = masks[note].ridge_index;
  const auto& ridges = analysis->analysis.track.ridges;
  if (ridge_index < 0 || static_cast<size_t>(ridge_index) >= ridges.size()) {
    set_last_error_static(SONARE_ERROR_INVALID_STATE,
                          "analysis note mask references a missing ridge");
    return SONARE_ERROR_INVALID_STATE;
  }

  // The ridge's curve, not the note's, so it is read through the ridge's own start
  // and a frame the ridge does not reach is 0 rather than the neighbour's value.
  const auto& ridge = ridges[static_cast<size_t>(ridge_index)];
  std::vector<float> span(static_cast<size_t>(std::max(0, object.frame_end - object.frame_start)),
                          0.0f);
  for (size_t i = 0; i < span.size(); ++i) {
    const long long frame = static_cast<long long>(object.frame_start) + static_cast<long long>(i) -
                            static_cast<long long>(ridge.frame_start);
    if (frame < 0 || static_cast<size_t>(frame) >= ridge.salience.size()) continue;
    span[i] = ridge.salience[static_cast<size_t>(frame)];
  }
  return copy_curve(span, out, capacity, out_count);
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, analysis, note, out, capacity, out_count);
#endif
  SONARE_C_CATCH
}

SonareError sonare_polyphonic_note_envelope(const SonarePolyphonicAnalysis* analysis, size_t note,
                                            float* out, size_t capacity, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  SONARE_C_TRY
#if defined(SONARE_WITH_PITCH_EDITOR)
  const SonareError error = check_note(analysis, note);
  if (error != SONARE_OK) return error;
  return copy_curve(analysis->analysis.notes[note].edit.amplitude_envelope, out, capacity,
                    out_count);
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, analysis, note, out, capacity, out_count);
#endif
  SONARE_C_CATCH
}

SonareError sonare_polyphonic_note_inharmonicity(const SonarePolyphonicAnalysis* analysis,
                                                 float* out, size_t capacity, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
  SONARE_C_TRY
#if defined(SONARE_WITH_PITCH_EDITOR)
  SONARE_C_REQUIRE_NONNULL(analysis);
  const auto& fitted = analysis->analysis.inharmonicity_fit;
  // Empty is the fit not having run; any other length breaks the per-note indexing
  // this reports under, which only a hand-built analysis can produce.
  if (!fitted.empty() && fitted.size() != analysis->analysis.notes.size()) {
    set_last_error_static(SONARE_ERROR_INVALID_STATE,
                          "analysis inharmonicity fit does not match the note count");
    return SONARE_ERROR_INVALID_STATE;
  }
  return copy_curve(fitted, out, capacity, out_count);
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, analysis, out, capacity, out_count);
#endif
  SONARE_C_CATCH
}

SonareError sonare_polyphonic_render(const SonarePolyphonicAnalysis* analysis,
                                     const SonareNoteRenderConfig* config, float** out,
                                     size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out, out_length);
#if defined(SONARE_WITH_PITCH_EDITOR)
  SONARE_C_REQUIRE_NONNULL(analysis);

  // Resolved the way sonare_render_notes resolves the same struct, so one config
  // means one thing whichever door applies it.
  sonare::editing::note_model::NoteRenderConfig render_config;
  const SonareError config_error = resolve_note_render_config(config, render_config);
  if (config_error != SONARE_OK) return config_error;
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);

  SONARE_C_TRY
  const Audio rendered =
      sonare::editing::polyphony::render_polyphonic(analysis->analysis, render_config);
  return copy_audio_result(rendered, out, out_length);
  SONARE_C_CATCH
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, analysis, config, out, out_length);
#endif
}
