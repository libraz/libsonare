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
///   - microphone: an optional explicit mic stage on the cab (capsule voicing,
///     off-axis position, distance), and an optional second mic summed with the
///     first through their path-length difference — the phase interference that
///     makes a two-mic blend sound unlike either mic alone.
///   - speaker: an optional cone stage (suspension nonlinearity + the
///     excursion-driven Doppler FM of a moving radiator) between the amp and
///     the cab EQ.
///
/// `topology` selects between that voiced-filter chain and a circuit-level one
/// (a cascade of Dempwolf triode stages into the real passive tone-stack
/// network, all inside a single oversampling region). The voiced chain is the
/// default and is unchanged.
///
/// Determinism: stateful biquads + the tube stage only; no RNG, no wall
/// clock. RT contract: prepare() allocates the per-channel filter chains, the
/// mic/Doppler delay lines, the cab-IR history and the tube or oversampler
/// scratch; process()/set_parameter() are allocation-free.

#include <algorithm>
#include <vector>

#include "mastering/saturation/cab_ir.h"
#include "mastering/saturation/cab_voicing.h"
#include "mastering/saturation/tone_stack.h"
#include "mastering/saturation/tube.h"
#include "rt/adaa.h"
#include "rt/biquad_design.h"
#include "rt/nonlinearities.h"
#include "rt/oversampler.h"
#include "rt/processor_base.h"

namespace sonare::mastering::saturation {

/// Amp voicing model: the fixed circuit character of the preamp/tone section
/// (the bright-cap shelf, the triode drive curve and the tone-stack centres).
/// Selects a whole voicing profile at once; the drive/tone knobs then ride on
/// top. Independent of `CabModel` (a preset does not force a cabinet).
enum class AmpModel {
  /// Classic crunch: the original AmpSim voicing — a mid-forward British-style
  /// circuit. `amp_model == kClassicCrunch` is bit-identical to the original,
  /// so an unset field changes nothing.
  kClassicCrunch = 0,
  /// American clean: less bright-cap grit and lower gain (more headroom, breaks
  /// up later), a slightly darker mid and an airier top — the chimey
  /// clean-to-edge-of-breakup voice.
  kFenderClean = 1,
  /// Modern high-gain: a bright, tight cascaded-gain circuit — more pre-emphasis
  /// pushed into the clip and a much hotter triode drive (saturates early), with
  /// an upper-mid focus that keeps a high-gain tone articulate.
  kModernHiGain = 2,
  /// Vintage tweed: a low-wattage American circuit — a warm, dark voice with an
  /// early, spongy breakup and little bright-cap grit.
  kTweed = 3,
  /// British class-A chime: a bright, upper-mid-forward voice that stays fairly
  /// clean (chimes rather than crunches) with an airy top.
  kVoxChime = 4,
  /// Modern rectifier: the hottest voicing — a saturated, thick low end with a
  /// scooped mid and a darker top (the loose, heavy high-gain voice).
  kRectifier = 5,
};

/// How the preamp and tone section are modelled.
enum class AmpTopology {
  /// Voiced filters: one triode stage between a bright-cap shelf and three
  /// independent tone biquads. The default, and bit-identical to the original
  /// processor. Every circuit-only field below is ignored here.
  kVoiced = 0,
  /// Circuit level: a cascade of triode stages into the passive treble-mid-bass
  /// ladder, with the whole preamp / tone stack / power stage inside one
  /// oversampling region. What this buys over `kVoiced` is the two things a
  /// voiced chain cannot produce — the intermodulation density of several
  /// nonlinearities separated by filters, and a tone control that interacts
  /// (moving the mid also moves the treble) and loses 8-12 dB the way the real
  /// network does. Costs more CPU, and is deliberately NOT the default so the
  /// shipped sound never moves.
  kCircuit = 1,
};

/// Output-tube class. This scales the drive into the power stage only —
/// how early each family of output tube reaches its rails. It deliberately does
/// not set a crossover bias: how far into class AB an amp is biased is the
/// `crossover` control, a property of the amp's bias adjustment rather than of
/// the tube in the socket, and folding one into the other would claim knowledge
/// this model does not have.
/// The drive scale is derived from each tube's maximum plate dissipation rather
/// than picked by ear — see `power_tube_scale()` in amp_physics.h.
enum class PowerTube {
  /// 6L6GC, 30 W: the most headroom of the four. Scale exactly 1.0, so this is
  /// bit-identical to the original power stage.
  k6L6 = 0,
  /// EL34, 25 W: compresses slightly earlier — the midrange squash of a cranked
  /// British amp. The margin over the 6L6 is small, and honestly so: the two are
  /// close in dissipation, and most of the difference players attribute to the
  /// bottle belongs to the circuits it is usually found in.
  kEL34 = 1,
  /// EL84, 12 W: the lowest headroom here, breaking up well before the others at
  /// the same setting.
  kEL84 = 2,
  /// 6V6GT, 14 W: nearly as early as the EL84, with a soft breakup.
  k6V6 = 3,
};

/// Largest preamp cascade (`kCircuit` only). Four is already past any common
/// guitar preamp; the interesting range is two to three.
inline constexpr int kMaxPreampStages = 4;

struct AmpSimConfig {
  /// Preamp/tone modelling. Defaults to `kVoiced`, the original chain, so an
  /// unset field is bit-identical.
  AmpTopology topology = AmpTopology::kVoiced;
  /// Drive amount in [0, 1] (0 = clean preamp, 1 = saturated lead).
  float drive = 0.5f;
  /// Tone stack gains (dB).
  ///
  /// Under `kCircuit` these three are read as POT POSITIONS on the passive
  /// ladder rather than as filter gains, mapped `position = 0.5 + dB/24`
  /// clamped to [0, 1] — so 0 dB is every control centred and +-12 dB are the
  /// extremes. Reusing the same three fields keeps the param bag and the
  /// automation ids identical across topologies; a passive stack has no notion
  /// of "gain in dB" to map onto, so some reinterpretation is unavoidable and
  /// this is the one that costs no new parameters.
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
  /// Amp voicing model (preamp/tone character). Defaults to the classic crunch
  /// (the original circuit constants), so an unset field is bit-identical.
  AmpModel amp_model = AmpModel::kClassicCrunch;
  /// Output trim (dB).
  float level_db = 0.0f;
  /// Power-amp drive in [0, 1] (off-by-default; 0 = the power stage is bypassed
  /// and the chain is bit-identical to a preamp-only amp). A push-pull class-AB
  /// power section after the tone stack: a symmetric, gain-compensated soft
  /// saturation (odd-harmonic grind — even harmonics cancel in push-pull) that
  /// compresses hard-driven signals, the "cranked amp" feel a preamp alone
  /// cannot give. Antialiased with ADAA (Macak & Schimmel 2011).
  float power = 0.0f;
  /// Power-supply sag: the FRACTIONAL RAIL DROOP AT FULL OUTPUT, in [0,1]
  /// (off-by-default; 0 = a stiff supply, bit-identical to no sag). Under heavy
  /// current draw the rail voltage (B+) droops and recovers with the reservoir's
  /// time constant — so a transient attack punches through before the rail sags,
  /// then the sustain compresses (the "bloom" and touch-sensitivity of a tube amp
  /// with a soft supply). Modelled as a lagged signal envelope pulling the rail
  /// down after the power stage.
  ///
  /// The value is calibrated rather than chosen: it is `I * R / B+`, so a 5Y3
  /// (350 ohms) passing 120 mA from a 350 V rail is 0.12, and a solid-state
  /// rectifier is 0. `sag_from_supply()` in amp_physics.h does that arithmetic;
  /// the realistic range is roughly 0.05-0.15 and 1.0 is far outside it.
  ///
  /// The droop at any instant scales with the actual draw — the signal's level
  /// against the power stage's own ceiling — so a lightly played amp sags less
  /// than a hard-driven one at the same setting, as a real supply does.
  ///
  /// Gated on `power > 0`, exactly like `nfb`: with no power stage there is no
  /// plate current to pull the rail down.
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
  /// Mic capsule in front of the cab (off-by-default; `kNone` = no explicit mic
  /// stage, bit-identical to the cab's baked-in close-mic voicing). Selecting a
  /// capsule is also what makes `mic_axis` / `mic_distance_cm` do anything. The
  /// whole mic section belongs to the cab stage and is skipped when `cab` is
  /// false.
  MicModel mic_model = MicModel::kNone;
  /// Mic position across the cone in [0,1]: 0 = on-axis at the dust cap (the
  /// brightest, most aggressive spot), 1 = at the cone edge / fully off-axis
  /// (darker and smoother). Moving off-axis both drops the capsule's presence
  /// peak and pulls the cab's top-end roll-off down, which is what the position
  /// actually does on a real cab.
  float mic_axis = 0.0f;
  /// Mic distance from the grille in cm, clamped to [0, kMaxMicDistanceCm].
  /// Tonal only for a single mic: it sets how much of the capsule's proximity
  /// lift survives and how much top the air has taken off. The propagation
  /// delay is deliberately NOT applied to a single mic — absolute delay is
  /// unobservable in a single-source render and would only cost the host a
  /// latency compensation. With a second mic it is the DIFFERENCE in distance
  /// that is applied, which is the audible part.
  float mic_distance_cm = kMicReferenceDistanceCm;
  /// Second-mic blend in [0,1] (off-by-default; 0 = a single mic, bit-identical
  /// — the second mic's filters and delay line are not even stepped). 1 = the
  /// second mic alone. A linear crossfade, matching how a two-fader mic blend
  /// behaves on a desk; the two mics are strongly correlated, so an equal-power
  /// law would overshoot in the middle.
  float mic_blend = 0.0f;
  /// Second-mic capsule (see `mic_model`). `kNone` is meaningful here: it gives
  /// a second mic with the cab's baked-in voicing, so a pure distance pair is
  /// expressible as two `kNone` mics at different distances.
  MicModel mic_b_model = MicModel::kNone;
  /// Second-mic position across the cone (see `mic_axis`).
  float mic_b_axis = 0.0f;
  /// Second-mic distance in cm (see `mic_distance_cm`). The path-length
  /// difference against `mic_distance_cm` is applied as a fractional delay on
  /// whichever mic is farther, so the near mic stays at zero latency and the
  /// pair combs exactly as a real two-mic setup does.
  float mic_b_distance_cm = 15.0f;
  /// Polarity invert on the second mic (the standard fix — and the standard
  /// creative abuse — of a two-mic phase relationship).
  bool mic_b_invert = false;
  /// Speaker-cone suspension nonlinearity in [0,1] (off-by-default; 0 = a
  /// linear cone, bit-identical). A driver's spider and surround stiffen as the
  /// cone travels, and asymmetrically — so a hard-driven speaker compresses its
  /// own excursion and adds even harmonics that no amp stage produces. Modelled
  /// on the extracted low band (where the excursion lives) as a squared
  /// asymmetry term inside a travel-limiting tanh: the asymmetry scales with the
  /// excursion, so a lightly driven cone stays linear instead of being reshaped
  /// at every level. Working on the low band alone is also why it needs no
  /// antialiasing — the harmonics it generates stay decades below Nyquist.
  float cone = 0.0f;
  /// Cone-excursion Doppler depth in [0,1] (off-by-default; 0 = a stationary
  /// radiator, bit-identical). The cone that radiates the treble is the same
  /// cone the bass is moving, so the high end is frequency-modulated by the low
  /// end — the intermodulation "cry" of a driven speaker. Modelled as a
  /// fractional delay modulated by the excursion proxy. Physically the effect
  /// is small (a +-3 mm excursion is +-9 us, well under half a sample at
  /// 48 kHz), so 1.0 exaggerates it to +-2 samples to make it a usable voice
  /// rather than a technicality. Not realtime-automatable: it costs a 2-sample
  /// base delay, and reported latency must not move under a live parameter
  /// change.
  float doppler = 0.0f;
  /// Number of cascaded triode stages, clamped to [1, kMaxPreampStages].
  /// `kCircuit` only. The total small-signal gain OF THE TRIODES is held fixed
  /// and split evenly across the stages, so raising this redistributes the same
  /// gain across more, gentler clipping events rather than piling gain on gain.
  /// That redistribution is the point: one nonlinearity produces harmonics of
  /// the input, several separated by filters produce harmonics of each other's
  /// harmonics, and that is a density a single stage cannot reach at any drive
  /// setting.
  ///
  /// It is not level-neutral, and the reason is worth knowing: each stage brings
  /// its own coupling cap and cathode shelf, and those accumulate. Measured on
  /// the default voicing at drive 0.75, going from one stage to four costs about
  /// 4 dB at 1 kHz and about 8 dB at 220 Hz — the low end more, because that is
  /// where the cathode shelves are cutting. Compare stage counts at matched
  /// output level, not at matched settings.
  int preamp_stages = 2;
  /// Blocking-distortion depth in [0,1] (off-by-default; 0 = the tracker is
  /// never stepped). `kCircuit` only. When a triode's grid swings positive it
  /// draws grid current, which charges the coupling cap and pushes the stage's
  /// operating point more negative; the stage then recovers with the cap's time
  /// constant. The result is the sputtering, gated collapse a heavily
  /// overdriven cascade makes on hard attacks. Modelled from the same triode
  /// law's grid-current equation, tracked with the coupling cap's own RC and
  /// subtracted from the following sample's grid voltage.
  float bias_shift = 0.0f;
  /// Class-AB crossover in [0,1] (off-by-default; 0 = class A, and the power
  /// stage is then bit-identical to a plain symmetric saturation). Biases the
  /// two halves of the push-pull pair apart, so neither conducts through the
  /// zero crossing and the composite curve has a low-gain region around it —
  /// the buzzy odd-harmonic notch of a coldly biased amp, which appears on
  /// small signals and disappears once the signal clears the dead zone. Only
  /// active when the power stage is (`power > 0`).
  ///
  /// The physical setting behind it is the idle plate dissipation as a fraction
  /// of the tube's maximum — the number an amp tech sets. Use
  /// `crossover_from_bias_fraction()` in amp_physics.h to work in those terms:
  /// a hot 0.7 gives 0, a cool 0.5 about 0.5, a cold 0.35 about 0.88.
  float crossover = 0.0f;
  /// Output-tube class (see PowerTube). Scales the drive into the power stage
  /// only; `k6L6` is unity and therefore bit-identical.
  PowerTube power_tube = PowerTube::k6L6;
};

class AmpSim : public rt::ProcessorBase {
 public:
  explicit AmpSim(AmpSimConfig config = {});
  void prepare(double sample_rate, int max_block_size) override;
  /// Channel-aware prepare: an offline mono or stereo caller pays for the
  /// channels it will actually pass to process() instead of the realtime cap.
  /// The per-channel state here is not scalar — each channel owns a cab-IR
  /// ring, a Doppler line and two mic delay lines — so preparing 64 of them for
  /// a mono render is the difference between megabytes and tens of megabytes.
  /// process() still grows past @p max_channels if a caller hands over more,
  /// exactly as the two-argument form does.
  void prepare(double sample_rate, int max_block_size, int max_channels) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  /// The head's oversampling latency, plus the Doppler stage's base delay when
  /// that stage is on (its modulation swings around a fixed 2-sample centre, so
  /// the centre is real latency the host should compensate). Neither term can
  /// move at runtime: `topology` is not automatable and neither is `doppler`.
  /// The cab IR adds nothing — it is a direct FIR, not a partitioned one.
  int latency_samples() const noexcept override {
    const int head = config_.topology == AmpTopology::kCircuit ? circuit_latency_samples_
                                                               : tube_.latency_samples();
    return head + (config_.doppler > 0.0f ? kDopplerBaseSamples : 0);
  }

  /// @brief Loads a cabinet impulse response, replacing the analytic cab chain.
  /// @param impulse_response IR samples, mono.
  /// @param num_samples Length. Anything past the kMaxCabIrMs budget at the
  ///        processor's rate is truncated.
  /// @param ir_sample_rate Rate the IR was captured at. Pass 0 (the default) to
  ///        declare it already at the processor's rate, which is the old
  ///        behaviour and the convolution reverb insert's contract. Pass the
  ///        real rate and the IR is resampled to match instead — a 48 kHz
  ///        capture used in a 96 kHz session is otherwise transposed an octave,
  ///        silently and with nothing to show for it but a wrong-sounding cab.
  /// @details A real cab IR is a mic'd capture, so it replaces the modelled
  ///          microphone as well as the cab EQ — layering the mic stage on top
  ///          would put two microphones in front of one speaker. Passing an
  ///          empty IR returns to the analytic chain.
  ///
  ///          The IR is kept as given and re-derived whenever the processor's
  ///          rate changes, so loading before prepare() is safe and a later
  ///          prepare() at a different rate resamples rather than mis-tunes.
  ///          Nothing is normalized: the IR's own gain is part of the capture,
  ///          matching the convolution reverb.
  ///
  ///          Control thread only (allocates). This layer never opens a file —
  ///          decode with `Audio::from_file()` (it downmixes to mono) and hand
  ///          over `data()`, `size()` and `sample_rate()`.
  void load_cab_ir(const float* impulse_response, int num_samples, double ir_sample_rate = 0.0);
  void load_cab_ir(const std::vector<float>& impulse_response, double ir_sample_rate = 0.0);

  /// @brief Uses a cabinet IR synthesized from the model in `cab_ir.h` instead of
  ///        a captured one.
  /// @param spec Cabinet, capsule and mic placement to generate from.
  /// @details The point of generating rather than loading is that a cab IR is
  ///          otherwise a recording, with a licence and a download attached to
  ///          it. What it buys over the analytic cab is the cabinet's geometry —
  ///          the neighbouring drivers a microphone in front of one speaker also
  ///          hears, and the distance-dependent comb that comes with them.
  ///
  ///          The spec is kept and the IR re-generated at the processor's own
  ///          rate on every prepare(), so nothing is ever resampled and a session
  ///          rate change re-derives the cabinet rather than transposing it. This
  ///          replaces any loaded IR, and a later `load_cab_ir()` replaces this.
  ///
  ///          Control thread only (allocates).
  void load_generated_cab_ir(const CabIrSpec& spec);

  /// True when an IR has been loaded or generated, whether or not prepare() has
  /// derived the convolved copy yet — this answers "did my IR take", which a
  /// caller can ask before the processor is prepared.
  bool has_cab_ir() const noexcept { return !cab_ir_source_.empty() || cab_ir_generated_; }
  /// Length of the IR actually convolved, after resampling and truncation to the
  /// duration budget. Zero until prepare() has run.
  int cab_ir_samples() const noexcept { return static_cast<int>(cab_ir_.size()); }
  /// Channels the last prepare() reserved per-channel state for. Each channel
  /// costs a cab-IR ring, a Doppler line and two mic delay lines, so an offline
  /// caller that prepared for one channel can confirm it is paying for one
  /// rather than for the realtime cap. Grows if process() is later handed more
  /// channels than prepare() was told about.
  int prepared_channels() const noexcept { return static_cast<int>(chains_.size()); }
  /// Two stages outlive their input: the second mic's path-length delay, and a
  /// loaded or generated cab IR, whose direct FIR keeps emitting for its length
  /// minus one after the last input sample. The two are mutually exclusive in
  /// the signal path (an IR replaces the whole analytic cab-and-mic chain), so
  /// the longer of them bounds the decay. Everything else is IIR filtering with
  /// no discrete tail.
  int tail_samples() const noexcept override {
    const int ir_tail = cab_ir_.empty() ? 0 : static_cast<int>(cab_ir_.size()) - 1;
    return std::max(mic_tail_samples_, ir_tail);
  }
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
  //  10 = mic_axis (clamped to [0, 1])
  //  11 = mic_b_axis (clamped to [0, 1])
  //  12 = mic_blend (clamped to [0, 1])
  //  13 = cone (clamped to [0, 1])
  //  14 = crossover (clamped to [0, 1])
  //  15 = bias_shift (clamped to [0, 1])
  // `cab`/`cab_model`/`amp_model`/`mic_model`/`mic_b_model`/`mic_b_invert`/
  // `power_tube` are discrete switches, so they are not exposed. Neither are
  // the two mic distances (they set delay-line taps, which cannot move
  // mid-stream without a click), `doppler` (it would move the reported
  // latency), `topology` (same, plus it selects which state was prepared) nor
  // `preamp_stages` (it changes how many filter states are in the signal path).
  bool set_parameter(unsigned int param_id, float value) override;
  // Automatable parameters: 0=drive, 1=bassDb, 2=midDb, 3=trebleDb, 4=presenceDb,
  // 5=levelDb, 6=power, 7=sag, 8=transformer, 9=nfb, 10=micAxis, 11=micBAxis,
  // 12=micBlend, 13=cone, 14=crossover, 15=biasShift.
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

 private:
  /// Doppler modulation centre, in samples. The delay swings over
  /// [0, 2 * kDopplerBaseSamples], so the centre is the stage's added latency.
  static constexpr int kDopplerBaseSamples = 2;

  static void validate_config(const AmpSimConfig& config);
  /// Recomputes every biquad design + gains from config_ (scalar math only).
  void design_chain();
  /// Sizes the per-channel delay lines from the current design. Allocates, so
  /// it runs on the control thread only (prepare, and the channel-count growth
  /// path that mirrors Tube::ensure_state).
  void allocate_delay_lines();
  /// Sizes the oversampling scratch and @p num_channels resampler states for the
  /// circuit head. Allocates; control thread only, and never called under
  /// kVoiced (where the Tube stage owns the oversampler instead).
  void allocate_circuit_scratch(int num_channels);
  /// Sizes the per-channel cab-IR history ring. Allocates; control thread only.
  void allocate_cab_ir_history();

  /// The voiced head: pre-emphasis -> single triode stage -> three tone biquads
  /// -> power stage, all as before.
  void process_voiced_head(float* const* channels, int num_channels, int num_samples);
  /// The circuit head: one oversampling region holding the triode cascade, the
  /// passive tone stack and the power stage.
  void process_circuit_head(float* const* channels, int num_channels, int num_samples);
  /// Everything after the power stage, shared by both heads: sag, output
  /// transformer, cone/Doppler, cab (analytic or IR) and output trim.
  void process_tail(float* const* channels, int num_channels, int num_samples);

  /// One cabinet-plus-mic filter state (one per mic).
  struct CabStage {
    rt::BiquadState hp;        // cab: low cut
    rt::BiquadState bump;      // cab: body bump
    rt::BiquadState presence;  // cab: presence peak
    rt::BiquadState lp1;       // cab: 4th-order roll-off
    rt::BiquadState lp2;
    rt::BiquadState mic_prox;      // mic: distance-scaled proximity shelf
    rt::BiquadState mic_presence;  // mic: capsule presence peak
    rt::BiquadState mic_top;       // mic: off-axis + distance top-end shelf
  };

  /// One cabinet-plus-mic coefficient set (one per mic; shared across channels).
  /// Runs one cabinet-plus-mic chain. The mic biquads are stepped only when the
  /// design carries a capsule, so a capsule-less design is exactly the original
  /// five-biquad cabinet.
  static float process_cab(float x, CabStage& stage, const CabDesign& design) noexcept;

  /// One triode stage of the circuit-head cascade (kCircuit only).
  struct PreampStage {
    float hp_x1 = 0.0f;  // interstage coupling cap (one-pole high-pass)
    float hp_y1 = 0.0f;
    rt::BiquadState cathode;  // cathode-bypass shelf
    float grid_env = 0.0f;    // tracked grid current, i.e. the bias shift
  };

  /// Per-channel filter states (coefficients shared via designs below).
  struct ChannelChain {
    rt::BiquadState pre;   // drive-scaled pre-emphasis shelf
    rt::BiquadState bass;  // tone stack
    rt::BiquadState mid;
    rt::BiquadState treble;
    PreampStage stages[kMaxPreampStages];  // circuit head: triode cascade
    ToneStackState stack;                  // circuit head: passive ladder
    std::vector<float> cab_ir_history;     // cab-IR ring (empty when no IR)
    size_t cab_ir_write = 0;
    CabStage cab_a;                   // first mic
    CabStage cab_b;                   // second mic (stepped only when mic_blend > 0)
    float sag_env = 0.0f;             // lagged rail-droop envelope (power-supply sag)
    float xf_lp = 0.0f;               // transformer low-band extractor (one-pole lowpass)
    rt::BiquadState nfb_shape;        // NFB feedback-path mid-band filter
    float nfb_fb = 0.0f;              // one-sample-delayed power-stage output (NFB loop)
    float cone_lp = 0.0f;             // cone-excursion proxy (one-pole lowpass)
    float cone_dc = 0.0f;             // tracked offset of the cone's rectified term
    std::vector<float> doppler_line;  // excursion-modulated delay (empty when off)
    size_t doppler_write = 0;
    std::vector<float> mic_a_line;  // mic path-length delay (empty when unused)
    size_t mic_a_write = 0;
    std::vector<float> mic_b_line;
    size_t mic_b_write = 0;

    /// Zeroes every filter/envelope state and the delay lines while keeping the
    /// lines' capacity, so reset() never allocates.
    void clear() noexcept;
  };

  /// The push-pull power stage plus its optional feedback loop. Shared by both
  /// heads so the two topologies cannot drift apart on the one stage they have
  /// in common; only the rate it runs at differs.
  float run_power_stage(float s, ChannelChain& chain, size_t channel) noexcept;

  AmpSimConfig config_{};
  bool prepared_ = false;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  Tube tube_;
  std::vector<ChannelChain> chains_;
  /// Per-channel push-pull power-amp saturation state. Only stepped when
  /// config_.power > 0, so the preamp-only path stays bit-identical; and at
  /// crossover 0 the pair reduces exactly to the symmetric tanh it replaced.
  std::vector<rt::Adaa1<rt::PushPullNonlinearity>> power_adaa_;

  // --- Circuit head (kCircuit only; all inert and unallocated under kVoiced) --
  /// Oversampling factor of the circuit head's single region. The same 4x the
  /// voiced head's Tube stage uses, so the two heads alias comparably.
  static constexpr int kCircuitOversample = 4;
  rt::Oversampler oversampler_{kCircuitOversample};
  std::vector<rt::Oversampler::StreamingState> oversampler_states_;
  std::vector<float> up_scratch_, down_scratch_;
  int circuit_latency_samples_ = 0;
  int active_stages_ = 1;
  /// Grid bias operating point (V), the grid volts one normalized unit of signal
  /// is worth, the idle plate current subtracted off, and the plate-current
  /// scaling that turns one stage into its documented small-signal gain.
  float stage_bias_v_ = 0.0f;
  float stage_volts_per_unit_ = 0.0f;
  float stage_idle_ma_ = 0.0f;
  float stage_scale_ = 0.0f;
  /// Trim applied at the end of the circuit head so it lands at the same
  /// internal level as the voiced head. Everything downstream (power, sag,
  /// transformer, cone) is calibrated against that level, so the two heads have
  /// to agree on it or every one of those knobs would mean something different
  /// per topology.
  float circuit_makeup_ = 1.0f;
  /// Interstage coupling-cap one-pole coefficient, at the oversampled rate.
  float coupling_alpha_ = 0.0f;
  /// Grid-current tracker coefficients: fast while the cap charges through the
  /// conducting grid, slow while it discharges through the grid leak.
  float grid_charge_alpha_ = 0.0f;
  float grid_discharge_alpha_ = 0.0f;
  rt::BiquadCoeffs cathode_c_;
  ToneStackCoeffs stack_c_;

  /// Cab impulse response and its ring mask. Empty when the analytic cab chain
  /// is in use. The ring is a power of two so the FIR walk needs a mask, not a
  /// modulo.
  std::vector<float> cab_ir_;
  size_t cab_ir_mask_ = 0;
  /// The IR exactly as the caller supplied it, plus the rate it was captured at
  /// (0 = "whatever the processor runs at"). Kept so a later prepare() at a
  /// different rate can re-derive `cab_ir_` from the original rather than from
  /// an already-resampled, already-truncated copy.
  std::vector<float> cab_ir_source_;
  double cab_ir_source_rate_ = 0.0;
  /// A synthesized cabinet, when one was requested. Held as the SPEC rather than
  /// as samples so every prepare() re-generates at the processor's own rate,
  /// which is the one case where no resampling is needed at all.
  CabIrSpec cab_ir_spec_{};
  bool cab_ir_generated_ = false;

  /// Rebuilds `cab_ir_`: generate at the current rate for a synthesized cabinet,
  /// otherwise resample the source if the rates differ and truncate to the
  /// kMaxCabIrMs budget. Allocates; control thread only.
  void rebuild_cab_ir();

  // Shared coefficient designs (refreshed by design_chain()).
  rt::BiquadCoeffs pre_c_, bass_c_, mid_c_, treble_c_;
  CabDesign cab_a_c_, cab_b_c_;   // cabinet + mic, one per mic
  rt::BiquadCoeffs nfb_shape_c_;  // NFB feedback-path mid-band filter
  float level_gain_ = 1.0f;
  /// Output-tube drive scale (PowerTube). 1.0 for k6L6, so the default power
  /// stage is untouched.
  float power_drive_scale_ = 1.0f;
  /// Path-length delays of the two mics in Q8.8 samples. Only the FARTHER mic
  /// is delayed, so at least one of these is always zero and the pair costs no
  /// latency — only the difference between them is audible.
  int mic_a_delay_q8_ = 0;
  int mic_b_delay_q8_ = 0;
  /// Delay-line capacity for the mic pair, in samples (0 when the second mic is
  /// off). Set by design_chain(), consumed by allocate_delay_lines().
  int mic_line_capacity_ = 0;
  /// Whole-sample mic delay reported as the processor's tail.
  int mic_tail_samples_ = 0;
  /// Power-supply sag envelope smoothing coefficient (per sample; ~40 ms cap
  /// recovery). Set from the sample rate in design_chain().
  float sag_alpha_ = 0.0f;
  /// Transformer low-band lowpass coefficient (per sample; ~120 Hz corner). Set
  /// from the sample rate in design_chain().
  float xf_alpha_ = 0.0f;
  /// Cone-excursion extractor coefficient (per sample; ~90 Hz corner — the band
  /// where a driver's displacement actually lives). Set in design_chain().
  float cone_alpha_ = 0.0f;
  /// Offset-tracker coefficient for the cone's rectified term (per sample; ~5 Hz).
  float cone_dc_alpha_ = 0.0f;
  /// Doppler modulation depth in Q8.8 samples, around the kDopplerBaseSamples
  /// centre. Zero when the stage is off.
  int doppler_mod_q8_ = 0;
};

}  // namespace sonare::mastering::saturation
