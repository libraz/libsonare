#include "midi/synth/percussion_voice.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

#include "midi/synth/bessel.h"
#include "midi/synth/pitch.h"
#include "util/constants.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

namespace {

using sonare::constants::kInvSqrt2;
using sonare::constants::kPi;
using sonare::constants::kSoundSpeedMps;
using sonare::constants::kTwoPi;
/// Noise draws live far above any other per-voice index range.
constexpr uint64_t kNoiseIndexBase = 1ull << 20;
/// Wire-rattle draws live above the noise-layer range so the two streams
/// stay decorrelated.
constexpr uint64_t kWireIndexBase = 1ull << 24;
/// Shimmer draws live above the wire-rattle range.
constexpr uint64_t kShimmerIndexBase = 1ull << 28;
/// PhISEM collision-probability draws and the particle-noise draws live in two
/// disjoint ranges above the shimmer range so all streams stay decorrelated.
constexpr uint64_t kPhisemProbIndexBase = 1ull << 30;
constexpr uint64_t kPhisemNoiseIndexBase = 1ull << 31;
/// Random bead collisions per bean per unit shake energy per second.
SONARE_TUNABLE(kPhisemCollisionRate, 100.0f);
/// Shake energy at zero velocity, as a fraction of its energy at full. The
/// energy scales both how loud a collision is and how often one happens, so it
/// reaches the output twice and reaches it on top of the velocity response the
/// voice already has. Left at the modes' and the noise band's own slope it made
/// every shaker and scraper swing 8 dB further from soft to hard than anything
/// else in the kit, which no sampled kit does — theirs swing no wider than
/// their drums. Held high enough that what velocity still carries here is the
/// collision rate, which is a shaker's own cue and not a level.
SONARE_TUNABLE(kPhisemVelocityFloor, 0.9f);

/// Pressure a compact axial dipole radiates per unit kL, against the monopole
/// of equal displacement. Not kInvSqrt2's neighbour: it is the solid-angle
/// average of cos(theta) and belongs to this pair and no other.
constexpr float kDipolePerKl = 0.57735026919f;

/// Below this the piece has stopped radiating. A hundred decibels under a
/// full-scale strike, so a slot is freed well after the last audible sample
/// and never while one is still coming.
constexpr float kSilenceFloor = 1.0e-5f;
/// Time constant of the peak follower that reads the level above. Short enough
/// that a dead slot is reclaimed promptly, long enough that the gap between two
/// collisions of a shaker does not read as the end of the shake.
constexpr float kSilenceFollowerMs = 40.0f;

float radius_for(double sample_rate, float t60_s) noexcept {
  return std::exp(-6.907755279f / (static_cast<float>(sample_rate) * std::max(0.005f, t60_s)));
}

/// Longest a single mode may ring, mirroring the ceiling `mode_decay_s` itself
/// carries. A mode far below the base divides the patch's decay by a small
/// number, and without this a low enough ratio reaches a radius of exactly 1.
constexpr float kMaxModeDecayS = 30.0f;

/// Magnitude spectrum of a raised-cosine contact force of duration tau, read at
/// x = f * tau and normalised to unity at DC: |sinc(x) / (1 - x^2)|.
float contact_spectrum(float x) noexcept {
  const float ax = std::abs(x);
  if (ax < 1.0e-6f) return 1.0f;
  // sinc and the denominator go to zero together at x = 1, where the limit is
  // 1/2 and the quotient is two cancellations divided by each other.
  if (std::abs(ax - 1.0f) < 1.0e-3f) return 0.5f;
  const float s = std::sin(kPi * ax) / (kPi * ax);
  return std::abs(s / (1.0f - ax * ax));
}

/// Piston-equivalent area fraction 2 J_1(alpha) / alpha of an axisymmetric mode
/// — the share of its displacement that moves air, and so what the cavity's
/// spring acts on. 0.432 for the (0,1), -0.123 for the (0,2).
/// (2m+1)!!, which is what divides a compact 2m-pole's radiated amplitude down
/// from the monopole's. Without it the multipole law crosses over at ka = 1 for
/// every order, so above that a mode with four nodal diameters radiates as well
/// as the one that moves the whole head — the crossover is at ka ~ m, not 1.
constexpr float kMultipoleNorm[] = {1.0f, 3.0f, 15.0f, 105.0f, 945.0f, 10395.0f};

float piston_area(float alpha) noexcept {
  if (alpha <= 1.0e-6f) return 1.0f;
  return 2.0f * bessel_j(1, alpha) / alpha;
}

}  // namespace

void PercussionVoiceCore::start(const PercussionPatchParams& params, double sample_rate,
                                uint8_t note, uint8_t velocity, uint64_t seed) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  noise_ = VoiceRandomSequence(seed);
  noise_index_ = 0;
  // Starts at full scale so the follower has to fall the whole way before a
  // slot can be reclaimed; a piece that opens quietly cannot read as finished
  // on its first sample.
  silence_env_ = 1.0f;
  silence_coeff_ = std::exp(-1.0f / (kSilenceFollowerMs * 0.001f * static_cast<float>(sr)));

  const float base_hz = params.base_freq_hz > 0.0f ? params.base_freq_hz : note_to_hz(note);
  const float vel01 = static_cast<float>(velocity & 0x7Fu) / 127.0f;

  // Membrane modes. The strike is a force with a duration and its spectrum is
  // how much of each mode it reaches; with mallet_ms at 0 it is the older law
  // in the mode index, which knows nothing about where the modes sit.
  const int asked = std::clamp(params.num_modes, 0, kMaxPercussionModes);
  tone_gain_ = std::max(0.0f, params.tone_gain);
  tone_peak_ = 0.0f;
  const float nyquist_limit = 0.45f * static_cast<float>(sr);
  // The head is still stretched while the stick is on it, so the force spectrum
  // is read at the frequency the mode starts on rather than its resting one.
  const float start_ratio = 1.0f + std::max(0.0f, params.pitch_drop);
  const float tau_s = params.mallet_ms > 0.0f ? 0.001f * params.mallet_ms *
                                                    std::pow(std::max(vel01, 1.0f / 127.0f),
                                                             -std::max(0.0f, params.mallet_vel_exp))
                                              : 0.0f;
  const float air_spring = std::max(0.0f, params.air_spring);
  // How fast damping rises with frequency. One is the law this engine has
  // always used and stays the sentinel's meaning; a membrane in air measures
  // near a half, its octave bands losing about 1.45x the rate of the one below
  // rather than twice it.
  const float decay_exp = params.mode_decay_exp > 0.0f ? params.mode_decay_exp : 1.0f;
  int placed = 0;

  // The slot a mode lands in is not the slot the patch wrote it in — a ratio of
  // 0 and a mode over Nyquist are both dropped, and a spawned partner needs a
  // slot of its own. `index` stays the patch's, because the older amplitude law
  // is keyed on it and a compacted index would re-voice every piece with a gap.
  const auto place = [&](int index, float ratio, int m, float alpha, bool dipole) {
    if (placed >= kMaxPercussionModes) return;
    const float placed_ratio = std::max(0.01f, ratio);
    const float freq = base_hz * placed_ratio;
    if (freq <= 0.0f || freq >= nyquist_limit) return;
    Mode& mode = modes_[static_cast<size_t>(placed)];
    mode = Mode{};
    mode.omega = kTwoPi * freq / static_cast<float>(sr);
    // Damping rises with frequency, read against the base. Unclamped below 1 so
    // that moving the base rescales every mode together and `mode_decay_s`
    // absorbs it — pinning the low modes would make the base's placement, which
    // is free, decide the decay of a fixed set of frequencies.
    const float damping = std::pow(placed_ratio, decay_exp);
    mode.r =
        radius_for(sr, std::min(kMaxModeDecayS, std::max(0.005f, params.mode_decay_s) / damping));
    // Strike-point weighting: each membrane mode is excited by the value of
    // its shape J_m(alpha_mn * r) * cos(m * theta) at the strike. A centre
    // hit (strike_r == 0) is the legacy uniform excitation.
    float strike_pos = 1.0f;
    if (params.strike_r > 0.0f) {
      strike_pos = std::abs(bessel_j(m, alpha * params.strike_r) *
                            std::cos(static_cast<float>(m) * params.strike_theta));
    }
    const float strike =
        tau_s > 0.0f ? contact_spectrum(freq * start_ratio * tau_s)
                     : (index == 0 ? 1.0f : (0.4f + 0.4f * vel01) / static_cast<float>(index + 1));
    // Displacement is not pressure: a mode with m nodal diameters moves no net
    // volume and radiates as a 2m-pole, and the pair member whose heads move
    // together is an axial dipole of the shell's depth.
    float radiation = 1.0f;
    const float wavenumber = kTwoPi * freq / kSoundSpeedMps;
    if (m >= 1 && params.head_diameter_m > 0.0f) {
      const int order = std::min(m, static_cast<int>(std::size(kMultipoleNorm)) - 1);
      radiation = std::min(
          1.0f, std::pow(wavenumber * 0.5f * params.head_diameter_m, static_cast<float>(m)) /
                    kMultipoleNorm[order]);
    } else if (m == 0 && dipole && params.shell_depth_m > 0.0f) {
      radiation = std::min(1.0f, wavenumber * params.shell_depth_m * kDipolePerKl);
    }
    mode.gain = strike * std::sin(mode.omega) * strike_pos * radiation;
    // The head's own peak swing at unit excitation. Every mode is impulse-
    // excited in phase, so the in-phase sum of the two-pole peaks (gain/sin w,
    // read at the frequency the strike starts on) is the bound the membrane
    // cannot exceed. It is what the wire gate measures its threshold against.
    const float w0 = std::min(mode.omega * start_ratio, 0.95f * kPi);
    tone_peak_ += mode.gain / std::max(1.0e-6f, std::sin(w0));
    ++placed;
  };

  // The (0,1)'s piston area and ratio: the air spring's split is stated against
  // it, so a higher axisymmetric mode splits by what it moves and how stiff it
  // is rather than by the same factor.
  float pair_area = 0.0f;
  float pair_ratio = 0.0f;
  for (int k = 0; k < asked; ++k) {
    const float ratio = params.mode_ratios[static_cast<size_t>(k)];
    if (ratio <= 0.0f) continue;
    const int m = static_cast<int>(params.mode_m[static_cast<size_t>(k)]);
    const float alpha = params.mode_alpha[static_cast<size_t>(k)];
    const bool splits = air_spring > 0.0f && m == 0;
    place(k, ratio, m, alpha, splits);
    if (!splits) continue;
    const float area = piston_area(alpha);
    if (pair_ratio <= 0.0f) {
      pair_area = area;
      pair_ratio = ratio;
    }
    const float area_share = pair_area != 0.0f ? area / pair_area : 0.0f;
    const float stiffness_share = pair_ratio / ratio;
    place(k,
          ratio * std::sqrt(1.0f + air_spring * area_share * area_share * stiffness_share *
                                       stiffness_share),
          m, alpha, false);
  }
  num_modes_ = placed;
  for (int k = placed; k < kMaxPercussionModes; ++k) modes_[static_cast<size_t>(k)] = Mode{};

  // Descending pitch envelope.
  drop_state_ = std::max(0.0f, params.pitch_drop);
  drop_coeff_ =
      std::exp(-1.0f / (std::max(1.0f, params.pitch_drop_ms) * 0.001f * static_cast<float>(sr)));
  cached_ratio_ = 0.0f;
  excite_ = num_modes_ > 0;

  // Noise layer.
  noise_level_ = std::max(0.0f, params.noise_gain) * (0.6f + 0.4f * vel01);
  noise_coeff_ =
      std::exp(-1.0f / (std::max(1.0f, params.noise_decay_ms) * 0.001f * static_cast<float>(sr)));
  noise_output_ = params.noise_output;
  noise_filter_.prepare(sr);
  noise_filter_.set(params.noise_cutoff_hz, std::max(0.5f, params.noise_q));
  noise_filter_.reset();

  // Burst train. The retriggers reopen the gate to the level the strike itself
  // opened it to, velocity scaling included, so a soft clap stays a soft clap.
  noise_peak_ = noise_level_;
  burst_level_ = 0.0f;
  burst_remaining_ = noise_peak_ > 0.0f ? std::max(0, params.noise_burst_count) : 0;
  burst_period_ = std::max(
      1,
      static_cast<int>(std::lround(std::max(0.1f, params.noise_burst_interval_ms) * 0.001f * sr)));
  burst_countdown_ = burst_period_;
  burst_coeff_ = std::exp(
      -1.0f / (std::max(1.0f, params.noise_burst_decay_ms) * 0.001f * static_cast<float>(sr)));

  // Radiated upper bound over every noise stream. Butterworth Q, because this
  // is a ceiling and a resonant one would put back a peak of its own.
  noise_air_hz_ = params.noise_air_hz > 0.0f
                      ? std::min(params.noise_air_hz, 0.45f * static_cast<float>(sr))
                      : 0.0f;
  if (noise_air_hz_ > 0.0f) {
    for (TptSvf* air : {&noise_air_, &wire_air_, &shimmer_air_}) {
      air->prepare(sr);
      air->set(noise_air_hz_, kInvSqrt2);
      air->reset();
    }
  }

  // Shell resonance: the summed hit rings through the drum body. A note-tracked
  // 0 Hz spec is taken to mean "track the struck key" so one tom patch voices
  // every tom size.
  const int shell_count = std::clamp(params.shell_num_modes, 0, kMaxShellModes);
  std::array<BodyResonator::Spec, kMaxShellModes> shell_specs{};
  for (int k = 0; k < shell_count; ++k) {
    const float spec_hz = params.shell_freq_hz[static_cast<size_t>(k)];
    shell_specs[static_cast<size_t>(k)] = {
        spec_hz > 0.0f ? spec_hz : base_hz,
        std::max(0.005f, params.shell_t60_s[static_cast<size_t>(k)]),
        params.shell_weight[static_cast<size_t>(k)]};
  }
  shell_.start_specs(shell_specs.data(), shell_count, sr, params.shell_mix);

  // Dense inharmonic plate. Off when the gain is zero (no delay lines cleared,
  // no state advanced, bit-identical to the voicing that predates the field).
  plate_gain_ = std::max(0.0f, params.plate_gain);
  // The split is between two radiation paths, so with no plate there is no
  // second path and the field is inert. Left live it would be a second name
  // for tone_gain on every membrane piece, and a fit handed two knobs for one
  // quantity trades them against each other.
  tone_direct_ = plate_gain_ > 0.0f ? params.tone_direct : 1.0f;
  if (plate_gain_ > 0.0f) {
    plate_.start(sr, params.plate_low_hz, params.plate_t60_s, params.plate_hf_ratio,
                 params.plate_air_hz);
  } else {
    plate_.reset();
  }

  // Direct contact radiation. Harder strikes press harder, so the level takes
  // the velocity; the contact time is the patch's, since what shortens it with
  // velocity is a fifth-root and worth less than the octave the knob spans.
  contact_ = std::max(0.0f, params.contact) * vel01;
  contact_i_ = 0;
  contact_len_ = 0;
  if (contact_ > 0.0f) {
    // One more sample than the period: the pulse spans [0, 1] inclusive, so the
    // period it voices is contact_len_ - 1 samples and the knob keeps its unit.
    const float period = std::max(0.001f, params.contact_ms) * 0.001f * static_cast<float>(sr);
    contact_len_ = std::max(2, 1 + static_cast<int>(std::lround(period)));
  }

  // Snare wire rattle: gated noise driven by the membrane crossing the wire
  // contact threshold. Voiced through a dedicated high-pass. The threshold is
  // read against the head's swing as a fraction of a full-velocity strike, so
  // velocity enters here and nowhere else in the rattle.
  wire_buzz_ = std::max(0.0f, params.wire_buzz);
  wire_threshold_ = std::max(0.0f, params.wire_threshold);
  wire_scale_ = tone_peak_ > 0.0f ? vel01 / tone_peak_ : 0.0f;
  wire_env_ = 0.0f;
  wire_release_ = params.wire_decay_ms > 0.0f
                      ? std::exp(-1.0f / (params.wire_decay_ms * 0.001f * static_cast<float>(sr)))
                      : 0.0f;
  wire_index_ = 0;
  wire_filter_.prepare(sr);
  wire_filter_.set(params.wire_cutoff_hz, 0.9f);
  wire_filter_.reset();

  // Nonlinear cymbal shimmer: the membrane energy pumps a high shimmer band
  // through a slow attack follower (the buildup lag).
  shimmer_ = std::max(0.0f, params.shimmer);
  shimmer_env_ = 0.0f;
  shimmer_attack_coeff_ = 1.0f - std::exp(-1.0f / (std::max(1.0f, params.shimmer_attack_ms) *
                                                   0.001f * static_cast<float>(sr)));
  shimmer_index_ = 0;
  shimmer_filter_.prepare(sr);
  shimmer_filter_.set(params.shimmer_cutoff_hz, 0.7f);
  shimmer_filter_.reset();

  // Stochastic particle excitation (PhISEM). Off when beans == 0 (bit-identical
  // — no draws, no state advance).
  phisem_beans_ = std::max(0.0f, params.phisem_beans);
  phisem_sr_ = static_cast<float>(sr);
  phisem_prob_index_ = 0;
  phisem_noise_index_ = 0;
  phisem_sound_level_ = 0.0f;
  phisem_scrape_phase_ = 0.0f;
  phisem_glide_state_ = 0.0f;
  if (phisem_beans_ > 0.0f) {
    // A shake gesture: the system energy is set by the strike and dies over
    // phisem_energy_ms; each collision bumps the sounding energy, which decays
    // over the short grain time phisem_sound_ms.
    phisem_shake_energy_ = kPhisemVelocityFloor + (1.0f - kPhisemVelocityFloor) * vel01;
    phisem_sys_decay_ = std::exp(
        -1.0f / (std::max(1.0f, params.phisem_energy_ms) * 0.001f * static_cast<float>(sr)));
    phisem_sound_decay_ = std::exp(
        -1.0f / (std::max(0.2f, params.phisem_sound_ms) * 0.001f * static_cast<float>(sr)));
    phisem_rate_ = kPhisemCollisionRate / static_cast<float>(sr);
    phisem_scrape_inc_ =
        params.phisem_scrape_hz > 0.0f ? params.phisem_scrape_hz / static_cast<float>(sr) : 0.0f;
    phisem_res_hz_ = params.phisem_res_hz;
    phisem_res_q_ = std::max(0.5f, params.phisem_res_q);
    phisem_glide_state_ = params.phisem_pitch_glide;
    phisem_glide_coeff_ = std::exp(
        -1.0f / (std::max(1.0f, params.phisem_energy_ms) * 0.001f * static_cast<float>(sr)));
    phisem_filter_.prepare(sr);
    if (phisem_res_hz_ > 0.0f) {
      const float c = phisem_res_hz_ * (1.0f + phisem_glide_state_);
      phisem_filter_.set(std::clamp(c, 20.0f, 0.45f * static_cast<float>(sr)), phisem_res_q_);
    }
    phisem_filter_.reset();
    phisem_body_gain_ =
        params.phisem_body_hz > 0.0f ? std::max(0.0f, params.phisem_body_gain) : 0.0f;
    if (phisem_body_gain_ > 0.0f) {
      const float c = std::clamp(params.phisem_body_hz, 20.0f, 0.45f * static_cast<float>(sr));
      const float q = std::max(0.5f, params.phisem_body_q);
      for (TptSvf* pair : {&phisem_body_, &phisem_body2_}) {
        pair->prepare(sr);
        pair->set(c, q);
        pair->reset();
      }
    }
  }
}

float PercussionVoiceCore::render(float pitch_ratio) noexcept {
  float mix = 0.0f;
  // The share of the tone layer that reaches the plate without radiating
  // directly. Zero unless tone_direct is below one.
  float plate_drive = 0.0f;

  if (num_modes_ > 0) {
    // Tone layer with the descending strike pitch folded into the ratio.
    float ratio = pitch_ratio * (1.0f + drop_state_);
    if (drop_state_ > 0.0f) {
      drop_state_ *= drop_coeff_;
      if (drop_state_ < 1.0e-3f) drop_state_ = 0.0f;
    }
    if (ratio != cached_ratio_) {
      cached_ratio_ = ratio;
      for (int k = 0; k < num_modes_; ++k) {
        Mode& mode = modes_[static_cast<size_t>(k)];
        if (mode.gain == 0.0f && mode.r == 0.0f) continue;
        const float w = std::min(mode.omega * ratio, 0.95f * kPi);
        mode.a1 = 2.0f * mode.r * std::cos(w);
        mode.a2 = -mode.r * mode.r;
      }
    }
    const float x = excite_ ? 1.0f : 0.0f;
    excite_ = false;
    float tone = 0.0f;
    for (int k = 0; k < num_modes_; ++k) {
      Mode& mode = modes_[static_cast<size_t>(k)];
      const float y = mode.a1 * mode.y1 + mode.a2 * mode.y2 + mode.gain * x;
      mode.y2 = mode.y1;
      mode.y1 = y;
      tone += y;
    }
    // Split so the plate keeps the whole modal field while the direct path
    // takes only its share: what the plate re-radiates arrives a delay line
    // later, which is where the reference puts it.
    const float voiced_tone = tone_gain_ * tone;
    mix += voiced_tone * tone_direct_;
    plate_drive = voiced_tone * (1.0f - tone_direct_);

    // Snare wire rattle: while the membrane swing exceeds the contact
    // threshold the wires buzz against the bottom head. The swing is measured
    // as a fraction of what a full-velocity strike on this piece reaches, so a
    // soft hit can stay under the threshold for its whole length and read as a
    // drum with the strainer off — which is the nonlinearity the wires are.
    if (wire_buzz_ > 0.0f) {
      const float contact = std::abs(tone) * wire_scale_ - wire_threshold_;
      const float gate = contact > 0.0f ? std::min(contact * 8.0f, 1.0f) : 0.0f;
      // The head opens the gate; the wires then ring on their own damping. With
      // `wire_decay_ms` at 0 the release coefficient is 0 and this is the gate.
      wire_env_ = std::max(gate, wire_env_ * wire_release_);
      const float n = noise_.bipolar_at(kWireIndexBase + wire_index_++) * wire_env_ * wire_buzz_;
      const float wire = wire_filter_.process(n).hp;
      mix += noise_air_hz_ > 0.0f ? wire_air_.process(wire).lp : wire;
    }

    // Nonlinear shimmer: the quadratic membrane energy (tone^2) drives a high
    // shimmer band through a slow-attack follower, so the wash swells after
    // the strike and rides the inharmonic ring. One-way, so it stays stable.
    if (shimmer_ > 0.0f) {
      shimmer_env_ += (tone * tone - shimmer_env_) * shimmer_attack_coeff_;
      const float n = noise_.bipolar_at(kShimmerIndexBase + shimmer_index_++);
      const float wash = shimmer_filter_.process(n * shimmer_env_ * shimmer_).hp;
      mix += noise_air_hz_ > 0.0f ? shimmer_air_.process(wash).lp : wash;
    }
  }

  // Counted outside the level gate: the train has to keep its schedule across
  // the silence between a short burst and the next retrigger.
  if (burst_remaining_ > 0 && --burst_countdown_ <= 0) {
    burst_level_ = noise_peak_;
    burst_countdown_ = burst_period_;
    --burst_remaining_;
  }
  const float noise_env = noise_level_ + burst_level_;
  if (noise_env > 1.0e-5f) {
    const float burst = noise_.bipolar_at(kNoiseIndexBase + noise_index_++) * noise_env;
    noise_level_ *= noise_coeff_;
    burst_level_ *= burst_coeff_;
    const TptSvf::Outputs out = noise_filter_.process(burst);
    float voiced = 0.0f;
    switch (noise_output_) {
      case SynthFilterOutput::kHighpass:
        voiced = out.hp;
        break;
      case SynthFilterOutput::kBandpass:
        voiced = out.bp;
        break;
      case SynthFilterOutput::kLowpass:
        voiced = out.lp;
        break;
    }
    mix += noise_air_hz_ > 0.0f ? noise_air_.process(voiced).lp : voiced;
  }

  // Stochastic particle excitation (PhISEM). The shake energy decays over the
  // gesture; bead/ridge collisions bump the sounding energy that scales a single
  // noise source, optionally rung through a gourd resonance (cuica glides it).
  if (phisem_beans_ > 0.0f) {
    phisem_shake_energy_ *= phisem_sys_decay_;
    bool collide = false;
    // Scrape (guiro/cuica): a ridge passes under the scraper each period.
    if (phisem_scrape_inc_ > 0.0f) {
      phisem_scrape_phase_ += phisem_scrape_inc_;
      if (phisem_scrape_phase_ >= 1.0f) {
        phisem_scrape_phase_ -= 1.0f;
        collide = true;
      }
    }
    // Random bead collisions on top; the rate falls as the shake dies out.
    const float p = phisem_beans_ * phisem_shake_energy_ * phisem_rate_;
    if (noise_.unipolar_at(kPhisemProbIndexBase + phisem_prob_index_++) < p) collide = true;
    if (collide) {
      phisem_sound_level_ = std::min(phisem_sound_level_ + phisem_shake_energy_ * 0.6f, 4.0f);
    }
    const float raw =
        noise_.bipolar_at(kPhisemNoiseIndexBase + phisem_noise_index_++) * phisem_sound_level_;
    phisem_sound_level_ *= phisem_sound_decay_;
    float particle = raw;
    if (phisem_res_hz_ > 0.0f) {
      // Cuica pitch glide: ease the resonance centre back to res_hz.
      if (phisem_glide_state_ != 0.0f) {
        phisem_glide_state_ *= phisem_glide_coeff_;
        if (std::abs(phisem_glide_state_) < 1.0e-3f) phisem_glide_state_ = 0.0f;
        const float c = phisem_res_hz_ * (1.0f + phisem_glide_state_);
        phisem_filter_.set(std::clamp(c, 20.0f, 0.45f * phisem_sr_), phisem_res_q_);
      }
      particle = phisem_filter_.process(particle).bp;
    }
    mix += particle;
    // The body is driven by the collisions themselves, not by the band above
    // it: the two are parallel radiation paths from one excitation, and
    // cascading them would leave the gourd with nothing left to resonate.
    if (phisem_body_gain_ > 0.0f) {
      const float b = phisem_body_.process(raw).bp;
      mix += phisem_body2_.process(b).bp * phisem_body_gain_;
    }
  }

  // Dense inharmonic plate: the whole strike drives the network, because what
  // sets a plate ringing is the hit and not one layer of it. Added over the
  // dry hit rather than blended with it — the strike is the noisy half of the
  // sound and the plate is the dense half, and metal needs both.
  if (plate_gain_ > 0.0f) {
    const float strike = mix + plate_drive;
    mix += plate_gain_ * plate_.process(strike);
  }

  if (shell_.active()) mix = shell_.process(mix);

  // Decaying peak of what the piece actually radiated, which is the only
  // reading that covers every layer at once: a mode still ringing, a burst
  // train yet to fire and a gourd resonance all reach it the same way.
  silence_env_ = std::max(std::abs(mix), silence_env_ * silence_coeff_);

  return mix;
}

bool PercussionVoiceCore::silent() const noexcept {
  return burst_remaining_ == 0 && silence_env_ < kSilenceFloor;
}

void PercussionVoiceCore::kill() noexcept {
  for (Mode& mode : modes_) {
    mode.y1 = 0.0f;
    mode.y2 = 0.0f;
    mode.gain = 0.0f;
  }
  num_modes_ = 0;
  silence_env_ = 0.0f;
  noise_level_ = 0.0f;
  noise_peak_ = 0.0f;
  burst_level_ = 0.0f;
  burst_remaining_ = 0;
  excite_ = false;
  noise_air_hz_ = 0.0f;
  noise_air_.reset();
  wire_air_.reset();
  shimmer_air_.reset();
  shell_.reset();
  plate_gain_ = 0.0f;
  plate_.reset();
  tone_direct_ = 1.0f;
  contact_ = 0.0f;
  contact_len_ = 0;
  contact_i_ = 0;
  wire_buzz_ = 0.0f;
  wire_env_ = 0.0f;
  wire_filter_.reset();
  shimmer_ = 0.0f;
  shimmer_env_ = 0.0f;
  shimmer_filter_.reset();
  phisem_beans_ = 0.0f;
  phisem_shake_energy_ = 0.0f;
  phisem_sound_level_ = 0.0f;
  phisem_filter_.reset();
  phisem_body_gain_ = 0.0f;
  phisem_body_.reset();
  phisem_body2_.reset();
}

}  // namespace sonare::midi::synth
