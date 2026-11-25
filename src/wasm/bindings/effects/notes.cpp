/// @file notes.cpp
/// @brief Embind bindings for the editable note model: extraction, rendering, split/merge and
/// targets.

#ifdef __EMSCRIPTEN__

#include <cmath>
#include <limits>

#include "c_api/daw_config_domains.h"
#include "editing/note_model/note_extractor.h"
#include "editing/note_model/note_renderer.h"
#include "editing/note_model/note_split_merge.h"
#include "editing/note_model/note_target.h"
#include "editing/note_model/pitch_decomposition.h"
#include "editing/pitch_editor/f0_provider.h"
#include "util/parameter_checks.h"
#include "wasm/bindings/common/common.h"
#include "wasm/bindings/common/note_val.h"

#if defined(SONARE_WITH_ARRANGEMENT)
#include "midi/note_targets.h"
#endif

namespace {

// The note-object and note-edit marshalling is the shared one: the polyphonic
// door reads and writes the same shapes, and one converter is what keeps them
// from parting.
using sonare_wasm_editing::noteObjectsToVal;
using sonare_wasm_editing::noteRowEditFromVal;

// An omitted key keeps the core default and a given one is read literally, checked against
// the domain SonareNoteExtractorConfig states.
editing::note_model::NoteExtractorConfig noteExtractorConfigFromVal(val options) {
  namespace check = sonare::util::check;
  editing::note_model::NoteExtractorConfig config;
  config.segmenter.segmentation_threshold_cents = floatProperty(
      options, "segmentationThresholdCents", config.segmenter.segmentation_threshold_cents);
  config.segmenter.min_note_ms = floatProperty(options, "minNoteMs", config.segmenter.min_note_ms);
  config.segmenter.reference_hz =
      floatProperty(options, "referenceHz", config.segmenter.reference_hz);
  config.voiced_threshold = floatProperty(options, "voicedThreshold", config.voiced_threshold);
  check::greater_than(config.segmenter.segmentation_threshold_cents, 0.0,
                      "segmentationThresholdCents");
  check::at_least(config.segmenter.min_note_ms, 0.0, "minNoteMs");
  check::greater_than(config.segmenter.reference_hz, 0.0, "referenceHz");
  check::in_range(config.voiced_threshold, 0.0, 1.0, "voicedThreshold");
  return config;
}

// A frame index that arrived as a bare JS number. wasmCountArg cannot serve: a
// frame outside its note is rejected downstream for being outside it, so a
// negative one has to survive the conversion to be rejected for what it is.
int noteFrameArg(double value, const char* subject) {
  constexpr double kLowest = static_cast<double>(std::numeric_limits<int>::lowest());
  constexpr double kHighest = static_cast<double>(std::numeric_limits<int>::max());
  if (!std::isfinite(value) || std::floor(value) != value || value < kLowest || value > kHighest) {
    throw WasmRangeError(std::string(subject) + " must be an integer in [-2147483648, 2147483647]");
  }
  return static_cast<int>(value);
}

// The F0 track every extraction-shaped note entry point is measured against.
// Mirrors the C ABI's validate_track_args and make_track: exactly one of the two
// voicing inputs is given, since they are two spellings of one input.
editing::pitch_editor::F0Track noteTrackFromVal(const val& samples, int sample_rate,
                                                const val& f0_hz, const val& voiced_prob,
                                                const val& voiced, float frame_rate,
                                                float voiced_threshold,
                                                std::size_t* cumulative_count) {
  const bool has_voiced = !voiced.isUndefined() && !voiced.isNull();
  const bool has_prob = !voiced_prob.isUndefined() && !voiced_prob.isNull();
  // Worded as the C ABI words them; a presence requirement makes each a shape refusal.
  if (!has_voiced && !has_prob) {
    sonare::util::check::refuse_relation({"voicedProb", "voiced"},
                                         sonare::util::refusal::kNotBothAbsentRelation);
  }
  if (has_voiced && has_prob) {
    sonare::util::check::refuse_relation({"voicedProb", "voiced"},
                                         sonare::util::refusal::kAbsentWhenPresentRelation);
  }
  if (!std::isfinite(frame_rate) || frame_rate <= 0.0f) {
    throw WasmRangeError("frameRate must be finite and greater than 0");
  }
  accumulateWasmFloat32ArrayLength(samples, "samples", cumulative_count);
  accumulateWasmFloat32ArrayLength(f0_hz, "f0Hz", cumulative_count);
  if (has_voiced) {
    accumulateWasmFloat32ArrayLength(voiced, "voiced", cumulative_count);
  }
  if (has_prob) {
    accumulateWasmFloat32ArrayLength(voiced_prob, "voicedProb", cumulative_count);
  }

  std::vector<float> f0 = float32ArrayToVector(f0_hz, "f0Hz");
  const size_t n_frames = f0.size();
  if (n_frames == 0) {
    throw WasmRangeError("f0Hz must not be empty");
  }
  std::vector<float> voiced_vec =
      has_voiced ? float32ArrayToVector(voiced, "voiced") : std::vector<float>{};
  std::vector<float> prob_vec =
      has_prob ? float32ArrayToVector(voiced_prob, "voicedProb") : std::vector<float>{};
  if (has_voiced && voiced_vec.size() != n_frames) {
    throw WasmRangeError("voiced must have the same length as f0Hz");
  }
  if (has_prob && prob_vec.size() != n_frames) {
    throw WasmRangeError("voicedProb must have the same length as f0Hz");
  }
  for (size_t i = 0; i < n_frames; ++i) {
    // f0Hz values are not checked: a frame carrying no pitch is spelled zero,
    // negative or non-finite -- pitchPyin leaves NaN there unless asked to fill
    // it -- and every consumer reads all three as contributing no measurement.
    if (has_prob) sonare::util::check::in_range(prob_vec[i], 0.0, 1.0, "voicedProb");
  }

  editing::pitch_editor::F0Track track;
  track.sample_rate = sample_rate;
  // Left at 0 deliberately: the cadence rule then reads frame_rate_hz, which is
  // the rate the caller actually stated.
  track.hop_length = 0;
  track.frame_rate_hz = frame_rate;
  track.f0_hz = std::move(f0);
  track.voiced.resize(n_frames);
  for (size_t i = 0; i < n_frames; ++i) {
    track.voiced[i] = has_voiced ? voiced_vec[i] != 0.0f : prob_vec[i] >= voiced_threshold;
  }
  return track;
}

// A direct note render/decompose call does not pass through noteTrackFromVal,
// so apply the same usable-pitch rule here before constructing a NoteObject.
// A supplied mask only suppresses a positive finite candidate; a true flag
// cannot make a zero, negative or non-finite F0 usable.
std::vector<float> normalizedF0FromVal(const val& f0_hz, const val& voiced,
                                       std::size_t* cumulative_count) {
  const bool has_voiced = !voiced.isUndefined() && !voiced.isNull();

  accumulateWasmFloat32ArrayLength(f0_hz, "f0Hz", cumulative_count);
  std::vector<float> f0 = float32ArrayToVector(f0_hz, "f0Hz");
  if (f0.empty()) {
    throw WasmRangeError("f0Hz must not be empty");
  }

  std::vector<float> voiced_vec;
  if (has_voiced) {
    accumulateWasmFloat32ArrayLength(voiced, "voiced", cumulative_count);
    voiced_vec = float32ArrayToVector(voiced, "voiced");
    if (voiced_vec.size() != f0.size()) {
      throw WasmRangeError("voiced must have the same length as f0Hz");
    }
  }

  for (std::size_t i = 0; i < f0.size(); ++i) {
    const bool usable = std::isfinite(f0[i]) && f0[i] > 0.0f;
    if (!usable || (has_voiced && voiced_vec[i] == 0.0f)) {
      f0[i] = 0.0f;
    }
  }
  return f0;
}

// Reads one note's span and pending edit, plus the frame bounds and the centre a
// curve edit is measured through when the caller handed a track in; every other
// field of a JS note object is ignored, exactly as render_notes ignores the rest
// of a NoteObject.
editing::note_model::NoteObject renderableNoteFromVal(const val& row, std::size_t index,
                                                      const std::vector<float>& f0,
                                                      float frame_rate, bool has_track,
                                                      std::size_t* cumulative_count) {
  editing::note_model::NoteObject note;
  const std::string at = "notes[" + std::to_string(index) + "]";
  note.onset_sample = requireInt64Property(row, "onsetSample", at.c_str());
  note.offset_sample = requireInt64Property(row, "offsetSample", at.c_str());
  note.edit = noteRowEditFromVal(row, at, cumulative_count);

  if (has_track) {
    // The note's pitch curve is the caller's own track sliced by its bounds.
    const std::string start_name = at + ".frameStart";
    const std::string end_name = at + ".frameEnd";
    const int frame_start = noteFrameArg(hasProperty(row, "frameStart")
                                             ? requireNumberProperty(row, "frameStart", at.c_str())
                                             : note.frame_start,
                                         start_name.c_str());
    const int frame_end = noteFrameArg(hasProperty(row, "frameEnd")
                                           ? requireNumberProperty(row, "frameEnd", at.c_str())
                                           : note.frame_end,
                                       end_name.c_str());
    // Checked in the C ABI's order and words (sonare_render_notes).
    sonare::util::check::integer_within(frame_start, sonare_c_detail::kNoteFrameStartDomain,
                                        start_name.c_str());
    if (frame_end < frame_start) {
      sonare::util::check::refuse_relation({end_name, start_name}, "{0} must not be less than {1}");
    }
    if (static_cast<std::size_t>(frame_end) > f0.size()) {
      sonare::util::check::refuse_relation({end_name, "f0Hz.length"}, "{0} must not exceed {1}");
    }
    note.median_hz = static_cast<float>(hasProperty(row, "medianHz")
                                            ? requireNumberProperty(row, "medianHz", at.c_str())
                                            : note.median_hz);
    note.f0_hz.values.assign(f0.begin() + frame_start, f0.begin() + frame_end);
    note.f0_hz.frame_rate_hz = frame_rate;
    note.f0_hz.frame_offset = frame_start;
  } else if (note.edit.vibrato_depth_change != 0.0f || note.edit.drift_change != 0.0f) {
    // A curve edit with no track has nothing to act on; worded as the C ABI words it.
    const std::string edit = at + ".edit.";
    throw sonare::SonareParameterRefusal::relation(
        sonare::ErrorCode::InvalidParameter,
        {edit + "vibratoDepthChange", edit + "driftChange", "f0Hz"},
        "{0} and {1} must be 0 when {2} {is_absent}")
        .as_caller_spelled();
  }
  return note;
}

// All a JS note set states: the frame span identifying each note and the edit
// riding on it. Every other field of a NoteObject is measured from the audio and
// the track, so it is re-derived rather than carried across the boundary.
struct NoteSetEntry {
  int frame_start = 0;
  int frame_end = 0;
  editing::note_model::NoteEdit edit;
};

// Reads the note set split and merge reshape. Neither ever renders, so the
// envelope values the renderer would have rejected are checked here instead
// (sonare_c_daw.cpp's to_core_edit_checked). Nothing here touches the audio: a
// malformed edit is a bad argument rather than a measurement, so it is rejected
// before any audio work.
std::vector<NoteSetEntry> noteSetFromVal(const val& notes, std::size_t* cumulative_count) {
  const std::size_t count = wasmArrayLength(notes, "notes");
  std::vector<NoteSetEntry> out;
  out.reserve(std::min(count, kMaxWasmObjectArrayReserve));
  for (std::size_t i = 0; i < count; ++i) {
    const val row = notes[i];
    const std::string at = "notes[" + std::to_string(i) + "]";
    NoteSetEntry entry;
    entry.frame_start = noteFrameArg(requireNumberProperty(row, "frameStart", at.c_str()),
                                     (at + ".frameStart").c_str());
    entry.frame_end = noteFrameArg(requireNumberProperty(row, "frameEnd", at.c_str()),
                                   (at + ".frameEnd").c_str());
    entry.edit = noteRowEditFromVal(row, at, cumulative_count);
    if (!editing::note_model::is_valid_note_edit(entry.edit)) {
      // Worded as the C ABI's to_core_edit_checked words it.
      sonare::util::check::refuse((at + ".edit").c_str(),
                                  "must hold finite values, a positive time-stretch ratio and a "
                                  "non-negative envelope whose product with the gain stays finite");
    }
    out.push_back(std::move(entry));
  }
  return out;
}

// Rebuilds the whole set from the audio and the track, exactly as extraction
// derived it, and hands each note its own edit back.
//
// Every note is re-derived, with no separate path for the one being reshaped:
// split and merge copy the notes they do not touch straight through, so a
// pass-through note would otherwise reach the result carrying no spans and no
// curves. Deriving is also the only way these cannot go stale, since a caller
// round-tripping them could hand back values measured against other audio.
// make_note validates the frame span, which is what these two cut on
// (sonare_c_daw.cpp's run_note_set_edit).
std::vector<editing::note_model::NoteObject> deriveNoteSet(
    const Audio& audio, const editing::pitch_editor::F0Track& track,
    std::vector<NoteSetEntry>& entries, const editing::note_model::NoteExtractorConfig& config) {
  std::vector<editing::note_model::NoteObject> out;
  out.reserve(entries.size());
  for (NoteSetEntry& entry : entries) {
    out.push_back(
        editing::note_model::make_note(audio, track, entry.frame_start, entry.frame_end, config));
    out.back().edit = std::move(entry.edit);
  }
  editing::note_model::repair_rederived_note_set_bounds(audio, track, out, config);
  return out;
}

}  // namespace

// Note objects: editable notes extracted from audio plus a caller-supplied F0
// track, the render pass that writes an edited set back over that audio, the
// split of one note's pitch curve into the parts an edit acts on, and the two
// calls that reshape the set itself.
val js_extract_notes(val samples, const val& sample_rate_val, val f0_hz, val voiced_prob,
                     val voiced, const val& frame_rate_val, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const float frame_rate = checkedFloatFromVal(frame_rate_val, "frameRate");
  const editing::note_model::NoteExtractorConfig config = noteExtractorConfigFromVal(options);
  std::size_t cumulative_count = 0;
  const editing::pitch_editor::F0Track track =
      noteTrackFromVal(samples, sample_rate, f0_hz, voiced_prob, voiced, frame_rate,
                       config.voiced_threshold, &cumulative_count);

  Audio audio = loadValidatedAudio("samples", samples, sample_rate);
  return noteObjectsToVal(editing::note_model::extract_notes(audio, track, config));
}

val js_render_notes(val samples, const val& sample_rate, val notes, val options) {
  const int rate = checkedIntFromVal(sample_rate, "sampleRate");
  editing::note_model::NoteRenderConfig config;
  config.fade_ms = domainFloatProperty(options, "fadeMs", config.fade_ms);
  config.decomposition.vibrato_cutoff_hz =
      domainFloatProperty(options, "vibratoCutoffHz", config.decomposition.vibrato_cutoff_hz);
  sonare::util::check::greater_than(config.fade_ms, 0.0, "fadeMs");
  sonare::util::check::greater_than(config.decomposition.vibrato_cutoff_hz, 0.0, "vibratoCutoffHz");

  std::size_t cumulative_count = 0;
  accumulateWasmFloat32ArrayLength(samples, "samples", &cumulative_count);

  // The track is optional; only a vibrato or drift edit reads it.
  const val f0_hz = objectProperty(options, "f0Hz");
  const bool has_track = !f0_hz.isUndefined();
  const val voiced = objectProperty(options, "voiced");
  const bool has_voiced = !voiced.isUndefined() && !voiced.isNull();
  if (has_voiced && !has_track) {
    // Worded as the C ABI words it; the presence requirement makes it a shape refusal.
    sonare::util::check::refuse_relation({"f0Hz", "voiced"},
                                         sonare::util::refusal::kPresentWhenPresentRelation);
  }
  const val frame_rate_value = objectProperty(options, "frameRate");
  const float frame_rate =
      frame_rate_value.isUndefined() ? float{} : checkedFloatFromVal(frame_rate_value, "frameRate");
  std::vector<float> f0;
  if (has_track) {
    if (!std::isfinite(frame_rate) || frame_rate <= 0.0f) {
      throw WasmRangeError("frameRate must be finite and greater than 0 when f0Hz is given");
    }
    f0 = normalizedF0FromVal(f0_hz, voiced, &cumulative_count);
  }

  const std::size_t count = wasmArrayLength(notes, "notes");
  std::vector<editing::note_model::NoteObject> core_notes;
  core_notes.reserve(std::min(count, kMaxWasmObjectArrayReserve));
  for (std::size_t i = 0; i < count; ++i) {
    core_notes.push_back(
        renderableNoteFromVal(notes[i], i, f0, frame_rate, has_track, &cumulative_count));
  }

  Audio audio = loadValidatedAudio("samples", samples, rate);
  Audio result = editing::note_model::render_notes(audio, core_notes, config);
  return vectorToFloat32Array(result.data(), result.size());
}

val js_decompose_note_pitch(val f0_hz, const val& voiced, const val& frame_rate_val,
                            const val& median_hz_val, const val& vibrato_cutoff_hz_val) {
  const float frame_rate = domainFloatFromVal(frame_rate_val, "frameRate");
  const float median_hz = domainFloatFromVal(median_hz_val, "medianHz");
  const float vibrato_cutoff_hz = domainFloatFromVal(vibrato_cutoff_hz_val, "vibratoCutoffHz");
  // A note with no pitch is spelled 0, so only a value that cannot be a centre
  // or a cutoff at all is rejected; checked in the C ABI's order and words.
  sonare::util::check::greater_than(frame_rate, 0.0, "frameRate");
  sonare::util::check::at_least(median_hz, 0.0, "medianHz");
  sonare::util::check::at_least(vibrato_cutoff_hz, 0.0, "vibratoCutoffHz");
  std::size_t cumulative_count = 0;
  std::vector<float> f0 = normalizedF0FromVal(f0_hz, voiced, &cumulative_count);

  editing::note_model::NoteObject note;
  note.median_hz = median_hz;
  note.f0_hz.values = std::move(f0);
  note.f0_hz.frame_rate_hz = frame_rate;
  editing::note_model::PitchDecompositionConfig config;
  if (vibrato_cutoff_hz > 0.0f) config.vibrato_cutoff_hz = vibrato_cutoff_hz;

  const editing::note_model::PitchDecomposition split =
      editing::note_model::decompose_pitch(note, config);
  // A note with no usable pitch comes back with a zero centre and two empty
  // curves, which needs no special case here.
  val out = val::object();
  out.set("centerHz", split.centre_hz);
  out.set("driftCents", vectorToFloat32Array(split.drift));
  out.set("vibratoCents", vectorToFloat32Array(split.vibrato));
  return out;
}

val js_split_note(val samples, const val& sample_rate_val, val f0_hz, val voiced_prob, val voiced,
                  const val& frame_rate_val, val notes, double index, double frame, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const float frame_rate = checkedFloatFromVal(frame_rate_val, "frameRate");
  const std::size_t note_index = wasmIndexArg(index, "index");
  const int cut_frame = noteFrameArg(frame, "frame");
  const editing::note_model::NoteExtractorConfig config = noteExtractorConfigFromVal(options);
  std::size_t cumulative_count = 0;
  const editing::pitch_editor::F0Track track =
      noteTrackFromVal(samples, sample_rate, f0_hz, voiced_prob, voiced, frame_rate,
                       config.voiced_threshold, &cumulative_count);
  std::vector<NoteSetEntry> entries = noteSetFromVal(notes, &cumulative_count);
  if (note_index >= entries.size()) {
    throw WasmRangeError("index must be less than notes.length");
  }

  Audio audio = loadValidatedAudio("samples", samples, sample_rate);
  const std::vector<editing::note_model::NoteObject> core_notes =
      deriveNoteSet(audio, track, entries, config);
  return noteObjectsToVal(
      editing::note_model::split_note(audio, track, core_notes, note_index, cut_frame, config));
}

val js_merge_notes(val samples, const val& sample_rate_val, val f0_hz, val voiced_prob, val voiced,
                   const val& frame_rate_val, val notes, double first, double last, val options) {
  const int sample_rate = checkedIntFromVal(sample_rate_val, "sampleRate");
  const float frame_rate = checkedFloatFromVal(frame_rate_val, "frameRate");
  const std::size_t first_index = wasmIndexArg(first, "first");
  const std::size_t last_index = wasmIndexArg(last, "last");
  const editing::note_model::NoteExtractorConfig config = noteExtractorConfigFromVal(options);
  std::size_t cumulative_count = 0;
  const editing::pitch_editor::F0Track track =
      noteTrackFromVal(samples, sample_rate, f0_hz, voiced_prob, voiced, frame_rate,
                       config.voiced_threshold, &cumulative_count);
  std::vector<NoteSetEntry> entries = noteSetFromVal(notes, &cumulative_count);
  if (!(first_index < last_index)) {
    throw WasmRangeError("first must be less than last");
  }
  if (last_index >= entries.size()) {
    throw WasmRangeError("last must be less than notes.length");
  }

  Audio audio = loadValidatedAudio("samples", samples, sample_rate);
  const std::vector<editing::note_model::NoteObject> core_notes =
      deriveNoteSet(audio, track, entries, config);
  return noteObjectsToVal(
      editing::note_model::merge_notes(audio, track, core_notes, first_index, last_index, config));
}

namespace {

using editing::note_model::NoteTarget;
using editing::note_model::NoteTargetAssignConfig;
using editing::note_model::UnmatchedTargetPolicy;

// The policy crosses as a name, so the ordinal the C enumeration fixes never
// reaches JS. An unknown name is refused rather than falling back to Leave,
// which would run a different rule than the caller asked for.
UnmatchedTargetPolicy unmatchedTargetPolicyFromVal(const val& request) {
  const val value = objectProperty(request, "unmatchedPolicy");
  if (value.isUndefined()) return UnmatchedTargetPolicy::Leave;
  if (value.typeOf().as<std::string>() == "string") {
    const std::string name = value.as<std::string>();
    if (name == "leave") return UnmatchedTargetPolicy::Leave;
    if (name == "mute") return UnmatchedTargetPolicy::Mute;
    if (name == "nearest") return UnmatchedTargetPolicy::Nearest;
  }
  throw WasmRangeError("unmatchedPolicy must be 'leave', 'mute' or 'nearest'");
}

// Both floats fall back to a default-constructed config, so the defaults are the
// core's own member initialisers rather than literals repeated here. Presence
// decides: 0 is each field's own meaning, so it cannot double as "unset".
NoteTargetAssignConfig noteTargetAssignConfigFromVal(const val& request) {
  NoteTargetAssignConfig config;
  config.unmatched_policy = unmatchedTargetPolicyFromVal(request);
  config.min_overlap_ratio =
      domainFloatProperty(request, "minOverlapRatio", config.min_overlap_ratio);
  config.max_correction_semitones =
      domainFloatProperty(request, "maxCorrectionSemitones", config.max_correction_semitones);
  return config;
}

// One reference note. A non-finite bound or pitch is refused here because every
// overlap comparison is false for a NaN, so the target would be taken by the
// nearest arm and written into pitch_shift_semitones with nothing left to catch
// it (sonare_assign_note_targets).
std::vector<NoteTarget> noteTargetsFromVal(const val& targets) {
  const std::size_t count = wasmArrayLength(targets, "targets");
  std::vector<NoteTarget> out;
  out.reserve(std::min(count, kMaxWasmObjectArrayReserve));
  for (std::size_t i = 0; i < count; ++i) {
    const val row = targets[i];
    const std::string subject = "targets[" + std::to_string(i) + "]";
    NoteTarget target;
    target.start_sec = requireNumberProperty(row, "startSec", subject.c_str());
    target.end_sec = requireNumberProperty(row, "endSec", subject.c_str());
    const double target_midi = requireNumberProperty(row, "targetMidi", subject.c_str());
    // float32 turns a finite value past FLT_MAX into an infinity, and the
    // correction clamp would then report the saturated bound as the answer.
    if (std::abs(target_midi) > static_cast<double>(std::numeric_limits<float>::max())) {
      throw WasmRangeError(subject +
                           ".targetMidi must be finite and in [-3.4028235e38, 3.4028235e38]");
    }
    target.target_midi = static_cast<float>(target_midi);
    out.push_back(target);
  }
  return out;
}

// All the rule reads -- the sample span and the measured pitch -- plus the two
// fields it writes, which are carried in so a note it never touches comes back
// with the edit it arrived with rather than a zeroed one.
editing::note_model::NoteObject assignableNoteFromVal(const val& row, const std::string& at) {
  editing::note_model::NoteObject note;
  note.onset_sample = requireInt64Property(row, "onsetSample", at.c_str());
  note.offset_sample = requireInt64Property(row, "offsetSample", at.c_str());
  // floatOption, not floatProperty: a note carrying no measured pitch spells it
  // 0 or non-finite and the rule reads both the same way, so a NaN here is the
  // caller's own "unvoiced" rather than a bad argument (note_target.h). 0 is
  // also NoteObject::median_hz's own default, so an absent field means the same.
  note.median_hz = floatOption(row, "medianHz", note.median_hz);
  const val edit = objectProperty(row, "edit");
  const std::string edit_at = at + ".edit";
  note.edit.pitch_shift_semitones = floatProperty(edit, "pitchSemitones", 0.0f, edit_at);
  note.edit.muted = boolProperty(edit, "muted", false, edit_at);
  return note;
}

// Replace the two fields the rule writes while preserving the caller's other
// note fields.
val assignedNoteToVal(const val& row, const editing::note_model::NoteEdit& edit) {
  const val object_ctor = val::global("Object");
  val out = object_ctor.call<val>("assign", val::object(), row);
  val out_edit = object_ctor.call<val>("assign", val::object(), objectProperty(row, "edit"));
  out_edit.set("pitchSemitones", edit.pitch_shift_semitones);
  out_edit.set("muted", edit.muted);
  // Copy note arrays so edits to the returned object do not mutate the input.
  const val envelope = objectProperty(out_edit, "amplitudeEnvelope");
  if (!envelope.isUndefined()) out_edit.set("amplitudeEnvelope", envelope.call<val>("slice"));
  out.set("edit", out_edit);
  const val amplitude = objectProperty(row, "amplitude");
  if (!amplitude.isUndefined()) out.set("amplitude", amplitude.call<val>("slice"));
  return out;
}

}  // namespace

// Note targets: a reference melody in seconds, and the rule that writes each
// note's pitch shift from the target it overlaps. The C-ABI translation unit is
// not linked into this module, so the core rule is called directly and the
// validation sonare_assign_note_targets performs is reproduced above -- without
// it WASM would be the one surface where a non-finite target reaches the edit.
val js_assign_note_targets(val notes, const val& sample_rate_val, val targets, val request) {
  const int sample_rate = domainNumberFromVal<int>(sample_rate_val, "sampleRate",
                                                   sonare_c_detail::kNoteTargetSampleRateDomain);
  const NoteTargetAssignConfig config = noteTargetAssignConfigFromVal(request);
  const std::vector<NoteTarget> core_targets = noteTargetsFromVal(targets);

  const std::size_t count = wasmArrayLength(notes, "notes");
  std::vector<editing::note_model::NoteObject> core_notes;
  core_notes.reserve(std::min(count, kMaxWasmObjectArrayReserve));
  for (std::size_t i = 0; i < count; ++i) {
    core_notes.push_back(assignableNoteFromVal(notes[i], "notes[" + std::to_string(i) + "]"));
  }

  const std::size_t assigned = [&] {
    try {
      return editing::note_model::assign_note_targets(core_notes, sample_rate, core_targets,
                                                      config);
    } catch (const sonare::SonareParameterRefusal& refusal) {
      throw sonare_field_spelling::wasm::Spelled(refusal, "assign_note_targets", "config");
    }
  }();

  val out_notes = val::array();
  for (std::size_t i = 0; i < count; ++i) {
    out_notes.call<void>("push", assignedNoteToVal(notes[i], core_notes[i].edit));
  }
  val out = val::object();
  out.set("notes", out_notes);
  // Counts cross as plain JS numbers, matching every other size on this surface.
  out.set("assignedCount", static_cast<double>(assigned));
  return out;
}

#if defined(SONARE_WITH_ARRANGEMENT)
// Gated with the library it calls: sonare_midi is linked only for an arrangement
// build, so the reader goes with it rather than resolving to a stub. Times follow
// the file's tempo map, and the index counts only MIDI-bearing tracks, so a file
// whose first track is a conductor track has its melody at 0.
val js_note_targets_from_smf(val data, const val& track_index_val) {
  const int track_index = checkedIntFromVal(track_index_val, "trackIndex");
  const std::vector<uint8_t> bytes = uint8ArrayToVector(data, "data");
  val out = val::array();
  for (const NoteTarget& target :
       midi::note_targets_from_smf(bytes.data(), bytes.size(), track_index)) {
    val row = val::object();
    row.set("startSec", target.start_sec);
    row.set("endSec", target.end_sec);
    row.set("targetMidi", target.target_midi);
    out.call<void>("push", row);
  }
  return out;
}
#endif

void registerEffectsNoteBindings() {
  function("extractNotes", &js_extract_notes);
  function("renderNotes", &js_render_notes);
  function("decomposeNotePitch", &js_decompose_note_pitch);
  function("splitNote", &js_split_note);
  function("mergeNotes", &js_merge_notes);
  function("assignNoteTargets", &js_assign_note_targets);
#if defined(SONARE_WITH_ARRANGEMENT)
  function("noteTargetsFromSmf", &js_note_targets_from_smf);
#endif
}

#endif  // __EMSCRIPTEN__
