#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "analysis/chord_analyzer.h"
#include "analysis/key_analyzer.h"
#include "c_api/sonare_c_internal.h"
#include "core/audio.h"
#include "editing/voice_changer/realtime.h"
#include "mastering/api/chain.h"
#include "mastering/eq/eq_band.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/spectrum_engine.h"
#include "mastering/match/match_eq.h"
#include "mastering/match/reference_spectrum.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_streaming.h"
#include "sonare_wrap_utils.h"
#include "util/numeric_validation.h"

namespace sonare_node {

namespace {

// The core spells the mel range fmin / fmax; the facade keys are fminHz / fmaxHz.
const std::array<sonare_c_detail::CName, 2> kMelRangeNames{
    {{"fmin", "fmin_hz"}, {"fmax", "fmax_hz"}}};

// Reads an optional quantization-range config from a JS value (null/undefined or
// a non-object yields the library defaults). Each field defaults to the
// corresponding sonare::QuantizeConfig default, so a partial object only
// overrides the ranges the caller supplied.
sonare::QuantizeConfig QuantizeConfigFromValue(const Napi::Value& value) {
  sonare::QuantizeConfig qconfig;
  if (!value.IsObject()) return qconfig;
  Napi::Object object = value.As<Napi::Object>();
  // Plain quantities, none documented as reading a wrong-typed value as
  // "unspecified" (types_features.ts's StreamQuantizeConfig only documents
  // omission, not type), so FloatProperty, which refuses a wrong type by name.
  qconfig.mel_db_min = FloatProperty(object, "melDbMin", qconfig.mel_db_min);
  qconfig.mel_db_max = FloatProperty(object, "melDbMax", qconfig.mel_db_max);
  qconfig.onset_max = FloatProperty(object, "onsetMax", qconfig.onset_max);
  qconfig.rms_max = FloatProperty(object, "rmsMax", qconfig.rms_max);
  qconfig.centroid_max = FloatProperty(object, "centroidMax", qconfig.centroid_max);
  return qconfig;
}

template <typename Array, typename Value>
Array TypedArrayFromVec(Napi::Env env, const std::vector<Value>& vec) {
  Array out = Array::New(env, vec.size());
  if constexpr (std::is_same_v<Value, int>) {
    for (size_t i = 0; i < vec.size(); ++i) {
      out[i] = vec[i];
    }
  } else if (!vec.empty()) {
    std::memcpy(sonare_node::ElementData(out), vec.data(), vec.size() * sizeof(Value));
  }
  return out;
}

Napi::Int32Array Int32FromVec(Napi::Env env, const std::vector<int>& vec) {
  return TypedArrayFromVec<Napi::Int32Array>(env, vec);
}

template <typename Buffer>
void SetFrameBufferMetadata(Napi::Env env, Napi::Object& out, const Buffer& buffer) {
  out.Set("nFrames", Napi::Number::New(env, static_cast<double>(buffer.n_frames)));
  out.Set("nMels", Napi::Number::New(env, buffer.n_mels));
  out.Set("nChroma", Napi::Number::New(env, buffer.n_chroma));
  out.Set("featureFlags", Napi::Number::New(env, buffer.feature_flags));
}

Napi::Object FrameBufferToObject(Napi::Env env, const sonare::FrameBuffer& buffer) {
  Napi::Object out = Napi::Object::New(env);
  SetFrameBufferMetadata(env, out, buffer);
  out.Set("timestampsSec", Float32ArrayFrom(env, buffer.timestamps));
  out.Set("mel", Float32ArrayFrom(env, buffer.mel));
  out.Set("chroma", Float32ArrayFrom(env, buffer.chroma));
  out.Set("onsetStrength", Float32ArrayFrom(env, buffer.onset_strength));
  out.Set("rmsEnergy", Float32ArrayFrom(env, buffer.rms_energy));
  out.Set("spectralCentroid", Float32ArrayFrom(env, buffer.spectral_centroid));
  out.Set("spectralFlatness", Float32ArrayFrom(env, buffer.spectral_flatness));
  out.Set("chordRoot", Int32FromVec(env, buffer.chord_root));
  out.Set("chordQuality", Int32FromVec(env, buffer.chord_quality));
  out.Set("chordConfidence", Float32ArrayFrom(env, buffer.chord_confidence));
  return out;
}

template <typename Array, typename Buffer>
Napi::Object QuantizedFrameBufferToObject(Napi::Env env, const Buffer& buffer) {
  Napi::Object out = Napi::Object::New(env);
  SetFrameBufferMetadata(env, out, buffer);
  out.Set("timestampsSec", Float32ArrayFrom(env, buffer.timestamps));
  out.Set("mel", TypedArrayFromVec<Array>(env, buffer.mel));
  out.Set("chroma", TypedArrayFromVec<Array>(env, buffer.chroma));
  out.Set("onsetStrength", TypedArrayFromVec<Array>(env, buffer.onset_strength));
  out.Set("rmsEnergy", TypedArrayFromVec<Array>(env, buffer.rms_energy));
  out.Set("spectralCentroid", TypedArrayFromVec<Array>(env, buffer.spectral_centroid));
  out.Set("spectralFlatness", TypedArrayFromVec<Array>(env, buffer.spectral_flatness));
  out.Set("chordRoot", TypedArrayFromVec<Napi::Int8Array>(env, buffer.chord_root));
  out.Set("chordQuality", TypedArrayFromVec<Napi::Int8Array>(env, buffer.chord_quality));
  out.Set("chordConfidence", TypedArrayFromVec<Array>(env, buffer.chord_confidence));
  return out;
}

// Narrows an already-read config value to a native int, rejecting anything the
// target type cannot hold exactly. Takes the value rather than (object, key) so
// the key stays visible at the call site on the shared DoubleProperty
// reader, which is what the option scanner counts.
int CheckedConfigInt(double value, const char* key) {
  int narrowed = 0;
  if (!sonare::numeric::checked_integral_cast(value, &narrowed)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  std::string(key) + " must be an integer within the int range");
  }
  return narrowed;
}

bool ParseWindowOption(const Napi::Object& options, Napi::Env env, sonare::WindowType* out) {
  const Napi::Value value = options.Get("window");
  if (value.IsUndefined()) return true;

  int ordinal = 0;
  constexpr int kWindowCount = SONARE_WINDOW_RECTANGULAR + 1;
  if (!value.IsNumber() ||
      !sonare::numeric::checked_integral_cast(value.As<Napi::Number>().DoubleValue(), &ordinal) ||
      ordinal < 0 || ordinal >= kWindowCount) {
    Napi::RangeError::New(
        env, "window must be a name or an ordinal in [0, " + std::to_string(kWindowCount) + ")")
        .ThrowAsJavaScriptException();
    return false;
  }

  switch (ordinal) {
    case 0:
      *out = sonare::WindowType::Hann;
      break;
    case 1:
      *out = sonare::WindowType::Hamming;
      break;
    case 2:
      *out = sonare::WindowType::Blackman;
      break;
    case 3:
      *out = sonare::WindowType::Rectangular;
      break;
  }
  return true;
}

// A stream root of -1 is the core's "unknown"; any other value outside the twelve has no name.
int StreamPitchClass(Napi::Env env, int root) {
  if (root < -1 || root > static_cast<int>(sonare::PitchClass::B)) {
    throw MakeSonareError(env, SONARE_ERROR_INVALID_FORMAT,
                          "unknown pitch class ordinal: " + std::to_string(root));
  }
  return root;
}

// The symbol of a stream chord the core wrote, refusing a root or quality it cannot name; a
// root of -1 is no chord, whose quality the core may leave at -1.
Napi::String StreamChordName(Napi::Env env, int root, int quality) {
  if (StreamPitchClass(env, root) >= 0 && (quality < 0 || quality >= sonare::kChordQualityCount)) {
    throw MakeSonareError(env, SONARE_ERROR_INVALID_FORMAT,
                          "unknown chord quality ordinal: " + std::to_string(quality));
  }
  return Napi::String::New(env, sonare::chord_symbol(root, quality));
}

}  // namespace

Napi::Object StreamAnalyzerWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "StreamAnalyzer",
      {
          InstanceMethod<&StreamAnalyzerWrap::Process>("process", napi_default_method),
          InstanceMethod<&StreamAnalyzerWrap::FinalizeStream>("finalize", napi_default_method),
          InstanceMethod<&StreamAnalyzerWrap::AvailableFrames>("availableFrames",
                                                               napi_default_method),
          InstanceMethod<&StreamAnalyzerWrap::ReadFramesSoa>("readFramesSoa", napi_default_method),
          InstanceMethod<&StreamAnalyzerWrap::ReadFramesU8>("readFramesU8", napi_default_method),
          InstanceMethod<&StreamAnalyzerWrap::ReadFramesI16>("readFramesI16", napi_default_method),
          InstanceMethod<&StreamAnalyzerWrap::Reset>("reset", napi_default_method),
          InstanceMethod<&StreamAnalyzerWrap::Stats>("stats", napi_default_method),
          InstanceMethod<&StreamAnalyzerWrap::FrameCount>("frameCount", napi_default_method),
          InstanceMethod<&StreamAnalyzerWrap::CurrentTimeSec>("currentTimeSec",
                                                              napi_default_method),
          InstanceMethod<&StreamAnalyzerWrap::SampleRate>("sampleRate", napi_default_method),
          InstanceMethod<&StreamAnalyzerWrap::SetExpectedDuration>("setExpectedDuration",
                                                                   napi_default_method),
          InstanceMethod<&StreamAnalyzerWrap::SetNormalizationGain>("setNormalizationGain",
                                                                    napi_default_method),
          InstanceMethod<&StreamAnalyzerWrap::SetReferenceHz>("setReferenceHz",
                                                              napi_default_method),
          InstanceMethod<&StreamAnalyzerWrap::Destroy>("destroy", napi_default_method),
      });

  exports.Set("StreamAnalyzer", func);
  return exports;
}

StreamAnalyzerWrap::StreamAnalyzerWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<StreamAnalyzerWrap>(info) {
  Napi::Env env = info.Env();

  try {
    sonare::StreamConfig config;
    if (info.Length() >= 1 && info[0].IsObject()) {
      Napi::Object opts = info[0].As<Napi::Object>();
      // Every field narrowed to a native int goes through checked_integral_cast:
      // an unchecked static_cast of an out-of-int-range double is undefined
      // behaviour, and on ARM64 it saturates to INT_MAX, which slips past the
      // core's positivity checks instead of being rejected here.
      config.sample_rate = DomainProperty<int>(opts, "sampleRate", config.sample_rate,
                                               sonare::kAudioSampleRateDomain);
      config.n_fft = CheckedConfigInt(DoubleProperty(opts, "nFft", config.n_fft), "nFft");
      config.hop_length =
          CheckedConfigInt(DoubleProperty(opts, "hopLength", config.hop_length), "hopLength");
      // Every field from here down is a plain quantity or flag, none documented
      // (types_features.ts's StreamAnalyzerConfig) as reading a wrong-typed
      // value as "unspecified" -- StreamAnalyzerConfig's own doc comment
      // requires the count fields to be refused rather than truncated, which
      // DoubleProperty/BoolProperty give by refusing a wrong TYPE by name before
      // CheckedConfigInt sees it.
      config.n_mels = CheckedConfigInt(DoubleProperty(opts, "nMels", config.n_mels), "nMels");
      config.fmin = FloatProperty(opts, "fmin", config.fmin);
      config.fmax = FloatProperty(opts, "fmax", config.fmax);
      config.tuning_ref_hz = FloatProperty(opts, "referenceHz", config.tuning_ref_hz);
      config.compute_magnitude = BoolProperty(opts, "computeMagnitude", config.compute_magnitude);
      config.compute_mel = BoolProperty(opts, "computeMel", config.compute_mel);
      config.compute_chroma = BoolProperty(opts, "computeChroma", config.compute_chroma);
      config.compute_onset = BoolProperty(opts, "computeOnset", config.compute_onset);
      config.compute_spectral = BoolProperty(opts, "computeSpectral", config.compute_spectral);
      config.emit_every_n_frames = CheckedConfigInt(
          DoubleProperty(opts, "emitEveryNFrames", config.emit_every_n_frames), "emitEveryNFrames");
      config.max_pending_frames =
          DomainProperty<size_t>(opts, "maxPendingFrames", config.max_pending_frames,
                                 sonare::kStreamConfigMaxPendingFramesDomain);
      config.max_progression_entries =
          DomainProperty<size_t>(opts, "maxProgressionEntries", config.max_progression_entries,
                                 sonare::kStreamConfigMaxProgressionEntriesDomain);
      config.key_update_interval_sec =
          FloatProperty(opts, "keyUpdateIntervalSec", config.key_update_interval_sec);
      config.bpm_update_interval_sec =
          FloatProperty(opts, "bpmUpdateIntervalSec", config.bpm_update_interval_sec);
      if (!ParseWindowOption(opts, env, &config.window)) return;
      const Napi::Value output_format_value = opts.Get("outputFormat");
      const double output_format_number = output_format_value.IsNumber()
                                              ? output_format_value.As<Napi::Number>().DoubleValue()
                                              : 0.0;
      if (!output_format_value.IsUndefined() && !output_format_value.IsNull() &&
          (!output_format_value.IsNumber() || !std::isfinite(output_format_number) ||
           output_format_number != 0.0)) {
        throw sonare::SonareException(
            sonare::ErrorCode::InvalidParameter,
            "outputFormat must be the integer 0 (Float32); use an explicit "
            "quantized read method");
      }
      config.output_format = sonare::OutputFormat::Float32;
    }

    // The shared facade check, not a copy of one: this surface builds
    // sonare::StreamAnalyzer directly, so it inherits nothing from the C ABI,
    // and computeMagnitude was accepted here while the C ABI, WASM and Python
    // all refused it -- the same options object valid on one surface and an
    // InvalidParameter on the other three.
    sonare::validate_soa_stream_config(config);
    config_ = config;
    // The core refuses its mel range under its own member names; the facade keys carry the unit.
    sonare_c_detail::with_c_names(
        kMelRangeNames, [&] { analyzer_ = std::make_unique<sonare::StreamAnalyzer>(config_); });
  } catch (...) {
    sonare_node::ThrowCurrentException(env);
    return;
  }
}

StreamAnalyzerWrap::~StreamAnalyzerWrap() = default;

// Releases the native analyzer up front rather than at GC. The class doc
// already documents destroy() as requiring both the producer and consumer roles
// to be stopped; every method guards on a null analyzer_, so a call afterwards
// throws instead of touching freed state.
Napi::Value StreamAnalyzerWrap::Destroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  analyzer_.reset();
  return info.Env().Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::Process(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, analyzer_, "StreamAnalyzer");
  if (!IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  // An absent, undefined or null startSample tracks the cumulative count internally.
  const bool has_offset = !info[1].IsUndefined() && !info[1].IsNull();
  size_t offset = 0;
  if (has_offset && !NonNegativeSizeTArg(env, info, 1, "startSample", &offset)) {
    return env.Undefined();
  }
  Napi::Float32Array typed = info[0].As<Napi::Float32Array>();
  if (has_offset) {
    analyzer_->process(sonare_node::ElementData(typed), typed.ElementLength(), offset);
  } else {
    analyzer_->process(sonare_node::ElementData(typed), typed.ElementLength());
  }
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::FinalizeStream(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, analyzer_, "StreamAnalyzer");
  SONARE_NODE_TRY
  analyzer_->finalize();
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::AvailableFrames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, analyzer_, "StreamAnalyzer");
  return Napi::Number::New(env, static_cast<double>(analyzer_->available_frames()));
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::ReadFramesSoa(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, analyzer_, "StreamAnalyzer");
  size_t max_frames = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "maxFrames", &max_frames)) return env.Undefined();
  sonare::FrameBuffer buffer;
  analyzer_->read_frames_soa(max_frames, buffer);
  return FrameBufferToObject(env, buffer);
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::ReadFramesU8(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, analyzer_, "StreamAnalyzer");
  size_t max_frames = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "maxFrames", &max_frames)) return env.Undefined();
  sonare::QuantizedFrameBufferU8 buffer;
  sonare::QuantizeConfig qconfig =
      QuantizeConfigFromValue(info.Length() > 1 ? info[1] : env.Null());
  analyzer_->read_frames_quantized_u8(max_frames, buffer, qconfig);
  return QuantizedFrameBufferToObject<Napi::Uint8Array>(env, buffer);
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::ReadFramesI16(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, analyzer_, "StreamAnalyzer");
  size_t max_frames = 0;
  if (!NonNegativeSizeTArg(env, info, 0, "maxFrames", &max_frames)) return env.Undefined();
  sonare::QuantizedFrameBufferI16 buffer;
  sonare::QuantizeConfig qconfig =
      QuantizeConfigFromValue(info.Length() > 1 ? info[1] : env.Null());
  analyzer_->read_frames_quantized_i16(max_frames, buffer, qconfig);
  return QuantizedFrameBufferToObject<Napi::Int16Array>(env, buffer);
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::Reset(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, analyzer_, "StreamAnalyzer");
  size_t base_offset = 0;
  if (info.Length() >= 1 && !info[0].IsUndefined() &&
      !NonNegativeSizeTArg(env, info, 0, "startSample", &base_offset)) {
    return env.Undefined();
  }
  analyzer_->reset(base_offset);
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::Stats(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, analyzer_, "StreamAnalyzer");
  SONARE_NODE_TRY
  sonare::AnalyzerStats s = analyzer_->stats();

  Napi::Object out = Napi::Object::New(env);
  out.Set("totalFrames", Napi::Number::New(env, s.total_frames));
  out.Set("totalSamples", Napi::Number::New(env, static_cast<double>(s.total_samples)));
  out.Set("durationSec", Napi::Number::New(env, s.duration_seconds));
  out.Set("pendingFrames", Napi::Number::New(env, static_cast<double>(s.pending_frames)));
  out.Set("droppedOutputFrames",
          Napi::Number::New(env, static_cast<double>(s.dropped_output_frames)));
  out.Set("droppedChordProgressionEntries",
          Napi::Number::New(env, static_cast<double>(s.dropped_chord_progression_entries)));
  out.Set("droppedBarProgressionEntries",
          Napi::Number::New(env, static_cast<double>(s.dropped_bar_progression_entries)));
  out.Set("nonFiniteDiscardBlocks",
          Napi::Number::New(env, static_cast<double>(s.non_finite_discard_blocks)));

  const sonare::ProgressiveEstimate& est = s.estimate;
  Napi::Object estimate = Napi::Object::New(env);
  estimate.Set("bpm", Napi::Number::New(env, est.bpm));
  estimate.Set("bpmConfidence", Napi::Number::New(env, est.bpm_confidence));
  estimate.Set("bpmCandidateCount", Napi::Number::New(env, est.bpm_candidate_count));
  estimate.Set("key", Napi::Number::New(env, est.key));
  estimate.Set("keyMinor", Napi::Boolean::New(env, est.key_minor));
  estimate.Set("keyConfidence", Napi::Number::New(env, est.key_confidence));
  if (StreamPitchClass(env, est.key) >= 0) {
    estimate.Set("keyName",
                 Napi::String::New(env, sonare::key_name(est.key, est.key_minor, false)));
    estimate.Set("keyShortName",
                 Napi::String::New(env, sonare::key_name(est.key, est.key_minor, true)));
  } else {
    estimate.Set("keyName", env.Null());
    estimate.Set("keyShortName", env.Null());
  }
  estimate.Set("chordName", StreamChordName(env, est.chord_root, est.chord_quality));
  estimate.Set("chordRoot", Napi::Number::New(env, est.chord_root));
  estimate.Set("chordQuality", Napi::Number::New(env, est.chord_quality));
  estimate.Set("chordConfidence", Napi::Number::New(env, est.chord_confidence));
  estimate.Set("chordStartSec", Napi::Number::New(env, est.chord_start_time));

  Napi::Array chordProgression = Napi::Array::New(env, est.chord_progression.size());
  for (size_t i = 0; i < est.chord_progression.size(); ++i) {
    const sonare::ChordChange& chord = est.chord_progression[i];
    Napi::Object c = Napi::Object::New(env);
    c.Set("root", Napi::Number::New(env, chord.root));
    c.Set("quality", Napi::Number::New(env, chord.quality));
    c.Set("startSec", Napi::Number::New(env, chord.start_time));
    c.Set("confidence", Napi::Number::New(env, chord.confidence));
    c.Set("name", StreamChordName(env, chord.root, chord.quality));
    chordProgression.Set(static_cast<uint32_t>(i), c);
  }
  estimate.Set("chordProgression", chordProgression);

  Napi::Array barChordProgression = Napi::Array::New(env, est.bar_chord_progression.size());
  for (size_t i = 0; i < est.bar_chord_progression.size(); ++i) {
    const sonare::BarChord& chord = est.bar_chord_progression[i];
    Napi::Object c = Napi::Object::New(env);
    c.Set("barIndex", Napi::Number::New(env, chord.bar_index));
    c.Set("root", Napi::Number::New(env, chord.root));
    c.Set("quality", Napi::Number::New(env, chord.quality));
    c.Set("startSec", Napi::Number::New(env, chord.start_time));
    c.Set("confidence", Napi::Number::New(env, chord.confidence));
    c.Set("name", StreamChordName(env, chord.root, chord.quality));
    barChordProgression.Set(static_cast<uint32_t>(i), c);
  }
  estimate.Set("barChordProgression", barChordProgression);
  estimate.Set("currentBar", Napi::Number::New(env, est.current_bar));
  estimate.Set("barDurationSec", Napi::Number::New(env, est.bar_duration));

  Napi::Array votedPattern = Napi::Array::New(env, est.voted_pattern.size());
  for (size_t i = 0; i < est.voted_pattern.size(); ++i) {
    const sonare::BarChord& chord = est.voted_pattern[i];
    Napi::Object c = Napi::Object::New(env);
    c.Set("barIndex", Napi::Number::New(env, chord.bar_index));
    c.Set("root", Napi::Number::New(env, chord.root));
    c.Set("quality", Napi::Number::New(env, chord.quality));
    c.Set("startSec", Napi::Number::New(env, chord.start_time));
    c.Set("confidence", Napi::Number::New(env, chord.confidence));
    c.Set("name", StreamChordName(env, chord.root, chord.quality));
    votedPattern.Set(static_cast<uint32_t>(i), c);
  }
  estimate.Set("votedPattern", votedPattern);
  estimate.Set("patternLength", Napi::Number::New(env, est.pattern_length));

  estimate.Set("detectedPatternName", Napi::String::New(env, est.detected_pattern_name));
  estimate.Set("detectedPatternScore", Napi::Number::New(env, est.detected_pattern_score));

  Napi::Array allPatternScores = Napi::Array::New(env, est.all_pattern_scores.size());
  for (size_t i = 0; i < est.all_pattern_scores.size(); ++i) {
    Napi::Object ps = Napi::Object::New(env);
    ps.Set("name", Napi::String::New(env, est.all_pattern_scores[i].first));
    ps.Set("score", Napi::Number::New(env, est.all_pattern_scores[i].second));
    allPatternScores.Set(static_cast<uint32_t>(i), ps);
  }
  estimate.Set("allPatternScores", allPatternScores);

  estimate.Set("accumulatedSec", Napi::Number::New(env, est.accumulated_seconds));
  estimate.Set("usedFrames", Napi::Number::New(env, est.used_frames));
  estimate.Set("updated", Napi::Boolean::New(env, est.updated));
  out.Set("estimate", estimate);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::FrameCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, analyzer_, "StreamAnalyzer");
  return Napi::Number::New(env, analyzer_->frame_count());
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::CurrentTimeSec(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  SONARE_NODE_REQUIRE_OPEN(env, analyzer_, "StreamAnalyzer");
  return Napi::Number::New(env, analyzer_->current_time());
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::SampleRate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  return Napi::Number::New(env, config_.sample_rate);
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::SetExpectedDuration(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, analyzer_, "StreamAnalyzer");
  if (!info[0].IsNumber()) {
    Napi::TypeError::New(env, "durationSec must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  analyzer_->set_expected_duration(node_narrow_float(env, info[0], "durationSec"));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::SetNormalizationGain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, analyzer_, "StreamAnalyzer");
  if (!info[0].IsNumber()) {
    Napi::TypeError::New(env, "gain must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  analyzer_->set_normalization_gain(node_narrow_float(env, info[0], "gain"));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::SetReferenceHz(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_REQUIRE_OPEN(env, analyzer_, "StreamAnalyzer");
  if (!info[0].IsNumber()) {
    Napi::TypeError::New(env, "referenceHz must be a number").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  analyzer_->set_tuning_ref_hz(node_narrow_float(env, info[0], "referenceHz"));
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

}  // namespace sonare_node
