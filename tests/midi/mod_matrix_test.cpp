/// @file mod_matrix_test.cpp
/// @brief NativeSynth modulation routing (midi/synth/mod_matrix), the second
///        LFO, glide/portamento and seeded unison determinism: each routing
///        moves its destination as configured (LFO2 -> amp tremolo,
///        velocity -> pan, key tracking -> pitch, mod wheel -> cutoff) and
///        unison detune is deterministic per (voice, note, age).

#include "midi/synth/mod_matrix.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"

namespace {

using sonare::midi::MidiEvent;
using sonare::midi::synth::evaluate_mod_matrix;
using sonare::midi::synth::ModDestination;
using sonare::midi::synth::ModMatrix;
using sonare::midi::synth::ModOffsets;
using sonare::midi::synth::ModSource;
using sonare::midi::synth::ModSourceValues;
using sonare::midi::synth::NativeSynth;
using sonare::midi::synth::NativeSynthConfig;
using sonare::midi::synth::NativeSynthPatch;
using sonare::midi::synth::NativeSynthVoice;
using sonare::midi::synth::VaWaveform;

constexpr double kRate = 48000.0;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

struct StereoRender {
  std::vector<float> left;
  std::vector<float> right;
};

StereoRender render(NativeSynth& synth, int num_samples) {
  StereoRender out;
  out.left.assign(static_cast<size_t>(num_samples), 0.0f);
  out.right.assign(static_cast<size_t>(num_samples), 0.0f);
  float* chans[2] = {out.left.data(), out.right.data()};
  synth.process(chans, 2, num_samples);
  return out;
}

float rms(const std::vector<float>& buf, size_t from, size_t to) {
  double acc = 0.0;
  size_t n = 0;
  for (size_t i = from; i < to && i < buf.size(); ++i) {
    acc += static_cast<double>(buf[i]) * buf[i];
    ++n;
  }
  return n > 0 ? static_cast<float>(std::sqrt(acc / static_cast<double>(n))) : 0.0f;
}

/// Dominant frequency from rising zero crossings in [from, to).
double estimate_frequency(const std::vector<float>& buf, size_t from, size_t to) {
  double first = -1.0;
  double last = -1.0;
  int cycles = -1;
  for (size_t i = from + 1; i < to && i < buf.size(); ++i) {
    if (buf[i - 1] < 0.0f && buf[i] >= 0.0f) {
      const double frac =
          static_cast<double>(buf[i - 1]) / (static_cast<double>(buf[i - 1]) - buf[i]);
      const double t = static_cast<double>(i - 1) + frac;
      if (first < 0.0) {
        first = t;
      } else {
        last = t;
      }
      ++cycles;
    }
  }
  if (cycles < 1 || last <= first) return 0.0;
  return kRate * static_cast<double>(cycles) / (last - first);
}

/// A clean sustained sine patch as the routing test bed.
NativeSynthPatch sine_patch() {
  NativeSynthPatch p;
  p.waveform = VaWaveform::kSine;
  p.amp_env.attack_ms = 1.0f;
  p.amp_env.decay_ms = 10.0f;
  p.amp_env.sustain = 1.0f;
  p.amp_env.release_ms = 30.0f;
  p.cutoff_hz = 20000.0f;
  return p;
}

}  // namespace

TEST_CASE("evaluate_mod_matrix accumulates routes into destination offsets", "[midi][synth]") {
  ModMatrix matrix;
  matrix.routes[0] = {ModSource::kLfo1, ModDestination::kPitchCents, 100.0f};
  matrix.routes[1] = {ModSource::kVelocity, ModDestination::kCutoffCents, 1200.0f};
  matrix.routes[2] = {ModSource::kModWheel, ModDestination::kAmpGain, -0.5f};
  matrix.routes[3] = {ModSource::kRandom, ModDestination::kPanUnits, 400.0f};

  ModSourceValues values;
  values.lfo1 = -0.5f;
  values.velocity = 1.0f;
  values.mod_wheel = 1.0f;
  values.random = 0.25f;

  const ModOffsets out = evaluate_mod_matrix(matrix, values);
  REQUIRE(out.pitch_cents == -50.0f);
  REQUIRE(out.cutoff_cents == 1200.0f);
  REQUIRE(out.amp_gain == 0.5f);
  REQUIRE(out.pan_units == 100.0f);

  REQUIRE(ModMatrix{}.empty());
  REQUIRE_FALSE(matrix.empty());
}

TEST_CASE("LFO2 -> amplitude routes as a tremolo at the LFO2 rate", "[midi][synth]") {
  NativeSynthConfig cfg;
  cfg.patch = sine_patch();
  cfg.patch.lfo2_rate_hz = 6.0f;
  cfg.patch.mod_matrix.routes[0] = {ModSource::kLfo2, ModDestination::kAmpGain, 0.9f};

  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 110)));
  const StereoRender out = render(synth, 24000);  // 0.5 s

  // 6 Hz tremolo: compare RMS at an LFO peak window vs a trough window.
  // Triangle LFO from phase 0: peak ~ t=1/24 s, trough ~ t=3/24 s.
  const size_t peak_at = 2000;    // 1/24 s
  const size_t trough_at = 6000;  // 3/24 s
  const float peak_rms = rms(out.left, peak_at - 480, peak_at + 480);
  const float trough_rms = rms(out.left, trough_at - 480, trough_at + 480);
  REQUIRE(peak_rms > 3.0f * trough_rms);
}

TEST_CASE("velocity -> pan routing shifts the stereo balance", "[midi][synth]") {
  NativeSynthConfig cfg;
  cfg.patch = sine_patch();
  cfg.patch.mod_matrix.routes[0] = {ModSource::kVelocity, ModDestination::kPanUnits, 500.0f};

  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  // Full velocity pans hard right.
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 127)));
  const StereoRender loud = render(synth, 4096);
  REQUIRE(rms(loud.right, 2048, 4096) > 5.0f * rms(loud.left, 2048, 4096));
}

TEST_CASE("key tracking -> pitch routing transposes by octave distance", "[midi][synth]") {
  NativeSynthConfig cfg;
  cfg.patch = sine_patch();
  // +1200 cents per octave above middle C doubles the transposition.
  cfg.patch.mod_matrix.routes[0] = {ModSource::kKeyTrack, ModDestination::kPitchCents, 1200.0f};

  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  // A4 (69) sits 0.75 octaves above 60 -> +900 cents -> 440 * 2^0.75.
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 110)));
  const StereoRender out = render(synth, 9600);
  const double freq = estimate_frequency(out.left, 4800, 9600);
  const double expected = 440.0 * std::exp2(0.75);
  REQUIRE(freq > expected * 0.98);
  REQUIRE(freq < expected * 1.02);
}

TEST_CASE("mod wheel -> cutoff routing brightens with CC1", "[midi][synth]") {
  NativeSynthConfig cfg;
  cfg.patch = sine_patch();
  cfg.patch.waveform = VaWaveform::kSaw;
  cfg.patch.cutoff_hz = 300.0f;
  cfg.patch.mod_matrix.routes[0] = {ModSource::kModWheel, ModDestination::kCutoffCents, 4800.0f};

  auto level_with_wheel = [&cfg](uint8_t wheel) {
    NativeSynth synth(cfg);
    synth.prepare(kRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 1, wheel)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 110)));
    const StereoRender out = render(synth, 9600);
    return rms(out.left, 4800, 9600);
  };
  // Opening the filter by 4 octaves passes far more saw energy.
  REQUIRE(level_with_wheel(127) > 1.5f * level_with_wheel(0));
}

TEST_CASE("the response destinations accumulate multiplicatively and clamp", "[midi][synth]") {
  ModMatrix matrix;
  matrix.routes[0] = {ModSource::kModWheel, ModDestination::kResonanceQ, 8.0f};
  matrix.routes[1] = {ModSource::kVelocity, ModDestination::kVibratoDepthCents, 300.0f};
  matrix.routes[2] = {ModSource::kAmpEnv, ModDestination::kFilterEnvDepth, -0.5f};
  matrix.routes[3] = {ModSource::kLfo2, ModDestination::kLfo1RateScale, 1.0f};

  ModSourceValues values;
  values.mod_wheel = 0.5f;
  values.velocity = 1.0f;
  values.amp_env = 1.0f;
  values.lfo2 = 1.0f;

  const ModOffsets out = evaluate_mod_matrix(matrix, values);
  CHECK(out.resonance_q == 4.0f);
  CHECK(out.vibrato_depth_cents == 300.0f);
  CHECK(out.filter_env_depth == 0.5f);
  CHECK(out.lfo1_rate_scale == 2.0f);

  // An unrouted matrix leaves each destination at the value that makes it a
  // no-op, which is what keeps a patch that never asked for one bit-identical.
  const ModOffsets idle = evaluate_mod_matrix(ModMatrix{}, values);
  CHECK(idle.resonance_q == 0.0f);
  CHECK(idle.vibrato_depth_cents == 0.0f);
  CHECK(idle.filter_env_depth == 1.0f);
  CHECK(idle.lfo1_rate_scale == 1.0f);
}

TEST_CASE("the LFO rate scale floor keeps the LFO moving", "[midi][synth]") {
  // A scale of zero would freeze the LFO wherever its phase stopped, which is a
  // stuck detune rather than an absence of vibrato.
  ModMatrix matrix;
  matrix.routes[0] = {ModSource::kModWheel, ModDestination::kLfo1RateScale, -4.0f};
  ModSourceValues values;
  values.mod_wheel = 1.0f;
  CHECK(evaluate_mod_matrix(matrix, values).lfo1_rate_scale == 0.0625f);

  matrix.routes[0] = {ModSource::kModWheel, ModDestination::kLfo1RateScale, 100.0f};
  CHECK(evaluate_mod_matrix(matrix, values).lfo1_rate_scale == 16.0f);
}

TEST_CASE("mod wheel -> resonance sharpens the filter peak", "[midi][synth]") {
  NativeSynthConfig cfg;
  cfg.patch = sine_patch();
  cfg.patch.waveform = VaWaveform::kSaw;
  cfg.patch.cutoff_hz = 800.0f;
  cfg.patch.mod_matrix.routes[0] = {ModSource::kModWheel, ModDestination::kResonanceQ, 10.0f};

  auto level_with_wheel = [&cfg](uint8_t wheel) {
    NativeSynth synth(cfg);
    synth.prepare(kRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 1, wheel)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 110)));
    const StereoRender out = render(synth, 9600);
    return rms(out.left, 4800, 9600);
  };
  // The resonant peak sits over a saw partial, so it adds energy the flat
  // response does not pass.
  REQUIRE(level_with_wheel(127) > 1.2f * level_with_wheel(0));
}

TEST_CASE("mod wheel -> vibrato depth opens a vibrato the patch does not have", "[midi][synth]") {
  NativeSynthConfig cfg;
  cfg.patch = sine_patch();
  // The patch itself asks for no vibrato; the wheel is the whole depth.
  cfg.patch.mod_matrix.routes[0] = {ModSource::kModWheel, ModDestination::kVibratoDepthCents,
                                    600.0f};

  auto swing = [&cfg](uint8_t wheel) {
    NativeSynth synth(cfg);
    synth.prepare(kRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 1, wheel)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 110)));
    const StereoRender out = render(synth, 12000);
    // LFO1 runs at 5 Hz: a triangle from phase 0 peaks at 2400 samples and
    // troughs at 7200.
    const double high = estimate_frequency(out.left, 1800, 3000);
    const double low = estimate_frequency(out.left, 6600, 7800);
    return low > 0.0 ? high / low : 0.0;
  };
  REQUIRE(swing(127) > 1.3);
  // Wheel down is the unmodulated patch: both windows report the same note.
  const double idle = swing(0);
  REQUIRE(idle > 0.98);
  REQUIRE(idle < 1.02);
}

TEST_CASE("mod wheel -> filter envelope depth scales the sweep, not its origin", "[midi][synth]") {
  NativeSynthConfig cfg;
  cfg.patch = sine_patch();
  cfg.patch.waveform = VaWaveform::kSaw;
  cfg.patch.cutoff_hz = 300.0f;
  cfg.patch.env_to_cutoff_cents = 4800.0f;
  cfg.patch.filter_env.attack_ms = 1.0f;
  cfg.patch.filter_env.decay_ms = 400.0f;
  cfg.patch.filter_env.sustain = 1.0f;
  // A full wheel cancels the envelope's contribution entirely (1 + -1 * 1).
  // The source is the wheel rather than velocity because velocity also sets the
  // amplitude, which would move the same measurement for the wrong reason.
  cfg.patch.mod_matrix.routes[0] = {ModSource::kModWheel, ModDestination::kFilterEnvDepth, -1.0f};

  auto level_with_wheel = [&cfg](uint8_t wheel) {
    NativeSynth synth(cfg);
    synth.prepare(kRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 1, wheel)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 110)));
    const StereoRender out = render(synth, 9600);
    return rms(out.left, 2400, 9600);
  };
  // The cutoff the envelope sweeps FROM is untouched: cancelling the depth
  // leaves the note at its 300 Hz origin instead of four octaves above it.
  REQUIRE(level_with_wheel(0) > 1.2f * level_with_wheel(127));
}

TEST_CASE("mod wheel -> LFO1 rate retunes the vibrato it is already driving", "[midi][synth]") {
  NativeSynthConfig cfg;
  cfg.patch = sine_patch();
  cfg.patch.lfo_rate_hz = 5.0f;
  cfg.patch.mod_matrix.routes[0] = {ModSource::kLfo1, ModDestination::kAmpGain, 0.9f};
  cfg.patch.mod_matrix.routes[1] = {ModSource::kModWheel, ModDestination::kLfo1RateScale, 3.0f};

  auto tremolo_cycles = [&cfg](uint8_t wheel) {
    NativeSynth synth(cfg);
    synth.prepare(kRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 1, wheel)));
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 110)));
    const StereoRender out = render(synth, 24000);  // 0.5 s
    // Short-window RMS is the tremolo envelope; count its crossings of its own
    // mean and halve to get cycles.
    constexpr size_t kWindow = 240;
    std::vector<float> env;
    for (size_t i = 0; i + kWindow <= out.left.size(); i += kWindow) {
      env.push_back(rms(out.left, i, i + kWindow));
    }
    double mean = 0.0;
    for (float v : env) mean += v;
    mean /= static_cast<double>(env.size());
    int crossings = 0;
    for (size_t i = 1; i < env.size(); ++i) {
      if ((env[i - 1] < mean) != (env[i] < mean)) ++crossings;
    }
    return crossings / 2.0;
  };
  // 5 Hz over half a second is about 2.5 cycles; a scale of 4 makes it 10.
  const double slow = tremolo_cycles(0);
  const double fast = tremolo_cycles(127);
  REQUIRE(slow > 1.5);
  REQUIRE(slow < 4.0);
  REQUIRE(fast > 2.5 * slow);
}

TEST_CASE("glide slides the pitch from the previous note", "[midi][synth]") {
  NativeSynthConfig cfg;
  cfg.patch = sine_patch();
  cfg.patch.glide_ms = 150.0f;

  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  // Establish A3 (220 Hz), then jump an octave to A4 (440 Hz).
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 57, 110)));
  render(synth, 4800);
  synth.on_event(0, event(sonare::midi::make_midi1_note_off(0, 0, 57, 0)));
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 110)));
  const StereoRender out = render(synth, 24000);

  // Early window: still near the old pitch, clearly below the target.
  const double early = estimate_frequency(out.left, 480, 2400);
  REQUIRE(early > 220.0);
  REQUIRE(early < 400.0);
  // After ~0.4 s the glide has landed on the target.
  const double late = estimate_frequency(out.left, 19200, 24000);
  REQUIRE(late > 440.0 * 0.99);
  REQUIRE(late < 440.0 * 1.01);

  // The first note of a channel starts on pitch (no glide source).
  NativeSynth fresh(cfg);
  fresh.prepare(kRate, 256);
  fresh.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 69, 110)));
  const StereoRender first = render(fresh, 9600);
  const double immediate = estimate_frequency(first.left, 480, 4800);
  REQUIRE(immediate > 440.0 * 0.99);
  REQUIRE(immediate < 440.0 * 1.01);
}

TEST_CASE("unison detune is seeded deterministically per voice", "[midi][synth]") {
  NativeSynthPatch patch;
  patch.unison = 7;
  patch.detune_cents = 20.0f;
  patch.cutoff_hz = 20000.0f;
  patch.amp_env.sustain = 1.0f;
  const NativeSynthPatch clamped = sonare::midi::synth::clamp_synth_patch(patch);

  sonare::midi::synth::Sf2ChannelMod mod;
  auto run_voice = [&](uint32_t voice_index, uint64_t age) {
    NativeSynthVoice voice{};
    voice.note = 60;
    voice.channel = 0;
    voice.age = age;
    voice.active = true;
    voice.start(clamped, kRate, 100, voice_index);
    std::vector<float> out(512);
    for (float& s : out) s = voice.render(mod);
    return out;
  };

  // Same (voice_index, note, age) -> bit-identical output.
  REQUIRE(run_voice(0, 1) == run_voice(0, 1));
  // A different voice slot or age decorrelates the seeded detune stack.
  REQUIRE(run_voice(0, 1) != run_voice(1, 1));
  REQUIRE(run_voice(0, 1) != run_voice(0, 2));
}
