#pragma once

/// @file amp_sim.h
/// @brief Guitar amp-sim insert: drive -> tone stack -> cab-EQ in one
///        processor ("saturation.ampSim").
///
/// The electric-guitar sound is two layers: the plucked string (the
/// Karplus-Strong NativeSynth voice) and the amp/cab chain AFTER it — this
/// processor is that second, track-insert layer. It composes existing
/// blocks rather than inventing new models:
///   - drive: the Dempwolf 12AX7 triode stage (saturation::Tube, oversampled)
///     behind one [0,1] drive knob, with a drive-scaled pre-emphasis shelf in
///     front (bright-cap voicing: more drive = more grit pushed into the
///     clip).
///   - tone stack: bass / mid / treble shelving-peak biquads (RBJ designs at
///     the classic 120 Hz / 550 Hz / 3 kHz centres).
///   - cab-EQ: a fixed parametric approximation of a 4x12 close-mic response
///     (75 Hz high-pass, 110 Hz body bump, presence peak, 4th-order 4.8 kHz
///     roll-off). A real cabinet is an IR convolution and therefore data —
///     this keeps the insert data-free; hosts wanting a real cab IR layer
///     "effects.reverb.convolution" behind it.
///
/// Determinism: stateful biquads + the tube stage only; no RNG, no wall
/// clock. RT contract: prepare() allocates the per-channel filter chains and
/// the tube scratch; process()/set_parameter() are allocation-free.

#include <vector>

#include "mastering/saturation/tube.h"
#include "rt/biquad_design.h"
#include "rt/processor_base.h"

namespace sonare::mastering::saturation {

struct AmpSimConfig {
  /// Drive amount in [0, 1] (0 = clean preamp, 1 = saturated lead).
  float drive = 0.5f;
  /// Tone stack gains (dB).
  float bass_db = 0.0f;
  float mid_db = 0.0f;
  float treble_db = 0.0f;
  /// Presence peak gain (dB) on the cab voicing (3.8 kHz).
  float presence_db = 0.0f;
  /// Cab-EQ enabled (false = direct/DI tone after the tone stack).
  bool cab = true;
  /// Output trim (dB).
  float level_db = 0.0f;
};

class AmpSim : public rt::ProcessorBase {
 public:
  explicit AmpSim(AmpSimConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  const AmpSimConfig& amp_config() const { return config_; }

  // Automatable parameters (RT-safe scalar redesigns, no allocation):
  //   0 = drive (clamped to [0, 1])
  //   1 = bass_db
  //   2 = mid_db
  //   3 = treble_db
  //   4 = presence_db
  //   5 = level_db
  // `cab` is a discrete topology switch and is not exposed.
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=drive, 1=bassDb, 2=midDb, 3=trebleDb, 4=presenceDb, 5=levelDb.
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  static void validate_config(const AmpSimConfig& config);
  /// Recomputes every biquad design + gains from config_ (scalar math only).
  void design_chain();

  /// Per-channel filter states (coefficients shared via designs below).
  struct ChannelChain {
    rt::BiquadState pre;   // drive-scaled pre-emphasis shelf
    rt::BiquadState bass;  // tone stack
    rt::BiquadState mid;
    rt::BiquadState treble;
    rt::BiquadState hp;        // cab: low cut
    rt::BiquadState bump;      // cab: body bump
    rt::BiquadState presence;  // cab: presence peak
    rt::BiquadState lp1;       // cab: 4th-order roll-off
    rt::BiquadState lp2;
  };

  AmpSimConfig config_{};
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  Tube tube_;
  std::vector<ChannelChain> chains_;
  // Shared coefficient designs (refreshed by design_chain()).
  rt::BiquadCoeffs pre_c_, bass_c_, mid_c_, treble_c_, hp_c_, bump_c_, presence_c_, lp1_c_, lp2_c_;
  float level_gain_ = 1.0f;
};

}  // namespace sonare::mastering::saturation
