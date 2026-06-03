#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/audio.h"
#include "editing/voice_changer/realtime.h"
#include "mastering/api/chain.h"
#include "mastering/eq/eq_band.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/spectrum_engine.h"
#include "mastering/match/match_eq.h"
#include "mastering/match/reference_spectrum.h"
#include "sonare_wrap_streaming.h"
#include "sonare_wrap_utils.h"

namespace sonare_node {

namespace {

bool HasKey(const Napi::Object& object, const char* key) {
  return object.Has(key) && !object.Get(key).IsUndefined() && !object.Get(key).IsNull();
}

double NumberKey(const Napi::Object& object, const char* key, double fallback) {
  if (!HasKey(object, key)) return fallback;
  Napi::Value value = object.Get(key);
  if (!value.IsNumber()) return fallback;
  return value.As<Napi::Number>().DoubleValue();
}

bool BoolKey(const Napi::Object& object, const char* key, bool fallback) {
  if (!HasKey(object, key)) return fallback;
  Napi::Value value = object.Get(key);
  if (!value.IsBoolean()) return fallback;
  return value.As<Napi::Boolean>().Value();
}

Napi::Float32Array Float32FromVec(Napi::Env env, const std::vector<float>& vec) {
  Napi::Float32Array out = Napi::Float32Array::New(env, vec.size());
  if (!vec.empty()) {
    std::memcpy(out.Data(), vec.data(), vec.size() * sizeof(float));
  }
  return out;
}

Napi::Int32Array Int32FromVec(Napi::Env env, const std::vector<int>& vec) {
  Napi::Int32Array out = Napi::Int32Array::New(env, vec.size());
  for (size_t i = 0; i < vec.size(); ++i) {
    out[i] = vec[i];
  }
  return out;
}

Napi::Uint8Array Uint8FromVec(Napi::Env env, const std::vector<uint8_t>& vec) {
  Napi::Uint8Array out = Napi::Uint8Array::New(env, vec.size());
  if (!vec.empty()) {
    std::memcpy(out.Data(), vec.data(), vec.size() * sizeof(uint8_t));
  }
  return out;
}

Napi::Int16Array Int16FromVec(Napi::Env env, const std::vector<int16_t>& vec) {
  Napi::Int16Array out = Napi::Int16Array::New(env, vec.size());
  if (!vec.empty()) {
    std::memcpy(out.Data(), vec.data(), vec.size() * sizeof(int16_t));
  }
  return out;
}

}  // namespace

Napi::FunctionReference StreamAnalyzerWrap::constructor_;

Napi::Object StreamAnalyzerWrap::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(
      env, "StreamAnalyzer",
      {
          InstanceMethod<&StreamAnalyzerWrap::Process>("process"),
          InstanceMethod<&StreamAnalyzerWrap::ProcessWithOffset>("processWithOffset"),
          InstanceMethod<&StreamAnalyzerWrap::AvailableFrames>("availableFrames"),
          InstanceMethod<&StreamAnalyzerWrap::ReadFramesSoa>("readFramesSoa"),
          InstanceMethod<&StreamAnalyzerWrap::ReadFramesU8>("readFramesU8"),
          InstanceMethod<&StreamAnalyzerWrap::ReadFramesI16>("readFramesI16"),
          InstanceMethod<&StreamAnalyzerWrap::Reset>("reset"),
          InstanceMethod<&StreamAnalyzerWrap::Stats>("stats"),
          InstanceMethod<&StreamAnalyzerWrap::FrameCount>("frameCount"),
          InstanceMethod<&StreamAnalyzerWrap::CurrentTime>("currentTime"),
          InstanceMethod<&StreamAnalyzerWrap::SampleRate>("sampleRate"),
          InstanceMethod<&StreamAnalyzerWrap::SetExpectedDuration>("setExpectedDuration"),
          InstanceMethod<&StreamAnalyzerWrap::SetNormalizationGain>("setNormalizationGain"),
          InstanceMethod<&StreamAnalyzerWrap::SetTuningRefHz>("setTuningRefHz"),
      });

  constructor_ = Napi::Persistent(func);
  constructor_.SuppressDestruct();
  exports.Set("StreamAnalyzer", func);
  return exports;
}

StreamAnalyzerWrap::StreamAnalyzerWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<StreamAnalyzerWrap>(info) {
  Napi::Env env = info.Env();

  sonare::StreamConfig config;
  if (info.Length() >= 1 && info[0].IsObject()) {
    Napi::Object opts = info[0].As<Napi::Object>();
    config.sample_rate = static_cast<int>(NumberKey(opts, "sampleRate", config.sample_rate));
    config.n_fft = static_cast<int>(NumberKey(opts, "nFft", config.n_fft));
    config.hop_length = static_cast<int>(NumberKey(opts, "hopLength", config.hop_length));
    config.n_mels = static_cast<int>(NumberKey(opts, "nMels", config.n_mels));
    config.fmin = static_cast<float>(NumberKey(opts, "fmin", config.fmin));
    config.fmax = static_cast<float>(NumberKey(opts, "fmax", config.fmax));
    config.tuning_ref_hz = static_cast<float>(NumberKey(opts, "tuningRefHz", config.tuning_ref_hz));
    config.compute_magnitude = BoolKey(opts, "computeMagnitude", config.compute_magnitude);
    config.compute_mel = BoolKey(opts, "computeMel", config.compute_mel);
    config.compute_chroma = BoolKey(opts, "computeChroma", config.compute_chroma);
    config.compute_onset = BoolKey(opts, "computeOnset", config.compute_onset);
    config.compute_spectral = BoolKey(opts, "computeSpectral", config.compute_spectral);
    config.emit_every_n_frames =
        static_cast<int>(NumberKey(opts, "emitEveryNFrames", config.emit_every_n_frames));
    config.magnitude_downsample =
        static_cast<int>(NumberKey(opts, "magnitudeDownsample", config.magnitude_downsample));
    config.key_update_interval_sec =
        static_cast<float>(NumberKey(opts, "keyUpdateIntervalSec", config.key_update_interval_sec));
    config.bpm_update_interval_sec =
        static_cast<float>(NumberKey(opts, "bpmUpdateIntervalSec", config.bpm_update_interval_sec));
    const int window = static_cast<int>(NumberKey(opts, "window", static_cast<int>(config.window)));
    config.window = window == 1   ? sonare::WindowType::Hamming
                    : window == 2 ? sonare::WindowType::Blackman
                    : window == 3 ? sonare::WindowType::Rectangular
                                  : sonare::WindowType::Hann;
    const int output_format =
        static_cast<int>(NumberKey(opts, "outputFormat", static_cast<int>(config.output_format)));
    config.output_format = output_format == 1   ? sonare::OutputFormat::Int16
                           : output_format == 2 ? sonare::OutputFormat::Uint8
                                                : sonare::OutputFormat::Float32;
  }

  config_ = config;
  try {
    analyzer_ = std::make_unique<sonare::StreamAnalyzer>(config_);
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return;
  }
}

StreamAnalyzerWrap::~StreamAnalyzerWrap() = default;

Napi::Value StreamAnalyzerWrap::Process(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "Expected (Float32Array)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array typed = info[0].As<Napi::Float32Array>();
  analyzer_->process(typed.Data(), typed.ElementLength());
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::ProcessWithOffset(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 2 || !IsFloat32Array(info[0]) || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "Expected (Float32Array, sampleOffset)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  Napi::Float32Array typed = info[0].As<Napi::Float32Array>();
  size_t offset = static_cast<size_t>(info[1].As<Napi::Number>().Int64Value());
  analyzer_->process(typed.Data(), typed.ElementLength(), offset);
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::AvailableFrames(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, static_cast<double>(analyzer_->available_frames()));
}

Napi::Value StreamAnalyzerWrap::ReadFramesSoa(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (maxFrames)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  size_t max_frames = static_cast<size_t>(info[0].As<Napi::Number>().Int64Value());
  sonare::FrameBuffer buffer;
  analyzer_->read_frames_soa(max_frames, buffer);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nFrames", Napi::Number::New(env, static_cast<double>(buffer.n_frames)));
  out.Set("nMels", Napi::Number::New(env, analyzer_->config().n_mels));
  out.Set("timestamps", Float32FromVec(env, buffer.timestamps));
  out.Set("mel", Float32FromVec(env, buffer.mel));
  out.Set("chroma", Float32FromVec(env, buffer.chroma));
  out.Set("onsetStrength", Float32FromVec(env, buffer.onset_strength));
  out.Set("rmsEnergy", Float32FromVec(env, buffer.rms_energy));
  out.Set("spectralCentroid", Float32FromVec(env, buffer.spectral_centroid));
  out.Set("spectralFlatness", Float32FromVec(env, buffer.spectral_flatness));
  out.Set("chordRoot", Int32FromVec(env, buffer.chord_root));
  out.Set("chordQuality", Int32FromVec(env, buffer.chord_quality));
  out.Set("chordConfidence", Float32FromVec(env, buffer.chord_confidence));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::ReadFramesU8(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (maxFrames)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  size_t max_frames = static_cast<size_t>(info[0].As<Napi::Number>().Int64Value());
  sonare::QuantizedFrameBufferU8 buffer;
  sonare::QuantizeConfig qconfig;
  analyzer_->read_frames_quantized_u8(max_frames, buffer, qconfig);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nFrames", Napi::Number::New(env, static_cast<double>(buffer.n_frames)));
  out.Set("nMels", Napi::Number::New(env, buffer.n_mels));
  out.Set("timestamps", Float32FromVec(env, buffer.timestamps));
  out.Set("mel", Uint8FromVec(env, buffer.mel));
  out.Set("chroma", Uint8FromVec(env, buffer.chroma));
  out.Set("onsetStrength", Uint8FromVec(env, buffer.onset_strength));
  out.Set("rmsEnergy", Uint8FromVec(env, buffer.rms_energy));
  out.Set("spectralCentroid", Uint8FromVec(env, buffer.spectral_centroid));
  out.Set("spectralFlatness", Uint8FromVec(env, buffer.spectral_flatness));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::ReadFramesI16(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (maxFrames)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  size_t max_frames = static_cast<size_t>(info[0].As<Napi::Number>().Int64Value());
  sonare::QuantizedFrameBufferI16 buffer;
  sonare::QuantizeConfig qconfig;
  analyzer_->read_frames_quantized_i16(max_frames, buffer, qconfig);

  Napi::Object out = Napi::Object::New(env);
  out.Set("nFrames", Napi::Number::New(env, static_cast<double>(buffer.n_frames)));
  out.Set("nMels", Napi::Number::New(env, buffer.n_mels));
  out.Set("timestamps", Float32FromVec(env, buffer.timestamps));
  out.Set("mel", Int16FromVec(env, buffer.mel));
  out.Set("chroma", Int16FromVec(env, buffer.chroma));
  out.Set("onsetStrength", Int16FromVec(env, buffer.onset_strength));
  out.Set("rmsEnergy", Int16FromVec(env, buffer.rms_energy));
  out.Set("spectralCentroid", Int16FromVec(env, buffer.spectral_centroid));
  out.Set("spectralFlatness", Int16FromVec(env, buffer.spectral_flatness));
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::Reset(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  size_t base_offset = info.Length() >= 1 && info[0].IsNumber()
                           ? static_cast<size_t>(info[0].As<Napi::Number>().Int64Value())
                           : 0;
  analyzer_->reset(base_offset);
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::Stats(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  sonare::AnalyzerStats s = analyzer_->stats();

  Napi::Object out = Napi::Object::New(env);
  out.Set("totalFrames", Napi::Number::New(env, s.total_frames));
  out.Set("totalSamples", Napi::Number::New(env, static_cast<double>(s.total_samples)));
  out.Set("durationSeconds", Napi::Number::New(env, s.duration_seconds));

  const sonare::ProgressiveEstimate& est = s.estimate;
  Napi::Object estimate = Napi::Object::New(env);
  estimate.Set("bpm", Napi::Number::New(env, est.bpm));
  estimate.Set("bpmConfidence", Napi::Number::New(env, est.bpm_confidence));
  estimate.Set("bpmCandidateCount", Napi::Number::New(env, est.bpm_candidate_count));
  estimate.Set("key", Napi::Number::New(env, est.key));
  estimate.Set("keyMinor", Napi::Boolean::New(env, est.key_minor));
  estimate.Set("keyConfidence", Napi::Number::New(env, est.key_confidence));
  estimate.Set("chordRoot", Napi::Number::New(env, est.chord_root));
  estimate.Set("chordQuality", Napi::Number::New(env, est.chord_quality));
  estimate.Set("chordConfidence", Napi::Number::New(env, est.chord_confidence));
  estimate.Set("chordStartTime", Napi::Number::New(env, est.chord_start_time));

  Napi::Array chordProgression = Napi::Array::New(env, est.chord_progression.size());
  for (size_t i = 0; i < est.chord_progression.size(); ++i) {
    const sonare::ChordChange& chord = est.chord_progression[i];
    Napi::Object c = Napi::Object::New(env);
    c.Set("root", Napi::Number::New(env, chord.root));
    c.Set("quality", Napi::Number::New(env, chord.quality));
    c.Set("startTime", Napi::Number::New(env, chord.start_time));
    c.Set("confidence", Napi::Number::New(env, chord.confidence));
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
    c.Set("startTime", Napi::Number::New(env, chord.start_time));
    c.Set("confidence", Napi::Number::New(env, chord.confidence));
    barChordProgression.Set(static_cast<uint32_t>(i), c);
  }
  estimate.Set("barChordProgression", barChordProgression);
  estimate.Set("currentBar", Napi::Number::New(env, est.current_bar));
  estimate.Set("barDuration", Napi::Number::New(env, est.bar_duration));

  Napi::Array votedPattern = Napi::Array::New(env, est.voted_pattern.size());
  for (size_t i = 0; i < est.voted_pattern.size(); ++i) {
    const sonare::BarChord& chord = est.voted_pattern[i];
    Napi::Object c = Napi::Object::New(env);
    c.Set("barIndex", Napi::Number::New(env, chord.bar_index));
    c.Set("root", Napi::Number::New(env, chord.root));
    c.Set("quality", Napi::Number::New(env, chord.quality));
    c.Set("startTime", Napi::Number::New(env, chord.start_time));
    c.Set("confidence", Napi::Number::New(env, chord.confidence));
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

  estimate.Set("accumulatedSeconds", Napi::Number::New(env, est.accumulated_seconds));
  estimate.Set("usedFrames", Napi::Number::New(env, est.used_frames));
  estimate.Set("updated", Napi::Boolean::New(env, est.updated));
  out.Set("estimate", estimate);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::FrameCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, analyzer_->frame_count());
}

Napi::Value StreamAnalyzerWrap::CurrentTime(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return Napi::Number::New(env, analyzer_->current_time());
}

Napi::Value StreamAnalyzerWrap::SampleRate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return Napi::Number::New(env, config_.sample_rate);
}

Napi::Value StreamAnalyzerWrap::SetExpectedDuration(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (durationSeconds)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  analyzer_->set_expected_duration(info[0].As<Napi::Number>().FloatValue());
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::SetNormalizationGain(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (gain)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  analyzer_->set_normalization_gain(info[0].As<Napi::Number>().FloatValue());
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

Napi::Value StreamAnalyzerWrap::SetTuningRefHz(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!analyzer_) {
    Napi::Error::New(env, "StreamAnalyzer is not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expected (refHz)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  analyzer_->set_tuning_ref_hz(info[0].As<Napi::Number>().FloatValue());
  return env.Undefined();
  SONARE_NODE_CATCH(env)
}

}  // namespace sonare_node
