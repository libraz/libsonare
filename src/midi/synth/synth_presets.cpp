#include "midi/synth/synth_presets.h"

#include <array>
#include <cstring>
#include <utility>

#include "midi/synth/gm_fallback_map.h"

namespace sonare::midi::synth {

namespace {

/// Catalog size (§E preset table).
constexpr size_t kPresetCount = 70;

NativeSynthConfig from_patch(const NativeSynthPatch& patch) noexcept {
  NativeSynthConfig cfg;
  cfg.patch = patch;
  return cfg;
}

/// Most catalog entries are the voiced GM fallback patches under their
/// instrument name (one data table, two address spaces); the pure-synth
/// entries are voiced here.
std::array<SynthPreset, kPresetCount> build_presets() noexcept {
  std::array<SynthPreset, kPresetCount> t{};
  size_t i = 0;

  // Rows that are nothing but an alias of a GM fallback program: gm_fallback_map
  // picks the engine, so the catalogue carries only the name and the program.
  // Written through one owner rather than repeated per row — each row otherwise
  // expands its own copy of a whole patch.
  auto alias = [&](const char* name, uint8_t program) {
    SynthPreset& v = t[i++];
    v.name = name;
    v.config = from_patch(gm_fallback_patch(0, program));
  };

  // --- subtractive ---
  NativeSynthPatch sine{};
  sine.waveform = VaWaveform::kSine;
  sine.cutoff_hz = 20000.0f;
  sine.amp_env.attack_ms = 3.0f;
  sine.amp_env.decay_ms = 60.0f;
  sine.amp_env.sustain = 0.8f;
  sine.amp_env.release_ms = 150.0f;
  t[i++] = {"sine", from_patch(clamp_synth_patch(sine))};

  // Bare single-oscillator waveforms, the simplest "make MIDI audible" choices
  // the CLI documents ([saw|square|triangle|sine]). They share sine's envelope
  // and open filter; the richer named leads (saw-lead/square-lead) stack unison
  // and voicing on top.
  for (const auto& [preset_name, wave] :
       {std::pair<const char*, VaWaveform>{"saw", VaWaveform::kSaw},
        {"square", VaWaveform::kSquare},
        {"triangle", VaWaveform::kTriangle}}) {
    NativeSynthPatch bare = sine;
    bare.waveform = wave;
    t[i++] = {preset_name, from_patch(clamp_synth_patch(bare))};
  }

  // The lead and pad programs are voiced apart now, so these two name the one
  // that matches the preset rather than the first of a family that was all saw.
  alias("saw-lead", 81);

  NativeSynthPatch square = gm_fallback_patch(0, 80);
  square.waveform = VaWaveform::kSquare;
  square.unison = 2;
  square.detune_cents = 8.0f;
  square.drift_cents = 4.0f;  // PWM-ish movement from the seeded drift
  square.cutoff_hz = 3000.0f;
  t[i++] = {"square-lead", from_patch(clamp_synth_patch(square))};

  // Synth Bass 1 (GM 38) stays subtractive; the plucked GM 32-35 members are
  // Karplus-Strong now, so the synth sub is voiced off the synth-bass family.
  NativeSynthPatch sub = gm_fallback_patch(0, 38);
  sub.unison = 1;
  sub.cutoff_hz = 600.0f;
  t[i++] = {"sub-bass", from_patch(clamp_synth_patch(sub))};

  {
    SynthPreset& pad = t[i++];
    pad.name = "warm-pad";
    pad.config = from_patch(gm_fallback_patch(0, 89));
    pad.config.bus_drive = 0.15f;  // glue the supersaw stack
  }

  // --- tuned sustain / struck (GM fallback aliases) ---
  // Every row below is an alias of a GM fallback program, so gm_fallback_map
  // picks the engine — not the grouping here. The trailing comment names the
  // engine each row actually resolves to; a heading naming one engine for the
  // whole group would be wrong for most of it.
  alias("e-piano", 4);  // FM
  alias("bell", 14);    // modal (struck bar)
  alias("brass", 56);   // brass (lip reed)

  // --- plucked strings (GM fallback aliases) ---
  alias("pluck", 104);  // plucked string (jawari bridge)
  // Classical (nylon) and steel acoustic guitars driven as standalone
  // instruments engage the shared sympathetic open-string bank (the "sound
  // halo") that the per-note GM fallback path cannot host.
  alias("classical-guitar", 24);  // Karplus-Strong
  alias("steel-guitar", 25);      // Karplus-Strong
  alias("electric-guitar", 26);   // Karplus-Strong
  alias("harp", 46);              // Karplus-Strong
  // The harpsichord is not a bright guitar and does not share their engine: it
  // is voiced by its own jack-and-plectrum model, whose parameters the synth
  // patch ABI does not carry, so this alias is how a caller reaches the voiced
  // instrument rather than the engine defaults.
  alias("harpsichord", 6);  // harpsichord (jack and plectrum)

  // --- electric / acoustic bass (plucked-string waveguide) ---
  // The bass family (GM 32-35): the Karplus-Strong string voiced for the low
  // register (long, stretched decays; a pickup lowpass on the electric members).
  // One data table, two address spaces — the voicings live in the GM fallback
  // map. Slap/pop and the two-polarization beat need the dedicated bass
  // excitation core and are voiced separately once that lands.
  alias("bass-acoustic", 32);
  alias("bass-fingered", 33);
  alias("bass-picked", 34);
  alias("bass-fretless", 35);
  alias("bass-slap", 36);

  // --- modal / additive ---
  alias("marimba", 12);
  alias("glass", 9);
  alias("organ", 16);

  // --- percussion / piano ---
  {
    SynthPreset& kit = t[i++];
    kit.name = "drum-kit";
    NativeSynthPatch patch{};
    patch.mode = SynthEngineMode::kPercussion;
    patch.percussion.gm_kit = true;
    patch.cutoff_hz = 20000.0f;
    patch.gain = 0.8f;
    kit.config = from_patch(clamp_synth_patch(patch));
    kit.config.polyphony = 24;  // a kit stacks pieces, not melodic lines
  }

  alias("acoustic-piano", 0);

  // --- pipe organ (flue pipe waveguide) ---
  // The full principal chorus (plenum) voiced under the GM Church Organ
  // program, with a gentle tremulant drawn for the showcase preset.
  {
    SynthPreset& organ = t[i++];
    organ.name = "church-organ";
    NativeSynthPatch patch = gm_fallback_patch(0, 19);
    patch.pipe_organ.tremulant_rate_hz = 5.2f;
    patch.pipe_organ.tremulant_depth = 0.5f;
    patch.pipe_organ.swell = 0.8f;  // behind a swell shutter (expression = CC11)
    organ.config = from_patch(clamp_synth_patch(patch));
  }
  {
    // Open flute: softer and darker than the principal (a wide, breathy stop).
    SynthPreset& flute = t[i++];
    flute.name = "church-flute";
    NativeSynthPatch patch{};
    patch.mode = SynthEngineMode::kPipeOrgan;
    patch.amp_env.attack_ms = 12.0f;
    patch.amp_env.sustain = 1.0f;
    patch.amp_env.release_ms = 120.0f;
    patch.cutoff_hz = 20000.0f;
    patch.pipe_organ.stopped = false;
    patch.pipe_organ.brightness = 0.4f;
    patch.pipe_organ.tone_decay_s = 8.0f;
    patch.pipe_organ.breath = 0.45f;
    patch.pipe_organ.chiff = 0.3f;
    patch.pipe_organ.radiation = 0.4f;  // an open flute speaks brightly into the room
    patch.gain = 0.7f;
    flute.config = from_patch(clamp_synth_patch(patch));
  }
  {
    // Bourdon / gedackt: a stopped pipe — closed at one end, so it speaks an
    // octave lower for its length and radiates odd harmonics only (a soft,
    // hollow flute).
    SynthPreset& bourdon = t[i++];
    bourdon.name = "church-bourdon";
    NativeSynthPatch patch{};
    patch.mode = SynthEngineMode::kPipeOrgan;
    patch.amp_env.attack_ms = 14.0f;
    patch.amp_env.sustain = 1.0f;
    patch.amp_env.release_ms = 120.0f;
    patch.cutoff_hz = 20000.0f;
    patch.pipe_organ.stopped = true;
    patch.pipe_organ.brightness = 0.35f;
    patch.pipe_organ.tone_decay_s = 8.0f;
    patch.pipe_organ.breath = 0.4f;
    patch.pipe_organ.chiff = 0.25f;
    patch.pipe_organ.radiation = 0.15f;  // a stopped flute stays soft and hollow
    patch.gain = 0.7f;
    bourdon.config = from_patch(clamp_synth_patch(patch));
  }
  {
    // Trompette / reed chorus: lingual reed pipes — the saturating reed valve
    // buzzes into a bright, brassy self-oscillation (an 8' reed under a 4'),
    // the fanfare colour of the full organ. Voiced under a swell shutter.
    SynthPreset& reed = t[i++];
    reed.name = "church-trumpet";
    // A lingual reed stop voiced directly on the pipe-organ waveguide (an 8'
    // reed under a 4'): the saturating reed valve buzzes into a bright,
    // self-oscillating tone. Not the GM Reed Organ, which is a free-reed core.
    NativeSynthPatch patch{};
    patch.mode = SynthEngineMode::kPipeOrgan;
    patch.amp_env.attack_ms = 14.0f;
    patch.amp_env.sustain = 1.0f;
    patch.amp_env.release_ms = 110.0f;
    patch.cutoff_hz = 20000.0f;
    patch.pipe_organ.tone_decay_s = 6.0f;
    patch.pipe_organ.breath = 0.35f;
    patch.pipe_organ.chiff = 0.3f;
    patch.pipe_organ.rank_count = 2;
    patch.pipe_organ.ranks[0] = {1.0f, /*stopped=*/false, 0.8f, 1.0f, 0.85f, 0.25f};  // 8'
    patch.pipe_organ.ranks[1] = {2.0f, false, 0.82f, 0.55f, 0.7f, 0.3f};              // 4'
    patch.pipe_organ.wind_sag = 0.2f;
    patch.pipe_organ.swell = 0.7f;  // Voiced under a swell shutter.
    patch.stereo_spread = 0.18f;
    patch.gain = 0.42f;
    reed.config = from_patch(clamp_synth_patch(patch));
  }

  // --- bowed string (friction-excited waveguide) ---
  // The violin family (GM 40-43): one bowed-string core voiced across four
  // instrument sizes. The engine tunes to the played note, so the members differ
  // by timbre rather than range — the larger the instrument, the darker and
  // slower-speaking the string and the more the corpus (the shared violin
  // BodyResonator) colours it. All bow near the natural playing point with a
  // touch of rosin grip; the bow contour handles the swell, so the amp envelope
  // just opens and holds.
  {
    auto bowed = [&](const char* name, float bow_position, float bow_force, float brightness,
                     float damping, float attack_ms, float release_ms, float body_mix, float gain) {
      SynthPreset& v = t[i++];
      v.name = name;
      NativeSynthPatch patch{};
      patch.mode = SynthEngineMode::kBowedString;
      patch.amp_env.attack_ms = 20.0f;
      patch.amp_env.sustain = 1.0f;
      patch.amp_env.release_ms = release_ms;
      patch.cutoff_hz = 20000.0f;
      patch.bowed_string.bow_position = bow_position;
      patch.bowed_string.bow_force = bow_force;
      patch.bowed_string.brightness = brightness;
      patch.bowed_string.damping = damping;
      patch.bowed_string.attack_ms = attack_ms;
      patch.bowed_string.release_ms = release_ms;
      patch.bowed_string.rosin = 0.15f;
      patch.body = BodyType::kViolin;
      patch.body_mix = body_mix;
      patch.gain = gain;
      v.config = from_patch(clamp_synth_patch(patch));
    };
    //     name          bow_pos force bright  damp  atk    rel   body  gain
    bowed("violin", 0.12f, 0.55f, 0.62f, 0.30f, 45.0f, 110.0f, 0.28f, 0.70f);
    bowed("viola", 0.13f, 0.55f, 0.52f, 0.34f, 55.0f, 120.0f, 0.34f, 0.70f);
    bowed("cello", 0.14f, 0.60f, 0.44f, 0.38f, 70.0f, 140.0f, 0.40f, 0.72f);
    bowed("contrabass", 0.15f, 0.62f, 0.36f, 0.44f, 90.0f, 160.0f, 0.46f, 0.72f);
  }

  // --- reed woodwind (breath-excited waveguide) ---
  // The reed family (GM 65-72): one reed core voiced across the single- and
  // double-reed winds. The engine tunes to the played note, so the members
  // differ by timbre: the CLARINET is the only cylinder (odd-harmonic, hollow);
  // the saxes and double reeds are conical (full harmonic series). The bell
  // brightness is the main timbral axis (bright/nasal oboe -> dark bassoon /
  // baritone), the shared wood-tube BodyResonator adds the bore/formant colour,
  // and the breath contour handles the speech so the amp envelope just holds.
  {
    auto reed = [&](const char* name, bool conical, float reed_stiffness, float reed_opening,
                    float brightness, float damping, float attack_ms, float release_ms,
                    float breath, float body_mix, float gain) {
      SynthPreset& v = t[i++];
      v.name = name;
      NativeSynthPatch patch{};
      patch.mode = SynthEngineMode::kReed;
      patch.amp_env.attack_ms = 15.0f;
      patch.amp_env.sustain = 1.0f;
      patch.amp_env.release_ms = release_ms;
      patch.cutoff_hz = 20000.0f;
      patch.reed.conical = conical;
      patch.reed.reed_stiffness = reed_stiffness;
      patch.reed.reed_opening = reed_opening;
      patch.reed.brightness = brightness;
      patch.reed.damping = damping;
      patch.reed.attack_ms = attack_ms;
      patch.reed.release_ms = release_ms;
      patch.reed.breath_pressure = breath;
      patch.body = BodyType::kWoodTube;
      patch.body_mix = body_mix;
      patch.gain = gain;
      v.config = from_patch(clamp_synth_patch(patch));
    };
    //    name             cone  stiff  open  bright damp  atk    rel    breath body  gain
    reed("clarinet", false, 0.40f, 0.50f, 0.45f, 0.30f, 25.0f, 90.0f, 0.60f, 0.25f, 0.70f);
    reed("soprano-sax", true, 0.55f, 0.55f, 0.60f, 0.32f, 20.0f, 80.0f, 0.65f, 0.30f, 0.70f);
    reed("alto-sax", true, 0.55f, 0.55f, 0.54f, 0.34f, 22.0f, 90.0f, 0.65f, 0.32f, 0.70f);
    reed("tenor-sax", true, 0.60f, 0.50f, 0.48f, 0.36f, 26.0f, 100.0f, 0.68f, 0.36f, 0.72f);
    reed("baritone-sax", true, 0.60f, 0.50f, 0.40f, 0.40f, 32.0f, 120.0f, 0.70f, 0.40f, 0.72f);
    reed("oboe", true, 0.80f, 0.35f, 0.70f, 0.30f, 18.0f, 70.0f, 0.62f, 0.30f, 0.68f);
    reed("english-horn", true, 0.70f, 0.40f, 0.60f, 0.34f, 24.0f, 90.0f, 0.64f, 0.34f, 0.68f);
    reed("bassoon", true, 0.65f, 0.45f, 0.42f, 0.40f, 30.0f, 120.0f, 0.68f, 0.40f, 0.72f);
  }

  // --- brass / lip reed (breath-excited waveguide) ---
  // The brass family (GM 57-64): one lip-reed core voiced across the trumpets,
  // horns and low brass. The lip resonance locks to the played note, so the
  // members differ by timbre: CYLINDRICAL bodies (trumpet / trombone /
  // muted-trumpet) are brighter and more brilliant; CONICAL bodies (horn / tuba /
  // cornet / flugelhorn / euphonium) are darker and rounder. lip_damping is the
  // buzz character (a tight, high-Q lip is bright and brassy; a loose lip is
  // mellow), brightness is the bell openness, and the breath contour handles the
  // speech so the amp envelope just holds. (The bright, blaring "cuivré" edge is
  // a later off-by-default enhancement; these presets are the round linear tone.)
  {
    auto brass = [&](const char* name, bool conical, float lip_tension, float lip_damping,
                     float brightness, float damping, float attack_ms, float release_ms,
                     float breath, float bell_mix, float gain) {
      SynthPreset& v = t[i++];
      v.name = name;
      NativeSynthPatch patch{};
      patch.mode = SynthEngineMode::kBrass;
      patch.amp_env.attack_ms = 12.0f;
      patch.amp_env.sustain = 1.0f;
      patch.amp_env.release_ms = release_ms;
      patch.cutoff_hz = 20000.0f;
      patch.brass.conical = conical;
      patch.brass.lip_tension = lip_tension;
      patch.brass.lip_damping = lip_damping;
      patch.brass.brightness = brightness;
      patch.brass.damping = damping;
      patch.brass.attack_ms = attack_ms;
      patch.brass.release_ms = release_ms;
      patch.brass.breath_pressure = breath;
      patch.brass.vel_to_breath = 0.5f;
      // The bright small-bore bells (trumpet family) get the radiation formant;
      // large-bore / mellow brass stays on the round linear tone.
      if (bell_mix > 0.0f) {
        patch.body = BodyType::kBrassBell;
        patch.body_mix = bell_mix;
      }
      patch.gain = gain;
      v.config = from_patch(clamp_synth_patch(patch));
    };
    //     name             cone  tens   damp   bright damp   atk    rel     breath bell  gain
    brass("trumpet", false, 0.55f, 0.30f, 0.72f, 0.28f, 18.0f, 80.0f, 0.85f, 0.50f, 0.70f);
    brass("trombone", false, 0.48f, 0.45f, 0.55f, 0.32f, 26.0f, 100.0f, 0.85f, 0.0f, 0.72f);
    brass("tuba", true, 0.42f, 0.70f, 0.30f, 0.42f, 40.0f, 140.0f, 0.88f, 0.0f, 0.74f);
    brass("french-horn", true, 0.50f, 0.55f, 0.42f, 0.34f, 30.0f, 110.0f, 0.82f, 0.0f, 0.70f);
    brass("muted-trumpet", false, 0.58f, 0.35f, 0.62f, 0.30f, 16.0f, 75.0f, 0.80f, 0.0f, 0.66f);
    brass("cornet", true, 0.52f, 0.45f, 0.55f, 0.30f, 20.0f, 85.0f, 0.84f, 0.40f, 0.70f);
    brass("flugelhorn", true, 0.48f, 0.62f, 0.40f, 0.34f, 24.0f, 95.0f, 0.84f, 0.0f, 0.70f);
    brass("euphonium", true, 0.45f, 0.60f, 0.40f, 0.36f, 30.0f, 110.0f, 0.86f, 0.0f, 0.72f);
  }

  // --- air-jet flute (edge-tone waveguide) ---
  // The flute family (GM 73-80): one air-jet core voiced across the open-pipe
  // flutes and their breathier relatives. The jet self-oscillates and locks the
  // first register; the members differ by jet_ratio (the register colour),
  // brightness (the open-end reflection openness), damping (a high-damped bore
  // approximates the non-overblowing Helmholtz voicings — ocarina / blown bottle),
  // breath_noise (the air texture: shakuhachi / pan flute are breathy, the tin
  // whistle is pure) and vibrato (a solo flute's own, per-voice). The asymmetric
  // jet drive voices the octave-rich open-flue-pipe spectrum.
  {
    auto flute = [&](const char* name, float jet_ratio, float brightness, float damping,
                     float breath_noise, float chiff, float vibrato_depth, float breath,
                     float gain) {
      SynthPreset& v = t[i++];
      v.name = name;
      NativeSynthPatch patch{};
      patch.mode = SynthEngineMode::kFlute;
      patch.amp_env.attack_ms = 8.0f;
      patch.amp_env.sustain = 1.0f;
      patch.amp_env.release_ms = 120.0f;
      patch.cutoff_hz = 20000.0f;
      patch.flute.jet_ratio = jet_ratio;
      patch.flute.brightness = brightness;
      patch.flute.damping = damping;
      patch.flute.breath_noise = breath_noise;
      patch.flute.chiff = chiff;
      patch.flute.vibrato_depth = vibrato_depth;
      patch.flute.vibrato_rate_hz = 5.0f;
      patch.flute.breath_pressure = breath;
      patch.flute.vel_to_breath = 0.5f;
      patch.gain = gain;
      v.config = from_patch(clamp_synth_patch(patch));
    };
    //     name             jetr   bright damp   bnoise chiff  vib    breath gain
    flute("concert-flute", 0.50f, 0.55f, 0.30f, 0.18f, 0.35f, 0.15f, 0.60f, 0.85f);
    flute("piccolo", 0.50f, 0.75f, 0.25f, 0.12f, 0.40f, 0.10f, 0.62f, 0.80f);
    flute("recorder", 0.50f, 0.50f, 0.35f, 0.14f, 0.55f, 0.05f, 0.55f, 0.85f);
    flute("pan-flute", 0.52f, 0.42f, 0.40f, 0.40f, 0.30f, 0.08f, 0.55f, 0.85f);
    flute("shakuhachi", 0.52f, 0.48f, 0.35f, 0.55f, 0.30f, 0.20f, 0.58f, 0.85f);
    flute("tin-whistle", 0.48f, 0.70f, 0.28f, 0.10f, 0.45f, 0.04f, 0.62f, 0.80f);
    flute("ocarina", 0.50f, 0.40f, 0.55f, 0.15f, 0.30f, 0.06f, 0.55f, 0.85f);
    flute("blown-bottle", 0.50f, 0.35f, 0.50f, 0.35f, 0.25f, 0.0f, 0.55f, 0.85f);
  }

  // --- buzzing-bridge plucked string ---
  // The harp / koto / sitar family: one plucked-string core voiced from a clean
  // termination (harp / koto, buzz == 0) to the shimmering distributed bridge
  // contact of a sitar / tanpura (buzz > 0).
  {
    auto plucked = [&](const char* name, float buzz, float brightness, float decay_s,
                       float pick_position, float gain) {
      SynthPreset& v = t[i++];
      v.name = name;
      NativeSynthPatch patch{};
      patch.mode = SynthEngineMode::kPluckedString;
      patch.amp_env.attack_ms = 0.0f;
      patch.amp_env.sustain = 1.0f;
      patch.amp_env.release_ms = 200.0f;
      patch.cutoff_hz = 20000.0f;
      patch.plucked_string.buzz = buzz;
      patch.plucked_string.brightness = brightness;
      patch.plucked_string.decay_s = decay_s;
      patch.plucked_string.pick_position = pick_position;
      patch.gain = gain;
      v.config = from_patch(clamp_synth_patch(patch));
    };
    //       name        buzz   bright decay  pickpos gain
    // Keep the established GM-fallback `harp` name above. This separate
    // physical-model voice is intentionally named for its plucked engine so
    // every public catalog key resolves to exactly one patch.
    plucked("harp-plucked", 0.0f, 0.70f, 4.5f, 0.16f, 0.85f);
    plucked("koto", 0.0f, 0.80f, 3.0f, 0.22f, 0.85f);
    plucked("sitar", 0.55f, 0.85f, 3.5f, 0.20f, 0.80f);
    plucked("tanpura", 0.70f, 0.78f, 5.0f, 0.12f, 0.80f);
  }

  // --- source-filter vocal (glottal + formant) ---
  // The choir / voice family (GM 53-55): a glottal pulse driving a formant bank
  // selected by the vowel (0 = /a/, 1 = /e/, 2 = /i/, 3 = /o/, 4 = /u/).
  {
    auto vocal = [&](const char* name, int vowel, float brightness, float vibrato_depth,
                     float breath_noise, float gain) {
      SynthPreset& v = t[i++];
      v.name = name;
      NativeSynthPatch patch{};
      patch.mode = SynthEngineMode::kVocal;
      patch.amp_env.attack_ms = 20.0f;
      patch.amp_env.sustain = 1.0f;
      patch.amp_env.release_ms = 150.0f;
      patch.cutoff_hz = 20000.0f;
      patch.vocal.vowel = vowel;
      patch.vocal.brightness = brightness;
      patch.vocal.vibrato_depth = vibrato_depth;
      patch.vocal.breath_noise = breath_noise;
      patch.gain = gain;
      v.config = from_patch(clamp_synth_patch(patch));
    };
    //     name          vowel bright vib    breath gain
    vocal("choir-aah", 0, 0.55f, 0.30f, 0.12f, 0.80f);
    vocal("choir-ooh", 4, 0.45f, 0.25f, 0.10f, 0.80f);
    vocal("voice-eeh", 2, 0.60f, 0.35f, 0.14f, 0.80f);
  }

  // --- free reed (accordion / harmonica) ---
  // The free-reed family (GM 22-24): a driven tongue oscillator with an
  // asymmetric buzz and a musette detune (two tongues per note).
  {
    auto free_reed = [&](const char* name, float brightness, float reed_stiffness, float detune,
                         float gain) {
      SynthPreset& v = t[i++];
      v.name = name;
      NativeSynthPatch patch{};
      patch.mode = SynthEngineMode::kFreeReed;
      patch.amp_env.attack_ms = 12.0f;
      patch.amp_env.sustain = 1.0f;
      patch.amp_env.release_ms = 100.0f;
      patch.cutoff_hz = 20000.0f;
      patch.free_reed.brightness = brightness;
      patch.free_reed.reed_stiffness = reed_stiffness;
      patch.free_reed.detune = detune;
      patch.gain = gain;
      v.config = from_patch(clamp_synth_patch(patch));
    };
    //         name          bright stiff  detune gain
    free_reed("accordion", 0.60f, 0.50f, 0.35f, 0.80f);
    free_reed("harmonica", 0.75f, 0.65f, 0.15f, 0.78f);
    free_reed("bandoneon", 0.55f, 0.45f, 0.30f, 0.80f);
    free_reed("reed-organ", 0.50f, 0.40f, 0.10f, 0.82f);
  }

  return t;
}

const std::array<SynthPreset, kPresetCount>& presets() noexcept {
  static const std::array<SynthPreset, kPresetCount> kTable = build_presets();
  return kTable;
}

}  // namespace

size_t synth_preset_count() noexcept { return kPresetCount; }

const SynthPreset* synth_preset_at(size_t index) noexcept {
  if (index >= kPresetCount) return nullptr;
  return &presets()[index];
}

const SynthPreset* find_synth_preset(const char* name) noexcept {
  if (name == nullptr) return nullptr;
  for (const SynthPreset& preset : presets()) {
    if (std::strcmp(preset.name, name) == 0) return &preset;
  }
  return nullptr;
}

}  // namespace sonare::midi::synth
