#pragma once

/// @file config.h
/// @brief Playback renderer configuration: the parsed form of
///        `schemas/playback-renderer-config.schema.json`.
///
/// Every schema key is either a prepare key (fixed when the renderer is
/// created) or a realtime key (published to the audio thread and adopted at the
/// next block boundary). The two halves live in separate structs so the
/// realtime half stays trivially copyable for `rt::RtPublisher`. Parsing fills
/// omitted keys with the schema defaults and rejects unknown keys, out-of-range
/// values and cross-field violations with `SonareException(InvalidParameter)`
/// whose message names the key.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "core/channel_layout.h"

namespace sonare::playback {

/// Number of `SpeakerRole` values; per-role arrays are indexed by the role.
inline constexpr int kSpeakerRoleCount = 8;

/// `input.layout`.
enum class InputLayout : uint8_t { Auto, Mono, Stereo, FivePointOne, SevenPointOne };

/// `target.kind`.
enum class TargetKind : uint8_t { Headphones, Speakers };

/// `target.speakers.<role>.size`.
enum class SpeakerSize : uint8_t { Large, Small };

/// `room.preset`.
enum class RoomPreset : uint8_t { None, LivingRoom, HomeTheater, ScreeningRoom };

/// Processing stages, in signal order. The names are the fixed identifiers of
/// `inactive_stages` and `latency.stages` in the diagnostics document.
enum class Stage : uint8_t {
  Reorder,
  DialogueLevel,
  Upmix,
  Loudness,
  NightMode,
  LayoutConvert,
  SpeakerCalibration,
  BassManagement,
  Binaural,
  RoomEarly,
  RoomLate,
  OutputLimiter,
};
inline constexpr int kStageCount = 12;

/// Diagnostics identifier of a stage.
constexpr const char* stage_name(Stage stage) noexcept {
  switch (stage) {
    case Stage::Reorder:
      return "reorder";
    case Stage::DialogueLevel:
      return "dialogue_level";
    case Stage::Upmix:
      return "upmix";
    case Stage::Loudness:
      return "loudness";
    case Stage::NightMode:
      return "night_mode";
    case Stage::LayoutConvert:
      return "layout_convert";
    case Stage::SpeakerCalibration:
      return "speaker_calibration";
    case Stage::BassManagement:
      return "bass_management";
    case Stage::Binaural:
      return "binaural";
    case Stage::RoomEarly:
      return "room_early";
    case Stage::RoomLate:
      return "room_late";
    case Stage::OutputLimiter:
      return "output_limiter";
  }
  return "";
}

/// Schema spelling of a speaker role ("L", "R", "C", "LFE", "Ls", "Rs", "Lss", "Rss").
constexpr const char* speaker_role_name(SpeakerRole role) noexcept {
  switch (role) {
    case SpeakerRole::L:
      return "L";
    case SpeakerRole::R:
      return "R";
    case SpeakerRole::C:
      return "C";
    case SpeakerRole::LFE:
      return "LFE";
    case SpeakerRole::Ls:
      return "Ls";
    case SpeakerRole::Rs:
      return "Rs";
    case SpeakerRole::Lss:
      return "Lss";
    case SpeakerRole::Rss:
      return "Rss";
  }
  return "";
}

/// Parses a schema role name. Returns false for anything else.
bool parse_speaker_role(std::string_view name, SpeakerRole* out) noexcept;

/// Schema spelling of an input layout ("auto" | "mono" | "stereo" | "5.1" | "7.1").
const char* input_layout_name(InputLayout layout) noexcept;

/// Schema spelling of a room preset.
const char* room_preset_name(RoomPreset preset) noexcept;

/// Fixed channel layout of a non-auto input layout; `Stereo` for `Auto`.
constexpr ChannelLayout to_channel_layout(InputLayout layout) noexcept {
  switch (layout) {
    case InputLayout::Mono:
      return ChannelLayout::Mono;
    case InputLayout::FivePointOne:
      return ChannelLayout::FivePointOne;
    case InputLayout::SevenPointOne:
      return ChannelLayout::SevenPointOne;
    case InputLayout::Auto:
    case InputLayout::Stereo:
      return ChannelLayout::Stereo;
  }
  return ChannelLayout::Stereo;
}

/// Prepare half of `target.speakers.<role>`.
struct SpeakerPrepare {
  bool has_distance = false;  ///< false: `distance_m` is null (no compensation)
  float distance_m = 0.0f;    ///< [0.1, 30]
  SpeakerSize size = SpeakerSize::Large;
};

/// `target.bass_management`, prepare keys.
struct BassManagementConfig {
  bool enabled = false;
  float crossover_hz = 80.0f;  ///< [40, 200]
  bool subwoofer = true;
};

/// Keys fixed at renderer creation.
struct PrepareConfig {
  InputLayout input_layout = InputLayout::Auto;
  /// `input.channel_map`: role of input channel i. Only with a fixed layout.
  bool has_channel_map = false;
  std::array<SpeakerRole, kSpeakerRoleCount> channel_map{};
  TargetKind target_kind = TargetKind::Headphones;
  /// `target.layout`; meaningful (and required) for speakers only.
  ChannelLayout target_layout = ChannelLayout::Stereo;
  std::array<SpeakerPrepare, kSpeakerRoleCount> speakers{};  ///< indexed by SpeakerRole
  BassManagementConfig bass_management{};
  RoomPreset room_preset = RoomPreset::LivingRoom;
};

/// Keys adopted at the next block boundary. Trivially copyable.
struct RealtimeConfig {
  std::array<float, kSpeakerRoleCount> trim_db{};  ///< indexed by SpeakerRole, [-20, 20]
  float lfe_gain_db = 10.0f;                       ///< [-10, 15]
  float lfe_mix_db = 0.0f;                         ///< [-60, 10]
  bool upmix_enabled = true;
  float upmix_center_width = 0.2f;    ///< [0.05, 1]
  float upmix_front_ambience = 0.5f;  ///< [0, 1]
  bool upmix_lfe_from_upmix = false;
  float dialogue_level_db = 0.0f;  ///< [-12, 12]
  bool has_program_lufs = false;   ///< false: `loudness.program_lufs` is null
  float program_lufs = -24.0f;     ///< [-70, 0]
  float target_lufs = -24.0f;      ///< [-40, -5]
  float night_amount = 0.0f;       ///< [0, 1]
  float room_mix_db = -6.0f;       ///< [-30, 6]
  bool room_enabled = true;
  bool head_tracking_enabled = true;
  bool limiter_enabled = true;
  float limiter_ceiling_db = -1.0f;  ///< [-12, 0]
};

/// A complete configuration.
struct RendererConfig {
  PrepareConfig prepare{};
  RealtimeConfig realtime{};
};

/// Parses a complete or partial document; omitted keys take schema defaults.
/// @throws SonareException(InvalidParameter) naming the offending key.
RendererConfig parse_renderer_config(std::string_view json);

/// Serializes a complete document (every key present) that parses back to
/// @p config.
std::string serialize_renderer_config(const RendererConfig& config);

/// Cross-field rules (target.layout presence, speakers keys within the output
/// layout, bass-management destinations, channel_map with auto).
/// @throws SonareException(InvalidParameter).
void validate_renderer_config(const RendererConfig& config);

/// Schema key path of the first prepare key that differs, or empty when equal.
std::string first_prepare_difference(const PrepareConfig& current, const PrepareConfig& next);

/// Output channel count of a configuration (headphones: 2).
int output_channel_count(const PrepareConfig& config) noexcept;

}  // namespace sonare::playback
