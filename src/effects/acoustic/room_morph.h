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

#include "acoustic/late_reverb.h"      // ReverbModel
#include "acoustic/rir_synthesizer.h"  // ReceiverLayout
#include "acoustic/room_model.h"       // ShoeboxRoom, SourceListener
#include "core/audio.h"
#include "effects/reverb/convolution_reverb.h"
#include "rt/processor_base.h"
#include "rt/tail_budget.h"

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
  /// The target RIR is convolved at unit energy rather than at its physical
  /// 1/(4*pi*d) scale, so a given `wet` means the same mix depth as the same
  /// dryWet on the plain convolution reverb.
  float wet = 0.5f;

  /// Target-RIR synthesis controls (see rir_synthesizer.h). Defaults preserve the
  /// historical behaviour (Eyring late model, auto mixing time, 5 ms crossfade).
  int ism_order = 3;
  unsigned seed = 1u;
  float max_seconds = 0.0f;  ///< 0 = natural target RIR length
  sonare::acoustic::ReverbModel late_model =
      sonare::acoustic::ReverbModel::Eyring;  ///< statistical tail RT60 model
  float mixing_time_ms = 0.0f;                ///< early/late crossover; 0 = auto (~sqrt(V) ms)
  float crossfade_ms = 5.0f;                  ///< equal-power crossfade width around mixing time
  /// Disabled by default so an existing config's target RIR is unchanged byte
  /// for byte. When enabled, the target late tail's per-band RT60 gains the
  /// ISO 9613-1 atmospheric-absorption term (see
  /// acoustic::shoebox_reverb_time); `air` supplies the temperature/humidity
  /// and defaults to the ISO reference climate.
  bool air_absorption_enabled = false;
  sonare::acoustic::AirAbsorption air{};
  /// Spacing (m) of the omnidirectional receiver pair centred on the listener, in (0, 4].
  /// Read only by the stereo layout (`ReceiverLayout::MonoAndPair`).
  float receiver_spacing_m = 0.5f;
};

/// Validates the target room, placement, morph controls, and every RIR synthesis
/// input (image-source order, timing, air absorption) before source samples or
/// processor state are changed. Throws ErrorCode::InvalidParameter on any value
/// RIR synthesis would refuse, since a refused synthesis yields an empty target
/// RIR that the convolution path would render as dry passthrough.
void validate_room_morph_config(const RoomMorphConfig& config);

/// @brief A morph and what the target-room synthesis had to change to make it.
/// @details Shaped like @ref sonare::acoustic::RirSynthResult because the same
///          synthesis runs underneath, and its clamps describe a room the caller
///          did not ask for. Errors are thrown rather than reported here, so the
///          list carries Warnings: an image-source order reduced to the safe
///          maximum, a tail cut against `max_seconds`, a request that produced no
///          late tail. Dropping them made a morph through a room the caller did
///          not ask for indistinguishable from one through the room they did.
struct RoomMorphResult {
  Audio audio;                          ///< morphed signal
  std::vector<Diagnostic> diagnostics;  ///< target-RIR synthesis telemetry
};

/// @brief Offline room-character morph.
///
/// `audio` is `recording.size()` samples plus the target room's reverb tail (so
/// the added reverberation is not truncated). The internal convolution latency is
/// compensated. An empty recording returns empty audio and no diagnostics.
RoomMorphResult room_morph(const Audio& recording, const RoomMorphConfig& config);

/// @brief A stereo morph: each channel convolved with its own receiver's target RIR.
struct RoomMorphStereoResult {
  Audio left;                           ///< morphed left channel
  Audio right;                          ///< morphed right channel
  std::vector<Diagnostic> diagnostics;  ///< mono + pair synthesis telemetry, deduplicated by code
};

/// @brief Offline stereo room-character morph (dual mono through a spaced receiver pair).
///
/// Runs RoomMorphProcessor with `ReceiverLayout::MonoAndPair`: left is convolved with the
/// left receiver's RIR, right with the right one's, and one linked suppressor gain is applied
/// to both. Each output is `left.size()` samples plus the reverb tail, latency-compensated.
/// Throws ErrorCode::InvalidParameter on a length or sample-rate mismatch, and on a receiver
/// pair outside the room or a spacing outside (0, 4] m.
RoomMorphStereoResult room_morph_stereo(const Audio& left, const Audio& right,
                                        const RoomMorphConfig& config);

/// @brief Streaming room-character morph.
///
/// `prepare()` synthesizes and partitions the target RIR and sizes the
/// suppressor state; `process()` allocates nothing and is real-time safe.
/// Everything synthesis would refuse is refused by the constructor except the
/// host sample rate, which prepare() sees first and rejects with
/// ErrorCode::InvalidParameter rather than discarding the diagnostics and
/// preparing an inert insert.
///
/// With `ReceiverLayout::MonoAndPair` (the default) the constructor also validates the
/// receiver pair, and prepare() loads the mono RIR plus the pair: one channel runs the mono
/// RIR, two channels run left/right. `ReceiverLayout::Mono` neither validates nor synthesizes
/// the pair. Two channels share one suppressor gain driven by the louder channel.
class RoomMorphProcessor : public rt::ProcessorBase {
 public:
  explicit RoomMorphProcessor(
      RoomMorphConfig config = {},
      sonare::acoustic::ReceiverLayout layout = sonare::acoustic::ReceiverLayout::MonoAndPair);

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  /// Latency is the underlying partitioned-convolution latency.
  int latency_samples() const noexcept override { return reverb_.latency_samples(); }
  /// Audible target-room decay follows the same prepared convolution contract.
  int tail_samples() const noexcept override {
    return rt::TailBudget::reported(reverb_.tail_samples()).samples();
  }

  /// Parameters (RT-safe):
  ///   0 = wet (target-room mix, [0,1])
  ///   1 = source_tail_suppression ([0,1])
  bool set_parameter_impl(unsigned int param_id, float value) override;
  // Automatable parameters: 0=dryWet, 1=sourceTailSuppression
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

  /// Synthesized target RIR length in samples (valid after `prepare`).
  int target_ir_size() const noexcept { return reverb_.ir_size(); }

  /// Target-RIR synthesis diagnostics from the last `prepare` (Warnings only; an
  /// Error is raised there). A host that reports what an insert changed reads
  /// them here rather than re-synthesizing the RIR to find out.
  const std::vector<Diagnostic>& diagnostics() const noexcept { return diagnostics_; }

 private:
  // State of the relative downward expander that suppresses the source
  // reverberation tail; one per processor so the channels share one gain.
  struct SuppressorState {
    float env = 0.0f;   ///< fast envelope of |x|
    float peak = 0.0f;  ///< slow peak follower (recent local maximum)
    float gain = 1.0f;  ///< smoothed gain
  };

  RoomMorphConfig config_{};
  sonare::acoustic::ReceiverLayout layout_ = sonare::acoustic::ReceiverLayout::MonoAndPair;
  reverb::ConvolutionReverb reverb_{};
  SuppressorState suppressor_{};
  std::vector<Diagnostic> diagnostics_;

  // One-pole coefficients computed from the sample rate in prepare().
  float env_attack_ = 0.0f;
  float env_release_ = 0.0f;
  float peak_release_ = 0.0f;
  float gain_smooth_ = 0.0f;
};

}  // namespace sonare::effects::acoustic
