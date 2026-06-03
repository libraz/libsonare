/// @file bindings.cpp
/// @brief Embind bindings for WebAssembly.

#ifdef __EMSCRIPTEN__

#include "bindings/common.h"

using namespace emscripten;
using namespace sonare;

// ============================================================================
// Version
// ============================================================================

std::string js_version() { return SONARE_VERSION_STRING; }

uint32_t js_engine_abi_version() { return sonare::rt::kEngineAbiVersion; }

uint32_t js_voice_changer_abi_version() { return editing::voice_changer::kVoiceChangerAbiVersion; }

// POD-flat ↔ nested C++ field bridge for the realtime voice-changer config.
// X(cpp_path, pod_field) — cpp_path is the dotted member on the C++
// RealtimeVoiceChangerConfig; pod_field is the flat key exposed to JS (matching
// the Python/C-ABI POD mirror). Calling the C++ accessors directly keeps this
// binding self-contained: the C-ABI translation unit is not linked into WASM.
#define SONARE_WASM_VC_FIELDS(X)                            \
  X(input_gain_db, input_gain_db)                           \
  X(output_gain_db, output_gain_db)                         \
  X(wet_mix, wet_mix)                                       \
  X(retune.semitones, retune_semitones)                     \
  X(retune.mix, retune_mix)                                 \
  X(retune.grain_size, retune_grain_size)                   \
  X(formant.factor, formant_factor)                         \
  X(formant.amount, formant_amount)                         \
  X(formant.body, formant_body)                             \
  X(formant.brightness, formant_brightness)                 \
  X(formant.nasal, formant_nasal)                           \
  X(eq.highpass_hz, eq_highpass_hz)                         \
  X(eq.body_db, eq_body_db)                                 \
  X(eq.presence_db, eq_presence_db)                         \
  X(eq.air_db, eq_air_db)                                   \
  X(gate.threshold_db, gate_threshold_db)                   \
  X(gate.attack_ms, gate_attack_ms)                         \
  X(gate.release_ms, gate_release_ms)                       \
  X(gate.range_db, gate_range_db)                           \
  X(compressor.threshold_db, compressor_threshold_db)       \
  X(compressor.ratio, compressor_ratio)                     \
  X(compressor.attack_ms, compressor_attack_ms)             \
  X(compressor.release_ms, compressor_release_ms)           \
  X(compressor.makeup_gain_db, compressor_makeup_gain_db)   \
  X(deesser.frequency_hz, deesser_frequency_hz)             \
  X(deesser.threshold_db, deesser_threshold_db)             \
  X(deesser.ratio, deesser_ratio)                           \
  X(deesser.range_db, deesser_range_db)                     \
  X(reverb.mix, reverb_mix)                                 \
  X(reverb.time_ms, reverb_time_ms)                         \
  X(reverb.damping, reverb_damping)                         \
  X(reverb.seed, reverb_seed)                               \
  X(limiter.ceiling_db, limiter_ceiling_db)                 \
  X(limiter.release_ms, limiter_release_ms)                 \
  X(limiter.enable_isp_limiter, limiter_enable_isp_limiter) \
  X(limiter.isp_ceiling_dbtp, limiter_isp_ceiling_dbtp)

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

// Returns the voice-changer config for a preset ordinal as a JS object. Field
// names match the Python/C-ABI POD mirror exactly. Null for an out-of-range
// ordinal.
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
  function("engineAbiVersion", &js_engine_abi_version);
  function("voiceChangerAbiVersion", &js_voice_changer_abi_version);
  function("voiceCharacterPresetId", &js_voice_character_preset_id);
  function("realtimeVoiceChangerPresetConfig", &js_realtime_voice_changer_preset_config);

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
