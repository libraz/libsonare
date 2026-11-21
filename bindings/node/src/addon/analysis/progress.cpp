#include <cstring>
#include <string>

#include "analysis/boundary_detector.h"
#include "analysis/melody_analyzer.h"
#include "analysis/section_analyzer.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;

namespace {

const char* SectionTypeName(SonareSectionType type) {
  switch (type) {
    case SONARE_SECTION_INTRO:
      return "Intro";
    case SONARE_SECTION_VERSE:
      return "Verse";
    case SONARE_SECTION_PRE_CHORUS:
      return "Pre-Chorus";
    case SONARE_SECTION_CHORUS:
      return "Chorus";
    case SONARE_SECTION_BRIDGE:
      return "Bridge";
    case SONARE_SECTION_INSTRUMENTAL:
      return "Instrumental";
    case SONARE_SECTION_OUTRO:
      return "Outro";
    case SONARE_SECTION_UNKNOWN:
    default:
      return "Unknown";
  }
}

}  // namespace

Napi::Value SonareWrap::AnalyzeSections(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  int sample_rate{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sample_rate)) return env.Undefined();
  SonareSectionOptions options;
  sonare_section_options_init(&options);
  if (!OptionalDomainArg(env, info, 2, "nFft", options.n_fft, sonare::kSectionCountDomain,
                         &options.n_fft) ||
      !OptionalDomainArg(env, info, 3, "hopLength", options.hop_length, sonare::kSectionCountDomain,
                         &options.hop_length) ||
      !OptionalFiniteFloatArg(env, info, 4, "minSectionSec", options.min_section_sec,
                              &options.min_section_sec) ||
      !OptionalFiniteFloatArg(env, info, 5, "threshold", options.threshold, &options.threshold)) {
    return env.Undefined();
  }

  SonareSectionResult result{};
  SonareError err =
      sonare_analyze_sections(typed.Data(), typed.ElementLength(), sample_rate, &options, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareErrorFlattening(env, err, "options");
    return env.Undefined();
  }

  Napi::Array sections = Napi::Array::New(env, result.section_count);
  for (size_t i = 0; i < result.section_count; ++i) {
    const SonareSection& section = result.sections[i];
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("type", Napi::Number::New(env, static_cast<int>(section.type)));
    obj.Set("name", Napi::String::New(env, SectionTypeName(section.type)));
    obj.Set("startSec", Napi::Number::New(env, section.start_sec));
    obj.Set("endSec", Napi::Number::New(env, section.end_sec));
    obj.Set("energyLevel", Napi::Number::New(env, section.energy_level));
    obj.Set("confidence", Napi::Number::New(env, section.confidence));
    obj.Set("group", Napi::Number::New(env, section.group));
    sections.Set(static_cast<uint32_t>(i), obj);
  }
  Napi::Object out = Napi::Object::New(env);
  out.Set("sections", sections);
  out.Set("form", Napi::String::New(env, result.form != nullptr ? result.form : ""));
  sonare_free_section_result(&result);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::DetectBoundaries(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  int sample_rate{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sample_rate)) return env.Undefined();

  // Seeded from the C ABI rather than restated, so the facade cannot drift from
  // the core's BoundaryConfig. No field here takes ZeroIsSentinel: the C entry
  // refuses a zero size and reads a zero threshold as "accept every peak".
  SonareBoundaryOptions options;
  sonare_boundary_options_init(&options);
  if (info.Length() >= 3 && info[2].IsObject()) {
    Napi::Object bag = info[2].As<Napi::Object>();
    options.n_fft = DomainProperty<int>(bag, "nFft", options.n_fft, sonare::kBoundaryCountDomain);
    options.hop_length =
        DomainProperty<int>(bag, "hopLength", options.hop_length, sonare::kBoundaryCountDomain);
    options.kernel_size =
        DomainProperty<int>(bag, "kernelSize", options.kernel_size, sonare::kBoundaryCountDomain);
    options.threshold = FiniteFloatProperty(bag, "threshold", options.threshold);
    options.absolute_threshold =
        FiniteFloatProperty(bag, "absoluteThreshold", options.absolute_threshold);
    options.n_mfcc =
        DomainProperty<int>(bag, "nMfcc", options.n_mfcc, sonare::kBoundaryCountDomain);
    options.n_chroma =
        DomainProperty<int>(bag, "nChroma", options.n_chroma, sonare::kBoundaryCountDomain);
    options.peak_distance_sec =
        FiniteFloatProperty(bag, "peakDistanceSec", options.peak_distance_sec);
    options.reference_window_sec =
        FiniteFloatProperty(bag, "referenceWindowSec", options.reference_window_sec);
    options.use_mfcc = BoolProperty(bag, "useMfcc", options.use_mfcc != 0) ? 1 : 0;
    options.use_chroma = BoolProperty(bag, "useChroma", options.use_chroma != 0) ? 1 : 0;
  }

  SonareBoundaryResult result{};
  SonareError err =
      sonare_detect_boundaries(typed.Data(), typed.ElementLength(), sample_rate, &options, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareErrorFlattening(env, err, "options");
    return env.Undefined();
  }

  Napi::Array boundaries = Napi::Array::New(env, result.boundary_count);
  for (size_t i = 0; i < result.boundary_count; ++i) {
    const SonareBoundary& boundary = result.boundaries[i];
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("time", Napi::Number::New(env, boundary.time_sec));
    obj.Set("frame", Napi::Number::New(env, boundary.frame));
    obj.Set("strength", Napi::Number::New(env, boundary.strength));
    boundaries.Set(static_cast<uint32_t>(i), obj);
  }

  auto novelty = Napi::Float32Array::New(env, result.novelty_length);
  if (result.novelty_length > 0 && result.novelty_curve != nullptr) {
    std::memcpy(novelty.Data(), result.novelty_curve, result.novelty_length * sizeof(float));
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("boundaries", boundaries);
  out.Set("noveltyCurve", novelty);
  out.Set("noveltyPeak", Napi::Number::New(env, result.novelty_peak));
  out.Set("sampleRate", Napi::Number::New(env, result.sample_rate));
  out.Set("hopLength", Napi::Number::New(env, result.hop_length));
  out.Set("nFrames", Napi::Number::New(env, result.n_frames));
  out.Set("frameStride", Napi::Number::New(env, result.frame_stride));
  sonare_free_boundary_result(&result);
  return out;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::AnalyzeMelody(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  if (info.Length() < 1 || !IsFloat32Array(info[0])) {
    Napi::TypeError::New(env, "samples must be a Float32Array").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  int sample_rate{};
  if (!RequiredIntArg(env, info, 1, "sampleRate", &sample_rate)) return env.Undefined();
  float fmin{};
  if (!OptionalDomainArg(env, info, 2, "f0MinHz", 65.0f, sonare::kMelodyF0MinHzDomain, &fmin)) {
    return env.Undefined();
  }
  // The core states f0MaxHz only against f0MinHz, so the reader asks for a finite float.
  float fmax{};
  if (!OptionalFiniteFloatArg(env, info, 3, "f0MaxHz", 2093.0f, &fmax)) return env.Undefined();
  int frame_length{};
  if (!OptionalDomainArg(env, info, 4, "frameLength", 2048, sonare::kMelodyFrameDomain,
                         &frame_length)) {
    return env.Undefined();
  }
  int hop_length{};
  if (!OptionalDomainArg(env, info, 5, "hopLength", 256, sonare::kMelodyFrameDomain, &hop_length)) {
    return env.Undefined();
  }
  float threshold{};
  if (!OptionalDomainArg(env, info, 6, "threshold", 0.1f, sonare::kMelodyThresholdDomain,
                         &threshold)) {
    return env.Undefined();
  }
  bool use_pyin = false;
  if (!OptionalBoolArg(env, info, 7, "usePyin", false, &use_pyin)) {
    return env.Undefined();
  }
  // center defaults to true (matches librosa.pyin(center=True)); only honored
  // when use_pyin is set.
  bool center = true;
  if (!OptionalBoolArg(env, info, 8, "center", true, &center)) {
    return env.Undefined();
  }

  SonareMelodyResult result{};
  SonareAnalyzeMelodyOptions melody_options;
  sonare_analyze_melody_options_init(&melody_options);
  melody_options.use_pyin = use_pyin ? 1 : 0;
  melody_options.center = center ? 1 : 0;
  SonareError err =
      sonare_analyze_melody(typed.Data(), typed.ElementLength(), sample_rate, fmin, fmax,
                            frame_length, hop_length, threshold, &melody_options, &result);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  Napi::Array points = Napi::Array::New(env, result.point_count);
  for (size_t i = 0; i < result.point_count; ++i) {
    const SonareMelodyPoint& point = result.points[i];
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("time", Napi::Number::New(env, point.time_sec));
    obj.Set("frequency", Napi::Number::New(env, point.frequency_hz));
    obj.Set("confidence", Napi::Number::New(env, point.confidence));
    points.Set(static_cast<uint32_t>(i), obj);
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("points", points);
  out.Set("pitchRangeOctaves", Napi::Number::New(env, result.pitch_range_octaves));
  out.Set("pitchStability", Napi::Number::New(env, result.pitch_stability));
  out.Set("meanFrequency", Napi::Number::New(env, result.mean_frequency_hz));
  out.Set("vibratoRate", Napi::Number::New(env, result.vibrato_rate_hz));
  sonare_free_melody_result(&result);
  return out;
  SONARE_NODE_CATCH(env)
}
