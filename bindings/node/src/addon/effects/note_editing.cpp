#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "sonare_wrap.h"
#include "sonare_wrap_note_objects.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;

namespace {

/// Releases a note-object result however the marshalling below leaves scope.
struct OwnedNoteObjects {
  SonareNoteObjectsResult value{};
  ~OwnedNoteObjects() { sonare_free_note_objects(&value); }
};

/// Releases a heap-owned note-target array however its marshalling leaves scope.
struct OwnedNoteTargets {
  SonareNoteTarget* value = nullptr;
  ~OwnedNoteTargets() { sonare_free_note_targets(value); }
};

/// Releases a percussive-event result however the marshalling below leaves scope.
struct OwnedPercussiveEvents {
  SonarePercussiveEventsResult value{};
  ~OwnedPercussiveEvents() { sonare_free_percussive_events(&value); }
};

/// Releases a pitch decomposition however the marshalling below leaves scope.
struct OwnedPitchDecomposition {
  SonarePitchDecompositionResult value{};
  ~OwnedPitchDecomposition() { sonare_free_pitch_decomposition(&value); }
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

/// Reads the shared options bag, which may be absent. A voicing array that does
/// not match the track leaves a RangeError pending, so the caller must bail out
/// on a false return before its C-ABI call.
bool ReadNoteTrackOptions(Napi::Env env, const Napi::Value& value, size_t n_frames,
                          NoteTrackOptions* out) {
  out->config.struct_version = 1;
  if (!value.IsObject()) {
    return true;
  }
  Napi::Object opts = value.As<Napi::Object>();
  // Effects options bag: the presence-checked reader family, so an explicit
  // `undefined` reads as the documented default and a wrong-typed value is
  // refused by name rather than silently taking it.
  out->config.segmentation_threshold_cents =
      FloatProperty(opts, "segmentationThresholdCents", 0.0f);
  out->config.min_note_ms = FloatProperty(opts, "minNoteMs", 0.0f);
  out->config.reference_hz = FloatProperty(opts, "referenceHz", 0.0f);
  out->config.voiced_threshold = FloatProperty(opts, "voicedThreshold", 0.0f);

  const Napi::Value voiced_value = opts.Get("voiced");
  if (IsInt32Array(voiced_value)) {
    auto arr = voiced_value.As<Napi::Int32Array>();
    if (arr.ElementLength() != n_frames) {
      Napi::RangeError::New(env, "voiced must match f0Hz length").ThrowAsJavaScriptException();
      return false;
    }
    out->voiced.assign(arr.Data(), arr.Data() + arr.ElementLength());
    out->voiced_ptr = out->voiced.data();
  }
  const Napi::Value prob_value = opts.Get("voicedProb");
  if (IsFloat32Array(prob_value)) {
    auto arr = prob_value.As<Napi::Float32Array>();
    if (arr.ElementLength() != n_frames) {
      Napi::RangeError::New(env, "voicedProb must match f0Hz length").ThrowAsJavaScriptException();
      return false;
    }
    out->voiced_prob.assign(arr.Data(), arr.Data() + arr.ElementLength());
    out->prob_ptr = out->voiced_prob.data();
  }
  return true;
}

/// Rejects a row whose declared-mandatory sample bound is absent or not a
/// number, in place of the type-checked reader's silent fallback. Throws the
/// error class the WASM surface throws for the same omission, so the two agree
/// on the rejection as well as on the policy. Shared by the note and the
/// percussive-event readers, whose input types declare the same two fields.
void RequireSpanKey(const char* fn, const char* subject, const Napi::Object& row, const char* key) {
  const Napi::Value value = row.Get(key);
  if (value.IsUndefined() || value.IsNull()) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  std::string(fn) + " " + subject + "." + key + " is required");
  }
  if (!value.IsNumber()) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidParameter,
        std::string(fn) + " " + subject + "." + key + " must be a number");
  }
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
void ReadNotes(const char* fn, const Napi::Array& js_notes, std::vector<SonareNoteObject>* notes,
               std::vector<float>* envelopes, bool require_span = false) {
  const uint32_t count = js_notes.Length();
  notes->assign(count, SonareNoteObject{});
  for (uint32_t i = 0; i < count; ++i) {
    Napi::Value item = js_notes.Get(i);
    if (!item.IsObject() || item.IsArray()) {
      throw std::runtime_error(std::string(fn) + ": each note must be a plain object");
    }
    Napi::Object note = item.As<Napi::Object>();
    SonareNoteObject& row = (*notes)[i];
    if (require_span) {
      RequireSpanKey(fn, "note", note, "onsetSample");
      RequireSpanKey(fn, "note", note, "offsetSample");
    }
    row.onset_sample = Int64Property(note, "onsetSample", 0);
    row.offset_sample = Int64Property(note, "offsetSample", 0);
    row.frame_start = IntProperty(note, "frameStart", 0);
    row.frame_end = IntProperty(note, "frameEnd", 0);
    row.median_hz = FloatProperty(note, "medianHz", 0.0f);
    ReadNoteEdit(fn, note, envelopes, &row.edit);
  }
}

/// Bounds-checks a note's slice of one of a result's shared pools. Only
/// reachable if the C ABI contradicted itself; say so, because an empty slice
/// here would read like a short note and hide the inconsistency.
void RequireSliceInRange(const char* fn, const char* field, int64_t offset, int64_t span,
                         size_t pool_count) {
  if (span < 0 || offset < 0 ||
      static_cast<uint64_t>(offset) + static_cast<uint64_t>(span) > pool_count) {
    throw std::runtime_error(std::string(fn) + ": " + field + " slice out of range");
  }
}

/// Marshals a heap-owned result into the JS note array, giving every note its
/// own copy of its amplitude and envelope slices.
Napi::Array NoteObjectsToJs(Napi::Env env, const char* fn, const SonareNoteObjectsResult& result) {
  Napi::Array notes = Napi::Array::New(env, result.count);
  for (size_t i = 0; i < result.count; ++i) {
    const SonareNoteObject& note = result.notes[i];
    // Each note is given its own copy of both slices, so neither pool offset
    // ever reaches JS.
    const size_t envelope_count = note.edit.envelope_count;
    RequireSliceInRange(fn, "amplitudeEnvelope", note.edit.envelope_offset,
                        static_cast<int64_t>(envelope_count), result.envelope_count);
    const int64_t span = note.frame_end - note.frame_start;
    RequireSliceInRange(fn, "amplitude", note.amplitude_offset, span, result.amplitude_count);
    notes.Set(static_cast<uint32_t>(i),
              NoteObjectToJs(env, note, result.amplitude + note.amplitude_offset,
                             static_cast<size_t>(span),
                             result.envelopes + note.edit.envelope_offset, envelope_count));
  }
  return notes;
}

/// Reads a JS reference-melody array onto the C structs. All three fields are
/// required: a target IS its span and its pitch, and an omitted one defaulting to
/// 0 is a zero-length span at the start of the take or a pitch five octaves below
/// middle C -- values a caller could have named, so nothing downstream could tell
/// the omission from a choice.
bool ReadNoteTargets(Napi::Env env, const char* fn, const Napi::Array& js_targets,
                     std::vector<SonareNoteTarget>* targets) {
  const uint32_t count = js_targets.Length();
  targets->assign(count, SonareNoteTarget{});
  for (uint32_t i = 0; i < count; ++i) {
    Napi::Value item = js_targets.Get(i);
    if (!item.IsObject() || item.IsArray()) {
      throw std::runtime_error(std::string(fn) + ": each target must be a plain object");
    }
    Napi::Object target = item.As<Napi::Object>();
    SonareNoteTarget& row = (*targets)[i];
    // Indexed, because the key alone cannot say which target was refused.
    const std::string label = std::string(fn) + " targets[" + std::to_string(i) + "]";
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
  const std::string policy = value.As<Napi::String>().Utf8Value();
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
void ReadPercussiveSeparation(const Napi::Object& opts, int32_t* n_fft, int32_t* hop_length,
                              int32_t* kernel_harmonic, int32_t* kernel_percussive) {
  *n_fft = IntProperty(opts, "nFft", kZeroIsSentinel);
  *hop_length = IntProperty(opts, "hopLength", kZeroIsSentinel);
  *kernel_harmonic = IntProperty(opts, "hpssKernelHarmonic", kZeroIsSentinel);
  *kernel_percussive = IntProperty(opts, "hpssKernelPercussive", kZeroIsSentinel);
}

/// Reads the extraction options bag, which may be absent. Every field takes its
/// default at 0 on the C side, so an omitted bag and an all-zero one agree.
void ReadPercussiveEventConfig(const Napi::Value& value, SonarePercussiveEventConfig* out) {
  out->struct_version = 1;
  if (!value.IsObject()) {
    return;
  }
  Napi::Object opts = value.As<Napi::Object>();
  // Effects options bag: the presence-checked reader family, so an explicit
  // `undefined` reads as the documented default and a wrong-typed value is
  // refused by name rather than silently taking it.
  ReadPercussiveSeparation(opts, &out->n_fft, &out->hop_length, &out->hpss_kernel_harmonic,
                           &out->hpss_kernel_percussive);
  out->onset_wait = IntProperty(opts, "onsetWait", kZeroIsSentinel);
  out->onset_delta = FloatProperty(opts, "onsetDelta", 0.0f);
  out->max_event_ms = FloatProperty(opts, "maxEventMs", 0.0f);
  // 0 is this field's own meaning as well as its default, and the C ABI assigns
  // it as-is, so "keep everything" stays reachable from here.
  out->min_percussive_ratio = FloatProperty(opts, "minPercussiveRatio", 0.0f);
}

/// Reads the render options bag, under the same rule.
void ReadPercussiveRenderConfig(const Napi::Value& value, SonarePercussiveRenderConfig* out) {
  out->struct_version = 1;
  if (!value.IsObject()) {
    return;
  }
  Napi::Object opts = value.As<Napi::Object>();
  ReadPercussiveSeparation(opts, &out->n_fft, &out->hop_length, &out->hpss_kernel_harmonic,
                           &out->hpss_kernel_percussive);
  out->fade_ms = FloatProperty(opts, "fadeMs", 0.0f);
}

/// Reads an event's optional `edit`. A zeroed SonarePercussiveEventEdit is the
/// identity, so an omitted edit, and an omitted key within one, is a no-op.
void ReadPercussiveEventEdit(const char* fn, const Napi::Object& event,
                             SonarePercussiveEventEdit* out) {
  const Napi::Value edit_value = event.Get("edit");
  if (edit_value.IsUndefined() || edit_value.IsNull()) {
    return;
  }
  if (!edit_value.IsObject() || edit_value.IsArray()) {
    throw std::runtime_error(std::string(fn) + ": event.edit must be a plain object");
  }
  Napi::Object edit = edit_value.As<Napi::Object>();
  out->time_offset_samples = Int64Property(edit, "timeOffsetSamples", 0);
  out->gain_db = FloatProperty(edit, "gainDb", 0.0f);
  out->muted = BoolProperty(edit, "muted", false) ? 1 : 0;
}

/// Reads a JS event array onto the C structs. Only the span and the edit are
/// read: rendering ignores the measured figures, so an event straight from an
/// extraction round-trips without them having to survive the trip. The span is
/// required, as PercussiveEventInput declares it — an omitted bound defaulted to
/// 0, and a zero-length span renders the event's edit as nothing.
void ReadPercussiveEvents(const char* fn, const Napi::Array& js_events,
                          std::vector<SonarePercussiveEvent>* events) {
  const uint32_t count = js_events.Length();
  events->assign(count, SonarePercussiveEvent{});
  for (uint32_t i = 0; i < count; ++i) {
    Napi::Value item = js_events.Get(i);
    if (!item.IsObject() || item.IsArray()) {
      throw std::runtime_error(std::string(fn) + ": each event must be a plain object");
    }
    Napi::Object event = item.As<Napi::Object>();
    SonarePercussiveEvent& row = (*events)[i];
    RequireSpanKey(fn, "event", event, "onsetSample");
    RequireSpanKey(fn, "event", event, "offsetSample");
    row.onset_sample = Int64Property(event, "onsetSample", 0);
    row.offset_sample = Int64Property(event, "offsetSample", 0);
    ReadPercussiveEventEdit(fn, event, &row.edit);
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
    row.Set("percussiveRatio", Napi::Number::New(env, event.percussive_ratio));

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
  if (info.Length() < 4 || !IsFloat32Array(info[0]) || !info[1].IsNumber() ||
      !IsFloat32Array(info[2]) || !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, f0Hz Float32Array, frameRate)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  const size_t length = typed.ElementLength();
  const int sr = node_narrow_int(env, info[1], "sr");
  auto f0 = info[2].As<Napi::Float32Array>();
  const size_t n_frames = f0.ElementLength();
  const float frame_rate = node_narrow_finite_float(env, info[3], "frameRate");

  NoteTrackOptions options;
  if (!ReadNoteTrackOptions(env, info[4], n_frames, &options)) {
    return env.Undefined();
  }

  OwnedNoteObjects result;
  const SonareError err =
      sonare_extract_notes(data, length, sr, f0.Data(), options.prob_ptr, options.voiced_ptr,
                           n_frames, frame_rate, &options.config, &result.value);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  return NoteObjectsToJs(env, "extractNotes", result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::RenderNotes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples, sampleRate, notes, options?)
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsArray()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, sampleRate, notes: object[], options?: object)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  const size_t length = typed.ElementLength();
  const int sr = node_narrow_int(env, info[1], "sr");

  SonareNoteRenderConfig config{};
  config.struct_version = 1;
  // The track a curve edit acts on rides in the same bag; the C ABI's frame
  // count is derived from its length rather than taken separately.
  Napi::Float32Array f0;
  const float* f0_data = nullptr;
  size_t n_frames = 0;
  float frame_rate = 0.0f;
  if (info[3].IsObject()) {
    Napi::Object opts = info[3].As<Napi::Object>();
    config.fade_ms = FloatProperty(opts, "fadeMs", 0.0f);
    config.vibrato_cutoff_hz = FloatProperty(opts, "vibratoCutoffHz", 0.0f);
    frame_rate = FloatProperty(opts, "frameRate", 0.0f);
    const Napi::Value f0_value = opts.Get("f0Hz");
    if (IsFloat32Array(f0_value)) {
      f0 = f0_value.As<Napi::Float32Array>();
      f0_data = f0.Data();
      n_frames = f0.ElementLength();
    }
  }

  std::vector<SonareNoteObject> notes;
  std::vector<float> envelopes;
  ReadNotes("renderNotes", info[2].As<Napi::Array>(), &notes, &envelopes, /*require_span=*/true);

  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err =
      sonare_render_notes(data, length, sr, notes.empty() ? nullptr : notes.data(), notes.size(),
                          envelopes.empty() ? nullptr : envelopes.data(), envelopes.size(), f0_data,
                          n_frames, frame_rate, &config, &out, &out_length);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  auto result = CopyToFloat32(env, out, out_length);
  sonare_free_floats(out);
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DecomposeNotePitch(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (f0Hz, frameRate, medianHz, vibratoCutoffHz)
  if (info.Length() < 4 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsNumber() ||
      !info[3].IsNumber()) {
    Napi::TypeError::New(env, "Expected (f0Hz Float32Array, frameRate, medianHz, vibratoCutoffHz)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto f0 = info[0].As<Napi::Float32Array>();
  const float frame_rate = node_narrow_finite_float(env, info[1], "frameRate");
  const float median_hz = node_narrow_finite_float(env, info[2], "medianHz");
  const float vibrato_cutoff_hz = node_narrow_finite_float(env, info[3], "vibratoCutoffHz");

  OwnedPitchDecomposition decomposition;
  const SonareError err =
      sonare_decompose_note_pitch(f0.Data(), f0.ElementLength(), frame_rate, median_hz,
                                  vibrato_cutoff_hz, &decomposition.value);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("centreHz", Napi::Number::New(env, decomposition.value.centre_hz));
  // A note with no usable pitch comes back as a zero centre and NULL curves,
  // which marshal to two empty arrays rather than to an error.
  out.Set("driftCents",
          CopyToFloat32(env, decomposition.value.drift_cents, decomposition.value.count));
  out.Set("vibratoCents",
          CopyToFloat32(env, decomposition.value.vibrato_cents, decomposition.value.count));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SplitNote(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples, sampleRate, f0Hz, frameRate, notes, index, frame, options?)
  if (info.Length() < 7 || !IsFloat32Array(info[0]) || !info[1].IsNumber() ||
      !IsFloat32Array(info[2]) || !info[3].IsNumber() || !info[4].IsArray() ||
      !info[5].IsNumber() || !info[6].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, sampleRate, f0Hz Float32Array, frameRate, "
                         "notes: object[], index, frame)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  const size_t length = typed.ElementLength();
  const int sr = node_narrow_int(env, info[1], "sr");
  auto f0 = info[2].As<Napi::Float32Array>();
  const size_t n_frames = f0.ElementLength();
  const float frame_rate = node_narrow_finite_float(env, info[3], "frameRate");
  // A negative index arrives as a size_t past every note, which the C ABI
  // rejects as the out-of-range index it is.
  const size_t index = static_cast<size_t>(node_narrow_int64(env, info[5], "index"));
  const int32_t frame = node_narrow_int(env, info[6], "frame");

  NoteTrackOptions options;
  if (!ReadNoteTrackOptions(env, info[7], n_frames, &options)) {
    return env.Undefined();
  }

  std::vector<SonareNoteObject> notes;
  std::vector<float> envelopes;
  ReadNotes("splitNote", info[4].As<Napi::Array>(), &notes, &envelopes);

  OwnedNoteObjects result;
  const SonareError err =
      sonare_split_note(data, length, sr, f0.Data(), options.prob_ptr, options.voiced_ptr, n_frames,
                        frame_rate, &options.config, notes.empty() ? nullptr : notes.data(),
                        notes.size(), envelopes.empty() ? nullptr : envelopes.data(),
                        envelopes.size(), index, frame, &result.value);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  return NoteObjectsToJs(env, "splitNote", result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::MergeNotes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  // (samples, sampleRate, f0Hz, frameRate, notes, first, last, options?)
  if (info.Length() < 7 || !IsFloat32Array(info[0]) || !info[1].IsNumber() ||
      !IsFloat32Array(info[2]) || !info[3].IsNumber() || !info[4].IsArray() ||
      !info[5].IsNumber() || !info[6].IsNumber()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, sampleRate, f0Hz Float32Array, frameRate, "
                         "notes: object[], first, last)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  const size_t length = typed.ElementLength();
  const int sr = node_narrow_int(env, info[1], "sr");
  auto f0 = info[2].As<Napi::Float32Array>();
  const size_t n_frames = f0.ElementLength();
  const float frame_rate = node_narrow_finite_float(env, info[3], "frameRate");
  const size_t first = static_cast<size_t>(node_narrow_int64(env, info[5], "first"));
  const size_t last = static_cast<size_t>(node_narrow_int64(env, info[6], "last"));

  NoteTrackOptions options;
  if (!ReadNoteTrackOptions(env, info[7], n_frames, &options)) {
    return env.Undefined();
  }

  std::vector<SonareNoteObject> notes;
  std::vector<float> envelopes;
  ReadNotes("mergeNotes", info[4].As<Napi::Array>(), &notes, &envelopes);

  OwnedNoteObjects result;
  const SonareError err = sonare_merge_notes(
      data, length, sr, f0.Data(), options.prob_ptr, options.voiced_ptr, n_frames, frame_rate,
      &options.config, notes.empty() ? nullptr : notes.data(), notes.size(),
      envelopes.empty() ? nullptr : envelopes.data(), envelopes.size(), first, last, &result.value);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  return NoteObjectsToJs(env, "mergeNotes", result.value);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::NoteTargetsFromSmf(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  SONARE_NODE_TRY
  // (bytes, track?)
  const uint8_t* bytes = nullptr;
  size_t len = 0;
  if (info.Length() > 0 && info[0].IsBuffer()) {
    Napi::Buffer<uint8_t> buffer = info[0].As<Napi::Buffer<uint8_t>>();
    bytes = buffer.Data();
    len = buffer.Length();
  } else if (info.Length() > 0 && IsUint8Array(info[0])) {
    Napi::Uint8Array typed = info[0].As<Napi::Uint8Array>();
    bytes = typed.Data();
    len = typed.ByteLength();
  } else {
    Napi::TypeError::New(env, "noteTargetsFromSmf expects a Buffer or Uint8Array")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
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
  if (info.Length() < 3 || !info[0].IsArray() || !info[1].IsNumber() || !info[2].IsArray()) {
    Napi::TypeError::New(
        env, "Expected (notes: object[], sampleRate, targets: object[], options?: object)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  const int sr = node_narrow_int(env, info[1], "sr");

  // Seeded from the library rather than from literals here: 0 is a legal value on
  // both floats, so it cannot spell "unset" and an omitted key has to keep
  // whatever the C ABI reports as its own default.
  SonareNoteTargetAssignConfig config{};
  const SonareError seeded = sonare_note_target_assign_config_default(&config);
  if (seeded != SONARE_OK) {
    ThrowIfError(env, seeded);
    return env.Undefined();
  }
  if (info.Length() > 3 && info[3].IsObject()) {
    Napi::Object opts = info[3].As<Napi::Object>();
    if (!ReadUnmatchedTargetPolicy(env, opts, &config.unmatched_policy)) {
      return env.Undefined();
    }
    // Neither field documents a non-finite spelling, so the finite reader: an
    // infinity is out of domain rather than a request.
    config.min_overlap_ratio =
        FiniteFloatProperty(opts, "minOverlapRatio", config.min_overlap_ratio);
    config.max_correction_semitones =
        FiniteFloatProperty(opts, "maxCorrectionSemitones", config.max_correction_semitones);
  }

  std::vector<SonareNoteObject> notes;
  std::vector<float> envelopes;
  // The sample bounds are what the rule measures each overlap against, so they
  // are required here as they are for a render.
  ReadNotes("assignNoteTargets", info[0].As<Napi::Array>(), &notes, &envelopes,
            /*require_span=*/true);

  std::vector<SonareNoteTarget> targets;
  if (!ReadNoteTargets(env, "assignNoteTargets", info[2].As<Napi::Array>(), &targets)) {
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
    edit.Set("pitchShiftSemitones", Napi::Number::New(env, notes[i].edit.pitch_shift_semitones));
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
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, options?: object)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  const size_t length = typed.ElementLength();
  const int sr = node_narrow_int(env, info[1], "sr");

  SonarePercussiveEventConfig config{};
  ReadPercussiveEventConfig(info[2], &config);

  OwnedPercussiveEvents result;
  const SonareError err =
      sonare_extract_percussive_events(data, length, sr, &config, &result.value);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
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
  if (info.Length() < 3 || !IsFloat32Array(info[0]) || !info[1].IsNumber() || !info[2].IsArray()) {
    Napi::TypeError::New(env,
                         "Expected (Float32Array, sampleRate, events: object[], options?: object)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  SONARE_NODE_TRY
  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  const size_t length = typed.ElementLength();
  const int sr = node_narrow_int(env, info[1], "sr");

  SonarePercussiveRenderConfig config{};
  ReadPercussiveRenderConfig(info[3], &config);

  std::vector<SonarePercussiveEvent> events;
  ReadPercussiveEvents("renderPercussiveEvents", info[2].As<Napi::Array>(), &events);

  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err =
      sonare_render_percussive_events(data, length, sr, events.empty() ? nullptr : events.data(),
                                      events.size(), &config, &out, &out_length);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  auto result = CopyToFloat32(env, out, out_length);
  sonare_free_floats(out);
  return result;
  SONARE_NODE_CATCH(env)
}
