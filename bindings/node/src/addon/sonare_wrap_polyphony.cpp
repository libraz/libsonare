#include "sonare_wrap_polyphony.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "sonare_wrap_note_objects.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {

namespace {

/// Reads the analysis configuration, which may be absent.
///
/// The engine/project-struct reader family rather than the options-bag one: every
/// field here is range-checked downstream -- by the C ABI for the struct version
/// and by the core for each value -- so a wrong-typed value is refused instead of
/// falling back to the default, which would otherwise analyse at a framing the
/// caller never asked for and report success.
///
/// @return false with one pending JS exception; the caller must return before any
///         further N-API call.
bool ReadPolyphonicConfig(Napi::Env env, const Napi::Value& value, SonarePolyphonicConfig* out) {
  out->struct_version = 1;
  if (value.IsUndefined() || value.IsNull()) {
    return true;
  }
  if (!value.IsObject() || value.IsArray()) {
    Napi::TypeError::New(env, "PolyphonicAnalysis: config must be a plain object")
        .ThrowAsJavaScriptException();
    return false;
  }
  Napi::Object opts = value.As<Napi::Object>();

  out->n_fft = IntProperty(opts, "nFft", kZeroIsSentinel);
  out->hop_length = IntProperty(opts, "hopLength", kZeroIsSentinel);
  out->win_length = IntProperty(opts, "winLength", kZeroIsSentinel);

  out->cent_ref_hz = FloatProperty(opts, "centRefHz", 0.0f);
  out->cents_per_bin = FloatProperty(opts, "centsPerBin", 0.0f);
  out->cent_max_hz = FloatProperty(opts, "centMaxHz", 0.0f);
  out->tonality_off = BoolProperty(opts, "tonalityOff", false) ? 1 : 0;

  out->salience_harmonics = IntProperty(opts, "salienceHarmonics", kZeroIsSentinel);
  out->f0_min_hz = FloatProperty(opts, "f0MinHz", 0.0f);
  out->f0_max_hz = FloatProperty(opts, "f0MaxHz", 0.0f);
  out->salience_alpha_hz = FloatProperty(opts, "salienceAlphaHz", 0.0f);
  out->salience_beta_hz = FloatProperty(opts, "salienceBetaHz", 0.0f);
  out->salience_inharmonicity = FloatProperty(opts, "salienceInharmonicity", 0.0f);

  out->max_polyphony = IntProperty(opts, "maxPolyphony", kZeroIsSentinel);
  out->min_frame_peak_ratio = FloatProperty(opts, "minFramePeakRatio", 0.0f);
  out->min_separation_cents = FloatProperty(opts, "minSeparationCents", 0.0f);
  out->subtraction_factor = FloatProperty(opts, "subtractionFactor", 0.0f);

  out->max_jump_cents = FloatProperty(opts, "maxJumpCents", 0.0f);
  out->min_ridge_peak_ratio = FloatProperty(opts, "minRidgePeakRatio", 0.0f);
  out->min_ridge_duration_ms = FloatProperty(opts, "minRidgeDurationMs", 0.0f);

  out->mask_harmonics = IntProperty(opts, "maskHarmonics", kZeroIsSentinel);
  out->claim_lobes = FloatProperty(opts, "claimLobes", 0.0f);
  out->inharmonicity = FloatProperty(opts, "inharmonicity", 0.0f);
  out->estimate_inharmonicity = BoolProperty(opts, "estimateInharmonicity", false) ? 1 : 0;
  out->inharmonicity_min_partials = IntProperty(opts, "inharmonicityMinPartials", kZeroIsSentinel);
  out->inharmonicity_max_residual_bins = FloatProperty(opts, "inharmonicityMaxResidualBins", 0.0f);
  out->inharmonicity_max_stretch = FloatProperty(opts, "inharmonicityMaxStretch", 0.0f);

  out->window_frames = IntProperty(opts, "windowFrames", kZeroIsSentinel);
  out->min_partial_separation = FloatProperty(opts, "minPartialSeparation", 0.0f);
  out->max_fit_residual = FloatProperty(opts, "maxFitResidual", 0.0f);
  out->max_weight_modulus = FloatProperty(opts, "maxWeightModulus", 0.0f);
  out->max_refine_hz = FloatProperty(opts, "maxRefineHz", 0.0f);
  out->f0_tolerance_cents = FloatProperty(opts, "f0ToleranceCents", 0.0f);

  out->segmentation_threshold_cents = FloatProperty(opts, "segmentationThresholdCents", 0.0f);
  out->min_note_ms = FloatProperty(opts, "minNoteMs", 0.0f);
  out->reference_hz = FloatProperty(opts, "referenceHz", 0.0f);
  return !env.IsExceptionPending();
}

}  // namespace

Napi::Object PolyphonicAnalysisWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "PolyphonicAnalysis",
      {
          InstanceMethod<&PolyphonicAnalysisWrap::NoteCount>("noteCount"),
          InstanceMethod<&PolyphonicAnalysisWrap::FrameCount>("frameCount"),
          InstanceMethod<&PolyphonicAnalysisWrap::Notes>("notes"),
          InstanceMethod<&PolyphonicAnalysisWrap::SetNoteEdit>("setNoteEdit"),
          InstanceMethod<&PolyphonicAnalysisWrap::Polyphony>("polyphony"),
          InstanceMethod<&PolyphonicAnalysisWrap::NoteF0>("noteF0"),
          InstanceMethod<&PolyphonicAnalysisWrap::NoteAmplitude>("noteAmplitude"),
          InstanceMethod<&PolyphonicAnalysisWrap::NoteSalience>("noteSalience"),
          InstanceMethod<&PolyphonicAnalysisWrap::NoteEnvelope>("noteEnvelope"),
          InstanceMethod<&PolyphonicAnalysisWrap::NoteInharmonicity>("noteInharmonicity"),
          InstanceMethod<&PolyphonicAnalysisWrap::Render>("render"),
          InstanceMethod<&PolyphonicAnalysisWrap::Destroy>("destroy"),
      });

  exports.Set("PolyphonicAnalysis", func);
  return exports;
}

PolyphonicAnalysisWrap::PolyphonicAnalysisWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<PolyphonicAnalysisWrap>(info) {
  Napi::Env env = info.Env();
  // The config readers report by throwing, and a constructor is as much an entry
  // point as a method: without the harness the throw leaves the N-API callback
  // uncaught and takes the process down instead of reporting a RangeError.
  SONARE_NODE_TRY
  if (info.Length() < 2 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleRate, config?: object)")
        .ThrowAsJavaScriptException();
    return;
  }
  int sample_rate = 0;
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sample_rate)) return;

  SonarePolyphonicConfig config{};
  if (!ReadPolyphonicConfig(env, info[2], &config)) return;

  auto samples = info[0].As<Napi::Float32Array>();
  SonareError err = sonare_polyphonic_analyze(samples.Data(), samples.ElementLength(), sample_rate,
                                              &config, &analysis_);
  if (err != SONARE_OK) {
    // analyze() clears its out-pointer before validating, so a failure leaves
    // nothing to release here.
    ThrowIfError(env, err);
    return;
  }

  // The counts a caller would otherwise have to query before every read. Each
  // note's frame span is the documented length of its three measured curves, and
  // the only settable thing on a note is its edit, so none of this can go stale.
  size_t note_count = 0;
  err = sonare_polyphonic_note_count(analysis_, &note_count);
  if (err == SONARE_OK) err = sonare_polyphonic_frame_count(analysis_, &frame_count_);
  if (err != SONARE_OK) {
    Release();
    ThrowIfError(env, err);
    return;
  }

  std::vector<SonareNoteObject> notes(note_count);
  size_t written = 0;
  err = sonare_polyphonic_notes(analysis_, notes.empty() ? nullptr : notes.data(), notes.size(),
                                &written);
  if (err != SONARE_OK) {
    Release();
    ThrowIfError(env, err);
    return;
  }
  // The capacity is the count just reported, so a short write would leave the
  // span cache addressing fewer notes than the handle holds -- and it is indexed
  // by a note index the C ABI validated against the handle, not against it.
  if (written != note_count) {
    Release();
    Napi::Error::New(env, "PolyphonicAnalysis: the analysis reported a different note count")
        .ThrowAsJavaScriptException();
    return;
  }
  spans_.reserve(written);
  for (size_t i = 0; i < written; ++i) {
    spans_.push_back(static_cast<size_t>(std::max(0, notes[i].frame_end - notes[i].frame_start)));
  }
  SONARE_NODE_CATCH_VOID(env)
}

PolyphonicAnalysisWrap::~PolyphonicAnalysisWrap() { Release(); }

void PolyphonicAnalysisWrap::Release() {
  if (analysis_ != nullptr) {
    sonare_polyphonic_analysis_destroy(analysis_);
    analysis_ = nullptr;
  }
}

bool PolyphonicAnalysisWrap::RequireOpen(Napi::Env env) {
  if (analysis_ == nullptr) {
    Napi::Error::New(env, "PolyphonicAnalysis has been destroyed").ThrowAsJavaScriptException();
    return false;
  }
  return true;
}

Napi::Value PolyphonicAnalysisWrap::NoteCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireOpen(env)) return env.Undefined();
  return Napi::Number::New(env, static_cast<double>(spans_.size()));
  SONARE_NODE_CATCH(env)
}

Napi::Value PolyphonicAnalysisWrap::FrameCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireOpen(env)) return env.Undefined();
  return Napi::Number::New(env, frame_count_);
  SONARE_NODE_CATCH(env)
}

Napi::Value PolyphonicAnalysisWrap::Notes(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireOpen(env)) return env.Undefined();

  std::vector<SonareNoteObject> notes(spans_.size());
  size_t written = 0;
  SonareError err = sonare_polyphonic_notes(analysis_, notes.empty() ? nullptr : notes.data(),
                                            notes.size(), &written);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  // The capacity is the count this handle reported at construction, so a short
  // write is an inconsistency rather than a buffer the caller sized badly.
  if (written != notes.size()) {
    Napi::Error::New(env, "notes: the analysis reported a different note count")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Array out = Napi::Array::New(env, written);
  std::vector<float> amplitude;
  std::vector<float> envelope;
  for (size_t i = 0; i < written; ++i) {
    const size_t span = spans_[i];
    amplitude.assign(span, 0.0f);
    size_t amplitude_count = 0;
    err = sonare_polyphonic_note_amplitude(
        analysis_, i, amplitude.empty() ? nullptr : amplitude.data(), span, &amplitude_count);
    if (err != SONARE_OK) {
      ThrowIfError(env, err);
      return env.Undefined();
    }
    // The note reports its own point count, and the points are indexed from 0
    // rather than over the span, so that count is the capacity they need.
    envelope.assign(notes[i].edit.envelope_count, 0.0f);
    size_t envelope_count = 0;
    err =
        sonare_polyphonic_note_envelope(analysis_, i, envelope.empty() ? nullptr : envelope.data(),
                                        envelope.size(), &envelope_count);
    if (err != SONARE_OK) {
      ThrowIfError(env, err);
      return env.Undefined();
    }
    out.Set(static_cast<uint32_t>(i),
            NoteObjectToJs(env, notes[i], amplitude.data(), amplitude_count, envelope.data(),
                           envelope_count));
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value PolyphonicAnalysisWrap::SetNoteEdit(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireOpen(env)) return env.Undefined();

  size_t note = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "note", &note)) return env.Undefined();

  SONARE_NODE_TRY
  SonareNoteEdit edit{};
  std::vector<float> envelope;
  const Napi::Value edit_value = info[1];
  if (!edit_value.IsUndefined() && !edit_value.IsNull()) {
    if (!edit_value.IsObject() || edit_value.IsArray()) {
      throw std::runtime_error("setNoteEdit: edit must be a plain object");
    }
    ReadNoteEditFields(edit_value.As<Napi::Object>(), &envelope, &edit);
  }

  // The edit's envelope_offset and envelope_count address the by-value door's
  // pool and are ignored here; the points travel as their own argument and the
  // handle copies them.
  const SonareError err = sonare_polyphonic_set_note_edit(
      analysis_, note, &edit, envelope.empty() ? nullptr : envelope.data(), envelope.size());
  ThrowIfError(env, err);
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value PolyphonicAnalysisWrap::Polyphony(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireOpen(env)) return env.Undefined();

  std::vector<int32_t> counts(frame_count_ > 0 ? static_cast<size_t>(frame_count_) : 0, 0);
  size_t written = 0;
  const SonareError err = sonare_polyphonic_polyphony(
      analysis_, counts.empty() ? nullptr : counts.data(), counts.size(), &written);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  auto out = Napi::Int32Array::New(env, written);
  if (written > 0) {
    std::memcpy(out.Data(), counts.data(), written * sizeof(int32_t));
  }
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value PolyphonicAnalysisWrap::Curve(const Napi::CallbackInfo& info, CurveReader reader) {
  Napi::Env env = info.Env();
  if (!RequireOpen(env)) return env.Undefined();

  size_t note = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "note", &note)) return env.Undefined();

  // An index past the last note is given a zero capacity and refused by the C
  // ABI, so the out-of-range rejection stays the C ABI's own error code.
  const size_t capacity = note < spans_.size() ? spans_[note] : 0;
  std::vector<float> values(capacity, 0.0f);
  size_t written = 0;
  const SonareError err =
      reader(analysis_, note, values.empty() ? nullptr : values.data(), capacity, &written);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  return CopyToFloat32(env, values.data(), written);
}

Napi::Value PolyphonicAnalysisWrap::NoteF0(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return Curve(info, &sonare_polyphonic_note_f0);
  SONARE_NODE_CATCH(env)
}

Napi::Value PolyphonicAnalysisWrap::NoteAmplitude(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return Curve(info, &sonare_polyphonic_note_amplitude);
  SONARE_NODE_CATCH(env)
}

Napi::Value PolyphonicAnalysisWrap::NoteSalience(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return Curve(info, &sonare_polyphonic_note_salience);
  SONARE_NODE_CATCH(env)
}

bool PolyphonicAnalysisWrap::ReadEnvelopeCount(Napi::Env env, size_t note, size_t* out) {
  *out = 0;
  if (note >= spans_.size()) return true;

  std::vector<SonareNoteObject> notes(spans_.size());
  size_t written = 0;
  const SonareError err = sonare_polyphonic_notes(analysis_, notes.data(), notes.size(), &written);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return false;
  }
  if (note < written) *out = notes[note].edit.envelope_count;
  return true;
}

Napi::Value PolyphonicAnalysisWrap::NoteEnvelope(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireOpen(env)) return env.Undefined();

  size_t note = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "note", &note)) return env.Undefined();

  // Not the frame span the three measured curves are sized by: an envelope is a
  // set of gain points stretched over whatever length the note renders at, so its
  // length is the note's own envelope count and nothing to do with its span.
  size_t capacity = 0;
  if (!ReadEnvelopeCount(env, note, &capacity)) return env.Undefined();
  std::vector<float> points(capacity, 0.0f);
  size_t written = 0;
  const SonareError err = sonare_polyphonic_note_envelope(
      analysis_, note, points.empty() ? nullptr : points.data(), capacity, &written);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  return CopyToFloat32(env, points.data(), written);
  SONARE_NODE_CATCH(env)
}

Napi::Value PolyphonicAnalysisWrap::NoteInharmonicity(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireOpen(env)) return env.Undefined();

  // One entry per note, so the note count is the capacity. A write of zero is
  // the documented answer for an analysis that never asked for the fit, not a
  // buffer this sized too small, so it is marshalled as the empty array.
  std::vector<float> stretches(spans_.size(), 0.0f);
  size_t written = 0;
  const SonareError err = sonare_polyphonic_note_inharmonicity(
      analysis_, stretches.empty() ? nullptr : stretches.data(), stretches.size(), &written);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  return CopyToFloat32(env, stretches.data(), written);
  SONARE_NODE_CATCH(env)
}

Napi::Value PolyphonicAnalysisWrap::Render(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireOpen(env)) return env.Undefined();

  SonareNoteRenderConfig config{};
  config.struct_version = 1;
  const Napi::Value options = info[0];
  if (!options.IsUndefined() && !options.IsNull()) {
    if (!options.IsObject() || options.IsArray()) {
      Napi::TypeError::New(env, "render: options must be a plain object")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Object opts = options.As<Napi::Object>();
    config.fade_ms = FloatProperty(opts, "fadeMs", 0.0f);
    config.vibrato_cutoff_hz = FloatProperty(opts, "vibratoCutoffHz", 0.0f);
    if (env.IsExceptionPending()) return env.Undefined();
  }

  float* out = nullptr;
  size_t out_length = 0;
  const SonareError err = sonare_polyphonic_render(analysis_, &config, &out, &out_length);
  if (err != SONARE_OK) {
    ThrowIfError(env, err);
    return env.Undefined();
  }
  auto result = CopyToFloat32(env, out, out_length);
  sonare_free_floats(out);
  return result;
  SONARE_NODE_CATCH(env)
}

void PolyphonicAnalysisWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY Release();
  SONARE_NODE_CATCH_VOID(env)
}

}  // namespace sonare_node
