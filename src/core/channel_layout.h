#pragma once

/// @file channel_layout.h
/// @brief Closed-set surround/multichannel layout model (phase 1: mono..7.1).
///
/// The canonical internal plane order is the Microsoft WAVE_FORMAT_EXTENSIBLE
/// speaker-mask order (also ITU-R BS.2051 / SMPTE for these beds), so mixer
/// buffers, meter taps, bounce interleave, and the WAV file share one order with
/// zero re-permutation. This header has no DSP dependencies and is includable
/// everywhere, including the WASM translation unit.

#include <cstdint>
#include <string_view>

namespace sonare {

/// Speaker bed layouts supported in phase 1. The integer values are part of the
/// C ABI (mirrored as SonareChannelLayout) and the JSON wire format — never
/// renumber.
enum class ChannelLayout : uint8_t {
  Mono = 0,           ///< C
  Stereo = 1,         ///< L R
  FivePointOne = 2,   ///< L R C LFE Ls Rs
  SevenPointOne = 3,  ///< L R C LFE Lss Rss Ls Rs
};

/// Per-plane speaker role, in canonical WAVE_FORMAT_EXTENSIBLE order.
enum class SpeakerRole : uint8_t { L, R, C, LFE, Ls, Rs, Lss, Rss };

/// Highest valid ChannelLayout enum value (for C-ABI uint8 validation).
inline constexpr uint8_t kMaxChannelLayoutValue =
    static_cast<uint8_t>(ChannelLayout::SevenPointOne);

/// Number of channels (planes) in a layout: 1, 2, 6, or 8.
constexpr int channel_count(ChannelLayout layout) noexcept {
  switch (layout) {
    case ChannelLayout::Mono:
      return 1;
    case ChannelLayout::Stereo:
      return 2;
    case ChannelLayout::FivePointOne:
      return 6;
    case ChannelLayout::SevenPointOne:
      return 8;
  }
  return 2;
}

/// Plane index of the LFE channel, or -1 when the layout has none.
constexpr int lfe_index(ChannelLayout layout) noexcept {
  switch (layout) {
    case ChannelLayout::Mono:
    case ChannelLayout::Stereo:
      return -1;
    case ChannelLayout::FivePointOne:
    case ChannelLayout::SevenPointOne:
      return 3;
  }
  return -1;
}

/// WAVE_FORMAT_EXTENSIBLE speaker mask (dwChannelMask). Mono/stereo return the
/// conceptual mask, but those layouts are written as plain WAVE_FORMAT_PCM with
/// no mask emitted (the WAV writer keys EXTENSIBLE on channel_count > 2, not on
/// this value).
constexpr uint32_t wave_channel_mask(ChannelLayout layout) noexcept {
  switch (layout) {
    case ChannelLayout::Mono:
      return 0x0u;  // single channel: plain PCM, no mask
    case ChannelLayout::Stereo:
      return 0x3u;  // FL FR
    case ChannelLayout::FivePointOne:
      return 0x3Fu;  // FL FR FC LFE BL BR
    case ChannelLayout::SevenPointOne:
      return 0x63Fu;  // FL FR FC LFE BL BR SL SR
  }
  return 0x0u;
}

/// Pointer to a static array of `channel_count(layout)` speaker roles in plane
/// order.
inline const SpeakerRole* speaker_roles(ChannelLayout layout) noexcept {
  using R = SpeakerRole;
  static constexpr R kMono[] = {R::C};
  static constexpr R kStereo[] = {R::L, R::R};
  static constexpr R kFiveOne[] = {R::L, R::R, R::C, R::LFE, R::Ls, R::Rs};
  static constexpr R kSevenOne[] = {R::L, R::R, R::C, R::LFE, R::Lss, R::Rss, R::Ls, R::Rs};
  switch (layout) {
    case ChannelLayout::Mono:
      return kMono;
    case ChannelLayout::Stereo:
      return kStereo;
    case ChannelLayout::FivePointOne:
      return kFiveOne;
    case ChannelLayout::SevenPointOne:
      return kSevenOne;
  }
  return kStereo;
}

/// True when `value` is a defined ChannelLayout enumerator.
constexpr bool is_valid_channel_layout(uint8_t value) noexcept {
  return value <= kMaxChannelLayoutValue;
}

/// True when a channel count maps to a surround bed (5.1 or 7.1).
constexpr bool is_surround_channel_count(int count) noexcept { return count == 6 || count == 8; }

/// Maps a channel count back to its canonical layout (1 -> mono, 2 -> stereo,
/// 6 -> 5.1, 8 -> 7.1). Any other count falls back to stereo — callers that
/// need strictness should validate the count against channel_count() first.
constexpr ChannelLayout layout_from_channel_count(int count) noexcept {
  switch (count) {
    case 1:
      return ChannelLayout::Mono;
    case 6:
      return ChannelLayout::FivePointOne;
    case 8:
      return ChannelLayout::SevenPointOne;
    default:
      return ChannelLayout::Stereo;
  }
}

/// Stable JSON/wire string for a layout ("mono" | "stereo" | "5.1" | "7.1").
constexpr const char* channel_layout_to_string(ChannelLayout layout) noexcept {
  switch (layout) {
    case ChannelLayout::Mono:
      return "mono";
    case ChannelLayout::Stereo:
      return "stereo";
    case ChannelLayout::FivePointOne:
      return "5.1";
    case ChannelLayout::SevenPointOne:
      return "7.1";
  }
  return "stereo";
}

/// Parse a layout wire string. Returns false (leaving `out` untouched) for an
/// unrecognized value, so callers can decide between fallback and error.
inline bool channel_layout_from_string(std::string_view value, ChannelLayout& out) noexcept {
  if (value == "mono") {
    out = ChannelLayout::Mono;
    return true;
  }
  if (value == "stereo") {
    out = ChannelLayout::Stereo;
    return true;
  }
  if (value == "5.1") {
    out = ChannelLayout::FivePointOne;
    return true;
  }
  if (value == "7.1") {
    out = ChannelLayout::SevenPointOne;
    return true;
  }
  return false;
}

}  // namespace sonare
