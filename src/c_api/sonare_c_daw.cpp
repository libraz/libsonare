#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(SONARE_WITH_PITCH_EDITOR)
#include "c_api/note_render_common.h"
#include "editing/note_model/note_extractor.h"
#include "editing/note_model/note_renderer.h"
#include "editing/note_model/note_split_merge.h"
#include "editing/note_model/note_target.h"
#include "editing/note_model/pitch_decomposition.h"
#include "editing/pitch_editor/auto_tune.h"
#include "editing/pitch_editor/note_editor.h"
#include "editing/pitch_editor/pitch_corrector.h"
#endif
// The SMF reader lives in the MIDI library rather than beside the assignment
// rule it feeds, so it carries the arrangement gate and not the pitch-editor
// one: a build with one and not the other ships exactly the half it linked.
#if defined(SONARE_WITH_ARRANGEMENT)
#include "midi/note_targets.h"
#endif
#include <sonare/sonare_c.h>

#include "c_api/daw_config_domains.h"
#include "core/audio.h"
#include "core/spectrum.h"
// Ungated: the percussive event model depends only on HPSS and the onset
// analyzer, both of which are compiled in every configuration.
#include "editing/event_model/event_extractor.h"
#include "editing/event_model/event_renderer.h"
#include "sonare_c_internal.h"
#include "util/parameter_checks.h"

using namespace sonare;
using namespace sonare_c_detail;

namespace {

/// Clears every field of a note-object result, so a failed call never leaves the
/// caller's struct holding stale pointers. Ungated: the free entry point shares
/// it and is compiled in every configuration.
void clear_note_objects_result(SonareNoteObjectsResult& out) {
  out.notes = nullptr;
  out.count = 0;
  out.amplitude = nullptr;
  out.amplitude_count = 0;
  out.envelopes = nullptr;
  out.envelope_count = 0;
}

/// The percussive event model reaches only for HPSS and the onset analyzer, so
/// unlike everything else in this file it needs no feature gate.
void clear_percussive_events_result(SonarePercussiveEventsResult& out) {
  out.events = nullptr;
  out.count = 0;
}

/// Refuses a note region with no sample inside the buffer, by the C parameter names. An
/// offset past the end is clamped to it, as the editor does.
void check_note_region(int64_t onset_sample, int64_t offset_sample, size_t length) {
  // The core holds sample indices as int, so a position past INT_MAX is refused, not narrowed.
  constexpr long long kMaxIndex = std::numeric_limits<int>::max();
  util::check::integer_in_range(
      onset_sample, 0, std::min(static_cast<long long>(length) - 1, kMaxIndex), "onset_sample");
  if (offset_sample <= onset_sample) {
    util::check::refuse_relation({"offset_sample", "onset_sample"}, "{0} must be greater than {1}");
  }
  util::check::integer_within(offset_sample, kNoteSampleIndexDomain, "offset_sample");
}

/// Checks the separation framing of a percussive config and copies it onto the core one.
SonareError resolve_percussive_separation(int32_t n_fft, size_t hop_length, int32_t kernel_harmonic,
                                          int32_t kernel_percussive,
                                          editing::event_model::PercussiveSeparationConfig& out) {
  if (const SonareError err = run_validation([&] {
        util::check::even_within(n_fft, sonare::kColaNFftDomain, "config.n_fft");
        check_stft_hop(hop_length, "config.hop_length");
        util::check::integer_within(kernel_harmonic, kPercussiveHpssKernelDomain,
                                    "config.hpss_kernel_harmonic");
        util::check::integer_within(kernel_percussive, kPercussiveHpssKernelDomain,
                                    "config.hpss_kernel_percussive");
      });
      err != SONARE_OK) {
    return err;
  }
  out.n_fft = n_fft;
  out.hop_length = core_stft_hop(hop_length);
  out.hpss.kernel_size_harmonic = kernel_harmonic;
  out.hpss.kernel_size_percussive = kernel_percussive;
  return SONARE_OK;
}

// The core's percussive config member names, as the C config structs spell them.
const std::array<CName, 6> kPercussiveEventNames{
    {{"n_fft", "config.n_fft"},
     {"hop_length", "config.hop_length"},
     {"onset_wait", "config.onset_wait"},
     {"onset_delta", "config.onset_delta"},
     {"max_event_ms", "config.max_event_ms"},
     {"min_percussive_ratio", "config.min_percussive_ratio"}}};
const std::array<CName, 3> kPercussiveRenderNames{{{"n_fft", "config.n_fft"},
                                                   {"hop_length", "config.hop_length"},
                                                   {"fade_ms", "config.fade_ms"}}};

SonareError fill_percussive_events_result(
    const std::vector<editing::event_model::PercussiveEvent>& events,
    SonarePercussiveEventsResult* out) {
  if (events.empty()) return SONARE_OK;

  auto* rows = new SonarePercussiveEvent[events.size()];
  for (size_t i = 0; i < events.size(); ++i) {
    const editing::event_model::PercussiveEvent& event = events[i];
    rows[i].onset_sample = event.onset_sample;
    rows[i].offset_sample = event.offset_sample;
    rows[i].strength = event.strength;
    rows[i].peak_amplitude = event.peak_amplitude;
    rows[i].percussive_ratio = event.percussive_ratio;
    rows[i].edit.time_offset_samples = event.edit.time_offset_samples;
    rows[i].edit.gain_db = event.edit.gain_db;
    rows[i].edit.muted = event.edit.muted ? 1 : 0;
  }
  out->events = rows;
  out->count = events.size();
  return SONARE_OK;
}

#if defined(SONARE_WITH_PITCH_EDITOR)

bool valid_pitch_track_f0(float f0_hz, bool voiced, int sample_rate) {
  if (!std::isfinite(f0_hz)) {
    return !voiced && std::isnan(f0_hz);
  }
  return f0_hz >= 0.0f && f0_hz <= 0.5f * static_cast<float>(sample_rate);
}

/// At most one array is given (the entries refuse both): voiced flags decide, or
/// voiced_prob >= 0.5 (sonare_note_segments' own voiced_threshold default); with
/// neither, every frame defaults to voiced.
bool is_voiced_frame(const int32_t* voiced, const float* voiced_prob, size_t i) {
  if (voiced) return voiced[i] != 0;
  if (voiced_prob) return voiced_prob[i] >= 0.5f;
  return true;
}

/// Pitch-track contract shared by the time-varying correction entry points: pYIN
/// NaN unvoiced F0 only where the voiced flag is false; no infinities, negative
/// frequencies or pitches above Nyquist; voiced_prob in [0, 1] when it is read.
/// Throws the refusal for the first frame that breaks it.
void check_pitch_track(const float* f0_hz, const float* voiced_prob, const int32_t* voiced,
                       size_t n_frames, int sample_rate) {
  for (size_t i = 0; i < n_frames; ++i) {
    const bool is_voiced = is_voiced_frame(voiced, voiced_prob, i);
    if (!valid_pitch_track_f0(f0_hz[i], is_voiced, sample_rate)) {
      util::check::refuse(
          "f0_hz", "must be finite and in [0, sample_rate / 2], or NaN on an unvoiced frame");
    }
    if (!voiced && voiced_prob) util::check::in_range(voiced_prob[i], 0.0, 1.0, "voiced_prob");
  }
}

/// Refuses the track arrays, frame count and hop of a time-varying correction call.
SonareError check_timevarying_args(const float* f0_hz, const float* voiced_prob,
                                   const int32_t* voiced, size_t n_frames, size_t hop_length) {
  SONARE_C_REQUIRE_NONNULL(f0_hz);
  return run_validation([&] {
    if (voiced != nullptr && voiced_prob != nullptr) {
      util::check::refuse_relation({"voiced_prob", "voiced"},
                                   "{1} {must_be_absent} when {0} {is_present}");
    }
    util::check::integer_within(n_frames, kTimeVaryingFrameCountDomain, "n_frames");
    util::check::integer_within(hop_length, editing::pitch_editor::kF0TrackHopLengthDomain,
                                "hop_length");
  });
}

/// Validates the correction knobs shared by every config-driven entry point and
/// copies them (and the scale reference) onto the core config. Throws the refusal.
void apply_correction_knobs(const SonarePitchCorrectionConfig& config,
                            editing::pitch_editor::PitchCorrectionConfig& core_config) {
  require_struct_size(config.struct_size, sizeof(SonarePitchCorrectionConfig), "config",
                      "sonare_pitch_correction_config_init");
  util::check::finite_float(config.scale_reference_midi, "config.scale_reference_midi");
  util::check::in_range(config.retune_amount, 0.0, 1.0, "config.retune_amount");
  util::check::at_least(config.max_correction_semitones, 0.0, "config.max_correction_semitones");
  util::check::at_least(config.retune_speed_ms, 0.0, "config.retune_speed_ms");
  util::check::at_least(config.vibrato_threshold_cents, 0.0, "config.vibrato_threshold_cents");
  core_config.scale.reference_midi = config.scale_reference_midi;
  core_config.retune_amount = config.retune_amount;
  core_config.max_correction_semitones = config.max_correction_semitones;
  core_config.retune_speed_ms = config.retune_speed_ms;
  core_config.vibrato_threshold_cents = config.vibrato_threshold_cents;
}

/// Builds the F0Track the time-varying correctors consume from the C arrays.
editing::pitch_editor::F0Track build_pitch_track(const float* f0_hz, const float* voiced_prob,
                                                 const int32_t* voiced, size_t n_frames,
                                                 size_t hop_length, int sample_rate) {
  editing::pitch_editor::F0Track track;
  track.sample_rate = sample_rate;
  track.hop_length = static_cast<int>(hop_length);
  track.f0_hz.assign(f0_hz, f0_hz + n_frames);
  track.voiced.resize(n_frames);
  track.voiced_prob.resize(n_frames);
  for (size_t i = 0; i < n_frames; ++i) {
    const bool is_voiced = is_voiced_frame(voiced, voiced_prob, i);
    track.voiced[i] = is_voiced;
    track.voiced_prob[i] = !voiced && voiced_prob ? voiced_prob[i] : (is_voiced ? 1.0f : 0.0f);
  }
  return track;
}

/// Checks a note-extractor config and copies it onto the core one; NULL keeps the core defaults.
SonareError resolve_extractor_config(const SonareNoteExtractorConfig* config,
                                     editing::note_model::NoteExtractorConfig& out) {
  if (config == nullptr) return SONARE_OK;
  if (const SonareError err = run_validation([&] {
        require_struct_size(config->struct_size, sizeof(SonareNoteExtractorConfig), "config",
                            "sonare_note_extractor_config_init");
        util::check::greater_than(config->segmentation_threshold_cents, 0.0,
                                  "config.segmentation_threshold_cents");
        util::check::at_least(config->min_note_ms, 0.0, "config.min_note_ms");
        util::check::greater_than(config->reference_hz, 0.0, "config.reference_hz");
        util::check::in_range(config->voiced_threshold, 0.0, 1.0, "config.voiced_threshold");
      });
      err != SONARE_OK) {
    return err;
  }
  out.segmenter.segmentation_threshold_cents = config->segmentation_threshold_cents;
  out.segmenter.min_note_ms = config->min_note_ms;
  out.segmenter.reference_hz = config->reference_hz;
  out.voiced_threshold = config->voiced_threshold;
  return SONARE_OK;
}

/// Validates the F0 frame array every note entry point takes. The frame values
/// are deliberately not checked: a frame carrying no pitch is spelled as a
/// non-finite or non-positive value -- which is what this library's own
/// sonare_pitch_pyin emits for an unvoiced frame -- and every consumer reads
/// each frame through the same usable-pitch predicate rather than assuming one.
SonareError validate_f0_frames(const float* f0_hz, size_t n_frames, float frame_rate) {
  SONARE_C_REQUIRE_NONNULL(f0_hz);
  return run_validation([&] {
    util::check::integer_within(static_cast<long long>(n_frames), kF0FrameCountDomain, "n_frames");
    util::check::greater_than(frame_rate, 0.0, "frame_rate");
  });
}

/// Normalizes the C F0/mask pair into the core's positive-finite pitch
/// spelling. A NULL mask infers voicing from positive finite F0;
/// an explicit zero flag suppresses an otherwise usable F0 frame.
std::vector<float> normalize_f0_frames(const float* f0_hz, const int32_t* voiced, size_t n_frames) {
  std::vector<float> normalized(n_frames, 0.0f);
  for (size_t i = 0; i < n_frames; ++i) {
    if (f0_hz[i] > 0.0f && std::isfinite(f0_hz[i]) && (voiced == nullptr || voiced[i] != 0)) {
      normalized[i] = f0_hz[i];
    }
  }
  return normalized;
}

/// Validates the whole F0 track the extraction-shaped entry points take.
SonareError validate_track_args(const float* f0_hz, const float* voiced_prob, const int32_t* voiced,
                                size_t n_frames, float frame_rate) {
  if (voiced == nullptr && voiced_prob == nullptr) {
    return run_validation([&] {
      util::check::refuse_relation({"voiced_prob", "voiced"},
                                   "{0} and {1} {must_not_both_be_absent}");
    });
  }
  if (voiced != nullptr && voiced_prob != nullptr) {
    return run_validation([&] {
      util::check::refuse_relation({"voiced_prob", "voiced"},
                                   "{1} {must_be_absent} when {0} {is_present}");
    });
  }
  const SonareError error = validate_f0_frames(f0_hz, n_frames, frame_rate);
  if (error != SONARE_OK) return error;
  if (voiced != nullptr) return SONARE_OK;
  return run_validation([&] {
    for (size_t i = 0; i < n_frames; ++i) {
      util::check::in_range(voiced_prob[i], 0.0, 1.0, "voiced_prob");
    }
  });
}

/// Refuses a track cadence the note model cannot step: at least one sample per frame, and a
/// frame no longer than an int of samples. Called once @p sample_rate is known to be valid.
SonareError validate_track_cadence(float frame_rate, int sample_rate) {
  return run_validation([&] {
    if (frame_rate > static_cast<float>(sample_rate)) {
      util::check::refuse_relation({"frame_rate", "sample_rate"}, "{0} must be at most {1}");
    }
    util::check::at_least(frame_rate,
                          static_cast<double>(sample_rate) / std::numeric_limits<int>::max(),
                          "frame_rate");
  });
}

/// Refuses a note span that does not lie inside the track, naming the note.
SonareError validate_note_spans(const SonareNoteObject* notes, size_t note_count, size_t n_frames) {
  return run_validation([&] {
    for (size_t i = 0; i < note_count; ++i) {
      const std::string note = "notes[" + std::to_string(i) + "]";
      const std::string start = note + ".frame_start";
      const std::string end = note + ".frame_end";
      if (notes[i].frame_start < 0) {
        util::check::refuse(start.c_str(), util::refusal::integer_at_least_predicate(0));
      }
      if (notes[i].frame_start >= notes[i].frame_end) {
        util::check::refuse_relation({start, end}, "{0} must be less than {1}");
      }
      if (static_cast<size_t>(notes[i].frame_end) > n_frames) {
        util::check::refuse_relation({end, "n_frames"}, "{0} must be at most {1}");
      }
    }
  });
}

/// Builds the core track from the caller's frame arrays.
editing::pitch_editor::F0Track make_track(const float* f0_hz, const float* voiced_prob,
                                          const int32_t* voiced, size_t n_frames, float frame_rate,
                                          int sample_rate, float voiced_threshold) {
  editing::pitch_editor::F0Track track;
  track.sample_rate = sample_rate;
  // Left at 0 deliberately: the extractor derives the hop from the cadence,
  // which is the rate the caller actually stated.
  track.hop_length = 0;
  track.frame_rate_hz = frame_rate;
  track.f0_hz.assign(f0_hz, f0_hz + n_frames);
  track.voiced.resize(n_frames);
  for (size_t i = 0; i < n_frames; ++i) {
    track.voiced[i] = voiced ? voiced[i] != 0 : voiced_prob[i] >= voiced_threshold;
  }
  return track;
}

/// Copies one note's envelope slice out of the caller's pool.
SonareError read_envelope(const SonareNoteEdit& edit, const std::string& edit_subject,
                          const float* envelopes, size_t envelope_count, std::vector<float>& out) {
  out.clear();
  if (edit.envelope_count == 0) return SONARE_OK;
  if (const SonareError err = run_validation([&] {
        util::check::non_null_when_count(envelopes, edit.envelope_count, "envelopes",
                                         (edit_subject + "envelope_count").c_str());
        util::check::integer_within(edit.envelope_offset, kEnvelopeOffsetDomain,
                                    (edit_subject + "envelope_offset").c_str());
        if (static_cast<uint64_t>(edit.envelope_offset) > static_cast<uint64_t>(envelope_count)) {
          util::check::refuse_relation({edit_subject + "envelope_offset", "envelope_count"},
                                       "{0} must not exceed {1}");
        }
        // Subtraction rather than offset + count, which would wrap.
        if (edit.envelope_count > envelope_count - static_cast<size_t>(edit.envelope_offset)) {
          util::check::refuse_relation(
              {edit_subject + "envelope_offset", edit_subject + "envelope_count", "envelope_count"},
              "{0} + {1} must not exceed {2}");
        }
      });
      err != SONARE_OK) {
    return err;
  }
  const size_t offset = static_cast<size_t>(edit.envelope_offset);
  out.assign(envelopes + offset, envelopes + offset + edit.envelope_count);
  return SONARE_OK;
}

/// Marshals a C edit onto its core mirror, resolving the envelope slice.
SonareError to_core_edit(const SonareNoteEdit& edit, const std::string& edit_subject,
                         const float* envelopes, size_t envelope_count,
                         editing::note_model::NoteEdit& out) {
  if (const SonareError err = run_validation([&] {
        require_struct_size(edit.struct_size, sizeof(SonareNoteEdit),
                            edit_subject.substr(0, edit_subject.size() - 1),
                            "sonare_note_edit_init");
        util::check::greater_than(edit.time_stretch_ratio, 0.0,
                                  (edit_subject + "time_stretch_ratio").c_str());
      });
      err != SONARE_OK) {
    return err;
  }
  out.time_offset_samples = edit.time_offset_samples;
  out.pitch_shift_semitones = edit.pitch_semitones;
  out.gain_db = edit.gain_db;
  out.time_stretch_ratio = edit.time_stretch_ratio;
  out.formant_shift_semitones = edit.formant_semitones;
  out.vibrato_depth_change = edit.vibrato_depth_change;
  out.drift_change = edit.drift_change;
  out.muted = edit.muted != 0;
  return read_envelope(edit, edit_subject, envelopes, envelope_count, out.amplitude_envelope);
}

/// Marshals a core edit back out, pointing it at its slice of the result's pool.
void from_core_edit(const editing::note_model::NoteEdit& edit, int64_t envelope_offset,
                    SonareNoteEdit& out) {
  sonare_note_edit_init(&out);
  out.time_offset_samples = edit.time_offset_samples;
  out.envelope_offset = envelope_offset;
  out.envelope_count = edit.amplitude_envelope.size();
  out.pitch_semitones = edit.pitch_shift_semitones;
  out.gain_db = edit.gain_db;
  out.time_stretch_ratio = edit.time_stretch_ratio;
  out.formant_semitones = edit.formant_shift_semitones;
  out.vibrato_depth_change = edit.vibrato_depth_change;
  out.drift_change = edit.drift_change;
  out.muted = edit.muted ? 1 : 0;
}

/// Marshals a C edit for split and merge, which never render: the envelope
/// values the renderer would have rejected are checked here instead.
SonareError to_core_edit_checked(const SonareNoteEdit& edit, const std::string& edit_subject,
                                 const float* envelopes, size_t envelope_count,
                                 editing::note_model::NoteEdit& out) {
  const SonareError error = to_core_edit(edit, edit_subject, envelopes, envelope_count, out);
  if (error != SONARE_OK) return error;
  if (editing::note_model::is_valid_note_edit(out)) return SONARE_OK;
  return run_validation([&] {
    util::check::refuse(edit_subject.substr(0, edit_subject.size() - 1).c_str(),
                        "must hold finite values, a positive time_stretch_ratio and a "
                        "non-negative envelope whose product with the gain stays finite");
  });
}

/// Marshals a core note list into a heap-owned C result: the notes, their
/// concatenated amplitude curves, and their concatenated edit envelopes.
SonareError fill_note_objects_result(const std::vector<editing::note_model::NoteObject>& notes,
                                     SonareNoteObjectsResult* out) {
  if (notes.empty()) return SONARE_OK;

  size_t total_amplitude = 0;
  size_t total_envelope = 0;
  for (const editing::note_model::NoteObject& note : notes) {
    total_amplitude += note.amplitude.values.size();
    total_envelope += note.edit.amplitude_envelope.size();
  }

  auto c_notes = std::make_unique<SonareNoteObject[]>(notes.size());
  std::unique_ptr<float[]> amplitude;
  if (total_amplitude > 0) amplitude = std::make_unique<float[]>(total_amplitude);
  std::unique_ptr<float[]> envelopes;
  if (total_envelope > 0) envelopes = std::make_unique<float[]>(total_envelope);

  int64_t amplitude_offset = 0;
  int64_t envelope_offset = 0;
  for (size_t i = 0; i < notes.size(); ++i) {
    const editing::note_model::NoteObject& note = notes[i];
    SonareNoteObject& row = c_notes[i];
    row.onset_sample = note.onset_sample;
    row.offset_sample = note.offset_sample;
    row.amplitude_offset = amplitude_offset;
    row.frame_start = note.frame_start;
    row.frame_end = note.frame_end;
    row.median_hz = note.median_hz;
    row.median_cents = note.median_cents;
    row.f0_stability = note.f0_stability;
    from_core_edit(note.edit, envelope_offset, row.edit);

    const size_t span = note.amplitude.values.size();
    if (span > 0) {
      std::memcpy(amplitude.get() + amplitude_offset, note.amplitude.values.data(),
                  span * sizeof(float));
    }
    amplitude_offset += static_cast<int64_t>(span);

    const size_t points = note.edit.amplitude_envelope.size();
    if (points > 0) {
      std::memcpy(envelopes.get() + envelope_offset, note.edit.amplitude_envelope.data(),
                  points * sizeof(float));
    }
    envelope_offset += static_cast<int64_t>(points);
  }

  out->count = notes.size();
  out->notes = c_notes.release();
  out->amplitude_count = total_amplitude;
  out->amplitude = amplitude.release();
  out->envelope_count = total_envelope;
  out->envelopes = envelopes.release();
  return SONARE_OK;
}

/// Shared body of sonare_split_note and sonare_merge_notes: both validate the
/// same arguments, rebuild the same note set, and return the same result shape.
/// @p out must already be non-null and cleared. @p check refuses the edit's own
/// arguments once the track and the note spans are known to be valid.
template <typename Check, typename Fn>
SonareError run_note_set_edit(const char* name, const float* samples, size_t length,
                              int sample_rate, const float* f0_hz, const float* voiced_prob,
                              const int32_t* voiced, size_t n_frames, float frame_rate,
                              const SonareNoteExtractorConfig* config,
                              const SonareNoteObject* notes, size_t note_count,
                              const float* envelopes, size_t envelope_count,
                              SonareNoteObjectsResult* out, Check check, Fn apply) {
  const SonareError track_error =
      validate_track_args(f0_hz, voiced_prob, voiced, n_frames, frame_rate);
  if (track_error != SONARE_OK) return track_error;
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(notes, note_count);
  const SonareError span_error = validate_note_spans(notes, note_count, n_frames);
  if (span_error != SONARE_OK) return span_error;
  const SonareError argument_error = check();
  if (argument_error != SONARE_OK) return argument_error;

  editing::note_model::NoteExtractorConfig extractor_config;
  const SonareError config_error = resolve_extractor_config(config, extractor_config);
  if (config_error != SONARE_OK) return config_error;

  // An envelope slice outside the caller's pool is a bad argument rather than a
  // measurement, so the edits are resolved before any audio work.
  std::vector<editing::note_model::NoteEdit> edits(note_count);
  for (size_t i = 0; i < note_count; ++i) {
    const SonareError edit_error =
        to_core_edit_checked(notes[i].edit, "notes[" + std::to_string(i) + "].edit.", envelopes,
                             envelope_count, edits[i]);
    if (edit_error != SONARE_OK) return edit_error;
  }

  return run_offline(name, samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
    const SonareError cadence_error = validate_track_cadence(frame_rate, sample_rate);
    if (cadence_error != SONARE_OK) return cadence_error;
    const editing::pitch_editor::F0Track track =
        make_track(f0_hz, voiced_prob, voiced, n_frames, frame_rate, sample_rate,
                   extractor_config.voiced_threshold);
    // SonareNoteObject carries no curves, so every note is re-derived from the
    // audio and the track the way extraction derived it; make_note validates the
    // frame span, which is what these two cut on.
    std::vector<editing::note_model::NoteObject> core_notes;
    core_notes.reserve(note_count);
    for (size_t i = 0; i < note_count; ++i) {
      core_notes.push_back(editing::note_model::make_note(audio, track, notes[i].frame_start,
                                                          notes[i].frame_end, extractor_config));
      core_notes.back().edit = std::move(edits[i]);
    }
    editing::note_model::repair_rederived_note_set_bounds(audio, track, core_notes,
                                                          extractor_config);
    return fill_note_objects_result(apply(audio, track, core_notes, extractor_config), out);
  });
}

// The C ordinals are the core enumeration's, so the two cannot drift apart
// without this failing to compile.
static_assert(static_cast<int>(editing::note_model::UnmatchedTargetPolicy::Leave) ==
                  SONARE_NOTE_TARGET_UNMATCHED_LEAVE,
              "UnmatchedTargetPolicy::Leave must match its C ordinal");
static_assert(static_cast<int>(editing::note_model::UnmatchedTargetPolicy::Mute) ==
                  SONARE_NOTE_TARGET_UNMATCHED_MUTE,
              "UnmatchedTargetPolicy::Mute must match its C ordinal");
static_assert(static_cast<int>(editing::note_model::UnmatchedTargetPolicy::Nearest) ==
                  SONARE_NOTE_TARGET_UNMATCHED_NEAREST,
              "UnmatchedTargetPolicy::Nearest must match its C ordinal");

/// Checks a note-target assign config and copies it onto the core one; NULL keeps the core
/// defaults. Both floats are taken literally: 0 is a legal value for each.
SonareError resolve_note_target_config(const SonareNoteTargetAssignConfig* config,
                                       editing::note_model::NoteTargetAssignConfig& out) {
  if (config == nullptr) return SONARE_OK;
  if (const SonareError err = run_validation([&] {
        require_struct_size(config->struct_size, sizeof(SonareNoteTargetAssignConfig), "config",
                            "sonare_note_target_assign_config_init");
      });
      err != SONARE_OK) {
    return err;
  }
  editing::note_model::UnmatchedTargetPolicy policy{};
  if (!ordinal_to_enum(config->unmatched_policy, SONARE_NOTE_TARGET_UNMATCHED_LEAVE,
                       SONARE_NOTE_TARGET_UNMATCHED_NEAREST, &policy)) {
    return run_validation([&] {
      util::check::refuse("config.unmatched_policy",
                          util::refusal::one_of_predicate(std::array<int, 3>{
                              SONARE_NOTE_TARGET_UNMATCHED_LEAVE, SONARE_NOTE_TARGET_UNMATCHED_MUTE,
                              SONARE_NOTE_TARGET_UNMATCHED_NEAREST}));
    });
  }
  if (const SonareError err = run_validation([&] {
        util::check::in_range(config->min_overlap_ratio, 0.0, 1.0, "config.min_overlap_ratio");
        util::check::at_least(config->max_correction_semitones, 0.0,
                              "config.max_correction_semitones");
      });
      err != SONARE_OK) {
    return err;
  }
  out.unmatched_policy = policy;
  out.min_overlap_ratio = config->min_overlap_ratio;
  out.max_correction_semitones = config->max_correction_semitones;
  return SONARE_OK;
}

#endif

}  // namespace

SonareError sonare_pitch_correct_to_midi(const float* samples, size_t length, int sample_rate,
                                         float current_midi, float target_midi, float** out,
                                         size_t* out_length) {
  SONARE_C_API_ENTRY;
  // Refused and zeroed before the gate, so the stub below leaves them defined too.
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  *out = nullptr;
  *out_length = 0;
#if defined(SONARE_WITH_PITCH_EDITOR)

  return run_offline("samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       editing::pitch_editor::PitchCorrector corrector;
                       Audio result = corrector.correct_to_midi(audio, current_midi, target_midi);
                       return copy_audio_result(result, out, out_length);
                     });
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, samples, length, sample_rate, current_midi,
                                target_midi, out, out_length);
#endif
}

SonareError sonare_pitch_correct_to_midi_timevarying(const float* samples, size_t length,
                                                     int sample_rate, const float* f0_hz,
                                                     const float* voiced_prob,
                                                     const int32_t* voiced, size_t n_frames,
                                                     size_t hop_length, float target_midi,
                                                     float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  // Refused and zeroed before the gate, so the stub below leaves them defined too.
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  *out = nullptr;
  *out_length = 0;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (const SonareError err =
          check_timevarying_args(f0_hz, voiced_prob, voiced, n_frames, hop_length);
      err != SONARE_OK) {
    return err;
  }
  // pYIN represents unvoiced F0 as NaN. Accept that canonical representation
  // only when the matching voiced flag is false; reject infinities, negative
  // frequencies, and pitches above Nyquist.
  if (const SonareError err = run_validation([&] {
        util::check::in_range(target_midi, 0.0, 127.0, "target_midi");
        check_pitch_track(f0_hz, voiced_prob, voiced, n_frames, sample_rate);
      });
      err != SONARE_OK) {
    return err;
  }

  return run_offline(
      "samples", samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        editing::pitch_editor::PitchCorrector corrector;
        const editing::pitch_editor::F0Track track =
            build_pitch_track(f0_hz, voiced_prob, voiced, n_frames, hop_length, sample_rate);
        Audio result = corrector.correct_to_midi_timevarying(audio, track, target_midi);
        return copy_audio_result(result, out, out_length);
      });
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, samples, length, sample_rate, f0_hz,
                                voiced_prob, voiced, n_frames, hop_length, target_midi, out,
                                out_length);
#endif
}

void sonare_pitch_correction_config_init(SonarePitchCorrectionConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
#if defined(SONARE_WITH_PITCH_EDITOR)
  const editing::pitch_editor::PitchCorrectionConfig defaults{};
  config->target_mode = SONARE_PITCH_TARGET_FIXED_MIDI;
  config->target_midi = constants::kMidiA4;
  config->scale_root = defaults.scale.root;
  config->scale_mode_mask = defaults.scale.mode_mask;
  config->scale_reference_midi = defaults.scale.reference_midi;
  config->retune_amount = defaults.retune_amount;
  config->max_correction_semitones = defaults.max_correction_semitones;
  config->retune_speed_ms = defaults.retune_speed_ms;
  config->vibrato_threshold_cents = defaults.vibrato_threshold_cents;
#endif
}

SonareError sonare_pitch_correct_timevarying(const float* samples, size_t length, int sample_rate,
                                             const float* f0_hz, const float* voiced_prob,
                                             const int32_t* voiced, size_t n_frames,
                                             size_t hop_length,
                                             const SonarePitchCorrectionConfig* config, float** out,
                                             size_t* out_length) {
  SONARE_C_API_ENTRY;
  // Refused and zeroed before the gate, so the stub below leaves them defined too.
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  *out = nullptr;
  *out_length = 0;
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (const SonareError err =
          check_timevarying_args(f0_hz, voiced_prob, voiced, n_frames, hop_length);
      err != SONARE_OK) {
    return err;
  }

  // Resolve the config (NULL = library defaults), validating every numeric knob
  // so a NaN/out-of-range value cannot poison the retune pipeline.
  editing::pitch_editor::PitchCorrectionConfig core_config{};
  bool scale_mode = false;
  if (config) {
    scale_mode = config->target_mode == SONARE_PITCH_TARGET_SCALE;
    if (const SonareError err = run_validation([&] {
          require_struct_size(config->struct_size, sizeof(SonarePitchCorrectionConfig), "config",
                              "sonare_pitch_correction_config_init");
          util::check::one_of(
              config->target_mode,
              std::array<int, 2>{SONARE_PITCH_TARGET_FIXED_MIDI, SONARE_PITCH_TARGET_SCALE},
              "config.target_mode");
          if (!scale_mode) {
            util::check::in_range(config->target_midi, 0.0, 127.0, "config.target_midi");
          }
          util::check::integer_within(config->scale_root, editing::pitch_editor::kScaleRootDomain,
                                      "config.scale_root");
          util::check::integer_within(config->scale_mode_mask,
                                      editing::pitch_editor::kScaleModeMaskDomain,
                                      "config.scale_mode_mask");
          apply_correction_knobs(*config, core_config);
        });
        err != SONARE_OK) {
      return err;
    }
    core_config.scale.root = config->scale_root;
    core_config.scale.mode_mask = static_cast<uint16_t>(config->scale_mode_mask);
  }
  const float target_midi = config ? config->target_midi : constants::kMidiA4;

  // Match the fixed-target entry point's pYIN/Nyquist contract.
  if (const SonareError err = run_validation(
          [&] { check_pitch_track(f0_hz, voiced_prob, voiced, n_frames, sample_rate); });
      err != SONARE_OK) {
    return err;
  }

  return run_offline(
      "samples", samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        editing::pitch_editor::PitchCorrector corrector(core_config);
        const editing::pitch_editor::F0Track track =
            build_pitch_track(f0_hz, voiced_prob, voiced, n_frames, hop_length, sample_rate);
        Audio result = scale_mode
                           ? corrector.correct_to_scale_timevarying(audio, track)
                           : corrector.correct_to_midi_timevarying(audio, track, target_midi);
        return copy_audio_result(result, out, out_length);
      });
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, samples, length, sample_rate, f0_hz,
                                voiced_prob, voiced, n_frames, hop_length, config, out, out_length);
#endif
}

SonareError sonare_auto_tune(const float* samples, size_t length, int sample_rate,
                             const SonareKey* key, const SonarePitchCorrectionConfig* config,
                             float** out, size_t* out_length, SonareKey* out_key) {
  SONARE_C_API_ENTRY;
  // Refused and zeroed before the gate, so the stub below leaves them defined too.
  if (out_key != nullptr) *out_key = SonareKey{};
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  SONARE_C_REQUIRE_NONNULL(out_key);
  *out = nullptr;
  *out_length = 0;
#if defined(SONARE_WITH_PITCH_EDITOR)
  std::optional<Key> named;
  if (key) {
    named = Key{PitchClass::C, Mode::Major, 1.0f};
    if (!ordinal_to_enum(key->root, SONARE_PITCH_C, SONARE_PITCH_B, &named->root)) {
      return run_validation(
          [&] { util::check::refuse("key.root", util::refusal::kDeclaredValuePredicate); });
    }
    if (!ordinal_to_enum(key->mode, SONARE_MODE_MAJOR, SONARE_MODE_LOCRIAN, &named->mode)) {
      return run_validation(
          [&] { util::check::refuse("key.mode", util::refusal::kDeclaredValuePredicate); });
    }
  }
  editing::pitch_editor::PitchCorrectionConfig core_config{};
  if (config) {
    if (const SonareError err =
            run_validation([&] { apply_correction_knobs(*config, core_config); });
        err != SONARE_OK) {
      return err;
    }
  }

  return run_offline("samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       const editing::pitch_editor::AutoTuneResult result =
                           editing::pitch_editor::auto_tune(audio, named, core_config);
                       out_key->root = static_cast<SonarePitchClass>(result.key.root);
                       out_key->mode = static_cast<SonareMode>(result.key.mode);
                       out_key->confidence = result.key.confidence;
                       return copy_audio_result(result.audio, out, out_length);
                     });
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, samples, length, sample_rate, key, config,
                                out, out_length, out_key);
#endif
}

SonareError sonare_note_stretch(const float* samples, size_t length, int sample_rate,
                                int64_t onset_sample, int64_t offset_sample, float stretch_ratio,
                                float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  // Validated before the gate, so the stub below refuses and zeroes the same way.
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err = validate_audio_params("samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] {
        check_note_region(onset_sample, offset_sample, length);
        util::check::greater_than(stretch_ratio, 0.0, "stretch_ratio");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
#if defined(SONARE_WITH_PITCH_EDITOR)

  return run_offline("samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       editing::pitch_editor::NoteRegion region;
                       region.onset_sample = static_cast<int>(onset_sample);
                       region.offset_sample = static_cast<int>(offset_sample);
                       editing::pitch_editor::NoteEditor editor;
                       Audio result = editor.stretch_note(audio, region, stretch_ratio);
                       return copy_audio_result(result, out, out_length);
                     });
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, samples, length, sample_rate, onset_sample,
                                offset_sample, stretch_ratio, out, out_length);
#endif
}

SonareError sonare_extract_notes(const float* samples, size_t length, int sample_rate,
                                 const float* f0_hz, const float* voiced_prob,
                                 const int32_t* voiced, size_t n_frames, float frame_rate,
                                 const SonareNoteExtractorConfig* config,
                                 SonareNoteObjectsResult* out) {
  SONARE_C_API_ENTRY;
  // Refused and cleared before the gate, so the stub below leaves it defined too.
  SONARE_C_REQUIRE_NONNULL(out);
  clear_note_objects_result(*out);
#if defined(SONARE_WITH_PITCH_EDITOR)

  const SonareError track_error =
      validate_track_args(f0_hz, voiced_prob, voiced, n_frames, frame_rate);
  if (track_error != SONARE_OK) return track_error;

  editing::note_model::NoteExtractorConfig extractor_config;
  const SonareError config_error = resolve_extractor_config(config, extractor_config);
  if (config_error != SONARE_OK) return config_error;

  return run_offline(
      "samples", samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        const SonareError cadence_error = validate_track_cadence(frame_rate, sample_rate);
        if (cadence_error != SONARE_OK) return cadence_error;
        const editing::pitch_editor::F0Track track =
            make_track(f0_hz, voiced_prob, voiced, n_frames, frame_rate, sample_rate,
                       extractor_config.voiced_threshold);
        // Extraction produces identity edits, so the result's envelope pool stays
        // NULL.
        return fill_note_objects_result(
            editing::note_model::extract_notes(audio, track, extractor_config), out);
      });
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, samples, length, sample_rate, f0_hz,
                                voiced_prob, voiced, n_frames, frame_rate, config, out);
#endif
}

void sonare_free_note_objects_result(SonareNoteObjectsResult* result) {
  if (result == nullptr) return;
  delete[] result->notes;
  delete[] result->amplitude;
  delete[] result->envelopes;
  clear_note_objects_result(*result);
}

SonareError sonare_render_notes(const float* samples, size_t length, int sample_rate,
                                const SonareNoteObject* notes, size_t note_count,
                                const float* envelopes, size_t envelope_count, const float* f0_hz,
                                const int32_t* voiced, size_t n_frames, float frame_rate,
                                const SonareNoteRenderConfig* config, float** out,
                                size_t* out_length) {
  SONARE_C_API_ENTRY;
  // Refused and zeroed before the gate, so the stub below leaves them defined too.
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  *out = nullptr;
  *out_length = 0;
#if defined(SONARE_WITH_PITCH_EDITOR)
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(notes, note_count);

  // The track is optional; only a curve edit reads it.
  if (voiced != nullptr && f0_hz == nullptr) {
    return run_validation([&] {
      util::check::refuse_relation({"f0_hz", "voiced"},
                                   "{0} {must_be_present} when {1} {is_present}");
    });
  }
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(f0_hz, n_frames);
  if (f0_hz != nullptr) {
    const SonareError track_error = validate_f0_frames(f0_hz, n_frames, frame_rate);
    if (track_error != SONARE_OK) return track_error;
  }

  editing::note_model::NoteRenderConfig render_config;
  const SonareError config_error = resolve_note_render_config(config, render_config);
  if (config_error != SONARE_OK) return config_error;

  return run_offline(
      "samples", samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        std::vector<editing::note_model::NoteObject> core_notes(note_count);
        for (size_t i = 0; i < note_count; ++i) {
          // The metrics and the amplitude offset are not carried: the renderer never
          // looks at them.
          const SonareNoteObject& row = notes[i];
          const std::string note_prefix = "notes[" + std::to_string(i) + "].";
          core_notes[i].onset_sample = row.onset_sample;
          core_notes[i].offset_sample = row.offset_sample;
          core_notes[i].frame_start = row.frame_start;
          core_notes[i].frame_end = row.frame_end;
          core_notes[i].median_hz = row.median_hz;
          if (f0_hz != nullptr) {
            // The note's pitch curve is the caller's track sliced by its own bounds.
            if (const SonareError err = run_validation([&] {
                  util::check::integer_within(row.frame_start, kNoteFrameStartDomain,
                                              (note_prefix + "frame_start").c_str());
                  if (row.frame_end < row.frame_start) {
                    util::check::refuse_relation(
                        {note_prefix + "frame_end", note_prefix + "frame_start"},
                        "{0} must not be less than {1}");
                  }
                  if (static_cast<size_t>(row.frame_end) > n_frames) {
                    util::check::refuse_relation({note_prefix + "frame_end", "n_frames"},
                                                 "{0} must not exceed {1}");
                  }
                });
                err != SONARE_OK) {
              return err;
            }
            const size_t note_frames = static_cast<size_t>(row.frame_end - row.frame_start);
            core_notes[i].f0_hz.values = normalize_f0_frames(
                f0_hz + row.frame_start, voiced ? voiced + row.frame_start : nullptr, note_frames);
            core_notes[i].f0_hz.frame_rate_hz = frame_rate;
            core_notes[i].f0_hz.frame_offset = row.frame_start;
          } else if (row.edit.vibrato_depth_change != 0.0f || row.edit.drift_change != 0.0f) {
            // A curve edit with no track has nothing to act on.
            return run_validation([&] {
              util::check::refuse_relation({note_prefix + "edit.vibrato_depth_change",
                                            note_prefix + "edit.drift_change", "f0_hz"},
                                           "{0} and {1} must be 0 when {2} {is_absent}");
            });
          }
          const SonareError edit_error = to_core_edit(row.edit, note_prefix + "edit.", envelopes,
                                                      envelope_count, core_notes[i].edit);
          if (edit_error != SONARE_OK) return edit_error;
        }
        Audio result = editing::note_model::render_notes(audio, core_notes, render_config);
        return copy_audio_result(result, out, out_length);
      });
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, samples, length, sample_rate, notes,
                                note_count, envelopes, envelope_count, f0_hz, voiced, n_frames,
                                frame_rate, config, out, out_length);
#endif
}

SonareError sonare_decompose_note_pitch(const float* f0_hz, const int32_t* voiced, size_t n_frames,
                                        float frame_rate, float median_hz, float vibrato_cutoff_hz,
                                        SonarePitchDecompositionResult* out) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  SONARE_C_REQUIRE_NONNULL(out);
  out->center_hz = 0.0f;
  out->drift_cents = nullptr;
  out->vibrato_cents = nullptr;
  out->count = 0;

  if (voiced != nullptr && f0_hz == nullptr) {
    return run_validation([&] {
      util::check::refuse_relation({"f0_hz", "voiced"},
                                   "{0} {must_be_present} when {1} {is_present}");
    });
  }
  const SonareError track_error = validate_f0_frames(f0_hz, n_frames, frame_rate);
  if (track_error != SONARE_OK) return track_error;
  if (const SonareError err = run_validation([&] {
        util::check::at_least(median_hz, 0.0, "median_hz");
        util::check::at_least(vibrato_cutoff_hz, 0.0, "vibrato_cutoff_hz");
      });
      err != SONARE_OK) {
    return err;
  }

  SONARE_C_TRY
  editing::note_model::NoteObject note;
  note.median_hz = median_hz;
  note.f0_hz.values = normalize_f0_frames(f0_hz, voiced, n_frames);
  note.f0_hz.frame_rate_hz = frame_rate;

  editing::note_model::PitchDecompositionConfig decomposition_config;
  if (vibrato_cutoff_hz > 0.0f) decomposition_config.vibrato_cutoff_hz = vibrato_cutoff_hz;

  const editing::note_model::PitchDecomposition split =
      editing::note_model::decompose_pitch(note, decomposition_config);
  // A note with no usable pitch comes back with a zero centre and empty curves,
  // which marshals to NULL and 0 without a special case.
  std::unique_ptr<float[]> drift(copy_vector(split.drift));
  std::unique_ptr<float[]> vibrato(copy_vector(split.vibrato));
  out->center_hz = split.centre_hz;
  out->count = split.drift.size();
  out->drift_cents = drift.release();
  out->vibrato_cents = vibrato.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out) *out = {};
  return refuse_not_compiled_in(Feature::kPitchEditor, f0_hz, voiced, n_frames, frame_rate,
                                median_hz, vibrato_cutoff_hz, out);
#endif
}

void sonare_free_pitch_decomposition_result(SonarePitchDecompositionResult* result) {
  if (result == nullptr) return;
  delete[] result->drift_cents;
  delete[] result->vibrato_cents;
  result->center_hz = 0.0f;
  result->drift_cents = nullptr;
  result->vibrato_cents = nullptr;
  result->count = 0;
}

SonareError sonare_split_note(const float* samples, size_t length, int sample_rate,
                              const float* f0_hz, const float* voiced_prob, const int32_t* voiced,
                              size_t n_frames, float frame_rate,
                              const SonareNoteExtractorConfig* config,
                              const SonareNoteObject* notes, size_t note_count,
                              const float* envelopes, size_t envelope_count, size_t index,
                              int32_t frame, SonareNoteObjectsResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  // Refused and cleared before the gate, so the stub below leaves it defined too.
  SONARE_C_REQUIRE_NONNULL(out);
  clear_note_objects_result(*out);
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (index >= note_count) {
    return run_validation([&] {
      util::check::refuse_relation({"index", "note_count"}, "{0} must be less than {1}");
    });
  }
  const auto check_frame = [&] {
    if (frame > notes[index].frame_start && frame < notes[index].frame_end) return SONARE_OK;
    return run_validation([&] {
      const std::string note = "notes[" + std::to_string(index) + "]";
      util::check::refuse_relation({"frame", note + ".frame_start", note + ".frame_end"},
                                   "{0} must be greater than {1} and less than {2}");
    });
  };

  return run_note_set_edit("samples", samples, length, sample_rate, f0_hz, voiced_prob, voiced,
                           n_frames, frame_rate, config, notes, note_count, envelopes,
                           envelope_count, out, check_frame,
                           [&](const Audio& audio, const editing::pitch_editor::F0Track& track,
                               const std::vector<editing::note_model::NoteObject>& core_notes,
                               const editing::note_model::NoteExtractorConfig& extractor_config) {
                             return editing::note_model::split_note(audio, track, core_notes, index,
                                                                    frame, extractor_config);
                           });
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, samples, length, sample_rate, f0_hz,
                                voiced_prob, voiced, n_frames, frame_rate, config, notes,
                                note_count, envelopes, envelope_count, index, frame, out);
#endif
  SONARE_C_CATCH
}

SonareError sonare_merge_notes(const float* samples, size_t length, int sample_rate,
                               const float* f0_hz, const float* voiced_prob, const int32_t* voiced,
                               size_t n_frames, float frame_rate,
                               const SonareNoteExtractorConfig* config,
                               const SonareNoteObject* notes, size_t note_count,
                               const float* envelopes, size_t envelope_count, size_t first,
                               size_t last, SonareNoteObjectsResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  // Refused and cleared before the gate, so the stub below leaves it defined too.
  SONARE_C_REQUIRE_NONNULL(out);
  clear_note_objects_result(*out);
#if defined(SONARE_WITH_PITCH_EDITOR)
  if (!(first < last)) {
    return run_validation(
        [&] { util::check::refuse_relation({"first", "last"}, "{0} must be less than {1}"); });
  }
  if (last >= note_count) {
    return run_validation(
        [&] { util::check::refuse_relation({"last", "note_count"}, "{0} must be less than {1}"); });
  }
  const auto check_order = [&] {
    if (notes[last].frame_end > notes[first].frame_start) return SONARE_OK;
    return run_validation([&] {
      util::check::refuse_relation({"notes[" + std::to_string(last) + "].frame_end",
                                    "notes[" + std::to_string(first) + "].frame_start"},
                                   "{0} must be greater than {1}");
    });
  };

  return run_note_set_edit("samples", samples, length, sample_rate, f0_hz, voiced_prob, voiced,
                           n_frames, frame_rate, config, notes, note_count, envelopes,
                           envelope_count, out, check_order,
                           [&](const Audio& audio, const editing::pitch_editor::F0Track& track,
                               const std::vector<editing::note_model::NoteObject>& core_notes,
                               const editing::note_model::NoteExtractorConfig& extractor_config) {
                             return editing::note_model::merge_notes(audio, track, core_notes,
                                                                     first, last, extractor_config);
                           });
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, samples, length, sample_rate, f0_hz,
                                voiced_prob, voiced, n_frames, frame_rate, config, notes,
                                note_count, envelopes, envelope_count, first, last, out);
#endif
  SONARE_C_CATCH
}

void sonare_note_target_assign_config_init(SonareNoteTargetAssignConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
#if defined(SONARE_WITH_PITCH_EDITOR)
  const editing::note_model::NoteTargetAssignConfig defaults{};
  config->unmatched_policy = static_cast<int32_t>(defaults.unmatched_policy);
  config->min_overlap_ratio = defaults.min_overlap_ratio;
  config->max_correction_semitones = defaults.max_correction_semitones;
#endif
}

SonareError sonare_note_targets_from_smf(const uint8_t* bytes, size_t len, int track_index,
                                         SonareNoteTarget** out, size_t* out_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_count);
  *out = nullptr;
  *out_count = 0;

  SONARE_C_TRY
  const std::vector<editing::note_model::NoteTarget> targets =
      midi::note_targets_from_smf(bytes, len, track_index);
  if (targets.empty()) return SONARE_OK;
  auto rows = std::make_unique<SonareNoteTarget[]>(targets.size());
  for (size_t i = 0; i < targets.size(); ++i) {
    rows[i].start_sec = targets[i].start_sec;
    rows[i].end_sec = targets[i].end_sec;
    rows[i].target_midi = targets[i].target_midi;
  }
  *out_count = targets.size();
  *out = rows.release();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out) *out = nullptr;
  if (out_count) *out_count = 0;
  return refuse_not_compiled_in(Feature::kArrangement, bytes, len, track_index, out, out_count);
#endif
}

void sonare_free_note_targets(SonareNoteTarget* targets) { delete[] targets; }

SonareError sonare_assign_note_targets(SonareNoteObject* notes, size_t note_count, int sample_rate,
                                       const SonareNoteTarget* targets, size_t target_count,
                                       const SonareNoteTargetAssignConfig* config,
                                       size_t* out_assigned_count) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_PITCH_EDITOR)
  SONARE_C_REQUIRE_NONNULL(out_assigned_count);
  *out_assigned_count = 0;
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(notes, note_count);
  if (const SonareError err = run_validation([&] {
        util::check::integer_within(sample_rate, kNoteTargetSampleRateDomain, "sample_rate");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(targets, target_count);

  SONARE_C_TRY
  editing::note_model::NoteTargetAssignConfig assign_config;
  const SonareError config_error = resolve_note_target_config(config, assign_config);
  if (config_error != SONARE_OK) return config_error;

  std::vector<editing::note_model::NoteTarget> core_targets(target_count);
  for (size_t i = 0; i < target_count; ++i) {
    // Rejected here rather than downstream: every overlap comparison is false for
    // a NaN, so a non-finite target would be selected and then written into
    // pitch_semitones with nothing left to catch it.
    if (const SonareError err = run_validation([&] {
          util::check::finite(targets[i].start_sec,
                              util::check::indexed_subject("targets", i, "start_sec").c_str());
          util::check::finite(targets[i].end_sec,
                              util::check::indexed_subject("targets", i, "end_sec").c_str());
          util::check::finite_float(
              targets[i].target_midi,
              util::check::indexed_subject("targets", i, "target_midi").c_str());
        });
        err != SONARE_OK) {
      return err;
    }
    core_targets[i].start_sec = targets[i].start_sec;
    core_targets[i].end_sec = targets[i].end_sec;
    core_targets[i].target_midi = targets[i].target_midi;
  }

  // Only the fields the rule reads go across, and only the two it writes come
  // back, so a note's amplitude slice and its other edits survive the call
  // untouched -- which is what editing in place has to mean here.
  std::vector<editing::note_model::NoteObject> core_notes(note_count);
  for (size_t i = 0; i < note_count; ++i) {
    if (const SonareError err = run_validation([&] {
          require_struct_size(notes[i].edit.struct_size, sizeof(SonareNoteEdit),
                              "notes[" + std::to_string(i) + "].edit", "sonare_note_edit_init");
        });
        err != SONARE_OK) {
      return err;
    }
    core_notes[i].onset_sample = notes[i].onset_sample;
    core_notes[i].offset_sample = notes[i].offset_sample;
    core_notes[i].median_hz = notes[i].median_hz;
    core_notes[i].edit.pitch_shift_semitones = notes[i].edit.pitch_semitones;
    core_notes[i].edit.muted = notes[i].edit.muted != 0;
  }
  const size_t assigned = editing::note_model::assign_note_targets(core_notes, sample_rate,
                                                                   core_targets, assign_config);
  for (size_t i = 0; i < note_count; ++i) {
    notes[i].edit.pitch_semitones = core_notes[i].edit.pitch_shift_semitones;
    notes[i].edit.muted = core_notes[i].edit.muted ? 1 : 0;
  }
  *out_assigned_count = assigned;
  return SONARE_OK;
  SONARE_C_CATCH
#else
  if (out_assigned_count) *out_assigned_count = 0;
  return refuse_not_compiled_in(Feature::kPitchEditor, notes, note_count, sample_rate, targets,
                                target_count, config, out_assigned_count);
#endif
}

void sonare_percussive_event_config_init(SonarePercussiveEventConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
  const editing::event_model::PercussiveEventExtractorConfig defaults;
  config->n_fft = defaults.separation.n_fft;
  config->hop_length = 0;
  config->hpss_kernel_harmonic = defaults.separation.hpss.kernel_size_harmonic;
  config->hpss_kernel_percussive = defaults.separation.hpss.kernel_size_percussive;
  config->onset_wait = defaults.onset.wait;
  config->onset_delta = defaults.onset.delta;
  config->max_event_ms = defaults.max_event_ms;
  config->min_percussive_ratio = defaults.min_percussive_ratio;
}

void sonare_percussive_render_config_init(SonarePercussiveRenderConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
  const editing::event_model::PercussiveEventRenderConfig defaults;
  config->n_fft = defaults.separation.n_fft;
  config->hop_length = 0;
  config->hpss_kernel_harmonic = defaults.separation.hpss.kernel_size_harmonic;
  config->hpss_kernel_percussive = defaults.separation.hpss.kernel_size_percussive;
  config->fade_ms = defaults.fade_ms;
}

void sonare_note_edit_init(SonareNoteEdit* edit) {
  if (edit == nullptr) return;
  *edit = {};
  edit->struct_size = sizeof(*edit);
  edit->time_stretch_ratio = 1.0f;
}

void sonare_note_extractor_config_init(SonareNoteExtractorConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
#if defined(SONARE_WITH_PITCH_EDITOR)
  const editing::note_model::NoteExtractorConfig defaults;
  config->segmentation_threshold_cents = defaults.segmenter.segmentation_threshold_cents;
  config->min_note_ms = defaults.segmenter.min_note_ms;
  config->reference_hz = defaults.segmenter.reference_hz;
  config->voiced_threshold = defaults.voiced_threshold;
#endif
}

void sonare_note_render_config_init(SonareNoteRenderConfig* config) {
  if (config == nullptr) return;
  *config = {};
  config->struct_size = sizeof(*config);
#if defined(SONARE_WITH_PITCH_EDITOR)
  const editing::note_model::NoteRenderConfig defaults;
  config->fade_ms = defaults.fade_ms;
  config->vibrato_cutoff_hz = defaults.decomposition.vibrato_cutoff_hz;
#endif
}

SonareError sonare_extract_percussive_events(const float* samples, size_t length, int sample_rate,
                                             const SonarePercussiveEventConfig* config,
                                             SonarePercussiveEventsResult* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(out);
  clear_percussive_events_result(*out);

  editing::event_model::PercussiveEventExtractorConfig extractor_config;
  if (config != nullptr) {
    if (const SonareError err = run_validation([&] {
          require_struct_size(config->struct_size, sizeof(SonarePercussiveEventConfig), "config",
                              "sonare_percussive_event_config_init");
        });
        err != SONARE_OK) {
      return err;
    }
    const SonareError separation_error = resolve_percussive_separation(
        config->n_fft, config->hop_length, config->hpss_kernel_harmonic,
        config->hpss_kernel_percussive, extractor_config.separation);
    if (separation_error != SONARE_OK) return separation_error;

    if (const SonareError err = run_validation([&] {
          util::check::greater_than(config->max_event_ms, 0.0, "config.max_event_ms");
          with_c_names(kPercussiveEventNames, [&] {
            editing::event_model::check_percussive_extract_controls(
                config->onset_wait, config->onset_delta, config->max_event_ms,
                config->min_percussive_ratio);
          });
        });
        err != SONARE_OK) {
      return err;
    }
    extractor_config.onset.wait = config->onset_wait;
    extractor_config.onset.delta = config->onset_delta;
    extractor_config.max_event_ms = config->max_event_ms;
    extractor_config.min_percussive_ratio = config->min_percussive_ratio;
  }

  return run_offline("samples", samples, length, sample_rate,
                     [&](const Audio& audio) -> SonareError {
                       return fill_percussive_events_result(
                           with_c_names(kPercussiveEventNames,
                                        [&] {
                                          return editing::event_model::extract_percussive_events(
                                              audio, extractor_config);
                                        }),
                           out);
                     });
}

void sonare_free_percussive_events_result(SonarePercussiveEventsResult* result) {
  if (result == nullptr) return;
  delete[] result->events;
  clear_percussive_events_result(*result);
}

SonareError sonare_render_percussive_events(const float* samples, size_t length, int sample_rate,
                                            const SonarePercussiveEvent* events, size_t count,
                                            const SonarePercussiveRenderConfig* config, float** out,
                                            size_t* out_length) {
  SONARE_C_API_ENTRY;
  SONARE_C_TRY
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
  *out = nullptr;
  *out_length = 0;
  SONARE_C_REQUIRE_NONNULL_WHEN_COUNT(events, count);

  editing::event_model::PercussiveEventRenderConfig render_config;
  if (config != nullptr) {
    if (const SonareError err = run_validation([&] {
          require_struct_size(config->struct_size, sizeof(SonarePercussiveRenderConfig), "config",
                              "sonare_percussive_render_config_init");
        });
        err != SONARE_OK) {
      return err;
    }
    const SonareError separation_error = resolve_percussive_separation(
        config->n_fft, config->hop_length, config->hpss_kernel_harmonic,
        config->hpss_kernel_percussive, render_config.separation);
    if (separation_error != SONARE_OK) return separation_error;

    if (const SonareError err = run_validation(
            [&] { util::check::greater_than(config->fade_ms, 0.0, "config.fade_ms"); });
        err != SONARE_OK) {
      return err;
    }
    render_config.fade_ms = config->fade_ms;
  }

  std::vector<editing::event_model::PercussiveEvent> core_events(count);
  for (size_t i = 0; i < count; ++i) {
    core_events[i].onset_sample = events[i].onset_sample;
    core_events[i].offset_sample = events[i].offset_sample;
    // The measured figures are not copied: rendering reads the span and the
    // edit, so a host may pass back exactly what extraction produced without
    // those fields having to survive the round trip.
    core_events[i].edit.time_offset_samples = events[i].edit.time_offset_samples;
    core_events[i].edit.gain_db = events[i].edit.gain_db;
    core_events[i].edit.muted = events[i].edit.muted != 0;
  }

  return run_offline(
      "samples", samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        Audio result = with_c_names(kPercussiveRenderNames, [&] {
          return editing::event_model::render_percussive_events(audio, core_events, render_config);
        });
        return copy_audio_result(result, out, out_length);
      });
  SONARE_C_CATCH
}

SonareError sonare_note_move(const float* samples, size_t length, int sample_rate,
                             int64_t onset_sample, int64_t offset_sample,
                             int64_t target_onset_sample, float** out, size_t* out_length) {
  SONARE_C_API_ENTRY;
  // Validated before the gate, so the stub below refuses and zeroes the same way.
  SONARE_C_ZERO_OUTS(out, out_length);
  if (const SonareError err = validate_audio_params("samples", samples, length, sample_rate);
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = run_validation([&] {
        check_note_region(onset_sample, offset_sample, length);
        util::check::integer_in_range(
            target_onset_sample, 0,
            std::min(static_cast<long long>(length) - 1,
                     static_cast<long long>(std::numeric_limits<int>::max())),
            "target_onset_sample");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_REQUIRE_NONNULL(out_length);
#if defined(SONARE_WITH_PITCH_EDITOR)

  return run_offline(
      "samples", samples, length, sample_rate, [&](const Audio& audio) -> SonareError {
        editing::pitch_editor::NoteRegion region;
        region.onset_sample = static_cast<int>(onset_sample);
        region.offset_sample = static_cast<int>(offset_sample);
        editing::pitch_editor::NoteEditor editor;
        Audio result = editor.move_note(audio, region, static_cast<int>(target_onset_sample));
        return copy_audio_result(result, out, out_length);
      });
#else
  return refuse_not_compiled_in(Feature::kPitchEditor, samples, length, sample_rate, onset_sample,
                                offset_sample, target_onset_sample, out, out_length);
#endif
}
