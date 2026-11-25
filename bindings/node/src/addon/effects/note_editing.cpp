#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "c_api/daw_config_domains.h"
#include "core/spectrum.h"
#include "editing/event_model/event_extractor.h"
#include "sonare_wrap.h"
#include "sonare_wrap_note_objects.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"
#include "util/parameter_checks.h"

using namespace sonare_node;

namespace {

/// Releases a note-object result however the marshalling below leaves scope.
struct OwnedNoteObjects {
  SonareNoteObjectsResult value{};
  ~OwnedNoteObjects() { sonare_free_note_objects_result(&value); }
};

/// Releases a heap-owned note-target array however its marshalling leaves scope.
struct OwnedNoteTargets {
  SonareNoteTarget* value = nullptr;
  ~OwnedNoteTargets() { sonare_free_note_targets(value); }
};

/// Releases a percussive-event result however the marshalling below leaves scope.
struct OwnedPercussiveEvents {
  SonarePercussiveEventsResult value{};
  ~OwnedPercussiveEvents() { sonare_free_percussive_events_result(&value); }
};

/// Releases a pitch decomposition however the marshalling below leaves scope.
struct OwnedPitchDecomposition {
  SonarePitchDecompositionResult value{};
  ~OwnedPitchDecomposition() { sonare_free_pitch_decomposition_result(&value); }
};

/// The options bag extraction, split and merge share: the segmentation knobs
/// plus the voicing arrays the track is thresholded with.
struct NoteTrackOptions {
  SonareNoteExtractorConfig config{};
  std::vector<int32_t> voiced;
  std::vector<float> voiced_prob;
  const int32_t* voiced_ptr = nullptr;
  const float* prob_ptr = nullptr;
};

/// Reads the optional render/decomposition voicing mask. The public facade
/// normalizes every supported flag spelling to Int32Array; the raw addon still
/// checks the type and length so a direct N-API caller cannot silently lose it.
bool ReadOptionalVoicedFlags(Napi::Env env, const Napi::Value& value, size_t n_frames,
                             Napi::Int32Array* storage, const int32_t** data) {
  *data = nullptr;
  if (value.IsUndefined() || value.IsNull()) return true;
  if (!IsInt32Array(value)) {
    Napi::TypeError::New(env, "voiced must be an Int32Array").ThrowAsJavaScriptException();
    return false;
  }
  *storage = value.As<Napi::Int32Array>();
  if (storage->ElementLength() != n_frames) {
    Napi::RangeError::New(env, "voiced must match f0Hz length").ThrowAsJavaScriptException();
    return false;
  }
  *data = sonare_node::ElementData(*storage);
  return true;
}

/// Reads the shared options bag, which may be absent. A voicing array that does
/// not match the track leaves a RangeError pending, so the caller must bail out
/// on a false return before its C-ABI call.
bool ReadNoteTrackOptions(Napi::Env env, const Napi::Value& value, size_t n_frames,
                          NoteTrackOptions* out) {
  sonare_note_extractor_config_init(&out->config);
  if (!value.IsObject()) {
    return true;
  }
  Napi::Object opts = value.As<Napi::Object>();
  // Effects options bag: the presence-checked reader family, so an explicit
  // `undefined` reads as the documented default and a wrong-typed value is
  // refused by name rather than silently taking it. A given value is read literally.
  SonareNoteExtractorConfig& config = out->config;
  config.segmentation_threshold_cents =
      FloatProperty(opts, "segmentationThresholdCents", config.segmentation_threshold_cents);
  config.min_note_ms = FloatProperty(opts, "minNoteMs", config.min_note_ms);
  config.reference_hz = FloatProperty(opts, "referenceHz", config.reference_hz);
  config.voiced_threshold = FloatProperty(opts, "voicedThreshold", config.voiced_threshold);

  const Napi::Value voiced_value = opts.Get("voiced");
  if (IsInt32Array(voiced_value)) {
    auto arr = voiced_value.As<Napi::Int32Array>();
    if (arr.ElementLength() != n_frames) {
      Napi::RangeError::New(env, "voiced must match f0Hz length").ThrowAsJavaScriptException();
      return false;
    }
    out->voiced.assign(sonare_node::ElementData(arr),
                       sonare_node::ElementData(arr) + arr.ElementLength());
    out->voiced_ptr = sonare_node::ElementData(out->voiced);
  }
  const Napi::Value prob_value = opts.Get("voicedProb");
  if (IsFloat32Array(prob_value)) {
    auto arr = prob_value.As<Napi::Float32Array>();
    if (!out->voiced_ptr && arr.ElementLength() != n_frames) {
      Napi::RangeError::New(env, "voicedProb must match f0Hz length").ThrowAsJavaScriptException();
      return false;
    }
    out->voiced_prob.assign(sonare_node::ElementData(arr),
                            sonare_node::ElementData(arr) + arr.ElementLength());
    // Beside voiced the core refuses the pair before reading either, so the length is not checked.
    out->voiced_prob.resize(n_frames);
    out->prob_ptr = sonare_node::ElementData(out->voiced_prob);
  }
  return true;
}

/// Rejects a row whose declared-mandatory sample bound is absent or not a
/// number, in place of the type-checked reader's silent fallback; an absent
/// bound is refused as a wrong type, as the WASM surface refuses it. Shared by
/// the note and the percussive-event readers, whose input types declare the
/// same two fields. @p at names the row, e.g. "notes[0]".
void RequireSpanKey(const std::string& at, const Napi::Object& row, const char* key) {
  if (!row.Get(key).IsNumber()) {
    throw Napi::TypeError::New(row.Env(), node_element_field(at, key) + " must be an integer");
  }
}

/// The element at @p index of @p list, which must be a non-array object, named
/// "<list_name>[<index>]" in @p at for its own field refusals.
Napi::Object RowObject(const Napi::Array& list, uint32_t index, const char* list_name,
                       std::string* at) {
  *at = std::string(list_name) + "[" + std::to_string(index) + "]";
  const Napi::Value item = list.Get(index);
  if (!item.IsObject() || item.IsArray()) {
    throw Napi::TypeError::New(list.Env(), *at + " must be an object");
  }
  return item.As<Napi::Object>();
}

/// Reads a JS note array onto the C structs, plus the envelope pool their edits
/// index into. A render reads the sample spans and a curve edit the frame
/// bounds; split and merge re-derive both from the frame bounds alone, so all
/// of them are read here and the entry point decides what it needs.
///
/// @p require_span demands the sample bounds the NoteObjectInput type declares
/// mandatory. Only a render reads them, and an omitted one used to default to
/// 0, which is a zero-length span: the note's edit silently rendered as nothing
/// while the same request threw on the WASM surface. The note-set entries split
/// and merge take do not declare the bounds at all, so they keep the default.
void ReadNotes(const Napi::Array& js_notes, std::vector<SonareNoteObject>* notes,
               std::vector<float>* envelopes, bool require_span = false) {
  const uint32_t count = node_list_length(js_notes, "notes");
  notes->assign(count, SonareNoteObject{});
  for (uint32_t i = 0; i < count; ++i) {
    sonare_note_edit_init(&(*notes)[i].edit);
    std::string at;
    const Napi::Object note = RowObject(js_notes, i, "notes", &at);
    SonareNoteObject& row = (*notes)[i];
    if (require_span) {
      RequireSpanKey(at, note, "onsetSample");
      RequireSpanKey(at, note, "offsetSample");
    }
    row.onset_sample = Int64Property(note, "onsetSample", row.onset_sample, at);
    row.offset_sample = Int64Property(note, "offsetSample", row.offset_sample, at);
    row.frame_start = IntProperty(note, "frameStart", row.frame_start, at);
    row.frame_end = IntProperty(note, "frameEnd", row.frame_end, at);
    row.median_hz = FloatDataProperty(note, "medianHz", row.median_hz, at);
    ReadNoteEdit(note, envelopes, &row.edit, at);
  }
}

/// Bounds-checks a note's slice of one of a result's shared pools. Only
/// reachable if the C ABI contradicted itself; say so, because an empty slice
/// here would read like a short note and hide the inconsistency.
void RequireSliceInRange(const char* field, int64_t offset, int64_t span, size_t pool_count) {
  if (span < 0 || offset < 0 ||
      static_cast<uint64_t>(offset) + static_cast<uint64_t>(span) > pool_count) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  std::string(field) + " slice out of range");
  }
}

/// Marshals a heap-owned result into the JS note array, giving every note its
/// own copy of its amplitude and envelope slices.
Napi::Array NoteObjectsToJs(Napi::Env env, const SonareNoteObjectsResult& result) {
  Napi::Array notes = Napi::Array::New(env, result.count);
  for (size_t i = 0; i < result.count; ++i) {
    const SonareNoteObject& note = result.notes[i];
    // Each note is given its own copy of both slices, so neither pool offset
    // ever reaches JS.
    const size_t envelope_count = note.edit.amplitude_envelope_count;
    RequireSliceInRange("amplitudeEnvelope", note.edit.amplitude_envelope_offset,
                        static_cast<int64_t>(envelope_count), result.envelope_count);
    const int64_t span = note.frame_end - note.frame_start;
    RequireSliceInRange("amplitude", note.amplitude_offset, span, result.amplitude_count);
    notes.Set(static_cast<uint32_t>(i),
              NoteObjectToJs(
                  env, note, result.amplitude + note.amplitude_offset, static_cast<size_t>(span),
                  result.envelopes + note.edit.amplitude_envelope_offset, envelope_count));
  }
  return notes;
}

/// Reads a JS reference-melody array onto the C structs. All three fields are
/// required: a target IS its span and its pitch, and an omitted one defaulting to
/// 0 is a zero-length span at the start of the take or a pitch five octaves below
/// middle C -- values a caller could have named, so nothing downstream could tell
/// the omission from a choice.
bool ReadNoteTargets(Napi::Env env, const Napi::Array& js_targets,
                     std::vector<SonareNoteTarget>* targets) {
  const uint32_t count = node_list_length(js_targets, "targets");
  targets->assign(count, SonareNoteTarget{});
  for (uint32_t i = 0; i < count; ++i) {
    std::string label;
    const Napi::Object target = RowObject(js_targets, i, "targets", &label);
    SonareNoteTarget& row = (*targets)[i];
    if (!RequiredDoubleValue(env, target.Get("startSec"), label + ".startSec", &row.start_sec) ||
        !RequiredDoubleValue(env, target.Get("endSec"), label + ".endSec", &row.end_sec) ||
        !RequiredFloatValue(env, target.Get("targetMidi"), label + ".targetMidi",
                            &row.target_midi)) {
      return false;
    }
  }
  return true;
}

/// Resolves the unmatched-note policy spelling to its C ordinal. An absent key
/// keeps whatever seeded @p out; any other value is refused by name, so a
/// misspelling cannot read as the default policy.
bool ReadUnmatchedTargetPolicy(Napi::Env env, const Napi::Object& opts, int32_t* out) {
  static const char* kExpected = "' (expected leave, mute, or nearest)";
  const Napi::Value value = opts.Get("unmatchedPolicy");
  if (value.IsUndefined() || value.IsNull()) {
    return true;
  }
  if (!value.IsString()) {
    Napi::TypeError::New(env, "unmatchedPolicy must be a string").ThrowAsJavaScriptException();
    return false;
  }
  const std::string policy = node_narrow_string(env, value, "unmatchedPolicy");
  if (policy == "leave") {
    *out = SONARE_NOTE_TARGET_UNMATCHED_LEAVE;
  } else if (policy == "mute") {
    *out = SONARE_NOTE_TARGET_UNMATCHED_MUTE;
  } else if (policy == "nearest") {
    *out = SONARE_NOTE_TARGET_UNMATCHED_NEAREST;
  } else {
    Napi::RangeError::New(env, "Unknown unmatchedPolicy: '" + policy + kExpected)
        .ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

/// Reads the four separation keys both percussive-event entry points share. The
/// framing is one input, not a knob each side restates, so neither can lift a
/// signal out on a framing the other did not measure on.
void ReadPercussiveSeparation(const Napi::Object& opts, int32_t* n_fft, size_t* hop_length,
                              int32_t* kernel_harmonic, int32_t* kernel_percussive) {
  *n_fft = IntProperty(opts, "nFft", *n_fft);
  *hop_length = static_cast<size_t>(DomainProperty<int>(
      opts, "hopLength", static_cast<int>(*hop_length), sonare::kStftHopDomain));
  *kernel_harmonic = IntProperty(opts, "hpssKernelHarmonic", *kernel_harmonic);
  *kernel_percussive = IntProperty(opts, "hpssKernelPercussive", *kernel_percussive);
}

/// Reads the extraction options bag, which may be absent. An omitted key keeps the
/// init function's default and a given one is read literally.
void ReadPercussiveEventConfig(const Napi::Value& value, SonarePercussiveEventConfig* out) {
  sonare_percussive_event_config_init(out);
  if (!value.IsObject()) {
    return;
  }
  Napi::Object opts = value.As<Napi::Object>();
  // Effects options bag: the presence-checked reader family, so an explicit
  // `undefined` reads as the documented default and a wrong-typed value is
  // refused by name rather than silently taking it.
  ReadPercussiveSeparation(opts, &out->n_fft, &out->hop_length, &out->hpss_kernel_harmonic,
                           &out->hpss_kernel_percussive);
  out->onset_wait = DomainProperty<int>(opts, "onsetWait", out->onset_wait,
                                        sonare::editing::event_model::kPercussiveOnsetWaitDomain);
  out->onset_delta = FloatProperty(opts, "onsetDelta", out->onset_delta);
  out->max_event_ms = FloatProperty(opts, "maxEventMs", out->max_event_ms);
  out->min_percussive_ratio = FloatProperty(opts, "minPercussiveRatio", out->min_percussive_ratio);
}

/// Reads the render options bag, under the same rule.
void ReadPercussiveRenderConfig(const Napi::Value& value, SonarePercussiveRenderConfig* out) {
  sonare_percussive_render_config_init(out);
  if (!value.IsObject()) {
    return;
  }
  Napi::Object opts = value.As<Napi::Object>();
  ReadPercussiveSeparation(opts, &out->n_fft, &out->hop_length, &out->hpss_kernel_harmonic,
                           &out->hpss_kernel_percussive);
  out->fade_ms = FloatProperty(opts, "fadeMs", out->fade_ms);
}

/// Reads an event's optional `edit`. A zeroed SonarePercussiveEventEdit is the
/// identity, so an omitted edit, and an omitted key within one, is a no-op.
void ReadPercussiveEventEdit(const Napi::Object& event, SonarePercussiveEventEdit* out,
                             const std::string& at) {
  const Napi::Value edit_value = event.Get("edit");
  if (edit_value.IsUndefined() || edit_value.IsNull()) {
    return;
  }
  const std::string edit_at = node_element_field(at, "edit");
  if (!edit_value.IsObject() || edit_value.IsArray()) {
    throw Napi::TypeError::New(event.Env(), edit_at + " must be an object");
  }
  Napi::Object edit = edit_value.As<Napi::Object>();
  out->time_offset_samples =
      Int64Property(edit, "timeOffsetSamples", out->time_offset_samples, edit_at);
  out->gain_db = FloatProperty(edit, "gainDb", out->gain_db, edit_at);
  out->muted = BoolProperty(edit, "muted", out->muted != 0, edit_at) ? 1 : 0;
}

/// Reads a JS event array onto the C structs. Only the span and the edit are
/// read: rendering ignores the measured figures, so an event straight from an
/// extraction round-trips without them having to survive the trip. The span is
/// required, as PercussiveEventInput declares it — an omitted bound defaulted to
/// 0, and a zero-length span renders the event's edit as nothing.
void ReadPercussiveEvents(const Napi::Array& js_events,
                          std::vector<SonarePercussiveEvent>* events) {
  const uint32_t count = node_list_length(js_events, "events");
  events->assign(count, SonarePercussiveEvent{});
  for (uint32_t i = 0; i < count; ++i) {
    std::string at;
    const Napi::Object event = RowObject(js_events, i, "events", &at);
    SonarePercussiveEvent& row = (*events)[i];
    RequireSpanKey(at, event, "onsetSample");
    RequireSpanKey(at, event, "offsetSample");
    row.onset_sample = Int64Property(event, "onsetSample", 0, at);
    row.offset_sample = Int64Property(event, "offsetSample", 0, at);
    ReadPercussiveEventEdit(event, &row.edit, at);
  }
}

/// Marshals a heap-owned result into the JS event array. One allocation, unlike
/// the note objects: an event carries three scalars and nothing per frame, so
/// there is no pool to slice.
Napi::Array PercussiveEventsToJs(Napi::Env env, const SonarePercussiveEventsResult& result) {
  Napi::Array events = Napi::Array::New(env, result.count);
  for (size_t i = 0; i < result.count; ++i) {
    const SonarePercussiveEvent& event = result.events[i];
    Napi::Object row = Napi::Object::New(env);
    // No int64 in N-API: sample positions marshal as JS numbers, exact up to
    // Number.MAX_SAFE_INTEGER (2^53-1 samples, millennia of audio).
    row.Set("onsetSample", Napi::Number::New(env, static_cast<double>(event.onset_sample)));
    row.Set("offsetSample", Napi::Number::New(env, static_cast<double>(event.offset_sample)));
    row.Set("strength", Napi::Number::New(env, event.strength));
    row.Set("peakAmplitude", Napi::Number::New(env, event.peak_amplitude));
    SetMeasured(row, "percussiveRatio", event.percussive_ratio);

    Napi::Object edit = Napi::Object::New(env);
    edit.Set("timeOffsetSamples",
             Napi::Number::New(env, static_cast<double>(event.edit.time_offset_samples)));
    edit.Set("gainDb", Napi::Number::New(env, event.edit.gain_db));
    edit.Set("muted", Napi::Boolean::New(env, event.edit.muted != 0));
    row.Set("edit", edit);

    events.Set(static_cast<uint32_t>(i), row);
  }
  return events;
}

}  // namespace

Napi::Value SonareWrap::ExtractNotes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples, sampleRate, f0Hz, frameRate, options?)
  if (!RequireMonoArgs(info)) return env.Undefined();
  if (!IsFloat32Array(info[2])) {
    Napi::TypeError::New(env, "f0Hz must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[3].IsNumber()) {
    Napi::TypeError::New(env, "frameRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = sonare_node::ElementData(typed);
  const size_t length = typed.ElementLength();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  auto f0 = info[2].As<Napi::Float32Array>();
  const size_t n_frames = f0.ElementLength();
  const float frame_rate = node_narrow_float(env, info[3], "frameRate");

  NoteTrackOptions options;
  if (!ReadNoteTrackOptions(env, info[4], n_frames, &options)) {
    return env.Undefined();
  }

  OwnedNoteObjects result;
  const SonareError err = sonare_extract_notes(data, length, sr, sonare_node::ElementData(f0),
                                               options.prob_ptr, options.voiced_ptr, n_frames,
                                               frame_rate, &options.config, &result.value);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  return NoteObjectsToJs(env, result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::RenderNotes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples, sampleRate, notes, options?)
  if (!RequireMonoArgs(info)) return env.Undefined();
  if (!info[2].IsArray()) {
    Napi::TypeError::New(env, "notes must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = sonare_node::ElementData(typed);
  const size_t length = typed.ElementLength();
  const int sr = node_narrow_int(env, info[1], "sampleRate");

  SonareNoteRenderConfig config;
  sonare_note_render_config_init(&config);
  // The track a curve edit acts on rides in the same bag; the C ABI's frame
  // count is derived from its length rather than taken separately.
  Napi::Float32Array f0;
  Napi::Int32Array voiced;
  const float* f0_data = nullptr;
  const int32_t* voiced_data = nullptr;
  size_t n_frames = 0;
  float frame_rate = 0.0f;
  if (info[3].IsObject()) {
    Napi::Object opts = info[3].As<Napi::Object>();
    config.fade_ms = DomainFloatProperty(opts, "fadeMs", config.fade_ms);
    config.vibrato_cutoff_hz =
        DomainFloatProperty(opts, "vibratoCutoffHz", config.vibrato_cutoff_hz);
    frame_rate = FloatProperty(opts, "frameRate", 0.0f);
    const Napi::Value f0_value = opts.Get("f0Hz");
    if (!f0_value.IsUndefined() && !f0_value.IsNull() && !IsFloat32Array(f0_value)) {
      Napi::TypeError::New(env, "f0Hz must be a Float32Array").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    if (IsFloat32Array(f0_value)) {
      f0 = f0_value.As<Napi::Float32Array>();
      f0_data = sonare_node::ElementData(f0);
      n_frames = f0.ElementLength();
    }
    const Napi::Value voiced_value = opts.Get("voiced");
    if (!voiced_value.IsUndefined() && !voiced_value.IsNull() && f0_data == nullptr) {
      // Worded as the C ABI words it; the presence requirement makes it a shape refusal.
      sonare::util::check::refuse_relation({"f0Hz", "voiced"},
                                           sonare::util::refusal::kPresentWhenPresentRelation);
    }
    if (!ReadOptionalVoicedFlags(env, voiced_value, n_frames, &voiced, &voiced_data)) {
      return env.Undefined();
    }
  }

  std::vector<SonareNoteObject> notes;
  std::vector<float> envelopes;
  ReadNotes(info[2].As<Napi::Array>(), &notes, &envelopes, /*require_span=*/true);

  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err =
      sonare_render_notes(data, length, sr, notes.empty() ? nullptr : notes.data(), notes.size(),
                          envelopes.empty() ? nullptr : envelopes.data(), envelopes.size(), f0_data,
                          voiced_data, n_frames, frame_rate, &config, &out, &out_length);
  if (err != SONARE_OK) {
    ThrowSonareError(env, err);
    return env.Undefined();
  }
  return TakeCFloats(env, out, out_length);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DecomposeNotePitch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (f0Hz, frameRate, medianHz, vibratoCutoffHz, voiced?)
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "f0Hz must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "frameRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsNumber()) {
    Napi::TypeError::New(env, "medianHz must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[3].IsNumber()) {
    Napi::TypeError::New(env, "vibratoCutoffHz must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto f0 = info[0].As<Napi::Float32Array>();
  // Type-only: the C ABI refuses NaN, infinities and out-of-domain values in its own words.
  float frame_rate = 0.0f;
  float median_hz = 0.0f;
  float vibrato_cutoff_hz = 0.0f;
  if (!RequiredDomainFloatArg(env, info, 1, "frameRate", &frame_rate) ||
      !RequiredDomainFloatArg(env, info, 2, "medianHz", &median_hz) ||
      !RequiredDomainFloatArg(env, info, 3, "vibratoCutoffHz", &vibrato_cutoff_hz)) {
    return env.Undefined();
  }
  Napi::Int32Array voiced;
  const int32_t* voiced_data = nullptr;
  const Napi::Value voiced_value = info.Length() > 4 ? info[4] : env.Undefined();
  if (!ReadOptionalVoicedFlags(env, voiced_value, f0.ElementLength(), &voiced, &voiced_data)) {
    return env.Undefined();
  }

  OwnedPitchDecomposition decomposition;
  const SonareError err =
      sonare_decompose_note_pitch(sonare_node::ElementData(f0), voiced_data, f0.ElementLength(),
                                  frame_rate, median_hz, vibrato_cutoff_hz, &decomposition.value);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("centerHz", Napi::Number::New(env, decomposition.value.center_hz));
  // A note with no usable pitch comes back as a zero centre and NULL curves,
  // which marshal to two empty arrays rather than to an error.
  out.Set("driftCents",
          Float32ArrayFrom(env, decomposition.value.drift_cents, decomposition.value.count));
  out.Set("vibratoCents",
          Float32ArrayFrom(env, decomposition.value.vibrato_cents, decomposition.value.count));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SplitNote(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples, sampleRate, f0Hz, frameRate, notes, index, frame, options?)
  if (!RequireMonoArgs(info)) return env.Undefined();
  if (!IsFloat32Array(info[2])) {
    Napi::TypeError::New(env, "f0Hz must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[3].IsNumber()) {
    Napi::TypeError::New(env, "frameRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[4].IsArray()) {
    Napi::TypeError::New(env, "notes must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[5].IsNumber()) {
    Napi::TypeError::New(env, "index must be an integer").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[6].IsNumber()) {
    Napi::TypeError::New(env, "frame must be an integer").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = sonare_node::ElementData(typed);
  const size_t length = typed.ElementLength();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  auto f0 = info[2].As<Napi::Float32Array>();
  const size_t n_frames = f0.ElementLength();
  const float frame_rate = node_narrow_float(env, info[3], "frameRate");
  const size_t index = node_narrow_uint32(env, info[5], "index");
  const int32_t frame = node_narrow_int(env, info[6], "frame");

  NoteTrackOptions options;
  if (!ReadNoteTrackOptions(env, info[7], n_frames, &options)) {
    return env.Undefined();
  }

  std::vector<SonareNoteObject> notes;
  std::vector<float> envelopes;
  ReadNotes(info[4].As<Napi::Array>(), &notes, &envelopes);

  OwnedNoteObjects result;
  const SonareError err = sonare_split_note(
      data, length, sr, sonare_node::ElementData(f0), options.prob_ptr, options.voiced_ptr,
      n_frames, frame_rate, &options.config, notes.empty() ? nullptr : notes.data(), notes.size(),
      envelopes.empty() ? nullptr : envelopes.data(), envelopes.size(), index, frame,
      &result.value);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  return NoteObjectsToJs(env, result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MergeNotes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples, sampleRate, f0Hz, frameRate, notes, first, last, options?)
  if (!RequireMonoArgs(info)) return env.Undefined();
  if (!IsFloat32Array(info[2])) {
    Napi::TypeError::New(env, "f0Hz must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[3].IsNumber()) {
    Napi::TypeError::New(env, "frameRate must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[4].IsArray()) {
    Napi::TypeError::New(env, "notes must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[5].IsNumber()) {
    Napi::TypeError::New(env, "first must be an integer").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[6].IsNumber()) {
    Napi::TypeError::New(env, "last must be an integer").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = sonare_node::ElementData(typed);
  const size_t length = typed.ElementLength();
  const int sr = node_narrow_int(env, info[1], "sampleRate");
  auto f0 = info[2].As<Napi::Float32Array>();
  const size_t n_frames = f0.ElementLength();
  const float frame_rate = node_narrow_float(env, info[3], "frameRate");
  const size_t first = node_narrow_uint32(env, info[5], "first");
  const size_t last = node_narrow_uint32(env, info[6], "last");

  NoteTrackOptions options;
  if (!ReadNoteTrackOptions(env, info[7], n_frames, &options)) {
    return env.Undefined();
  }

  std::vector<SonareNoteObject> notes;
  std::vector<float> envelopes;
  ReadNotes(info[4].As<Napi::Array>(), &notes, &envelopes);

  OwnedNoteObjects result;
  const SonareError err = sonare_merge_notes(
      data, length, sr, sonare_node::ElementData(f0), options.prob_ptr, options.voiced_ptr,
      n_frames, frame_rate, &options.config, notes.empty() ? nullptr : notes.data(), notes.size(),
      envelopes.empty() ? nullptr : envelopes.data(), envelopes.size(), first, last, &result.value);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  return NoteObjectsToJs(env, result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::NoteTargetsFromSmf(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  SONARE_NODE_TRY
  // (bytes, track?)
  const uint8_t* bytes = nullptr;
  size_t len = 0;
  if (!RequiredBytesArg(env, info, 0, "data", &bytes, &len)) return env.Undefined();
  // An index is an ordinal, so the strict reader: 1.5 truncating to 1 would read
  // a track the caller never named. A negative one is the C ABI's to refuse.
  int track_index = 0;
  if (!Int32Arg(env, info, 1, "trackIndex", 0, &track_index)) {
    return env.Undefined();
  }

  OwnedNoteTargets targets;
  size_t count = 0;
  const SonareError err =
      sonare_note_targets_from_smf(bytes, len, track_index, &targets.value, &count);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }

  Napi::Array out = Napi::Array::New(env, count);
  for (size_t i = 0; i < count; ++i) {
    const SonareNoteTarget& target = targets.value[i];
    Napi::Object row = Napi::Object::New(env);
    row.Set("startSec", Napi::Number::New(env, target.start_sec));
    row.Set("endSec", Napi::Number::New(env, target.end_sec));
    row.Set("targetMidi", Napi::Number::New(env, target.target_midi));
    out.Set(static_cast<uint32_t>(i), row);
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::AssignNoteTargets(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (notes, sampleRate, targets, options?)
  if (!info[0].IsArray()) {
    Napi::TypeError::New(env, "notes must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[1].IsNumber()) {
    Napi::TypeError::New(env, "sampleRate must be an integer").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (!info[2].IsArray()) {
    Napi::TypeError::New(env, "targets must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  const int sr =
      node_domain_number<int>(info[1], "sampleRate", sonare_c_detail::kNoteTargetSampleRateDomain);

  // Seeded from the library rather than from literals here: 0 is a legal value on
  // both floats, so it cannot spell "unset" and an omitted key has to keep
  // whatever the C ABI reports as its own default.
  SonareNoteTargetAssignConfig config;
  sonare_note_target_assign_config_init(&config);
  if (info.Length() > 3 && info[3].IsObject()) {
    Napi::Object opts = info[3].As<Napi::Object>();
    if (!ReadUnmatchedTargetPolicy(env, opts, &config.unmatched_policy)) {
      return env.Undefined();
    }
    config.min_overlap_ratio =
        DomainFloatProperty(opts, "minOverlapRatio", config.min_overlap_ratio);
    config.max_correction_semitones =
        DomainFloatProperty(opts, "maxCorrectionSemitones", config.max_correction_semitones);
  }

  std::vector<SonareNoteObject> notes;
  std::vector<float> envelopes;
  // The sample bounds are what the rule measures each overlap against, so they
  // are required here as they are for a render.
  ReadNotes(info[0].As<Napi::Array>(), &notes, &envelopes,
            /*require_span=*/true);

  std::vector<SonareNoteTarget> targets;
  if (!ReadNoteTargets(env, info[2].As<Napi::Array>(), &targets)) {
    return env.Undefined();
  }

  size_t assigned = 0;
  const SonareError err = sonare_assign_note_targets(
      notes.empty() ? nullptr : notes.data(), notes.size(), sr,
      targets.empty() ? nullptr : targets.data(), targets.size(), &config, &assigned);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }

  // Only the two fields the C ABI writes come back. The measured fields and the
  // amplitude curve were never read here, so the facade merges these onto the
  // caller's own notes rather than this door rebuilding a note it cannot.
  Napi::Array edits = Napi::Array::New(env, notes.size());
  for (size_t i = 0; i < notes.size(); ++i) {
    Napi::Object edit = Napi::Object::New(env);
    edit.Set("pitchSemitones", Napi::Number::New(env, notes[i].edit.pitch_semitones));
    edit.Set("muted", Napi::Boolean::New(env, notes[i].edit.muted != 0));
    edits.Set(static_cast<uint32_t>(i), edit);
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("edits", edits);
  out.Set("assignedCount", Napi::Number::New(env, static_cast<double>(assigned)));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ExtractPercussiveEvents(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples, sampleRate, options?)
  if (!RequireMonoArgs(info)) return env.Undefined();

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = sonare_node::ElementData(typed);
  const size_t length = typed.ElementLength();
  const int sr = node_narrow_int(env, info[1], "sampleRate");

  SonarePercussiveEventConfig config{};
  ReadPercussiveEventConfig(info[2], &config);

  OwnedPercussiveEvents result;
  const SonareError err =
      sonare_extract_percussive_events(data, length, sr, &config, &result.value);
  if (err != SONARE_OK) {
    ThrowSonareError(env, err);
    return env.Undefined();
  }
  // Audio in which nothing was detected comes back as a NULL pointer and a zero
  // count, which marshals to an empty array rather than to an error.
  return PercussiveEventsToJs(env, result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::RenderPercussiveEvents(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples, sampleRate, events, options?)
  if (!RequireMonoArgs(info)) return env.Undefined();
  if (!info[2].IsArray()) {
    Napi::TypeError::New(env, "events must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = sonare_node::ElementData(typed);
  const size_t length = typed.ElementLength();
  const int sr = node_narrow_int(env, info[1], "sampleRate");

  SonarePercussiveRenderConfig config{};
  ReadPercussiveRenderConfig(info[3], &config);

  std::vector<SonarePercussiveEvent> events;
  ReadPercussiveEvents(info[2].As<Napi::Array>(), &events);

  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err =
      sonare_render_percussive_events(data, length, sr, events.empty() ? nullptr : events.data(),
                                      events.size(), &config, &out, &out_length);
  if (err != SONARE_OK) {
    ThrowSonareError(env, err);
    return env.Undefined();
  }
  return TakeCFloats(env, out, out_length);
  SONARE_NODE_CATCH(env)
}
