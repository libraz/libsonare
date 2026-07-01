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
#include "rt/adaa.h"
#include "rt/biquad_design.h"
#include "rt/nonlinearities.h"
#include "rt/processor_base.h"

namespace sonare::mastering::saturation {

/// Cabinet voicing model. Selects the fixed EQ centres of the cab-EQ stage
/// (`cab == true`); has no effect when the cab EQ is bypassed.
enum class CabModel {
  /// Guitar 4x12, close-mic: 75 Hz cut, 110 Hz body bump, 3.8 kHz presence,
  /// 4.8 kHz roll-off (the original AmpSim voicing).
  kGuitar4x12 = 0,
  /// Bass 8x10 (SVT-style): extends lower (40 Hz cut, 80 Hz body bump), a
  /// darker, presence-restrained top (2.2 kHz presence, 3.5 kHz roll-off) so a
  /// bass NativeSynth voice sits in a big-cab response rather than a guitar's.
  kBass8x10 = 1,
};

struct AmpSimConfig {
  /// Drive amount in [0, 1] (0 = clean preamp, 1 = saturated lead).
  float drive = 0.5f;
  /// Tone stack gains (dB).
  float bass_db = 0.0f;
  float mid_db = 0.0f;
  float treble_db = 0.0f;
  /// Presence peak gain (dB) on the cab voicing (3.8 kHz).
  float presence_db = 0.0f;
  /// Cab-EQ enabled (false = direct/DI tone after the tone stack, e.g. to feed
  /// a host convolution cab IR downstream).
  bool cab = true;
  /// Cab voicing model (only meaningful when `cab == true`). Defaults to the
  /// guitar 4x12, so an unset field is bit-identical to the original voicing.
  CabModel cab_model = CabModel::kGuitar4x12;
  /// Output trim (dB).
  float level_db = 0.0f;
  /// Power-amp drive in [0, 1] (off-by-default; 0 = the power stage is bypassed
  /// and the chain is bit-identical to a preamp-only amp). A push-pull class-AB
  /// power section after the tone stack: a symmetric, gain-compensated soft
  /// saturation (odd-harmonic grind — even harmonics cancel in push-pull) that
  /// compresses hard-driven signals, the "cranked amp" feel a preamp alone
  /// cannot give. Antialiased with ADAA (Macak & Schimmel 2011).
  float power = 0.0f;
  /// Power-supply sag in [0,1] (off-by-default; 0 = a stiff supply, bit-identical
  /// to no sag). Under heavy current draw the rail voltage (B+) droops and
  /// recovers with the reservoir-cap time constant — so a transient attack
  /// punches through before the rail sags, then the sustain compresses (the
  /// "bloom" and touch-sensitivity of a tube amp with a soft supply). Modelled
  /// as a lagged signal envelope pulling the rail down after the power stage.
  float sag = 0.0f;
  /// Output-transformer core saturation in [0,1] (off-by-default; 0 = a linear
  /// transformer, bit-identical to no saturation). The core magnetises with the
  /// flux (the integral of the voltage), so it saturates at LOW frequencies —
  /// a frequency-dependent nonlinearity that thickens and gently compresses the
  /// bass (the "thump" of a real output transformer, strongest on bass amps).
  /// Modelled as a soft saturation of the extracted low band only (Macak 2011).
  float transformer = 0.0f;
  /// Global negative feedback (NFB) depth in [0,1] (off-by-default; 0 = an
  /// open-loop power stage, bit-identical to no NFB). A real power amp feeds a
  /// portion of its output back to an earlier stage with inverted polarity,
  /// which tightens and de-distorts the band it covers. The feedback path is a
  /// wide mid-band filter, so the midrange sees strong feedback (tight, flat)
  /// while the extremes see little — the top opens up (the "presence" of an NFB
  /// loop) and the low end blooms (the "resonance"/"depth"). Modelled as a
  /// one-sample-delay feedback loop around the power stage, so it is only active
  /// when the power stage is (`power > 0`). (Macak & Schimmel 2011.)
  float nfb = 0.0f;
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
  //   6 = power (clamped to [0, 1])
  //   7 = sag (clamped to [0, 1])
  //   8 = transformer (clamped to [0, 1])
  //   9 = nfb (clamped to [0, 1])
  // `cab`/`cab_model` are discrete topology switches and are not exposed.
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=drive, 1=bassDb, 2=midDb, 3=trebleDb, 4=presenceDb,
  // 5=levelDb, 6=power, 7=sag, 8=transformer, 9=nfb.
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
    float sag_env = 0.0f;       // lagged rail-droop envelope (power-supply sag)
    float xf_lp = 0.0f;         // transformer low-band extractor (one-pole lowpass)
    rt::BiquadState nfb_shape;  // NFB feedback-path mid-band filter
    float nfb_fb = 0.0f;        // one-sample-delayed power-stage output (NFB loop)
  };

  AmpSimConfig config_{};
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  Tube tube_;
  std::vector<ChannelChain> chains_;
  /// Per-channel push-pull power-amp saturation state (ADAA tanh). Only stepped
  /// when config_.power > 0, so the preamp-only path stays bit-identical.
  std::vector<rt::Adaa1<rt::TanhNonlinearity>> power_adaa_;
  // Shared coefficient designs (refreshed by design_chain()).
  rt::BiquadCoeffs pre_c_, bass_c_, mid_c_, treble_c_, hp_c_, bump_c_, presence_c_, lp1_c_, lp2_c_;
  rt::BiquadCoeffs nfb_shape_c_;  // NFB feedback-path mid-band filter
  float level_gain_ = 1.0f;
  /// Power-supply sag envelope smoothing coefficient (per sample; ~40 ms cap
  /// recovery). Set from the sample rate in design_chain().
  float sag_alpha_ = 0.0f;
  /// Transformer low-band lowpass coefficient (per sample; ~120 Hz corner). Set
  /// from the sample rate in design_chain().
  float xf_alpha_ = 0.0f;
};

}  // namespace sonare::mastering::saturation
