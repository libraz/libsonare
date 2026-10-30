// SONARE_WASM_EXCEPTION_UNWIND: release the preset id list when allocation propagates.
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "editing/voice_changer/realtime.h"
#include "editing/voice_changer/realtime/config_ranges.h"
#include "util/exception.h"

namespace sonare::editing::voice_changer {
using namespace detail;

namespace {

constexpr std::array<PresetMetadata, 12> kPresetMetadata = {{
    {VoiceCharacterPreset::NeutralMonitor, "neutral-monitor", "Neutral Monitor", "monitor"},
    {VoiceCharacterPreset::BrightIdol, "bright-idol", "Bright Idol", "bright"},
    {VoiceCharacterPreset::SoftWhisper, "soft-whisper", "Soft Whisper", "soft"},
    {VoiceCharacterPreset::DeepNarrator, "deep-narrator", "Deep Narrator", "deep"},
    {VoiceCharacterPreset::RobotMascot, "robot-mascot", "Robot Mascot", "robot"},
    {VoiceCharacterPreset::DarkVillain, "dark-villain", "Dark Villain", "dark"},
    {VoiceCharacterPreset::HardTune, "hard-tune", "Hard Tune", "tune"},
    {VoiceCharacterPreset::RobotMonotone, "robot-monotone", "Robot Monotone", "tune"},
    {VoiceCharacterPreset::ChoirHarmony, "choir-harmony", "Choir Harmony", "harmony"},
    {VoiceCharacterPreset::VocoderStrings, "vocoder-strings", "Vocoder Strings", "vocoder"},
    {VoiceCharacterPreset::Megaphone, "megaphone", "Megaphone", "character"},
    {VoiceCharacterPreset::RadioVoice, "radio-voice", "Radio Voice", "character"},
}};

// Compile-time check that every enumerator gets a metadata row. Adding a new
// preset value to VoiceCharacterPreset without updating the table now fails
// the build rather than silently falling back to NeutralMonitor at runtime.
static_assert(kPresetMetadata.size() ==
                  static_cast<std::size_t>(VoiceCharacterPreset::RadioVoice) + 1,
              "kPresetMetadata is out of sync with VoiceCharacterPreset");

}  // namespace

namespace detail {

const PresetMetadata& preset_metadata(VoiceCharacterPreset preset) noexcept {
  for (const auto& row : kPresetMetadata) {
    if (row.preset == preset) return row;
  }
  // Unreachable for any value of the enum (every case is in the table).
  return kPresetMetadata[0];
}

const char* realtime_voice_changer_preset_display_name(VoiceCharacterPreset preset) noexcept {
  return preset_metadata(preset).display_name.data();
}

// A bare preset name inside a config document is document content; the public lookup below takes
// the name as the caller's own argument. The two differ only in the code they refuse with.
VoiceCharacterPreset preset_from_id_or_throw(std::string_view id, ErrorCode code) {
  for (const auto& row : kPresetMetadata) {
    if (row.id == id) return row.preset;
  }
  throw SonareException(code, "unknown realtime voice changer preset");
}

}  // namespace detail

RealtimeVoiceChangerConfig realtime_voice_changer_preset(VoiceCharacterPreset preset) {
  RealtimeVoiceChangerConfig c;
  c.limiter.ceiling_db = -1.0f;
  switch (preset) {
    case VoiceCharacterPreset::NeutralMonitor:
      c.retune = {0.0f, 0.0f, 0};
      c.formant = {1.0f, 0.0f, 0.0f, 0.1f, 0.0f};
      c.eq = {75.0f, 0.0f, 1.2f, 0.5f};
      c.reverb.mix = 0.0f;
      c.compressor = {-24.0f, 1.8f, 8.0f, 120.0f, 0.5f};
      break;
    case VoiceCharacterPreset::BrightIdol:
      c.output_gain_db = 2.2f;
      c.retune = {4.0f, 1.0f, 0};
      // formant.body kept at 0 because the body-region EQ already drops 2.5 dB
      // around 180 Hz; doubling the cut here muddied the lower-mids audibly.
      c.formant = {1.18f, 1.0f, 0.0f, 0.7f, 0.05f};
      c.eq = {160.0f, -2.5f, 4.5f, 3.5f};
      c.gate = {-50.0f, 2.0f, 80.0f, 22.0f};
      c.compressor = {-23.0f, 3.2f, 4.5f, 75.0f, 2.0f};
      c.deesser = {7600.0f, -30.0f, 4.0f, 9.0f};
      c.reverb = {0.11f, 380.0f, 0.35f, 7};
      break;
    case VoiceCharacterPreset::SoftWhisper:
      c.output_gain_db = 3.0f;
      c.retune = {2.0f, 1.0f, 0};
      c.formant = {1.10f, 0.85f, 0.2f, 0.25f, -0.2f};
      c.eq = {110.0f, 1.0f, -2.0f, 2.5f};
      // Release shortened from 180→90 ms: whisper passages have short
      // inter-word gaps; the previous 180 ms kept the gate open so background
      // breath leaked between words.
      c.gate = {-62.0f, 6.0f, 90.0f, 10.0f};
      c.compressor = {-28.0f, 2.6f, 12.0f, 180.0f, 3.0f};
      c.deesser = {7000.0f, -32.0f, 4.5f, 10.0f};
      c.reverb = {0.09f, 520.0f, 0.65f, 11};
      break;
    case VoiceCharacterPreset::DeepNarrator:
      c.output_gain_db = 3.5f;
      c.retune = {-5.0f, 1.0f, 0};
      c.formant = {0.84f, 1.0f, 0.65f, -0.25f, -0.15f};
      c.eq = {70.0f, 4.0f, 2.0f, -1.5f};
      c.gate = {-52.0f, 2.5f, 120.0f, 18.0f};
      c.compressor = {-24.0f, 3.0f, 14.0f, 140.0f, 1.2f};
      c.deesser = {6500.0f, -26.0f, 3.5f, 5.0f};
      c.reverb = {0.08f, 650.0f, 0.8f, 13};
      break;
    case VoiceCharacterPreset::RobotMascot:
      c.output_gain_db = 4.0f;
      // Retune capped at +7 (from +9): grain-based pitch shift has no
      // anti-aliasing, so combining +9 with formant 1.35 and presence +6 dB
      // produced audible aliasing above ~5 kHz, especially at 22050 Hz.
      // factor lowered 1.35→1.30 for the same reason.
      c.retune = {7.0f, 1.0f, 0};
      c.formant = {1.30f, 1.0f, -0.6f, 0.75f, 0.35f};
      c.eq = {240.0f, -6.0f, 4.5f, 0.0f};
      c.gate = {-45.0f, 1.0f, 55.0f, 34.0f};
      c.compressor = {-26.0f, 5.5f, 2.0f, 55.0f, 2.5f};
      c.deesser = {8200.0f, -31.0f, 5.0f, 8.0f};
      c.reverb = {0.06f, 260.0f, 0.3f, 17};
      break;
    case VoiceCharacterPreset::DarkVillain:
      c.output_gain_db = 4.5f;
      c.retune = {-9.0f, 1.0f, 0};
      c.formant = {0.72f, 1.0f, 0.8f, -0.7f, -0.2f};
      // HPF raised 55→80 Hz to keep the long, dense reverb tail (820 ms,
      // 0.13 mix) from making sustained low-mid frequencies muddy. The dark
      // character is carried by formant+brightness shaping, not sub-bass.
      c.eq = {80.0f, 5.5f, -2.0f, -5.0f};
      c.gate = {-50.0f, 2.0f, 110.0f, 26.0f};
      c.compressor = {-25.0f, 4.2f, 8.0f, 120.0f, 1.0f};
      c.deesser = {6100.0f, -24.0f, 3.0f, 4.0f};
      c.reverb = {0.13f, 820.0f, 0.9f, 19};
      break;
    case VoiceCharacterPreset::HardTune:
      c.output_gain_db = 1.5f;
      c.retune = {0.0f, 1.0f, 0};
      c.formant = {1.0f, 0.0f, 0.0f, 0.2f, 0.0f};
      c.eq = {90.0f, 0.0f, 2.5f, 2.0f};
      c.compressor = {-24.0f, 3.0f, 5.0f, 80.0f, 1.5f};
      c.deesser = {7400.0f, -30.0f, 4.0f, 7.0f};
      c.reverb = {0.08f, 420.0f, 0.4f, 23};
      // Instant snap to C major: the audible stepping is the effect.
      c.pitch_correction = {PitchCorrectionMode::Auto, 1.0f, 0.0f, 0, ScaleKind::Major, 0};
      break;
    case VoiceCharacterPreset::RobotMonotone:
      c.output_gain_db = 2.0f;
      c.retune = {0.0f, 1.0f, 0};
      c.formant = {1.05f, 0.0f, -0.2f, 0.4f, 0.2f};
      c.eq = {180.0f, -3.0f, 4.0f, 1.0f};
      c.compressor = {-26.0f, 4.0f, 3.0f, 70.0f, 2.0f};
      c.deesser = {7800.0f, -30.0f, 4.0f, 7.0f};
      c.reverb = {0.06f, 300.0f, 0.4f, 25};
      c.pitch_correction = {PitchCorrectionMode::Robot, 1.0f, 0.0f, 0, ScaleKind::Major, 0};
      break;
    case VoiceCharacterPreset::ChoirHarmony:
      c.output_gain_db = 1.0f;
      c.retune = {0.0f, 1.0f, 0};
      c.formant = {1.0f, 0.0f, 0.1f, 0.2f, 0.0f};
      c.eq = {100.0f, 0.5f, 1.5f, 2.0f};
      c.compressor = {-24.0f, 2.5f, 8.0f, 120.0f, 1.0f};
      c.reverb = {0.2f, 1100.0f, 0.6f, 29};
      // Light correction keeps the stacked voices in tune with each other.
      c.pitch_correction = {PitchCorrectionMode::Auto, 0.5f, 60.0f, 0, ScaleKind::Major, 0};
      c.harmony.voices = 3;
      c.harmony.level = 0.6f;
      c.harmony.detune_cents = 8.0f;
      c.harmony.delay_ms = 18.0f;
      break;
    case VoiceCharacterPreset::VocoderStrings:
      c.output_gain_db = 2.0f;
      c.retune = {0.0f, 1.0f, 0};
      c.formant = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
      c.eq = {120.0f, 0.0f, 1.5f, 1.0f};
      c.compressor = {-24.0f, 3.0f, 6.0f, 100.0f, 1.0f};
      c.reverb = {0.15f, 900.0f, 0.5f, 31};
      c.vocoder.enabled = true;
      c.vocoder.attack_ms = 4.0f;
      c.vocoder.release_ms = 60.0f;
      c.vocoder.sibilance = 0.25f;
      c.vocoder.carrier_wave = VocoderCarrierWave::Ensemble;
      break;
    case VoiceCharacterPreset::Megaphone:
      c.output_gain_db = 1.0f;
      c.retune = {0.0f, 1.0f, 0};
      c.formant = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
      c.eq = {300.0f, -4.0f, 3.0f, -6.0f};
      c.compressor = {-28.0f, 6.0f, 2.0f, 60.0f, 3.0f};
      c.reverb = {0.05f, 250.0f, 0.5f, 37};
      c.character = {CharacterType::Megaphone, 0.6f, 0.0f};
      break;
    case VoiceCharacterPreset::RadioVoice:
      c.output_gain_db = 1.0f;
      c.retune = {0.0f, 1.0f, 0};
      c.formant = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
      c.eq = {200.0f, -2.0f, 2.0f, -4.0f};
      c.compressor = {-26.0f, 5.0f, 3.0f, 80.0f, 2.0f};
      c.reverb = {0.0f, 320.0f, 0.5f, 41};
      c.character = {CharacterType::Radio, 0.4f, 0.0f};
      break;
  }
  return normalize_config(c);
}

const char* realtime_voice_changer_preset_id(VoiceCharacterPreset preset) noexcept {
  return preset_metadata(preset).id.data();
}

VoiceCharacterPreset realtime_voice_changer_preset_from_id(std::string_view id) {
  return preset_from_id_or_throw(id, ErrorCode::InvalidParameter);
}

std::vector<std::string> realtime_voice_changer_preset_names() {
  std::vector<std::string> out;
  out.reserve(kPresetMetadata.size());
  for (const auto& row : kPresetMetadata) {
    out.emplace_back(row.id);
  }
  return out;
}

}  // namespace sonare::editing::voice_changer
