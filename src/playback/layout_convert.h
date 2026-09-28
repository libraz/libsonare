#pragma once

/// @file layout_convert.h
/// @brief Channel-map validation and the source-bed to output-bus conversion
///        (stage [6]): the conversion table, narrowing through
///        `mixing::downmix` with the LFE dropped, and the two LFE rules.
///
/// The output bus is what every front end writes and the back end reads. For
/// speakers it is the output layout itself. For headphones it is the virtual
/// speaker slots followed by two direct-ear planes (left, right) that carry the
/// LFE fold-down past the HRIR, reflection and late-reverb paths.

#include <memory>

#include "core/channel_layout.h"
#include "playback/config.h"
#include "playback/speaker_geometry.h"

namespace sonare::playback {

/// Plane layout of the bus between the front ends and the back end.
struct OutputBus {
  TargetKind kind = TargetKind::Headphones;
  ChannelLayout speaker_layout = ChannelLayout::Stereo;         ///< speakers only
  HeadphoneSlotSet slots = headphone_slots(InputLayout::Auto);  ///< headphones only

  /// Bus planes: the speaker count, or slots + 2 direct-ear planes.
  int channel_count() const noexcept {
    return kind == TargetKind::Speakers ? sonare::channel_count(speaker_layout) : slots.count + 2;
  }
  /// Bus plane of the direct left ear (headphones only).
  int direct_left_index() const noexcept { return slots.count; }
  /// Bus plane of the direct right ear (headphones only).
  int direct_right_index() const noexcept { return slots.count + 1; }
};

/// Bus for a prepared configuration.
OutputBus make_output_bus(const PrepareConfig& config) noexcept;

/// Validates `input.channel_map`: @p length equals `channel_count(layout)` and
/// the roles cover the layout's role set exactly once.
/// @throws SonareException(InvalidParameter) describing the violation.
void validate_channel_map(ChannelLayout layout, const SpeakerRole* map, int length);

/// Bed a front end hands to the converter: the input layout, or the upmix
/// output layout when the source is stereo and the bus is not stereo speakers.
ChannelLayout converted_source_layout(ChannelLayout input, const OutputBus& bus) noexcept;

/// Stage [6]: maps a source bed onto the output bus and applies the LFE rule
/// that the bus calls for (fold-down, or pass-through to an LFE plane).
class LayoutConverter {
 public:
  LayoutConverter();
  ~LayoutConverter();
  LayoutConverter(const LayoutConverter&) = delete;
  LayoutConverter& operator=(const LayoutConverter&) = delete;

  /// Control thread. A bus without an LFE plane (stereo speakers, headphones)
  /// takes the fold-down rule, built at @p sample_rate; otherwise LFE passes to
  /// the bus LFE plane, where the speaker stage applies its own rules.
  void prepare(double sample_rate, int max_block_size, ChannelLayout source, const OutputBus& bus);
  /// Realtime: `lfe_mix_db` of the fold-down rule.
  void set_lfe_mix_db(float db) noexcept;
  /// Adds `channel_count(source)` planes into the bus planes (accumulates).
  void process(const float* const* source, float* const* bus, int frames) noexcept;
  void reset() noexcept;
  /// Frames for the fold-down filter to decay to -60 dB (0 without fold-down).
  int decay_frames() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sonare::playback
