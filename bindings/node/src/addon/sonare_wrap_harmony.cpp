#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "analysis/acoustic_analyzer.h"
#include "analysis/bpm_analyzer.h"
#include "analysis/chord_analyzer.h"
#include "analysis/dynamics_analyzer.h"
#include "analysis/key_analyzer.h"
#include "analysis/music_analyzer.h"
#include "analysis/onset_analyzer.h"
#include "analysis/timbre_analyzer.h"
#include "core/audio.h"
#include "sonare_wrap.h"
#include "sonare_wrap_analysis_input.h"
#include "sonare_wrap_key_options.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"
#include "util/constants.h"

using namespace sonare_node;

namespace {

// The C chord record carries the root, bass and quality but not the rendered
// symbol, so the canonical spelling is recovered by rebuilding the core chord
// and asking it. Formatting the symbol here instead would fork the spelling
// away from the core the moment a quality is added.
//
// Rebuilding relies on the two quality enumerations agreeing value for value,
// which the C API's own root/bass conversion already assumes. These assertions
// make a future divergence a build failure rather than a mislabelled chord.
static_assert(static_cast<int>(sonare::ChordQuality::Major) == SONARE_CHORD_MAJOR, "");
static_assert(static_cast<int>(sonare::ChordQuality::Minor) == SONARE_CHORD_MINOR, "");
static_assert(static_cast<int>(sonare::ChordQuality::Diminished) == SONARE_CHORD_DIMINISHED, "");
static_assert(static_cast<int>(sonare::ChordQuality::Augmented) == SONARE_CHORD_AUGMENTED, "");
static_assert(static_cast<int>(sonare::ChordQuality::Dominant7) == SONARE_CHORD_DOMINANT7, "");
static_assert(static_cast<int>(sonare::ChordQuality::Major7) == SONARE_CHORD_MAJOR7, "");
static_assert(static_cast<int>(sonare::ChordQuality::Minor7) == SONARE_CHORD_MINOR7, "");
static_assert(static_cast<int>(sonare::ChordQuality::Sus2) == SONARE_CHORD_SUS2, "");
static_assert(static_cast<int>(sonare::ChordQuality::Sus4) == SONARE_CHORD_SUS4, "");
static_assert(static_cast<int>(sonare::ChordQuality::Unknown) == SONARE_CHORD_UNKNOWN, "");
static_assert(static_cast<int>(sonare::ChordQuality::Add9) == SONARE_CHORD_ADD9, "");
static_assert(static_cast<int>(sonare::ChordQuality::MinorAdd9) == SONARE_CHORD_MINOR_ADD9, "");
static_assert(static_cast<int>(sonare::ChordQuality::Dim7) == SONARE_CHORD_DIM7, "");
static_assert(static_cast<int>(sonare::ChordQuality::HalfDim7) == SONARE_CHORD_HALF_DIM7, "");
static_assert(static_cast<int>(sonare::ChordQuality::Major9) == SONARE_CHORD_MAJOR9, "");
static_assert(static_cast<int>(sonare::ChordQuality::Dominant9) == SONARE_CHORD_DOMINANT9, "");
static_assert(static_cast<int>(sonare::ChordQuality::Sus2Add4) == SONARE_CHORD_SUS2_ADD4, "");
static_assert(static_cast<int>(sonare::ChordQuality::Major6) == SONARE_CHORD_MAJOR6, "");
static_assert(static_cast<int>(sonare::ChordQuality::Minor6) == SONARE_CHORD_MINOR6, "");
static_assert(static_cast<int>(sonare::ChordQuality::MinorMajor7) == SONARE_CHORD_MINOR_MAJOR7, "");
static_assert(static_cast<int>(sonare::ChordQuality::Dominant7Sus4) == SONARE_CHORD_DOMINANT7_SUS4,
              "");
static_assert(static_cast<int>(sonare::ChordQuality::Dominant11) == SONARE_CHORD_DOMINANT11, "");
static_assert(static_cast<int>(sonare::ChordQuality::Dominant13) == SONARE_CHORD_DOMINANT13, "");
static_assert(static_cast<int>(sonare::ChordQuality::Dominant7b9) == SONARE_CHORD_DOMINANT7_FLAT9,
              "");
static_assert(static_cast<int>(sonare::ChordQuality::Dominant7s9) == SONARE_CHORD_DOMINANT7_SHARP9,
              "");

/// @brief Returns the core's canonical symbol for one C chord record.
std::string ChordSymbol(const SonareChord& chord) {
  sonare::Chord core{};
  core.root = static_cast<sonare::PitchClass>(chord.root);
  core.bass = static_cast<sonare::PitchClass>(chord.bass);
  core.quality = static_cast<sonare::ChordQuality>(chord.quality);
  return core.to_string();
}

}  // namespace

namespace {

Napi::Array StringArrayToJs(Napi::Env env, const SonareStringArray& labels) {
  Napi::Array result = Napi::Array::New(env, labels.count);
  for (size_t i = 0; i < labels.count; ++i) {
    result.Set(static_cast<uint32_t>(i),
               Napi::String::New(env, labels.items[i] != nullptr ? labels.items[i] : ""));
  }
  return result;
}

// The timed chord entries of a detection result, in the shape detectChords reports.
Napi::Array ChordsToJs(Napi::Env env, const SonareChordAnalysisResult& analysis) {
  Napi::Array chords = Napi::Array::New(env, analysis.chord_count);
  for (size_t i = 0; i < analysis.chord_count; ++i) {
    Napi::Object chord = Napi::Object::New(env);
    chord.Set("root", Napi::Number::New(env, static_cast<int>(analysis.chords[i].root)));
    chord.Set("bass", Napi::Number::New(env, static_cast<int>(analysis.chords[i].bass)));
    chord.Set("rootName", Napi::String::New(env, PitchClassNameLocal(analysis.chords[i].root)));
    chord.Set("bassName", Napi::String::New(env, PitchClassNameLocal(analysis.chords[i].bass)));
    chord.Set("quality", Napi::Number::New(env, static_cast<int>(analysis.chords[i].quality)));
    chord.Set("name", Napi::String::New(env, ChordSymbol(analysis.chords[i])));
    chord.Set("startSec", Napi::Number::New(env, analysis.chords[i].start_sec));
    chord.Set("endSec", Napi::Number::New(env, analysis.chords[i].end_sec));
    chord.Set("durationSec",
              Napi::Number::New(env, analysis.chords[i].end_sec - analysis.chords[i].start_sec));
    chord.Set("confidence", Napi::Number::New(env, analysis.chords[i].confidence));
    chords.Set(static_cast<uint32_t>(i), chord);
  }
  return chords;
}

}  // namespace

Napi::Value SonareWrap::DetectChords(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  MonoAnalysisInput input;
  if (!ReadMonoAnalysisInput(info, &input, sonare::constants::kDefaultSampleRate))
    return env.Undefined();
  const float* data = input.data;
  size_t length = input.length;
  int sample_rate = input.sample_rate;
  float min_duration{};
  if (!OptionalFloatArg(env, info, 2, "minDuration", 0.3f, &min_duration)) return env.Undefined();
  float smoothing_window{};
  if (!OptionalFloatArg(env, info, 3, "smoothingWindow", 2.0f, &smoothing_window))
    return env.Undefined();
  float threshold{};
  if (!OptionalFloatArg(env, info, 4, "threshold", 0.5f, &threshold)) return env.Undefined();
  const sonare::ChordConfig chord_defaults;
  bool use_triads_only{};
  if (!OptionalBoolArg(env, info, 5, "useTriadsOnly", chord_defaults.use_triads_only,
                       &use_triads_only))
    return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 6, "nFft", sonare::constants::kDefaultNFft, &n_fft)) {
    return env.Undefined();
  }
  int hop_length{};
  if (!OptionalDomainArg(env, info, 7, "hopLength", sonare::constants::kDefaultHopLength,
                         sonare::kChordHopLengthDomain, &hop_length)) {
    return env.Undefined();
  }
  bool use_beat_sync{};
  if (!OptionalBoolArg(env, info, 8, "useBeatSync", true, &use_beat_sync)) return env.Undefined();
  bool use_hmm{};
  if (!OptionalBoolArg(env, info, 9, "useHmm", chord_defaults.use_hmm, &use_hmm))
    return env.Undefined();
  int hmm_beam_width{};
  if (!OptionalDomainArg(env, info, 10, "hmmBeamWidth", 24, sonare::kChordBeamWidthDomain,
                         &hmm_beam_width)) {
    return env.Undefined();
  }
  bool use_key_context{};
  if (!OptionalBoolArg(env, info, 11, "useKeyContext", false, &use_key_context))
    return env.Undefined();
  const SonarePitchClass key_root = info[12].IsUndefined() || info[12].IsNull()
                                        ? SONARE_PITCH_C
                                        : PitchClassFromValue(info[12], "keyRoot");
  const SonareMode key_mode = info[13].IsUndefined() || info[13].IsNull()
                                  ? SONARE_MODE_MAJOR
                                  : node_mode_from_value(info[13], "keyMode");
  bool detect_inversions{};
  if (!OptionalBoolArg(env, info, 14, "detectInversions", false, &detect_inversions))
    return env.Undefined();
  int chroma_method{};
  if (!OptionalIntArg(env, info, 15, "chromaMethod", static_cast<int>(chord_defaults.chroma_method),
                      &chroma_method))
    return env.Undefined();
  float tuning_cents{};
  if (!RequiredFloatArg(env, info, 16, "tuningCents", &tuning_cents)) return env.Undefined();
  bool tuning_auto{};
  if (!RequiredBoolArg(env, info, 17, "tuningAuto", &tuning_auto)) return env.Undefined();

  SonareChordAnalysisResult analysis{};
  SonareChordDetectionOptions options{};
  sonare_chord_detection_options_init(&options);
  options.min_duration_sec = min_duration;
  options.smoothing_window_sec = smoothing_window;
  options.threshold = threshold;
  options.use_triads_only = use_triads_only ? 1 : 0;
  options.n_fft = n_fft;
  options.hop_length = hop_length;
  options.use_beat_sync = use_beat_sync ? 1 : 0;
  options.use_hmm = use_hmm ? 1 : 0;
  options.hmm_beam_width = hmm_beam_width;
  options.use_key_context = use_key_context ? 1 : 0;
  options.key_root = key_root;
  options.key_mode = key_mode;
  options.detect_inversions = detect_inversions ? 1 : 0;
  options.chroma_method = static_cast<SonareChromaMethod>(chroma_method);
  options.tuning_cents = tuning_cents;
  options.tuning_auto = tuning_auto ? 1 : 0;
  SonareError err = sonare_detect_chords(data, length, sample_rate, &options, &analysis);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  Napi::Array chords = ChordsToJs(env, analysis);

  Napi::Object result = Napi::Object::New(env);
  result.Set("chords", chords);
  result.Set("tuningCents", Napi::Number::New(env, static_cast<double>(analysis.tuning_cents)));
  sonare_free_chord_analysis_result(&analysis);
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::FunctionalAnalysis(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY

  if (!RequireFloat32Array(info, 0, "samples must be a Float32Array")) {
    return env.Undefined();
  }

  auto typed = info[0].As<Napi::Float32Array>();
  const float* data = typed.Data();
  size_t length = typed.ElementLength();
  const SonarePitchClass key_root = info[1].IsUndefined() || info[1].IsNull()
                                        ? SONARE_PITCH_C
                                        : PitchClassFromValue(info[1], "keyRoot");
  const SonareMode key_mode = info[2].IsUndefined() || info[2].IsNull()
                                  ? SONARE_MODE_MAJOR
                                  : node_mode_from_value(info[2], "keyMode");
  int sample_rate{};
  if (!RequiredIntArg(env, info, 3, "sampleRate", &sample_rate)) return env.Undefined();
  float min_duration{};
  if (!OptionalFloatArg(env, info, 4, "minDuration", 0.3f, &min_duration)) return env.Undefined();
  float smoothing_window{};
  if (!OptionalFloatArg(env, info, 5, "smoothingWindow", 2.0f, &smoothing_window))
    return env.Undefined();
  float threshold{};
  if (!OptionalFloatArg(env, info, 6, "threshold", 0.5f, &threshold)) return env.Undefined();
  const sonare::ChordConfig chord_defaults;
  bool use_triads_only{};
  if (!OptionalBoolArg(env, info, 7, "useTriadsOnly", chord_defaults.use_triads_only,
                       &use_triads_only))
    return env.Undefined();
  int n_fft{};
  if (!OptionalIntArg(env, info, 8, "nFft", sonare::constants::kDefaultNFft, &n_fft)) {
    return env.Undefined();
  }
  int hop_length{};
  if (!OptionalDomainArg(env, info, 9, "hopLength", sonare::constants::kDefaultHopLength,
                         sonare::kChordHopLengthDomain, &hop_length)) {
    return env.Undefined();
  }
  bool use_beat_sync{};
  if (!OptionalBoolArg(env, info, 10, "useBeatSync", true, &use_beat_sync)) return env.Undefined();
  bool use_hmm{};
  if (!OptionalBoolArg(env, info, 11, "useHmm", chord_defaults.use_hmm, &use_hmm))
    return env.Undefined();
  int hmm_beam_width{};
  if (!OptionalDomainArg(env, info, 12, "hmmBeamWidth", 24, sonare::kChordBeamWidthDomain,
                         &hmm_beam_width)) {
    return env.Undefined();
  }
  bool use_key_context{};
  if (!OptionalBoolArg(env, info, 13, "useKeyContext", false, &use_key_context))
    return env.Undefined();
  bool detect_inversions{};
  if (!OptionalBoolArg(env, info, 14, "detectInversions", false, &detect_inversions))
    return env.Undefined();
  int chroma_method{};
  if (!OptionalIntArg(env, info, 15, "chromaMethod", static_cast<int>(chord_defaults.chroma_method),
                      &chroma_method))
    return env.Undefined();
  float tuning_cents{};
  if (!RequiredFloatArg(env, info, 16, "tuningCents", &tuning_cents)) return env.Undefined();
  bool tuning_auto{};
  if (!RequiredBoolArg(env, info, 17, "tuningAuto", &tuning_auto)) return env.Undefined();

  SonareChordDetectionOptions options{};
  sonare_chord_detection_options_init(&options);
  options.min_duration_sec = min_duration;
  options.smoothing_window_sec = smoothing_window;
  options.threshold = threshold;
  options.use_triads_only = use_triads_only ? 1 : 0;
  options.n_fft = n_fft;
  options.hop_length = hop_length;
  options.use_beat_sync = use_beat_sync ? 1 : 0;
  options.use_hmm = use_hmm ? 1 : 0;
  options.hmm_beam_width = hmm_beam_width;
  options.use_key_context = use_key_context ? 1 : 0;
  options.key_root = key_root;
  options.key_mode = key_mode;
  options.detect_inversions = detect_inversions ? 1 : 0;
  options.chroma_method = static_cast<SonareChromaMethod>(chroma_method);
  options.tuning_cents = tuning_cents;
  options.tuning_auto = tuning_auto ? 1 : 0;

  SonareChordAnalysisResult detected{};
  SonareStringArray roman{};
  SonareStringArray functions{};
  SonareError err = sonare_chord_functional_analysis(data, length, sample_rate, &options, key_root,
                                                     key_mode, &detected, &roman, &functions);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }

  Napi::Object result = Napi::Object::New(env);
  result.Set("chords", ChordsToJs(env, detected));
  result.Set("roman", StringArrayToJs(env, roman));
  result.Set("functions", StringArrayToJs(env, functions));
  sonare_free_chord_analysis_result(&detected);
  sonare_free_string_array(&roman);
  sonare_free_string_array(&functions);
  return result;
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::ChordFunctions(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!info[0].IsArray()) {
    Napi::TypeError::New(env, "chords must be an array").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const Napi::Array entries = info[0].As<Napi::Array>();
  const uint32_t entry_count = node_list_length(entries, "chords");
  std::vector<SonareChord> chords(entry_count);
  for (uint32_t i = 0; i < entry_count; ++i) {
    const Napi::Value entry = entries.Get(i);
    if (!entry.IsObject()) {
      throw Napi::TypeError::New(env, "chords[" + std::to_string(i) + "] must be an object");
    }
    const Napi::Object chord = entry.As<Napi::Object>();
    const std::string at = "chords[" + std::to_string(i) + "]";
    chords[i] = SonareChord{};
    chords[i].root = PitchClassFromValue(chord.Get("root"), (at + ".root").c_str());
    chords[i].quality = ChordQualityFromValue(chord.Get("quality"), (at + ".quality").c_str());
    chords[i].bass = chords[i].root;
  }
  const SonarePitchClass key_root = PitchClassFromValue(info[1], "key.root");
  const SonareMode key_mode = node_mode_from_value(info[2], "key.mode");

  SonareStringArray roman{};
  SonareStringArray functions{};
  const SonareError err =
      sonare_chord_functions(chords.empty() ? nullptr : chords.data(), chords.size(), key_root,
                             key_mode, &roman, &functions);
  if (err != SONARE_OK) {
    sonare_node::ThrowSonareError(env, err);
    return env.Undefined();
  }
  Napi::Object result = Napi::Object::New(env);
  result.Set("roman", StringArrayToJs(env, roman));
  result.Set("functions", StringArrayToJs(env, functions));
  sonare_free_string_array(&roman);
  sonare_free_string_array(&functions);
  return result;
  SONARE_NODE_CATCH(env)
}
