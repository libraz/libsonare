/// @file bindings.cpp
/// @brief Embind bindings for WebAssembly.

#ifdef __EMSCRIPTEN__

#include <emscripten/emscripten.h>

#include "bindings/common/common.h"

// Pulled in for the compile-time SONARE_ABI_VERSION macro only. The macro packs
// the per-subsystem ABI versions into one 32-bit value with no link dependency
// on the C-ABI translation unit (which is not linked into WASM).
#include <sonare/sonare_c.h>

using namespace emscripten;
using namespace sonare;

// ============================================================================
// Version
// ============================================================================

std::string js_version() { return SONARE_VERSION_STRING; }

namespace {

constexpr bool capability_mastering_enabled() {
#if defined(SONARE_BUILD_MASTERING) && SONARE_BUILD_MASTERING
  return true;
#else
  return false;
#endif
}

constexpr bool capability_mixing_enabled() {
#if defined(SONARE_BUILD_MIXING) && SONARE_BUILD_MIXING
  return true;
#else
  return false;
#endif
}

constexpr bool capability_fx_enabled() {
#if defined(SONARE_BUILD_FX) && SONARE_BUILD_FX
  return true;
#else
  return false;
#endif
}

constexpr bool capability_ffmpeg_enabled() {
#if defined(SONARE_WITH_FFMPEG)
  return true;
#else
  return false;
#endif
}

constexpr const char* capability_simd() {
#if defined(__wasm_simd128__)
  return "wasm-simd128";
#else
  return "none";
#endif
}

int capability_hardware_concurrency() {
  // Query the browser at call time rather than baking in the build machine's
  // value. A worker may not expose navigator, in which case one is the safe
  // synchronous fallback.
  return EM_ASM_INT({
    if (!globalThis.navigator || !Number.isFinite(globalThis.navigator.hardwareConcurrency)) {
      return 1;
    }
    return Math.max(1, Math.floor(globalThis.navigator.hardwareConcurrency));
  });
}

val string_array(std::initializer_list<const char*> values) {
  val result = val::array();
  for (const char* value : values) {
    result.call<void>("push", std::string(value));
  }
  return result;
}

}  // namespace

val js_capabilities() {
  val result = val::object();
  result.set("version", std::string(SONARE_VERSION_STRING));

  val abi = val::object();
  abi.set("project", SONARE_PROJECT_ABI_VERSION);
  abi.set("engine", sonare::rt::kEngineAbiVersion);
  result.set("abi", abi);
  result.set("platform", std::string("wasm32"));

  val features = val::object();
  features.set("mastering", capability_mastering_enabled());
  features.set("mixing", capability_mixing_enabled());
  features.set("fx", capability_fx_enabled());
  features.set("ffmpeg", capability_ffmpeg_enabled());
  result.set("features", features);

  val decode = val::object();
  decode.set("builtin", string_array({"wav", "mp3"}));
#if defined(SONARE_WITH_FFMPEG)
  decode.set("ffmpeg", string_array({"m4a", "aac", "flac", "ogg", "opus", "wma"}));
#else
  decode.set("ffmpeg", string_array({}));
#endif
  result.set("decode", decode);
  result.set("simd", std::string(capability_simd()));
  result.set("hardwareConcurrency", capability_hardware_concurrency());
  return result;
}

// ----------------------------------------------------------------------------
// Error introspection
// ----------------------------------------------------------------------------
// With emscripten's classic exception handling a C++ throw reaches JS as the
// raw exception-object pointer (a number). Given that pointer, surface a
// structured { code, codeName, message } so the JS glue can rebuild a
// SonareError whose numeric code matches the C ABI / Node / Python surfaces.
// The integer codes mirror the C ABI SonareError enum (the C-ABI TU is not
// linked into WASM, so the values are written out here rather than referenced).
val js_sonare_exception_info(std::uintptr_t exception_ptr) {
  val info = val::object();
  int code = 99;  // SONARE_ERROR_UNKNOWN
  const char* code_name = "Unknown";
  std::string message;
  auto* base = reinterpret_cast<std::exception*>(exception_ptr);
  if (base != nullptr) {
    message = base->what();
    if (const auto* se = dynamic_cast<const sonare::SonareException*>(base)) {
      switch (se->code()) {
        case sonare::ErrorCode::Ok:
          code = 0;
          code_name = "Ok";
          break;
        case sonare::ErrorCode::FileNotFound:
          code = 1;
          code_name = "FileNotFound";
          break;
        case sonare::ErrorCode::InvalidFormat:
          code = 2;
          code_name = "InvalidFormat";
          break;
        case sonare::ErrorCode::DecodeFailed:
          code = 3;
          code_name = "DecodeFailed";
          break;
        case sonare::ErrorCode::InvalidParameter:
          code = 4;
          code_name = "InvalidParameter";
          break;
        case sonare::ErrorCode::OutOfMemory:
          code = 5;
          code_name = "OutOfMemory";
          break;
        case sonare::ErrorCode::NotImplemented:
          code = 6;
          code_name = "NotSupported";
          break;
        case sonare::ErrorCode::InvalidState:
          code = 7;
          code_name = "InvalidState";
          break;
      }
    }
  }
  info.set("code", code);
  info.set("codeName", std::string(code_name));
  info.set("message", message);
  return info;
}

// Aggregate C-ABI version: the per-subsystem ABI macros folded into one 32-bit
// value, matching the Node addon (sonare_abi_version) and Python. Computed from
// the compile-time SONARE_ABI_VERSION macro so it stays correct without linking
// the C-ABI translation unit.
uint32_t js_abi_version() { return SONARE_ABI_VERSION; }

uint32_t js_engine_abi_version() { return sonare::rt::kEngineAbiVersion; }

uint32_t js_voice_changer_abi_version() { return editing::voice_changer::kVoiceChangerAbiVersion; }

// POD-flat ↔ nested C++ field bridge for the realtime voice-changer config.
// X(cpp_path, js_key) — cpp_path is the dotted member on the C++
// RealtimeVoiceChangerConfig; js_key is the camelCase key exposed to JS,
// matching the Node addon getter (bindings/node/src/addon/effects/extra.cpp)
// so a config reads identically across both JS surfaces. Calling the C++
// accessors directly keeps this binding self-contained: the C-ABI translation
// unit is not linked into WASM.
#define SONARE_WASM_VC_FIELDS(X)                         \
  X(input_gain_db, inputGainDb)                          \
  X(output_gain_db, outputGainDb)                        \
  X(wet_mix, wetMix)                                     \
  X(retune.semitones, retuneSemitones)                   \
  X(retune.mix, retuneMix)                               \
  X(retune.grain_size, retuneGrainSize)                  \
  X(formant.factor, formantFactor)                       \
  X(formant.amount, formantAmount)                       \
  X(formant.body, formantBody)                           \
  X(formant.brightness, formantBrightness)               \
  X(formant.nasal, formantNasal)                         \
  X(eq.highpass_hz, eqHighpassHz)                        \
  X(eq.body_db, eqBodyDb)                                \
  X(eq.presence_db, eqPresenceDb)                        \
  X(eq.air_db, eqAirDb)                                  \
  X(gate.threshold_db, gateThresholdDb)                  \
  X(gate.attack_ms, gateAttackMs)                        \
  X(gate.release_ms, gateReleaseMs)                      \
  X(gate.range_db, gateRangeDb)                          \
  X(compressor.threshold_db, compressorThresholdDb)      \
  X(compressor.ratio, compressorRatio)                   \
  X(compressor.attack_ms, compressorAttackMs)            \
  X(compressor.release_ms, compressorReleaseMs)          \
  X(compressor.makeup_gain_db, compressorMakeupGainDb)   \
  X(deesser.frequency_hz, deesserFrequencyHz)            \
  X(deesser.threshold_db, deesserThresholdDb)            \
  X(deesser.ratio, deesserRatio)                         \
  X(deesser.range_db, deesserRangeDb)                    \
  X(reverb.mix, reverbMix)                               \
  X(reverb.time_ms, reverbTimeMs)                        \
  X(reverb.damping, reverbDamping)                       \
  X(reverb.seed, reverbSeed)                             \
  X(limiter.ceiling_db, limiterCeilingDb)                \
  X(limiter.release_ms, limiterReleaseMs)                \
  X(limiter.enable_isp_limiter, limiterEnableIspLimiter) \
  X(limiter.isp_ceiling_dbtp, limiterIspCeilingDbtp)

// Validates a preset ordinal against the C++ VoiceCharacterPreset enum range.
// The C-ABI and C++ enumerators share an identical ordering, so the integer
// ordinal exposed to JS maps straight onto the C++ enum.
bool vc_preset_in_range(int preset) {
  return preset >= 0 &&
         preset <= static_cast<int>(editing::voice_changer::VoiceCharacterPreset::DarkVillain);
}

// Maps a voice-character preset ordinal to its canonical id string (e.g.
// "bright-idol"). Returns null for an out-of-range / unknown ordinal.
val js_voice_character_preset_id(int preset) {
  if (!vc_preset_in_range(preset)) return val::null();
  const char* id = editing::voice_changer::realtime_voice_changer_preset_id(
      static_cast<editing::voice_changer::VoiceCharacterPreset>(preset));
  if (id == nullptr || id[0] == '\0') return val::null();
  return val(std::string(id));
}

// Returns the voice-changer config for a preset ordinal as a JS object. Keys
// are camelCase to match the Node addon getter (the two JS surfaces agree).
// Null for an out-of-range ordinal.
val js_realtime_voice_changer_preset_config(int preset) {
  if (!vc_preset_in_range(preset)) return val::null();
  const auto cfg = editing::voice_changer::realtime_voice_changer_preset(
      static_cast<editing::voice_changer::VoiceCharacterPreset>(preset));
  val out = val::object();
#define X(cpp_path, pod_field) out.set(#pod_field, cfg.cpp_path);
  SONARE_WASM_VC_FIELDS(X)
#undef X
  return out;
}
#undef SONARE_WASM_VC_FIELDS

val js_audio_from_memory(val bytes) {
  std::vector<uint8_t> data = uint8ArrayToVector(bytes);
  const Audio audio = Audio::from_memory(data.data(), data.size());
  const std::vector<float> samples(audio.data(), audio.data() + audio.size());
  val out = val::object();
  out.set("samples", vectorToFloat32Array(samples));
  out.set("sampleRate", audio.sample_rate());
  return out;
}

// ============================================================================
// Embind Registrations
// ============================================================================

EMSCRIPTEN_BINDINGS(sonare) {
  // Enums
  enum_<PitchClass>("PitchClass")
      .value("C", PitchClass::C)
      .value("Cs", PitchClass::Cs)
      .value("D", PitchClass::D)
      .value("Ds", PitchClass::Ds)
      .value("E", PitchClass::E)
      .value("F", PitchClass::F)
      .value("Fs", PitchClass::Fs)
      .value("G", PitchClass::G)
      .value("Gs", PitchClass::Gs)
      .value("A", PitchClass::A)
      .value("As", PitchClass::As)
      .value("B", PitchClass::B);

  enum_<Mode>("Mode")
      .value("Major", Mode::Major)
      .value("Minor", Mode::Minor)
      .value("Dorian", Mode::Dorian)
      .value("Phrygian", Mode::Phrygian)
      .value("Lydian", Mode::Lydian)
      .value("Mixolydian", Mode::Mixolydian)
      .value("Locrian", Mode::Locrian);

  enum_<ChordQuality>("ChordQuality")
      .value("Major", ChordQuality::Major)
      .value("Minor", ChordQuality::Minor)
      .value("Diminished", ChordQuality::Diminished)
      .value("Augmented", ChordQuality::Augmented)
      .value("Dominant7", ChordQuality::Dominant7)
      .value("Major7", ChordQuality::Major7)
      .value("Minor7", ChordQuality::Minor7)
      .value("Sus2", ChordQuality::Sus2)
      .value("Sus4", ChordQuality::Sus4)
      .value("Unknown", ChordQuality::Unknown)
      .value("Add9", ChordQuality::Add9)
      .value("MinorAdd9", ChordQuality::MinorAdd9)
      .value("Dim7", ChordQuality::Dim7)
      .value("HalfDim7", ChordQuality::HalfDim7)
      .value("Major9", ChordQuality::Major9)
      .value("Dominant9", ChordQuality::Dominant9)
      .value("Sus2Add4", ChordQuality::Sus2Add4);

  enum_<SectionType>("SectionType")
      .value("Intro", SectionType::Intro)
      .value("Verse", SectionType::Verse)
      .value("PreChorus", SectionType::PreChorus)
      .value("Chorus", SectionType::Chorus)
      .value("Bridge", SectionType::Bridge)
      .value("Instrumental", SectionType::Instrumental)
      .value("Outro", SectionType::Outro)
      .value("Unknown", SectionType::Unknown);

  function("version", &js_version);
  function("capabilities", &js_capabilities);
  function("sonareExceptionInfo", &js_sonare_exception_info);
  function("abiVersion", &js_abi_version);
  function("engineAbiVersion", &js_engine_abi_version);
  function("voiceChangerAbiVersion", &js_voice_changer_abi_version);
  function("voiceCharacterPresetId", &js_voice_character_preset_id);
  function("realtimeVoiceChangerPresetConfig", &js_realtime_voice_changer_preset_config);
  function("audioFromMemory", &js_audio_from_memory);

  registerQuickAnalysisBindings();
  registerEffectsAudioBindings();
  registerMasteringChainBindings();
  registerMasteringApiBindings();
  registerMixingBindings();

  registerRealtimeEngineBindings();

  registerProjectBindings();

  registerOfflineBindings();

  registerStreamingMasteringChainBindings();
  registerStreamingEqualizerBindings();
  registerStreamingRetuneBindings();
  registerRealtimeVoiceChangerStreamingBindings();

  registerStreamAnalyzerBindings();
}

#endif  // __EMSCRIPTEN__
