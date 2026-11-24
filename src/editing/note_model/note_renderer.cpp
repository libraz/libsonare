#include "editing/note_model/note_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "editing/note_model/pitch_decomposition.h"
#include "editing/note_model/seam_crossfade.h"
#include "editing/pitch_editor/pitch_corrector.h"
#include "effects/formant_warp.h"
#include "effects/pitch_shift.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"
#include "util/insertion_sort.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"

namespace sonare::editing::note_model {
namespace {

using sonare::constants::kCentsPerSemitone;
using sonare::constants::kHalfPi;
using sonare::constants::kSemitonesPerOctave;

void check_disjoint_spans(const std::vector<NoteObject>& notes) {
  std::vector<size_t> order(notes.size());
  std::iota(order.begin(), order.end(), size_t{0});
  insertion_sort(order.begin(), order.end(), [&notes](size_t a, size_t b) {
    return notes[a].onset_sample < notes[b].onset_sample;
  });
  for (size_t i = 1; i < order.size(); ++i) {
    if (notes[order[i]].onset_sample < notes[order[i - 1]].offset_sample) {
      util::check::refuse_relation(
          {util::check::indexed_subject("notes", order[i], "onset_sample"),
           util::check::indexed_subject("notes", order[i - 1], "offset_sample")},
          "{0} must be at or after {1}");
    }
  }
}

/// Writes @p segment at @p dest, cross-fading its edges against @p output's
/// OWN current content over @p fade samples with the level-keeping weights of
/// seam_crossfade.h: a segment over the source it was cut from fades at equal
/// gain, one over unrelated audio at equal power.
/// @details Blends against @p output rather than the pristine source
///          deliberately: all edited source spans are vacated before any
///          destination is written, and a neighbouring note earlier in the
///          render can have already overwritten part of this range too.
///          output starts as a copy of the source and only diverges where an
///          erase_span/overlay call touched it. Blending against output[j]
///          preserves the source in untouched spans and uses the current
///          taper or destination in edited spans.
void overlay(std::vector<float>& output, const std::vector<float>& segment, int64_t dest,
             int64_t fade) {
  const int64_t n = static_cast<int64_t>(output.size());
  const int64_t seg_len = static_cast<int64_t>(segment.size());
  if (seg_len <= 0 || dest >= n || dest <= -seg_len) return;

  const int64_t begin = std::max<int64_t>(dest, 0);
  const int64_t end = std::min<int64_t>(dest + seg_len, n);
  const int64_t edge = std::max<int64_t>(fade, 0);
  for (int64_t j = begin; j < end; ++j) {
    const int64_t k = j - dest;
    if (k >= edge && k < seg_len - edge) {
      output[static_cast<size_t>(j)] = segment[static_cast<size_t>(k)];
    }
  }
  // One fade zone of the segment, [first, first + edge), clipped to the buffer;
  // the correlation is taken over the part that lands.
  const auto blend = [&](int64_t first, bool fading_in) {
    const int64_t lo = std::max(first, begin - dest);
    const int64_t hi = std::min(first + edge, end - dest);
    if (lo >= hi) return;
    float* existing = output.data() + (dest + lo);
    const float rho = seam_correlation(existing, segment.data() + lo, hi - lo);
    for (int64_t k = lo; k < hi; ++k) {
      const float phase = (static_cast<float>(k - first) + 0.5f) / static_cast<float>(edge);
      const SeamGains gains = seam_gains(rho, phase);
      float& sample = output[static_cast<size_t>(dest + k)];
      const float incoming = segment[static_cast<size_t>(k)];
      sample = fading_in ? gains.outgoing * sample + gains.incoming * incoming
                         : gains.outgoing * incoming + gains.incoming * sample;
    }
  };
  if (edge > 0) {
    blend(0, true);
    blend(seg_len - edge, false);
  }
}

/// Vacates [@p begin, @p end) of @p output as erase_span does, except that an
/// edge whose @p keep flag is set keeps the source over its fade zone for the
/// cross-fade a destination landing on that edge will make against it.
void vacate(std::vector<float>& output, const Audio& source, int64_t begin, int64_t end,
            int64_t fade, bool keep_head, bool keep_tail) {
  const int64_t length = end - begin;
  for (int64_t j = begin; j < end; ++j) {
    const int64_t k = j - begin;
    if ((keep_head && k < fade) || (keep_tail && k >= length - fade)) continue;
    const float phase = fade_phase(k, length, fade);
    const float source_gain = phase >= 1.0f ? 0.0f : std::cos(kHalfPi * phase);
    output[static_cast<size_t>(j)] = source_gain * source[static_cast<size_t>(j)];
  }
}

/// Scales @p segment by @p envelope and the note's linear gain in one double
/// precision operation. The envelope is a set of gain points rather than a
/// signal, so it is resampled by linear interpolation between its endpoints;
/// one entry is a constant gain. Combining the two factors before narrowing
/// avoids an intermediate float overflow that a later finite gain could undo.
void apply_envelope(std::vector<float>& segment, const std::vector<float>& envelope,
                    float linear_gain) {
  const size_t n = segment.size();
  const size_t points = envelope.size();
  if (n == 0) return;
  const double max_float = static_cast<double>(std::numeric_limits<float>::max());
  const auto scale_sample = [linear_gain, max_float](float sample, double envelope_gain) {
    const double scaled =
        static_cast<double>(sample) * envelope_gain * static_cast<double>(linear_gain);
    SONARE_CHECK(std::isfinite(scaled) && std::abs(scaled) <= max_float,
                 ErrorCode::InvalidParameter);
    return static_cast<float>(scaled);
  };
  if (points == 1) {
    for (float& sample : segment) {
      sample = scale_sample(sample, static_cast<double>(envelope[0]));
    }
    return;
  }

  if (points == 0) {
    for (float& sample : segment) {
      sample = scale_sample(sample, 1.0);
    }
    return;
  }

  const double step =
      static_cast<double>(points - 1) / static_cast<double>(n > 1 ? n - 1 : size_t{1});
  for (size_t i = 0; i < n; ++i) {
    const double position = static_cast<double>(i) * step;
    const size_t lo = std::min(static_cast<size_t>(position), points - 1);
    const size_t hi = std::min(lo + 1, points - 1);
    const double frac = position - static_cast<double>(lo);
    const double envelope_gain =
        static_cast<double>(envelope[lo]) * (1.0 - frac) + static_cast<double>(envelope[hi]) * frac;
    segment[i] = scale_sample(segment[i], envelope_gain);
  }
}

/// True when at least one frame carries a pitch decompose_pitch can measure.
bool has_usable_pitch(const std::vector<float>& f0_hz) noexcept {
  return std::any_of(f0_hz.begin(), f0_hz.end(),
                     [](float hz) { return hz > 0.0f && std::isfinite(hz); });
}

/// Rescales the note's own drift and vibrato by the edit's two changes and
/// repitches @p segment through them. The deltas are a change from the measured
/// curve, so -1 cancels a component and 0 leaves it alone. Duration-preserving,
/// so the segment keeps its length for the rest of the chain.
Audio apply_pitch_curve(const Audio& segment, const NoteObject& note,
                        const NoteRenderConfig& config, size_t note_index) {
  PitchDecomposition split;
  try {
    split = decompose_pitch(note, config.decomposition);
  } catch (const SonareParameterRefusal& e) {
    throw e.respelled(
        e.code(),
        [note_index](const std::string& subject) {
          if (subject.compare(0, 5, "note.") == 0) {
            return util::check::indexed_subject("notes", note_index, subject.substr(5).c_str());
          }
          if (subject == "config.vibrato_cutoff_hz") {
            return std::string("config.decomposition.vibrato_cutoff_hz");
          }
          return subject;
        },
        e.caller_spelled());
  }
  const size_t n = note.f0_hz.values.size();
  // Unreachable while validation and decompose_pitch agree on what a usable
  // curve is; an error rather than a skip so that they cannot part silently.
  if (split.drift.size() != n) {
    util::check::finite_float(
        note.median_hz, util::check::indexed_subject("notes", note_index, "median_hz").c_str());
    SONARE_CHECK(split.drift.size() == n, ErrorCode::InvalidParameter);
  }

  std::vector<float> deltas(n);
  for (size_t i = 0; i < n; ++i) {
    deltas[i] = (split.drift[i] * note.edit.drift_change +
                 split.vibrato[i] * note.edit.vibrato_depth_change) /
                kCentsPerSemitone;
  }

  // The note's own frames, which is the cadence the deltas are stated over.
  pitch_editor::F0Track track;
  track.f0_hz = note.f0_hz.values;
  track.voiced.resize(n);
  for (size_t i = 0; i < n; ++i) {
    track.voiced[i] = track.f0_hz[i] > 0.0f && std::isfinite(track.f0_hz[i]);
    // Decomposition accepts any unusable value as a missing measurement;
    // the pitch corrector consumes a normalized, validated F0 track.
    if (!track.voiced[i]) track.f0_hz[i] = 0.0f;
  }
  track.sample_rate = segment.sample_rate();
  track.frame_rate_hz = note.f0_hz.frame_rate_hz;

  try {
    return pitch_editor::PitchCorrector().resynthesize(segment, track, deltas);
  } catch (const SonareParameterRefusal& e) {
    if (e.subject().compare(0, 17, "deltas_semitones[") != 0) throw;
    const std::string drift_subject =
        util::check::indexed_subject("notes", note_index, "edit.drift_change");
    const std::string vibrato_subject =
        util::check::indexed_subject("notes", note_index, "edit.vibrato_depth_change");
    if (note.edit.drift_change != 0.0f && note.edit.vibrato_depth_change != 0.0f) {
      throw SonareParameterRefusal::relation(e.code(), {drift_subject, vibrato_subject},
                                             "{0} and {1} must keep the pitch correction finite");
    }
    util::check::refuse(
        note.edit.drift_change != 0.0f ? drift_subject.c_str() : vibrato_subject.c_str(),
        "must keep the pitch correction finite");
  }
}

void validate_note_for_render_fields(const NoteObject& note, int64_t audio_samples,
                                     const std::string& subject_prefix) {
  validate_note_span_and_edit(note, audio_samples, subject_prefix);
  const auto subject = [&subject_prefix](const char* field) { return subject_prefix + field; };
  const NoteEdit& edit = note.edit;
  if (edit.vibrato_depth_change != 0.0f || edit.drift_change != 0.0f) {
    const bool has_pitch = note.median_hz > 0.0f && note.f0_hz.frame_rate_hz > 0.0f &&
                           has_usable_pitch(note.f0_hz.values);
    if (!has_pitch && edit.vibrato_depth_change != 0.0f) {
      util::check::refuse(subject("edit.vibrato_depth_change").c_str(),
                          "must be 0 when the note has no usable pitch curve");
    }
    if (!has_pitch && edit.drift_change != 0.0f) {
      util::check::refuse(subject("edit.drift_change").c_str(),
                          "must be 0 when the note has no usable pitch curve");
    }
  }
}

}  // namespace

void validate_amplitude_envelope(const float* values, std::size_t count, const char* subject) {
  for (std::size_t i = 0; i < count; ++i) {
    const float value = values[i];
    if (!numeric::finite_non_negative(value)) {
      util::check::at_least(value, 0.0, util::check::indexed_subject(subject, i).c_str());
    }
  }
}

void validate_note_span_and_edit(const NoteObject& note, int64_t audio_samples,
                                 const std::string& subject_prefix) {
  const auto subject = [&subject_prefix](const char* field) { return subject_prefix + field; };
  constexpr util::check::NumericDomain kNoteOnsetSampleDomain =
      util::check::integer_domain_of<decltype(note.onset_sample)>(0);
  if (note.onset_sample < 0) {
    util::check::integer_within(note.onset_sample, kNoteOnsetSampleDomain,
                                subject("onset_sample").c_str());
  }
  if (!(note.offset_sample > note.onset_sample)) {
    util::check::refuse_relation({subject("offset_sample"), subject("onset_sample")},
                                 "{0} must be greater than {1}");
  }
  if (audio_samples >= 0 && note.offset_sample > audio_samples) {
    util::check::integer_in_range(note.offset_sample, 0, audio_samples,
                                  subject("offset_sample").c_str());
  }

  const NoteEdit& edit = note.edit;
  if (!std::isfinite(edit.pitch_shift_semitones)) {
    util::check::finite_float(edit.pitch_shift_semitones, subject("edit.pitch_semitones").c_str());
  }
  if (!std::isfinite(edit.gain_db)) {
    util::check::finite_float(edit.gain_db, subject("edit.gain_db").c_str());
  }
  if (!(std::isfinite(edit.time_stretch_ratio) && edit.time_stretch_ratio > 0.0f)) {
    util::check::greater_than(edit.time_stretch_ratio, 0.0,
                              subject("edit.time_stretch_ratio").c_str());
  }
  if (!std::isfinite(edit.formant_shift_semitones)) {
    util::check::finite_float(edit.formant_shift_semitones,
                              subject("edit.formant_semitones").c_str());
  }
  if (!std::isfinite(edit.vibrato_depth_change)) {
    util::check::finite_float(edit.vibrato_depth_change,
                              subject("edit.vibrato_depth_change").c_str());
  }
  if (!std::isfinite(edit.drift_change)) {
    util::check::finite_float(edit.drift_change, subject("edit.drift_change").c_str());
  }
  const float linear_gain = db_to_linear(edit.gain_db);
  if (!std::isfinite(linear_gain)) {
    util::check::refuse(subject("edit.gain_db").c_str(), "must produce a finite linear gain");
  }
  for (const float value : edit.amplitude_envelope) {
    if (!(std::isfinite(value) && value >= 0.0f)) {
      util::check::at_least(value, 0.0, subject("edit.amplitude_envelope").c_str());
    }
    const double combined_gain = static_cast<double>(linear_gain) * static_cast<double>(value);
    if (!(std::isfinite(combined_gain) &&
          combined_gain <= static_cast<double>(std::numeric_limits<float>::max()))) {
      util::check::refuse_relation({subject("edit.amplitude_envelope"), subject("edit.gain_db")},
                                   "{0} must produce finite gain when combined with {1}");
    }
  }
}

void erase_span(std::vector<float>& output, const Audio& source, int64_t begin, int64_t end,
                int64_t fade) {
  vacate(output, source, begin, end, fade, false, false);
}

bool is_valid_note_edit(const NoteEdit& edit) noexcept {
  if (!std::isfinite(edit.pitch_shift_semitones) || !std::isfinite(edit.gain_db) ||
      !std::isfinite(edit.time_stretch_ratio) || edit.time_stretch_ratio <= 0.0f ||
      !std::isfinite(edit.formant_shift_semitones) || !std::isfinite(edit.vibrato_depth_change) ||
      !std::isfinite(edit.drift_change)) {
    return false;
  }

  const float linear_gain = db_to_linear(edit.gain_db);
  if (!std::isfinite(linear_gain)) return false;
  const double gain = static_cast<double>(linear_gain);
  const double max_float = static_cast<double>(std::numeric_limits<float>::max());
  for (const float value : edit.amplitude_envelope) {
    if (!std::isfinite(value) || value < 0.0f) return false;
    const double combined_gain = gain * static_cast<double>(value);
    if (!std::isfinite(combined_gain) || combined_gain > max_float) return false;
  }
  return true;
}

bool is_valid_note_span(const NoteObject& note, int64_t audio_samples) noexcept {
  return audio_samples >= 0 && note.onset_sample >= 0 && note.offset_sample > note.onset_sample &&
         note.offset_sample <= audio_samples;
}

void validate_note_for_render(const NoteObject& note, int64_t audio_samples) {
  validate_note_for_render_fields(note, audio_samples, "note.");
}

void validate_note_for_render(const NoteObject& note) { validate_note_for_render(note, -1); }

void validate_render_config(const NoteRenderConfig& config) {
  util::check::at_least(config.fade_ms, 0.0, "config.fade_ms");
}

Audio render_notes(const Audio& audio, const std::vector<NoteObject>& notes,
                   const NoteRenderConfig& config) {
  if (audio.empty()) util::check::refuse("audio", "must not be empty");
  validate_render_config(config);

  bool all_identity = true;
  for (size_t i = 0; i < notes.size(); ++i) {
    const NoteObject& note = notes[i];
    try {
      validate_note_for_render_fields(note, static_cast<int64_t>(audio.size()), "note.");
    } catch (const SonareParameterRefusal& e) {
      throw e.respelled(
          e.code(),
          [i](const std::string& subject) {
            if (subject.compare(0, 5, "note.") == 0) {
              return util::check::indexed_subject("notes", i, subject.substr(5).c_str());
            }
            return subject;
          },
          e.caller_spelled());
    }
    all_identity = all_identity && note.edit.is_identity();
  }
  check_disjoint_spans(notes);

  // Nothing to resynthesize: the source passes through unchanged, bit for bit.
  if (all_identity) {
    validate_offline_audio_samples("audio", audio.data(), audio.size(), 0);
    return Audio::from_buffer(audio.data(), audio.size(), audio.sample_rate());
  }

  const int sample_rate = audio.sample_rate();
  std::vector<float> output(audio.begin(), audio.end());

  // Vacate every edited source span before rendering any destination. A moved
  // note may land on another edited note's source span, so erasing one note
  // while rendering the next would otherwise remove an earlier destination.
  for (size_t i = 0; i < notes.size(); ++i) {
    const NoteObject& note = notes[i];
    if (note.edit.is_identity()) continue;

    const int64_t onset = note.onset_sample;
    const int64_t offset = note.offset_sample;

    // An edge the rendered note lands back on keeps its source for overlay to fade against.
    const bool keeps_onset = !note.edit.muted && note.edit.time_offset_samples == 0;
    const bool keeps_offset = keeps_onset && note.edit.time_stretch_ratio == 1.0f;
    vacate(output, audio, onset, offset,
           edge_fade_samples(config.fade_ms, sample_rate, offset - onset), keeps_onset,
           keeps_offset);
  }

  // Render in vector order after clearing all edited source spans. Later notes
  // overwrite earlier destinations, with an edge blend against existing audio.
  for (size_t i = 0; i < notes.size(); ++i) {
    const NoteObject& note = notes[i];
    if (note.edit.is_identity() || note.edit.muted) continue;

    const int64_t onset = note.onset_sample;
    const int64_t offset = note.offset_sample;

    // Each stage reads the previous stage's Audio directly; the source slice shares the input
    // buffer.
    Audio staged = audio.slice_samples(static_cast<size_t>(onset), static_cast<size_t>(offset));
    if (note.edit.vibrato_depth_change != 0.0f || note.edit.drift_change != 0.0f) {
      staged = apply_pitch_curve(staged, note, config, i);
    }
    if (note.edit.time_stretch_ratio != 1.0f) {
      TimeStretchConfig stretch_config;
      stretch_config.backend = config.stretch_backend;
      // time_stretch takes a rate, which is the reciprocal of a stretch ratio.
      staged = time_stretch(staged, 1.0f / note.edit.time_stretch_ratio, stretch_config);
    }
    if (note.edit.pitch_shift_semitones != 0.0f) {
      PitchShiftConfig shift_config;
      shift_config.backend = config.stretch_backend;
      staged = pitch_shift(staged, note.edit.pitch_shift_semitones, shift_config);
    }
    if (staged.empty()) continue;
    if (note.edit.formant_shift_semitones != 0.0f) {
      FormantWarpConfig warp_config;
      // The warp takes a frequency ratio for what the edit states in semitones.
      warp_config.factor = std::pow(2.0f, note.edit.formant_shift_semitones / kSemitonesPerOctave);
      staged = FormantWarp(warp_config).process(staged);
    }
    std::vector<float> segment(staged.begin(), staged.end());
    const float gain = db_to_linear(note.edit.gain_db);
    apply_envelope(segment, note.edit.amplitude_envelope, gain);

    const int64_t fade =
        edge_fade_samples(config.fade_ms, sample_rate, static_cast<int64_t>(segment.size()));
    overlay(output, segment, numeric::saturating_add<int64_t>(onset, note.edit.time_offset_samples),
            fade);
  }

  SONARE_CHECK(numeric::all_finite(output.data(), output.size()), ErrorCode::InvalidParameter);
  return Audio::from_vector(std::move(output), sample_rate);
}

}  // namespace sonare::editing::note_model
