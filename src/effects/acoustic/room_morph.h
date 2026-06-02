#pragma once

/// @file room_morph.h
/// @brief Room-character morph: nudge a recording's reverberation toward a
///        target virtual room.
///
/// This is a *creative* effect, not dereverberation. It does NOT attempt to
/// recover the dry signal. Instead it shapes the difference between the source
/// room (already baked into the recording) and a target room in two gentle
/// steps:
///   1. A light, energy-based suppression of the source reverberation tail --
///      a relative downward expander that pulls down decaying, low-level
///      content (the reverberant tail) while largely preserving direct sound
///      and onsets (the gain smoothing keeps transients near unity). Capped so
///      it never fully gates: it reduces, never removes.
///   2. Addition of the target room's reverberation by convolving with a RIR
///      synthesized from the target geometry (the same path the 5th reverb
///      engine uses).
/// The net effect moves the perceived RT60/DRR toward the target room. Because
/// the source reverb is only attenuated, the morph is directional (it adds a
/// new room more convincingly than it removes the old one), which is the
/// honest scope of a no-dereverb design.

#include <vector>

#include "acoustic/room_model.h"  // ShoeboxRoom, SourceListener
#include "core/audio.h"
#include "effects/reverb/convolution_reverb.h"
#include "rt/processor_base.h"

namespace sonare::effects::acoustic {

/// @brief Configuration for the room-character morph.
struct RoomMorphConfig {
  /// Target room geometry + wall materials (drives the synthesized target RIR).
  sonare::acoustic::ShoeboxRoom target{};
  /// Source/listener positions inside the target room.
  sonare::acoustic::SourceListener placement{};

  /// Source-reverb tail suppression amount in [0, 1]. 0 = bypass (no source
  /// suppression at all); 1 = the strongest (still partial) reduction.
  float source_tail_suppression = 0.5f;
  /// Target-room mix in [0, 1]. 0 = suppressed dry only; 1 = target room only.
  float wet = 0.5f;

  /// Target-RIR synthesis controls (see rir_synthesizer.h).
  int ism_order = 3;
  unsigned seed = 1u;
  float max_seconds = 0.0f;  ///< 0 = natural target RIR length
};

/// @brief Offline room-character morph.
///
/// Returns a buffer of length `recording.size()` plus the target room's reverb
/// tail (so the added reverberation is not truncated). The internal
/// convolution latency is compensated. An empty recording returns empty audio.
Audio room_morph(const Audio& recording, const RoomMorphConfig& config);

/// @brief Streaming room-character morph.
///
/// `prepare()` synthesizes and partitions the target RIR and sizes the
/// suppressor state; `process()` allocates nothing and is real-time safe.
class RoomMorphProcessor : public rt::ProcessorBase {
 public:
  explicit RoomMorphProcessor(RoomMorphConfig config = {});

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  /// Latency is the underlying partitioned-convolution latency.
  int latency_samples() const noexcept override { return reverb_.latency_samples(); }

  /// Parameters (RT-safe):
  ///   0 = wet (target-room mix, [0,1])
  ///   1 = source_tail_suppression ([0,1])
  bool set_parameter(unsigned int param_id, float value) override;

  /// Synthesized target RIR length in samples (valid after `prepare`).
  int target_ir_size() const noexcept { return reverb_.ir_size(); }

 private:
  // Per-channel state for the relative downward expander that suppresses the
  // source reverberation tail.
  struct SuppressorState {
    float env = 0.0f;   ///< fast envelope of |x|
    float peak = 0.0f;  ///< slow peak follower (recent local maximum)
    float gain = 1.0f;  ///< smoothed gain
  };

  RoomMorphConfig config_{};
  reverb::ConvolutionReverb reverb_{};
  std::vector<SuppressorState> suppressor_;

  // One-pole coefficients computed from the sample rate in prepare().
  float env_attack_ = 0.0f;
  float env_release_ = 0.0f;
  float peak_release_ = 0.0f;
  float gain_smooth_ = 0.0f;
};

}  // namespace sonare::effects::acoustic
