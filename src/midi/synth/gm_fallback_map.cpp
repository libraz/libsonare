#include "midi/synth/gm_fallback_map.h"

#include <algorithm>
#include <array>

namespace sonare::midi::synth {

namespace {

DahdsrConfig env(float attack_ms, float decay_ms, float sustain, float release_ms) noexcept {
  DahdsrConfig e;
  e.attack_ms = attack_ms;
  e.decay_ms = decay_ms;
  e.sustain = sustain;
  e.release_ms = release_ms;
  return e;
}

/// One GM family (programs family*8 .. family*8+7) -> one subtractive patch.
/// Voiced for "honest sketch" quality (§E coverage tiers): leads/pads/basses
/// are strong, plucked/keys are decent decay-shaped patches, winds/strings
/// are sustained approximations. Later phases retarget families to their
/// dedicated engine modes by editing entries here only.
std::array<NativeSynthPatch, 16> build_family_patches() noexcept {
  std::array<NativeSynthPatch, 16> t{};

  // 0-7 piano: percussive saw pair, envelope-closed filter.
  t[0].waveform = VaWaveform::kSaw;
  t[0].unison = 2;
  t[0].detune_cents = 5.0f;
  t[0].amp_env = env(2.0f, 900.0f, 0.0f, 300.0f);
  t[0].cutoff_hz = 2500.0f;
  t[0].filter_env = env(1.0f, 700.0f, 0.25f, 300.0f);
  t[0].env_to_cutoff_cents = 2400.0f;
  t[0].key_track = 0.5f;
  t[0].vel_to_cutoff_cents = 1800.0f;

  // 8-15 chromatic percussion: FM bell (inharmonic 3.5 ratio, long
  // key-rate-scaled decay).
  t[1].mode = SynthEngineMode::kFm;
  t[1].amp_env = env(1.0f, 2500.0f, 0.0f, 600.0f);
  t[1].fm.algorithm = FmAlgorithm::kStack2;
  t[1].fm.ops[0].ratio = 1.0f;
  t[1].fm.ops[0].level = 1.0f;
  t[1].fm.ops[0].env = env(1.0f, 2500.0f, 0.0f, 600.0f);
  t[1].fm.ops[0].key_rate_scale = 0.4f;
  t[1].fm.ops[1].ratio = 3.5f;  // inharmonic bell partials
  t[1].fm.ops[1].level = 3.0f;
  t[1].fm.ops[1].env = env(1.0f, 900.0f, 0.0f, 400.0f);
  t[1].fm.ops[1].vel_to_level = 0.6f;
  t[1].fm.ops[1].key_rate_scale = 0.5f;
  t[1].gain = 0.6f;

  // 16-23 organ: steady detuned squares, no filter envelope.
  t[2].waveform = VaWaveform::kSquare;
  t[2].unison = 2;
  t[2].detune_cents = 4.0f;
  t[2].drift_cents = 2.0f;
  t[2].amp_env = env(5.0f, 60.0f, 1.0f, 80.0f);
  t[2].cutoff_hz = 5000.0f;
  t[2].key_track = 0.3f;

  // 24-31 guitar: Karplus-Strong steel-string pluck (method (3)); the string
  // itself decays, so the amp envelope just gates note-off. Program-level
  // overrides voice the nylon/electric/muted/driven variants.
  t[3].mode = SynthEngineMode::kKarplusStrong;
  t[3].amp_env = env(1.0f, 0.0f, 1.0f, 250.0f);
  t[3].cutoff_hz = 20000.0f;
  t[3].ks.brightness = 0.62f;
  t[3].ks.decay_s = 3.5f;
  t[3].ks.decay_stretch = 0.6f;
  t[3].ks.pick_position = 0.18f;
  t[3].ks.exc_brightness = 0.85f;
  t[3].ks.vel_to_brightness = 0.6f;
  t[3].ks.release_damp_s = 0.08f;
  t[3].gain = 0.8f;

  // 32-39 bass: single dark saw through the transistor ladder, punchy
  // filter envelope and a touch of drive.
  t[4].waveform = VaWaveform::kSaw;
  t[4].filter_model = SynthFilterModel::kMoogLadder;
  t[4].drive = 0.15f;
  t[4].amp_env = env(3.0f, 350.0f, 0.7f, 150.0f);
  t[4].cutoff_hz = 900.0f;
  t[4].filter_env = env(1.0f, 250.0f, 0.3f, 150.0f);
  t[4].env_to_cutoff_cents = 1500.0f;
  t[4].key_track = 0.3f;
  t[4].vel_to_cutoff_cents = 1200.0f;

  // 40-47 strings: slow detuned saws with drift.
  t[5].waveform = VaWaveform::kSaw;
  t[5].unison = 3;
  t[5].detune_cents = 10.0f;
  t[5].drift_cents = 3.0f;
  t[5].amp_env = env(120.0f, 300.0f, 0.85f, 350.0f);
  t[5].cutoff_hz = 4000.0f;
  t[5].key_track = 0.3f;
  t[5].lfo_to_pitch_cents = 5.0f;

  // 48-55 ensemble / choir: wide slow supersaw pad.
  t[6].waveform = VaWaveform::kSaw;
  t[6].unison = 5;
  t[6].detune_cents = 14.0f;
  t[6].drift_cents = 4.0f;
  t[6].amp_env = env(200.0f, 400.0f, 0.8f, 500.0f);
  t[6].cutoff_hz = 3200.0f;

  // 56-63 brass: 3-op FM stack with a feedback operator (the DX brass
  // recipe), index swelling in through the modulator envelope.
  t[7].mode = SynthEngineMode::kFm;
  t[7].amp_env = env(40.0f, 200.0f, 0.85f, 200.0f);
  t[7].fm.algorithm = FmAlgorithm::kStack3;
  t[7].fm.ops[0].ratio = 1.0f;
  t[7].fm.ops[0].level = 1.0f;
  t[7].fm.ops[0].env = env(40.0f, 200.0f, 0.85f, 200.0f);
  t[7].fm.ops[1].ratio = 1.0f;
  t[7].fm.ops[1].level = 1.6f;
  t[7].fm.ops[1].env = env(80.0f, 300.0f, 0.7f, 200.0f);  // brightness swell
  t[7].fm.ops[1].vel_to_level = 0.5f;
  t[7].fm.ops[2].ratio = 1.0f;
  t[7].fm.ops[2].level = 0.8f;
  t[7].fm.ops[2].feedback = 1.2f;  // feedback op: saw-like brass spectrum
  t[7].fm.ops[2].env = env(80.0f, 400.0f, 0.6f, 200.0f);

  // 64-71 reed: hollow square, light vibrato.
  t[8].waveform = VaWaveform::kSquare;
  t[8].amp_env = env(30.0f, 150.0f, 0.85f, 180.0f);
  t[8].cutoff_hz = 2800.0f;
  t[8].key_track = 0.4f;
  t[8].lfo_to_pitch_cents = 4.0f;

  // 72-79 pipe / flute: near-pure triangle with vibrato.
  t[9].waveform = VaWaveform::kTriangle;
  t[9].amp_env = env(50.0f, 100.0f, 0.9f, 150.0f);
  t[9].cutoff_hz = 4500.0f;
  t[9].key_track = 0.5f;
  t[9].lfo_to_pitch_cents = 7.0f;

  // 80-87 synth lead: classic 3-osc detuned saw lead through the ladder.
  t[10].waveform = VaWaveform::kSaw;
  t[10].unison = 3;
  t[10].detune_cents = 12.0f;
  t[10].filter_model = SynthFilterModel::kMoogLadder;
  t[10].drive = 0.1f;
  t[10].amp_env = env(5.0f, 200.0f, 0.8f, 150.0f);
  t[10].cutoff_hz = 3500.0f;
  t[10].filter_env = env(1.0f, 350.0f, 0.4f, 150.0f);
  t[10].env_to_cutoff_cents = 1800.0f;
  t[10].vel_to_cutoff_cents = 1200.0f;

  // 88-95 synth pad: 7-osc supersaw, slow envelope, drift.
  t[11].waveform = VaWaveform::kSaw;
  t[11].unison = 7;
  t[11].detune_cents = 18.0f;
  t[11].drift_cents = 5.0f;
  t[11].amp_env = env(400.0f, 600.0f, 0.8f, 800.0f);
  t[11].cutoff_hz = 2800.0f;

  // 96-103 synth FX: drifting detuned triangles.
  t[12].waveform = VaWaveform::kTriangle;
  t[12].unison = 3;
  t[12].detune_cents = 15.0f;
  t[12].drift_cents = 8.0f;
  t[12].amp_env = env(300.0f, 800.0f, 0.7f, 900.0f);
  t[12].cutoff_hz = 3000.0f;
  t[12].lfo_to_pitch_cents = 10.0f;

  // 104-111 ethnic (plucked): bright short KS pluck (banjo/sitar/koto
  // sketch — near-bridge pick, fast decay).
  t[13].mode = SynthEngineMode::kKarplusStrong;
  t[13].amp_env = env(1.0f, 0.0f, 1.0f, 200.0f);
  t[13].cutoff_hz = 20000.0f;
  t[13].ks.brightness = 0.75f;
  t[13].ks.decay_s = 1.6f;
  t[13].ks.decay_stretch = 0.4f;
  t[13].ks.pick_position = 0.09f;
  t[13].ks.exc_brightness = 0.95f;
  t[13].ks.vel_to_brightness = 0.6f;
  t[13].ks.release_damp_s = 0.05f;
  t[13].gain = 0.8f;

  // 112-119 percussive: short bright strike.
  t[14].waveform = VaWaveform::kTriangle;
  t[14].amp_env = env(1.0f, 280.0f, 0.0f, 200.0f);
  t[14].cutoff_hz = 4000.0f;
  t[14].filter_env = env(1.0f, 200.0f, 0.0f, 200.0f);
  t[14].env_to_cutoff_cents = 2000.0f;
  t[14].key_track = 0.5f;

  // 120-127 SFX: resonant band-passed noise wash.
  t[15].waveform = VaWaveform::kNoise;
  t[15].filter_output = SynthFilterOutput::kBandpass;
  t[15].amp_env = env(50.0f, 600.0f, 0.5f, 400.0f);
  t[15].cutoff_hz = 2000.0f;
  t[15].resonance_q = 2.0f;
  t[15].gain = 0.7f;

  for (NativeSynthPatch& p : t) p = clamp_synth_patch(p);
  return t;
}

/// Program-level overrides inside a family: the electric pianos and
/// clavi/harpsichord are FM instruments (method (2)), and the guitar family
/// + orchestral harp voice their Karplus-Strong variants (method (3)).
struct ProgramOverrides {
  NativeSynthPatch e_piano;          // programs 4-5 (Electric Piano 1/2)
  NativeSynthPatch clav;             // programs 6-7 (Harpsichord / Clavi)
  NativeSynthPatch nylon_guitar;     // program 24
  NativeSynthPatch electric_guitar;  // programs 26-27 (jazz / clean)
  NativeSynthPatch muted_guitar;     // program 28 (palm mute)
  NativeSynthPatch overdriven;       // program 29
  NativeSynthPatch distortion;       // program 30
  NativeSynthPatch harp;             // program 46 (Orchestral Harp)
};

ProgramOverrides build_program_overrides() noexcept {
  ProgramOverrides o{};

  // FM e-piano (Rhodes/Wurli sketch): body pair at 1:1 with a velocity-driven
  // index plus a fast-decaying 14:1 "tine" pair — the exponential index
  // fall-off is what reads as an electric piano.
  NativeSynthPatch& ep = o.e_piano;
  ep.mode = SynthEngineMode::kFm;
  ep.amp_env = env(1.0f, 3000.0f, 0.0f, 250.0f);
  ep.fm.algorithm = FmAlgorithm::kPair2x2;
  ep.fm.ops[0].ratio = 1.0f;  // body carrier
  ep.fm.ops[0].level = 1.0f;
  ep.fm.ops[0].env = env(1.0f, 3000.0f, 0.0f, 250.0f);
  ep.fm.ops[0].key_rate_scale = 0.4f;
  ep.fm.ops[1].ratio = 1.0f;  // body modulator (warmth -> velocity)
  ep.fm.ops[1].level = 0.9f;
  ep.fm.ops[1].env = env(1.0f, 1200.0f, 0.0f, 250.0f);
  ep.fm.ops[1].vel_to_level = 0.7f;
  ep.fm.ops[1].key_rate_scale = 0.5f;
  ep.fm.ops[2].ratio = 1.0f;  // tine carrier (quiet sparkle)
  ep.fm.ops[2].level = 0.3f;
  ep.fm.ops[2].env = env(1.0f, 600.0f, 0.0f, 150.0f);
  ep.fm.ops[2].key_rate_scale = 0.5f;
  ep.fm.ops[3].ratio = 14.0f;  // tine "ping"
  ep.fm.ops[3].level = 1.2f;
  ep.fm.ops[3].env = env(1.0f, 120.0f, 0.0f, 80.0f);
  ep.fm.ops[3].vel_to_level = 0.8f;
  ep.fm.ops[3].key_rate_scale = 0.6f;
  ep.gain = 0.6f;

  // FM clavi / harpsichord: bright high-ratio pluck with a fast index decay.
  NativeSynthPatch& cl = o.clav;
  cl.mode = SynthEngineMode::kFm;
  cl.amp_env = env(1.0f, 1000.0f, 0.0f, 120.0f);
  cl.fm.algorithm = FmAlgorithm::kStack2;
  cl.fm.ops[0].ratio = 1.0f;
  cl.fm.ops[0].level = 1.0f;
  cl.fm.ops[0].env = env(1.0f, 1000.0f, 0.0f, 120.0f);
  cl.fm.ops[0].key_rate_scale = 0.4f;
  cl.fm.ops[1].ratio = 7.0f;
  cl.fm.ops[1].level = 2.0f;
  cl.fm.ops[1].env = env(1.0f, 150.0f, 0.0f, 100.0f);
  cl.fm.ops[1].vel_to_level = 0.6f;
  cl.fm.ops[1].key_rate_scale = 0.5f;
  cl.gain = 0.6f;

  // KS guitar variants: all share the family-3 steel string and differ in
  // pick position / loop brightness / decay (the Jaffe-Smith knobs).
  NativeSynthPatch steel{};
  steel.mode = SynthEngineMode::kKarplusStrong;
  steel.amp_env = env(1.0f, 0.0f, 1.0f, 250.0f);
  steel.cutoff_hz = 20000.0f;
  steel.ks.brightness = 0.62f;
  steel.ks.decay_s = 3.5f;
  steel.ks.decay_stretch = 0.6f;
  steel.ks.pick_position = 0.18f;
  steel.ks.exc_brightness = 0.85f;
  steel.ks.vel_to_brightness = 0.6f;
  steel.ks.release_damp_s = 0.08f;
  steel.gain = 0.8f;

  // Nylon: soft finger pluck near the middle of the string, dull loop.
  o.nylon_guitar = steel;
  o.nylon_guitar.ks.brightness = 0.42f;
  o.nylon_guitar.ks.exc_brightness = 0.55f;
  o.nylon_guitar.ks.pick_position = 0.27f;
  o.nylon_guitar.ks.decay_s = 2.2f;

  // Electric (jazz/clean) — the `electric-guitar` preset: bright sustaining
  // loop, near-bridge pick, a pickup-ish lowpass instead of the open string.
  o.electric_guitar = steel;
  o.electric_guitar.ks.brightness = 0.8f;
  o.electric_guitar.ks.decay_s = 4.5f;
  o.electric_guitar.ks.pick_position = 0.12f;
  o.electric_guitar.ks.exc_brightness = 0.9f;
  o.electric_guitar.cutoff_hz = 5500.0f;
  o.electric_guitar.gain = 0.7f;

  // Palm mute: same electric string, choked decay.
  o.muted_guitar = o.electric_guitar;
  o.muted_guitar.ks.decay_s = 0.35f;
  o.muted_guitar.ks.brightness = 0.55f;
  o.muted_guitar.ks.release_damp_s = 0.04f;

  // Overdriven / distortion: electric string into the gain-compensated tanh
  // drive (the voice-level stage; the track-level `saturation.ampSim` insert
  // supplies the full amp/cab character).
  o.overdriven = o.electric_guitar;
  o.overdriven.drive = 0.45f;
  o.overdriven.cutoff_hz = 4000.0f;
  o.distortion = o.electric_guitar;
  o.distortion.drive = 0.8f;
  o.distortion.cutoff_hz = 3500.0f;
  o.distortion.gain = 0.6f;

  // Orchestral harp: long stretched decay, strings keep ringing after
  // note-off (no damper grip), mid-string pluck.
  o.harp = steel;
  o.harp.amp_env = env(1.0f, 0.0f, 1.0f, 1200.0f);
  o.harp.ks.brightness = 0.5f;
  o.harp.ks.decay_s = 5.0f;
  o.harp.ks.decay_stretch = 0.8f;
  o.harp.ks.pick_position = 0.3f;
  o.harp.ks.vel_to_brightness = 0.5f;
  o.harp.ks.release_damp_s = 1.0f;

  o.e_piano = clamp_synth_patch(o.e_piano);
  o.clav = clamp_synth_patch(o.clav);
  o.nylon_guitar = clamp_synth_patch(o.nylon_guitar);
  o.electric_guitar = clamp_synth_patch(o.electric_guitar);
  o.muted_guitar = clamp_synth_patch(o.muted_guitar);
  o.overdriven = clamp_synth_patch(o.overdriven);
  o.distortion = clamp_synth_patch(o.distortion);
  o.harp = clamp_synth_patch(o.harp);
  return o;
}

const ProgramOverrides& program_overrides() noexcept {
  static const ProgramOverrides kTable = build_program_overrides();
  return kTable;
}

/// GM drum-note categories -> one-shot patches. Pitched pieces (kick / toms)
/// play at the struck key's frequency; wires / hats / cymbals are filtered
/// seeded noise.
struct DrumPatches {
  NativeSynthPatch kick;
  NativeSynthPatch snare;
  NativeSynthPatch closed_hat;
  NativeSynthPatch open_hat;
  NativeSynthPatch tom;
  NativeSynthPatch cymbal;
  NativeSynthPatch percussion;
};

DrumPatches build_drum_patches() noexcept {
  DrumPatches d{};

  // Kick: low sine thump (note 35/36 ~= 61/65 Hz).
  d.kick.waveform = VaWaveform::kSine;
  d.kick.one_shot = true;
  d.kick.amp_env = env(1.0f, 180.0f, 0.0f, 60.0f);
  d.kick.cutoff_hz = 400.0f;
  d.kick.gain = 0.9f;

  // Snare: band-passed noise crack.
  d.snare.waveform = VaWaveform::kNoise;
  d.snare.one_shot = true;
  d.snare.filter_output = SynthFilterOutput::kBandpass;
  d.snare.amp_env = env(1.0f, 200.0f, 0.0f, 80.0f);
  d.snare.cutoff_hz = 1800.0f;
  d.snare.resonance_q = 1.2f;
  d.snare.gain = 0.8f;

  // Hi-hats: high-passed noise, closed short / open ringing.
  d.closed_hat.waveform = VaWaveform::kNoise;
  d.closed_hat.one_shot = true;
  d.closed_hat.filter_output = SynthFilterOutput::kHighpass;
  d.closed_hat.amp_env = env(1.0f, 60.0f, 0.0f, 40.0f);
  d.closed_hat.cutoff_hz = 7000.0f;
  d.closed_hat.gain = 0.5f;
  d.open_hat = d.closed_hat;
  d.open_hat.amp_env = env(1.0f, 450.0f, 0.0f, 150.0f);

  // Toms: pitched triangle knock at the struck key.
  d.tom.waveform = VaWaveform::kTriangle;
  d.tom.one_shot = true;
  d.tom.amp_env = env(1.0f, 300.0f, 0.0f, 120.0f);
  d.tom.cutoff_hz = 1200.0f;
  d.tom.gain = 0.9f;

  // Cymbals: long high-passed noise shimmer.
  d.cymbal.waveform = VaWaveform::kNoise;
  d.cymbal.one_shot = true;
  d.cymbal.filter_output = SynthFilterOutput::kHighpass;
  d.cymbal.amp_env = env(1.0f, 1200.0f, 0.0f, 400.0f);
  d.cymbal.cutoff_hz = 6000.0f;
  d.cymbal.gain = 0.5f;

  // Everything else (claps, shakers, latin percussion): short noise burst.
  d.percussion.waveform = VaWaveform::kNoise;
  d.percussion.one_shot = true;
  d.percussion.filter_output = SynthFilterOutput::kBandpass;
  d.percussion.amp_env = env(1.0f, 150.0f, 0.0f, 80.0f);
  d.percussion.cutoff_hz = 2500.0f;
  d.percussion.resonance_q = 1.5f;
  d.percussion.gain = 0.7f;

  d.kick = clamp_synth_patch(d.kick);
  d.snare = clamp_synth_patch(d.snare);
  d.closed_hat = clamp_synth_patch(d.closed_hat);
  d.open_hat = clamp_synth_patch(d.open_hat);
  d.tom = clamp_synth_patch(d.tom);
  d.cymbal = clamp_synth_patch(d.cymbal);
  d.percussion = clamp_synth_patch(d.percussion);
  return d;
}

const std::array<NativeSynthPatch, 16>& family_patches() noexcept {
  static const std::array<NativeSynthPatch, 16> kTable = build_family_patches();
  return kTable;
}

const DrumPatches& drum_patches() noexcept {
  static const DrumPatches kTable = build_drum_patches();
  return kTable;
}

}  // namespace

const NativeSynthPatch& gm_fallback_patch(uint16_t /*bank*/, uint8_t program) noexcept {
  // GS variation banks fall back to their capital tone's family (same rule as
  // resolve_gs_preset: the variation differs in character, not family).
  switch (program & 0x7Fu) {
    case 4:  // Electric Piano 1
    case 5:  // Electric Piano 2
      return program_overrides().e_piano;
    case 6:  // Harpsichord
    case 7:  // Clavi
      return program_overrides().clav;
    case 24:  // Acoustic Guitar (nylon)
      return program_overrides().nylon_guitar;
    case 26:  // Electric Guitar (jazz)
    case 27:  // Electric Guitar (clean)
      return program_overrides().electric_guitar;
    case 28:  // Electric Guitar (muted)
      return program_overrides().muted_guitar;
    case 29:  // Overdriven Guitar
      return program_overrides().overdriven;
    case 30:  // Distortion Guitar
      return program_overrides().distortion;
    case 46:  // Orchestral Harp
      return program_overrides().harp;
    default:
      break;
  }
  return family_patches()[static_cast<size_t>((program & 0x7Fu) >> 3)];
}

const NativeSynthPatch& gm_fallback_drum_patch(uint8_t note) noexcept {
  const DrumPatches& d = drum_patches();
  switch (note) {
    case 35:  // Acoustic Bass Drum
    case 36:  // Bass Drum 1
      return d.kick;
    case 38:  // Acoustic Snare
    case 40:  // Electric Snare
      return d.snare;
    case 42:  // Closed Hi-Hat
    case 44:  // Pedal Hi-Hat
      return d.closed_hat;
    case 46:  // Open Hi-Hat
      return d.open_hat;
    case 41:  // Low Floor Tom
    case 43:  // High Floor Tom
    case 45:  // Low Tom
    case 47:  // Low-Mid Tom
    case 48:  // Hi-Mid Tom
    case 50:  // High Tom
      return d.tom;
    case 49:  // Crash Cymbal 1
    case 51:  // Ride Cymbal 1
    case 52:  // Chinese Cymbal
    case 53:  // Ride Bell
    case 55:  // Splash Cymbal
    case 57:  // Crash Cymbal 2
    case 59:  // Ride Cymbal 2
      return d.cymbal;
    default:
      return d.percussion;
  }
}

float gm_fallback_max_release_ms() noexcept {
  static const float kMax = [] {
    float max_ms = 0.0f;
    for (const NativeSynthPatch& p : family_patches()) {
      // Zero-sustain (percussive/one-shot) patches ring through their decay
      // after note-off, so the decay bounds the tail too.
      max_ms = std::max(max_ms, std::max(p.amp_env.release_ms, p.amp_env.decay_ms));
    }
    const DrumPatches& d = drum_patches();
    const ProgramOverrides& o = program_overrides();
    for (const NativeSynthPatch* p :
         {&d.kick, &d.snare, &d.closed_hat, &d.open_hat, &d.tom, &d.cymbal, &d.percussion,
          &o.e_piano, &o.clav, &o.nylon_guitar, &o.electric_guitar, &o.muted_guitar, &o.overdriven,
          &o.distortion, &o.harp}) {
      max_ms = std::max(max_ms, std::max(p->amp_env.release_ms, p->amp_env.decay_ms));
    }
    return max_ms;
  }();
  return kMax;
}

}  // namespace sonare::midi::synth
