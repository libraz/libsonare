#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#if defined(SONARE_WITH_PITCH_EDITOR)
#include "editing/polyphony/polyphonic_edit.h"
#endif
#include <sonare/sonare_c.h>

#include "core/audio.h"
#include "sonare_c_internal.h"

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

/// @brief Resolves a versioned config onto the core defaults.
/// @details 0 takes the default on every field. On the four that also accept 0 as a
///          value, a negative selects it; each of those rejects a negative otherwise,
///          so the two meanings cannot collide.
SonareError resolve_config(const SonarePolyphonicConfig* in, PolyphonicEditConfig& out) {
  if (in == nullptr) return SONARE_OK;
  if (in->struct_version < 0 || in->struct_version > 1) return SONARE_ERROR_INVALID_PARAMETER;

  // Anything the core range-checks is forwarded as given and refused there, so one
  // stage's bounds are stated in one place. Only the sentinels are resolved here.
  const auto set_positive = [](float value, float& field) {
    if (value != 0.0f) field = value;
  };
  const auto set_floor = [](float value, float& field) {
    if (value < 0.0f) {
      field = 0.0f;
    } else if (value != 0.0f) {
      field = value;
    }
  };
  const auto set_count = [](int32_t value, int& field) {
    if (value != 0) field = static_cast<int>(value);
  };

  auto& stft = out.extraction.stft;
  set_count(in->n_fft, stft.n_fft);
  set_count(in->hop_length, stft.hop_length);
  set_count(in->win_length, stft.win_length);

  auto& spectrum = out.extraction.spectrum;
  set_positive(in->cent_ref_hz, spectrum.ref_hz);
  set_positive(in->cents_per_bin, spectrum.cents_per_bin);
  set_positive(in->cent_max_hz, spectrum.max_hz);
  if (in->tonality_off != 0) spectrum.use_tonality = false;

  auto& salience = out.extraction.estimation.salience;
  set_count(in->salience_harmonics, salience.n_harmonics);
  set_positive(in->f0_min_hz, salience.f0_min_hz);
  set_positive(in->f0_max_hz, salience.f0_max_hz);
  set_positive(in->salience_alpha_hz, salience.alpha_hz);
  set_positive(in->salience_beta_hz, salience.beta_hz);
  // Zero is this field's default and also a stretch of zero, so it needs no sentinel.
  salience.inharmonicity = in->salience_inharmonicity;

  auto& estimation = out.extraction.estimation;
  set_count(in->max_polyphony, estimation.max_polyphony);
  set_floor(in->min_frame_peak_ratio, estimation.min_frame_peak_ratio);
  set_floor(in->min_separation_cents, estimation.min_separation_cents);
  set_positive(in->subtraction_factor, estimation.subtraction_factor);

  auto& ridges = out.extraction.ridges;
  set_positive(in->max_jump_cents, ridges.max_jump_cents);
  set_floor(in->min_ridge_peak_ratio, ridges.min_ridge_peak_ratio);
  set_floor(in->min_ridge_duration_ms, ridges.min_duration_ms);

  set_count(in->mask_harmonics, out.masks.n_harmonics);
  set_positive(in->claim_lobes, out.masks.claim_lobes);
  out.masks.inharmonicity = in->inharmonicity;

  set_count(in->window_frames, out.shared_bins.window_frames);
  set_positive(in->min_partial_separation, out.shared_bins.min_partial_separation);
  set_positive(in->max_fit_residual, out.shared_bins.max_fit_residual);
  set_positive(in->max_weight_modulus, out.shared_bins.max_weight_modulus);
  // Zero already means "derive one" on this field.
  out.shared_bins.max_refine_hz = in->max_refine_hz;
  set_positive(in->f0_tolerance_cents, out.shared_bins.f0_tolerance_cents);

  set_positive(in->segmentation_threshold_cents, out.notes.segmenter.segmentation_threshold_cents);
  set_positive(in->min_note_ms, out.notes.segmenter.min_note_ms);
  set_positive(in->reference_hz, out.notes.segmenter.reference_hz);
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
  to.edit.time_offset_samples = from.edit.time_offset_samples;
  to.edit.envelope_offset = 0;
  to.edit.envelope_count = from.edit.amplitude_envelope.size();
  to.edit.pitch_shift_semitones = from.edit.pitch_shift_semitones;
  to.edit.gain_db = from.edit.gain_db;
  to.edit.time_stretch_ratio = from.edit.time_stretch_ratio;
  to.edit.formant_shift_semitones = from.edit.formant_shift_semitones;
  to.edit.vibrato_depth_change = from.edit.vibrato_depth_change;
  to.edit.drift_change = from.edit.drift_change;
  to.edit.muted = from.edit.muted ? 1 : 0;
}

/// @brief Copies @p values into @p out, clamped to @p capacity.
SonareError copy_curve(const std::vector<float>& values, float* out, size_t capacity,
                       size_t* out_count) {
  const size_t written = std::min(values.size(), capacity);
  if (written > 0 && out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  std::copy(values.begin(), values.begin() + static_cast<ptrdiff_t>(written), out);
  *out_count = written;
  return SONARE_OK;
}

/// @brief Common preamble of every per-note accessor.
SonareError check_note(const SonarePolyphonicAnalysis* analysis, size_t note, size_t* out_count) {
  if (analysis == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = 0;
  if (note >= analysis->analysis.notes.size()) return SONARE_ERROR_INVALID_PARAMETER;
  return SONARE_OK;
}

}  // namespace

#else

/// Defined in the feature-off build too, so releasing a handle nothing can have
/// produced is a well-formed delete rather than one through an incomplete type.
struct SonarePolyphonicAnalysis {};

#endif

SonareError sonare_polyphonic_analyze(const float* samples, size_t length, int sample_rate,
                                      const SonarePolyphonicConfig* config,
                                      SonarePolyphonicAnalysis** out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out = nullptr;

  PolyphonicEditConfig resolved;
  const SonareError config_error = resolve_config(config, resolved);
  if (config_error != SONARE_OK) return config_error;

  return run_offline(samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    auto handle = std::make_unique<SonarePolyphonicAnalysis>();
    handle->analysis = sonare::editing::polyphony::analyze_polyphonic(audio, resolved);
    *out = handle.release();
    return SONARE_OK;
  });
#else
  SONARE_C_STUB_NOT_SUPPORTED(samples, length, sample_rate, config, out);
#endif
}

void sonare_polyphonic_analysis_destroy(SonarePolyphonicAnalysis* analysis) { delete analysis; }

SonareError sonare_polyphonic_note_count(const SonarePolyphonicAnalysis* analysis,
                                         size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (analysis == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = analysis->analysis.notes.size();
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(analysis, out_count);
#endif
}

SonareError sonare_polyphonic_frame_count(const SonarePolyphonicAnalysis* analysis,
                                          int32_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (analysis == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = static_cast<int32_t>(analysis->analysis.spectrum.n_frames());
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(analysis, out_count);
#endif
}

SonareError sonare_polyphonic_notes(const SonarePolyphonicAnalysis* analysis, SonareNoteObject* out,
                                    size_t capacity, size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (analysis == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = 0;
  const auto& notes = analysis->analysis.notes;
  const size_t written = std::min(notes.size(), capacity);
  if (written > 0 && out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  for (size_t i = 0; i < written; ++i) fill_note(notes[i], out[i]);
  *out_count = written;
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(analysis, out, capacity, out_count);
#endif
}

SonareError sonare_polyphonic_set_note_edit(SonarePolyphonicAnalysis* analysis, size_t note,
                                            const SonareNoteEdit* edit, const float* envelope,
                                            size_t envelope_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (analysis == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  if (note >= analysis->analysis.notes.size()) return SONARE_ERROR_INVALID_PARAMETER;
  if (envelope_count > 0 && envelope == nullptr) return SONARE_ERROR_INVALID_PARAMETER;

  SONARE_C_TRY
  sonare::editing::note_model::NoteEdit replacement;
  if (edit != nullptr) {
    replacement.pitch_shift_semitones = edit->pitch_shift_semitones;
    replacement.gain_db = edit->gain_db;
    replacement.time_offset_samples = edit->time_offset_samples;
    // The by-value door's 0-reads-as-1 rule, kept so one struct means one thing.
    replacement.time_stretch_ratio =
        edit->time_stretch_ratio == 0.0f ? 1.0f : edit->time_stretch_ratio;
    replacement.muted = edit->muted != 0;
    replacement.formant_shift_semitones = edit->formant_shift_semitones;
    replacement.vibrato_depth_change = edit->vibrato_depth_change;
    replacement.drift_change = edit->drift_change;
  }
  replacement.amplitude_envelope.assign(envelope, envelope + envelope_count);
  // Whatever the render rejects about an edit it rejects on the way out, so a
  // field is not checked twice here and a refusal names one place.
  analysis->analysis.notes[note].edit = std::move(replacement);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(analysis, note, edit, envelope, envelope_count);
#endif
}

SonareError sonare_polyphonic_polyphony(const SonarePolyphonicAnalysis* analysis, int32_t* out,
                                        size_t capacity, size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (analysis == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = 0;
  const auto& counts = analysis->analysis.track.polyphony;
  const size_t written = std::min(counts.size(), capacity);
  if (written > 0 && out == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  for (size_t i = 0; i < written; ++i) out[i] = static_cast<int32_t>(counts[i]);
  *out_count = written;
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(analysis, out, capacity, out_count);
#endif
}

SonareError sonare_polyphonic_note_f0(const SonarePolyphonicAnalysis* analysis, size_t note,
                                      float* out, size_t capacity, size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  const SonareError error = check_note(analysis, note, out_count);
  if (error != SONARE_OK) return error;
  return copy_curve(analysis->analysis.notes[note].f0_hz.values, out, capacity, out_count);
#else
  SONARE_C_STUB_NOT_SUPPORTED(analysis, note, out, capacity, out_count);
#endif
}

SonareError sonare_polyphonic_note_amplitude(const SonarePolyphonicAnalysis* analysis, size_t note,
                                             float* out, size_t capacity, size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  const SonareError error = check_note(analysis, note, out_count);
  if (error != SONARE_OK) return error;
  return copy_curve(analysis->analysis.notes[note].amplitude.values, out, capacity, out_count);
#else
  SONARE_C_STUB_NOT_SUPPORTED(analysis, note, out, capacity, out_count);
#endif
}

SonareError sonare_polyphonic_note_salience(const SonarePolyphonicAnalysis* analysis, size_t note,
                                            float* out, size_t capacity, size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  const SonareError error = check_note(analysis, note, out_count);
  if (error != SONARE_OK) return error;

  const auto& object = analysis->analysis.notes[note];
  const auto& masks = analysis->analysis.masks.notes;
  if (note >= masks.size()) return SONARE_ERROR_INVALID_STATE;
  const int ridge_index = masks[note].ridge_index;
  const auto& ridges = analysis->analysis.track.ridges;
  if (ridge_index < 0 || static_cast<size_t>(ridge_index) >= ridges.size()) {
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
  SONARE_C_STUB_NOT_SUPPORTED(analysis, note, out, capacity, out_count);
#endif
}

SonareError sonare_polyphonic_note_envelope(const SonarePolyphonicAnalysis* analysis, size_t note,
                                            float* out, size_t capacity, size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  const SonareError error = check_note(analysis, note, out_count);
  if (error != SONARE_OK) return error;
  return copy_curve(analysis->analysis.notes[note].edit.amplitude_envelope, out, capacity,
                    out_count);
#else
  SONARE_C_STUB_NOT_SUPPORTED(analysis, note, out, capacity, out_count);
#endif
}

SonareError sonare_polyphonic_render(const SonarePolyphonicAnalysis* analysis,
                                     const SonareNoteRenderConfig* config, float** out,
                                     size_t* out_length) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (analysis == nullptr || out == nullptr || out_length == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out = nullptr;
  *out_length = 0;

  // Resolved the way sonare_render_notes resolves the same struct, so one config
  // means one thing whichever door applies it.
  sonare::editing::note_model::NoteRenderConfig render_config;
  if (config != nullptr) {
    if (config->struct_version < 0 || config->struct_version > 1) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (!std::isfinite(config->fade_ms) || config->fade_ms < 0.0f ||
        !std::isfinite(config->vibrato_cutoff_hz) || config->vibrato_cutoff_hz < 0.0f) {
      return SONARE_ERROR_INVALID_PARAMETER;
    }
    if (config->fade_ms > 0.0f) render_config.fade_ms = config->fade_ms;
    if (config->vibrato_cutoff_hz > 0.0f) {
      render_config.decomposition.vibrato_cutoff_hz = config->vibrato_cutoff_hz;
    }
  }

  SONARE_C_TRY
  const Audio rendered =
      sonare::editing::polyphony::render_polyphonic(analysis->analysis, render_config);
  return copy_audio_result(rendered, out, out_length);
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(analysis, config, out, out_length);
#endif
}
