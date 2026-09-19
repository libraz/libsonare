/// @file mod_matrix_test.cpp
/// @brief NativeSynth modulation routing (midi/synth/mod_matrix), the second
///        LFO, glide/portamento and seeded unison determinism: each routing
///        moves its destination as configured (LFO2 -> amp tremolo,
///        velocity -> pan, key tracking -> pitch, mod wheel -> cutoff) and
///        unison detune is deterministic per (voice, note, age).

#include "midi/synth/mod_matrix.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <utility>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "support/midi_render.h"

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

using sonare::test::event;

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

/// Brightness proxy: the first difference scales each partial by
/// |2 sin(pi f / fs)|, which rises monotonically with frequency, so the
/// differenced-to-plain RMS ratio orders two renders by where their energy sits
/// without an FFT.
double high_frequency_ratio(const std::vector<float>& buf, size_t from) {
  std::vector<float> diff;
  diff.reserve(buf.size());
  for (size_t i = from + 1; i < buf.size(); ++i) diff.push_back(buf[i] - buf[i - 1]);
  const float plain = rms(buf, from, buf.size());
  if (plain <= 0.0f) return 0.0;
  return static_cast<double>(rms(diff, 0, diff.size())) / plain;
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

TEST_CASE("the series highpass narrows the voice to a band", "[midi][synth]") {
  NativeSynthConfig cfg;
  cfg.patch = sine_patch();
  cfg.patch.waveform = VaWaveform::kSaw;
  cfg.patch.cutoff_hz = 2000.0f;

  auto level_with_hp = [&cfg](float hp_hz) {
    NativeSynthConfig local = cfg;
    local.patch.hp_cutoff_hz = hp_hz;
    NativeSynth synth(local);
    synth.prepare(kRate, 256);
    // A2 (45) puts the fundamental near 110 Hz, well under the highpass.
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 45, 110)));
    const StereoRender out = render(synth, 9600);
    return rms(out.left, 4800, 9600);
  };

  // 0 leaves the stage out entirely, which is what every existing patch asks
  // for; 800 Hz removes the fundamental and everything under it.
  const float wide = level_with_hp(0.0f);
  const float banded = level_with_hp(800.0f);
  REQUIRE(wide > 0.0f);
  REQUIRE(banded < 0.5f * wide);
}

TEST_CASE("the series highpass keeps its own resonance out of the band", "[midi][synth]") {
  // The patch Q belongs to the main filter: raising it must not sharpen the
  // highpass corner, or a band would grow a peak at each end.
  NativeSynthConfig cfg;
  cfg.patch = sine_patch();
  cfg.patch.waveform = VaWaveform::kSaw;
  cfg.patch.cutoff_hz = 20000.0f;
  cfg.patch.hp_cutoff_hz = 1000.0f;

  auto level_at_q = [&cfg](float q) {
    NativeSynthConfig local = cfg;
    local.patch.resonance_q = q;
    NativeSynth synth(local);
    synth.prepare(kRate, 256);
    synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 45, 110)));
    const StereoRender out = render(synth, 9600);
    return rms(out.left, 4800, 9600);
  };
  // The main filter sits wide open at 20 kHz, so its Q has nothing to colour
  // and the highpass corner stays where it is.
  const float flat = level_at_q(0.707f);
  const float sharp = level_at_q(12.0f);
  REQUIRE(sharp < 1.1f * flat);
  REQUIRE(sharp > 0.9f * flat);
}

namespace {

/// A saw voice with a flat amplitude envelope, so what the converter does is
/// the only thing shaping the samples the assertions look at.
std::vector<float> render_converted(float sample_hold_hz, float bit_depth, int num_samples) {
  NativeSynthConfig cfg;
  cfg.patch = sine_patch();
  cfg.patch.waveform = VaWaveform::kSaw;
  cfg.patch.cutoff_hz = 20000.0f;
  cfg.patch.sample_hold_hz = sample_hold_hz;
  cfg.patch.bit_depth = bit_depth;
  // The converter is a VOICE stage and the mix bus filters what leaves it, so
  // the bus DC blocker is switched off here: it is an IIR and it would smear the
  // held steps and the quantizer's lattice into a continuum before either could
  // be measured. Nothing about the stage under test depends on it.
  cfg.dc_block = false;
  NativeSynth synth(cfg);
  synth.prepare(kRate, 512);
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 45, 110)));
  return render(synth, num_samples).left;
}

}  // namespace

TEST_CASE("the voice converter holds its output at the rate it was given", "[midi][synth]") {
  // The hold has to be visible as repeated samples: a stage that filtered
  // instead of held would smooth the steps away, and the aliased images those
  // steps fold down are the reason to run a converter below the mix rate.
  const int n = 9600;
  const std::vector<float> held = render_converted(4800.0f, 0.0f, n);  // a tenth of the mix rate

  int changes = 0;
  for (size_t i = 4801; i < held.size(); ++i) {
    if (held[i] != held[i - 1]) ++changes;
  }
  const int expected = static_cast<int>((held.size() - 4801) / 10);
  REQUIRE(changes > expected - 3);
  REQUIRE(changes < expected + 3);
}

TEST_CASE("the voice quantizer lands the output on a lattice", "[midi][synth]") {
  const int n = 9600;
  const std::vector<float> crushed = render_converted(0.0f, 4.0f, n);

  // The quantizer runs before the amplitude envelope and the patch gain, so the
  // lattice that reaches the output is the step scaled by everything after it.
  // Its spacing is what the test can see; recover it from the smallest gap.
  float step = 1.0f;
  for (size_t i = 4801; i < crushed.size(); ++i) {
    const float gap = std::abs(crushed[i] - crushed[i - 1]);
    if (gap > 1.0e-7f && gap < step) step = gap;
  }
  REQUIRE(step < 0.5f);  // a lattice was found at all

  int off_lattice = 0;
  for (size_t i = 4801; i < crushed.size(); ++i) {
    const float ratio = crushed[i] / step;
    if (std::abs(ratio - std::round(ratio)) > 1.0e-3f) ++off_lattice;
  }
  REQUIRE(off_lattice == 0);
}

TEST_CASE("a converter left at zero renders the voice unchanged", "[midi][synth]") {
  // Both halves off has to be the voicing that predates the stage, or no patch
  // calibrated before it can be trusted afterwards.
  const std::vector<float> plain = render_converted(0.0f, 0.0f, 4096);
  const std::vector<float> again = render_converted(0.0f, 0.0f, 4096);
  REQUIRE(plain == again);

  // A hold rate at or above the mix rate holds nothing, so it is the same
  // signal too — the stage switches itself off rather than repeating a sample.
  const std::vector<float> unheld = render_converted(static_cast<float>(kRate), 0.0f, 4096);
  REQUIRE(unheld == plain);
}

// ---------------------------------------------------------------------------
// Excitation axes: the matrix reaching the physical model's exciter.
// ---------------------------------------------------------------------------

namespace {

/// A sustained brass patch: the bore is continuously excited, so an excitation
/// axis has something to act on for the whole note.
NativeSynthPatch brass_excitation_patch() {
  NativeSynthPatch p;
  p.mode = sonare::midi::synth::SynthEngineMode::kBrass;
  p.cutoff_hz = 20000.0f;
  p.amp_env.attack_ms = 5.0f;
  p.amp_env.sustain = 1.0f;
  p.amp_env.release_ms = 100.0f;
  p.brass.breath_pressure = 0.5f;
  p.brass.vel_to_breath = 0.0f;  // the patch alone sets the base, not velocity
  p.brass.brightness = 0.5f;
  return p;
}

/// A drawbar-organ patch carrying two registrations far enough apart to order
/// by brightness: the 16'/5-1/3'/8' base against the 1' alone. Their
/// unnormalized sums differ (three stops against one), which is what lets a
/// crossfade normalized against only the first end read as a level error.
NativeSynthPatch additive_morph_patch() {
  NativeSynthPatch p;
  p.mode = sonare::midi::synth::SynthEngineMode::kAdditive;
  p.cutoff_hz = 20000.0f;
  p.amp_env.attack_ms = 5.0f;
  p.amp_env.sustain = 1.0f;
  p.additive.drawbars = {8.0f, 8.0f, 8.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  p.additive.drawbars_b = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 8.0f};
  p.additive.key_click = 0.0f;  // a note-on transient is not on this axis
  return p;
}

/// Renders one held note, optionally sending CC @p cc at @p cc_value first.
std::vector<float> render_note(const NativeSynthPatch& patch, int num_samples, int cc = -1,
                               uint8_t cc_value = 0) {
  NativeSynthConfig cfg;
  cfg.patch = patch;
  NativeSynth synth(cfg);
  synth.prepare(kRate, 256);
  if (cc >= 0) {
    synth.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, static_cast<uint8_t>(cc),
                                                                    cc_value)));
  }
  synth.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 53, 100)));
  return render(synth, num_samples).left;
}

}  // namespace

TEST_CASE("the excitation axes accumulate and clamp to one axis span", "[midi][synth]") {
  ModMatrix matrix;
  matrix.routes[0] = {ModSource::kVelocity, ModDestination::kExcitationForce, 0.4f};
  matrix.routes[1] = {ModSource::kModWheel, ModDestination::kExcitationPosition, -0.3f};
  matrix.routes[2] = {ModSource::kLfo1, ModDestination::kExcitationBrightness, 0.5f};

  ModSourceValues values;
  values.velocity = 0.5f;
  values.mod_wheel = 1.0f;
  values.lfo1 = -1.0f;

  const ModOffsets out = evaluate_mod_matrix(matrix, values);
  REQUIRE(out.excitation_force == 0.2f);
  REQUIRE(out.excitation_position == -0.3f);
  REQUIRE(out.excitation_brightness == -0.5f);

  // The clamp is load-bearing rather than declared: two routes that together
  // ask for three axis spans arrive as one.
  ModMatrix piled;
  piled.routes[0] = {ModSource::kVelocity, ModDestination::kExcitationForce, 2.0f};
  piled.routes[1] = {ModSource::kModWheel, ModDestination::kExcitationForce, 1.0f};
  ModSourceValues full;
  full.velocity = 1.0f;
  full.mod_wheel = 1.0f;
  const ModOffsets piled_out = evaluate_mod_matrix(piled, full);
  REQUIRE(piled_out.excitation_force == 1.0f);

  ModMatrix negative;
  negative.routes[0] = {ModSource::kVelocity, ModDestination::kExcitationBrightness, -4.0f};
  REQUIRE(evaluate_mod_matrix(negative, full).excitation_brightness == -1.0f);
}

TEST_CASE("has_engine_control_route answers for the live routes only", "[midi][synth]") {
  REQUIRE_FALSE(ModMatrix{}.has_engine_control_route());

  ModMatrix pitch_only;
  pitch_only.routes[0] = {ModSource::kLfo1, ModDestination::kPitchCents, 50.0f};
  REQUIRE_FALSE(pitch_only.has_engine_control_route());
  REQUIRE_FALSE(pitch_only.empty());

  ModMatrix dead_depth;
  dead_depth.routes[0] = {ModSource::kLfo1, ModDestination::kExcitationForce, 0.0f};
  REQUIRE_FALSE(dead_depth.has_engine_control_route());

  ModMatrix no_source;
  no_source.routes[0] = {ModSource::kNone, ModDestination::kExcitationForce, 0.5f};
  REQUIRE_FALSE(no_source.has_engine_control_route());

  ModMatrix live;
  live.routes[0] = {ModSource::kLfo1, ModDestination::kPitchCents, 50.0f};
  live.routes[3] = {ModSource::kVelocity, ModDestination::kExcitationPosition, 0.5f};
  REQUIRE(live.has_engine_control_route());

  // The morph is an engine-owned axis too, so a matrix carrying only that one
  // must still reach the dispatch.
  ModMatrix morph_only;
  morph_only.routes[0] = {ModSource::kVelocity, ModDestination::kSpectrumMorph, 0.5f};
  REQUIRE(morph_only.has_engine_control_route());
}

TEST_CASE("an excitation force route drives the breath axis it names", "[midi][synth]") {
  // Velocity is constant over the note, so this is a steady offset on the mouth
  // pressure rather than a moving one: what is measured is the axis arriving.
  // Loudness is the direction the breath axis has — the same one CC2 is tested
  // for — so the assertion is on level rather than on spectrum. The bell
  // brightness deliberately gets no direction assertion below: the linear bore
  // has no monotone one to give, which is why the CC74 test does not make one
  // either.
  const std::vector<float> plain = render_note(brass_excitation_patch(), 24000);
  REQUIRE(rms(plain, 12000, 24000) > 0.001f);  // the note sounds at all

  NativeSynthPatch harder = brass_excitation_patch();
  harder.mod_matrix.routes[0] = {ModSource::kVelocity, ModDestination::kExcitationForce, 0.5f};
  NativeSynthPatch softer = brass_excitation_patch();
  softer.mod_matrix.routes[0] = {ModSource::kVelocity, ModDestination::kExcitationForce, -0.4f};

  const float plain_rms = rms(plain, 12000, 24000);
  const float hard_rms = rms(render_note(harder, 24000), 12000, 24000);
  const float soft_rms = rms(render_note(softer, 24000), 12000, 24000);

  // Direction, not just difference: the sign of the depth decides which way the
  // breath moves. A test that only asserted "the buffers differ" would pass on
  // a routing wired to the wrong axis, or to the right axis backwards.
  REQUIRE(hard_rms > plain_rms);
  REQUIRE(soft_rms < plain_rms);
}

TEST_CASE("the brightness route lands on the axis CC74 drives", "[midi][synth]") {
  // No direction is claimed for the bell, so the axis is pinned a different
  // way: the route and the CC must saturate together. If the route landed on
  // some other quantity, pushing past the CC's ceiling would still move it.
  const NativeSynthPatch base = brass_excitation_patch();
  const std::vector<float> at_ceiling = render_note(base, 16000, 74, 127);

  NativeSynthPatch pushed = base;
  pushed.mod_matrix.routes[0] = {ModSource::kVelocity, ModDestination::kExcitationBrightness, 0.6f};
  REQUIRE(render_note(pushed, 16000, 74, 127) == at_ceiling);

  NativeSynthPatch pulled = base;
  pulled.mod_matrix.routes[0] = {ModSource::kVelocity, ModDestination::kExcitationBrightness,
                                 -0.6f};
  const std::vector<float> below = render_note(pulled, 16000, 74, 127);
  REQUIRE(below != at_ceiling);
  REQUIRE(std::isfinite(below.back()));
}

TEST_CASE("the bowed string takes force and contact point apart", "[midi][synth]") {
  NativeSynthPatch bowed;
  bowed.mode = sonare::midi::synth::SynthEngineMode::kBowedString;
  bowed.cutoff_hz = 20000.0f;
  bowed.amp_env.attack_ms = 5.0f;
  bowed.amp_env.sustain = 1.0f;
  bowed.bowed_string.bow_force = 0.5f;
  bowed.bowed_string.bow_position = 0.13f;
  bowed.bowed_string.vel_to_speed = 0.0f;

  const std::vector<float> plain = render_note(bowed, 24000);
  REQUIRE(rms(plain, 12000, 24000) > 0.001f);

  // Position is the one axis with a single engine behind it, so it is asserted
  // on its own rather than riding along with force.
  NativeSynthPatch moved = bowed;
  moved.mod_matrix.routes[0] = {ModSource::kVelocity, ModDestination::kExcitationPosition, 0.6f};
  const std::vector<float> shifted = render_note(moved, 24000);
  REQUIRE(shifted != plain);
  REQUIRE(std::isfinite(shifted.back()));

  NativeSynthPatch pressed = bowed;
  pressed.mod_matrix.routes[0] = {ModSource::kVelocity, ModDestination::kExcitationForce, 0.5f};
  const std::vector<float> harder = render_note(pressed, 24000);
  REQUIRE(harder != plain);
  REQUIRE(harder != shifted);  // the two axes are not the same wire
  REQUIRE(std::isfinite(harder.back()));
}

TEST_CASE("an excitation route whose source stays at zero changes nothing", "[midi][synth]") {
  // The regression this guards: the plumbing runs every sample on a voice that
  // has the route, and never on a voice that has not. If composing an offset of
  // zero were not exactly the identity, every patch that gained a route would
  // shift under it before the source ever moved.
  NativeSynthPatch routed = brass_excitation_patch();
  routed.mod_matrix.routes[0] = {ModSource::kModWheel, ModDestination::kExcitationForce, 0.8f};
  routed.mod_matrix.routes[1] = {ModSource::kModWheel, ModDestination::kExcitationBrightness,
                                 -0.8f};

  // CC1 is never sent, so the mod wheel reads 0 for the whole note.
  const std::vector<float> with_route = render_note(routed, 16000);
  const std::vector<float> without = render_note(brass_excitation_patch(), 16000);
  REQUIRE(with_route == without);

  // Positive control for the comparison above: the same route with a source
  // that is not zero does move the render, so the equality is a result rather
  // than a buffer nothing ever wrote to.
  NativeSynthPatch driven = routed;
  driven.mod_matrix.routes[0].source = ModSource::kVelocity;
  driven.mod_matrix.routes[1].source = ModSource::kVelocity;
  REQUIRE(render_note(driven, 16000) != without);
}

TEST_CASE("an engine with no exciter to reach declines the axes", "[midi][synth]") {
  // A subtractive voice has no continuous exciter and no second spectral table,
  // so the routes fall through the dispatch rather than landing somewhere
  // approximate.
  NativeSynthPatch routed = sine_patch();
  routed.mod_matrix.routes[0] = {ModSource::kVelocity, ModDestination::kExcitationForce, 1.0f};
  routed.mod_matrix.routes[1] = {ModSource::kVelocity, ModDestination::kExcitationBrightness,
                                 -1.0f};
  routed.mod_matrix.routes[2] = {ModSource::kVelocity, ModDestination::kSpectrumMorph, 1.0f};
  REQUIRE(render_note(routed, 8000) == render_note(sine_patch(), 8000));
}

TEST_CASE("an excitation offset composes with the CC on the same axis", "[midi][synth]") {
  const NativeSynthPatch base = brass_excitation_patch();

  // CC2 puts the breath axis at its ceiling; a positive force offset on top has
  // nowhere to go, so the engine's own clamp holds and the render is unmoved.
  const std::vector<float> cc_only = render_note(base, 16000, 2, 127);
  NativeSynthPatch pushed = base;
  pushed.mod_matrix.routes[0] = {ModSource::kVelocity, ModDestination::kExcitationForce, 0.6f};
  REQUIRE(render_note(pushed, 16000, 2, 127) == cc_only);

  // Pulling down from the same ceiling does move, which is what says the
  // equality above is the clamp rather than the offset being dropped.
  NativeSynthPatch pulled = base;
  pulled.mod_matrix.routes[0] = {ModSource::kVelocity, ModDestination::kExcitationForce, -0.6f};
  REQUIRE(render_note(pulled, 16000, 2, 127) != cc_only);

  // And the CC still wins over the patch where the matrix is silent: the base
  // the offset composes with is the CC's, not the patch's.
  REQUIRE(cc_only != render_note(base, 16000));
}

TEST_CASE("every engine the dispatch names is actually reached", "[midi][synth]") {
  // Four engines are wired and two of them are asserted above through their own
  // axes. This covers the other two, so a wire cannot be declared in the switch
  // and reach nothing: each engine renders differently with the route than
  // without, and the four are checked as one population rather than one by one.
  struct Case {
    const char* name;
    sonare::midi::synth::SynthEngineMode mode;
  };
  const Case cases[] = {
      {"reed", sonare::midi::synth::SynthEngineMode::kReed},
      {"flute", sonare::midi::synth::SynthEngineMode::kFlute},
  };

  for (const Case& c : cases) {
    NativeSynthPatch p;
    p.mode = c.mode;
    p.cutoff_hz = 20000.0f;
    p.amp_env.attack_ms = 5.0f;
    p.amp_env.sustain = 1.0f;
    if (c.mode == sonare::midi::synth::SynthEngineMode::kReed) {
      p.reed.vel_to_breath = 0.0f;
    } else {
      p.flute.vel_to_breath = 0.0f;
    }

    INFO(c.name);
    const std::vector<float> plain = render_note(p, 24000);
    REQUIRE(rms(plain, 12000, 24000) > 0.0005f);  // the note sounds at all

    NativeSynthPatch forced = p;
    forced.mod_matrix.routes[0] = {ModSource::kVelocity, ModDestination::kExcitationForce, -0.4f};
    const std::vector<float> by_force = render_note(forced, 24000);
    REQUIRE(by_force != plain);
    REQUIRE(std::isfinite(by_force.back()));

    NativeSynthPatch lit = p;
    lit.mod_matrix.routes[0] = {ModSource::kVelocity, ModDestination::kExcitationBrightness, -0.5f};
    const std::vector<float> by_brightness = render_note(lit, 24000);
    REQUIRE(by_brightness != plain);
    REQUIRE(by_brightness != by_force);  // the two axes are not the same wire
    REQUIRE(std::isfinite(by_brightness.back()));

    // The position axis has no meaning on a wind bore, so it must be declined
    // rather than folded onto the nearest thing the engine does have.
    NativeSynthPatch placed = p;
    placed.mod_matrix.routes[0] = {ModSource::kVelocity, ModDestination::kExcitationPosition, 1.0f};
    REQUIRE(render_note(placed, 24000) == plain);
  }
}

TEST_CASE("the spectrum morph accumulates and clamps on the same span", "[midi][synth]") {
  ModMatrix matrix;
  matrix.routes[0] = {ModSource::kVelocity, ModDestination::kSpectrumMorph, 0.4f};
  ModSourceValues values;
  values.velocity = 0.5f;
  REQUIRE(evaluate_mod_matrix(matrix, values).spectrum_morph == 0.2f);

  // Load-bearing rather than declared, in both directions: the morph position
  // spans [0,1], so an offset of more than one span cannot mean anything.
  ModMatrix piled;
  piled.routes[0] = {ModSource::kVelocity, ModDestination::kSpectrumMorph, 2.0f};
  piled.routes[1] = {ModSource::kModWheel, ModDestination::kSpectrumMorph, 1.0f};
  ModSourceValues full;
  full.velocity = 1.0f;
  full.mod_wheel = 1.0f;
  REQUIRE(evaluate_mod_matrix(piled, full).spectrum_morph == 1.0f);

  ModMatrix negative;
  negative.routes[0] = {ModSource::kVelocity, ModDestination::kSpectrumMorph, -4.0f};
  REQUIRE(evaluate_mod_matrix(negative, full).spectrum_morph == -1.0f);
}

TEST_CASE("a spectrum morph route travels toward the registration it names", "[midi][synth]") {
  // Unlike the bell, this axis has a direction, and it is the patch's own two
  // tables that supply it: the base is the bottom three drawbars and the second
  // registration is the 1' alone, four octaves up.
  const NativeSynthPatch base = additive_morph_patch();
  const std::vector<float> at_base = render_note(base, 24000);
  REQUIRE(rms(at_base, 12000, 24000) > 0.001f);  // the note sounds at all

  NativeSynthPatch swept = base;
  swept.mod_matrix.routes[0] = {ModSource::kVelocity, ModDestination::kSpectrumMorph, 2.0f};
  const std::vector<float> at_second = render_note(swept, 24000);
  REQUIRE(high_frequency_ratio(at_second, 12000) > 4.0 * high_frequency_ratio(at_base, 12000));

  // And back the other way from the far end, so a route wired backwards fails
  // rather than passing on "the two buffers differ".
  NativeSynthPatch from_second = base;
  from_second.additive.morph = 1.0f;
  const std::vector<float> held_at_second = render_note(from_second, 24000);
  NativeSynthPatch pulled = from_second;
  pulled.mod_matrix.routes[0] = {ModSource::kVelocity, ModDestination::kSpectrumMorph, -2.0f};
  REQUIRE(high_frequency_ratio(render_note(pulled, 24000), 12000) <
          0.5 * high_frequency_ratio(held_at_second, 12000));
}

TEST_CASE("each registration is normalized against its own sum", "[midi][synth]") {
  // The far end of the crossfade must render as the second registration would
  // on its own. Normalizing the whole sweep against the first end's sum would
  // leave this one three stops' worth of level out, and nothing about the
  // spectrum would look wrong.
  NativeSynthPatch at_far_end = additive_morph_patch();
  at_far_end.additive.morph = 1.0f;

  NativeSynthPatch only_second = additive_morph_patch();
  only_second.additive.drawbars = only_second.additive.drawbars_b;

  REQUIRE(render_note(at_far_end, 16000) == render_note(only_second, 16000));
  // Positive control: the two ends are not the same render to begin with.
  REQUIRE(render_note(at_far_end, 16000) != render_note(additive_morph_patch(), 16000));
}

TEST_CASE("a patch naming no second registration is not swept anywhere", "[midi][synth]") {
  // The axis is declined by the data rather than by a branch: a patch that has
  // not said where it is going stays where it is, however hard it is driven.
  // Both ways of saying nothing are covered, and the first is the one every
  // organ patch in the bank takes — it sets its own registration and leaves the
  // second at the default, which is all stops in.
  NativeSynthPatch undeclared = additive_morph_patch();
  undeclared.additive.drawbars_b = {};
  NativeSynthPatch spelled_out = additive_morph_patch();
  spelled_out.additive.drawbars_b = spelled_out.additive.drawbars;

  for (const NativeSynthPatch& one_table : {undeclared, spelled_out}) {
    NativeSynthPatch driven = one_table;
    driven.mod_matrix.routes[0] = {ModSource::kVelocity, ModDestination::kSpectrumMorph, 2.0f};
    REQUIRE(render_note(driven, 16000) == render_note(one_table, 16000));
  }
  // The two spellings are also the same render as each other, so the sentinel
  // reads as "the first registration" rather than as silence.
  REQUIRE(render_note(undeclared, 16000) == render_note(spelled_out, 16000));

  // Positive control: the identical route on a patch that does name a second
  // registration moves the render, so the equalities above are a result.
  NativeSynthPatch two_tables = additive_morph_patch();
  two_tables.mod_matrix.routes[0] = {ModSource::kVelocity, ModDestination::kSpectrumMorph, 2.0f};
  REQUIRE(render_note(two_tables, 16000) != render_note(additive_morph_patch(), 16000));
}

TEST_CASE("a morph route whose source stays at zero changes nothing", "[midi][synth]") {
  // Same regression as the excitation axes: the plumbing runs every sample on a
  // voice that has the route. If composing an offset of zero were not exactly
  // the identity, every organ patch that gained a route would shift under it
  // before the source ever moved.
  NativeSynthPatch routed = additive_morph_patch();
  routed.mod_matrix.routes[0] = {ModSource::kModWheel, ModDestination::kSpectrumMorph, 1.0f};

  // CC1 is never sent, so the mod wheel reads 0 for the whole note.
  const std::vector<float> without = render_note(additive_morph_patch(), 16000);
  REQUIRE(render_note(routed, 16000) == without);

  NativeSynthPatch driven = routed;
  driven.mod_matrix.routes[0].source = ModSource::kVelocity;
  REQUIRE(render_note(driven, 16000) != without);
}

TEST_CASE("the live controller sources reach their destinations", "[midi][synth]") {
  // One route per new source, each at unit depth, so a source wired to the
  // wrong field shows up as a zero rather than as a shifted value.
  ModSourceValues values;
  values.breath = 0.25f;
  values.aftertouch = 0.5f;
  values.expression_cc = 0.75f;
  values.pitch_bend = -1.0f;

  const std::pair<ModSource, float> cases[] = {
      {ModSource::kBreath, 0.25f},
      {ModSource::kAftertouch, 0.5f},
      {ModSource::kExpressionCc, 0.75f},
      {ModSource::kPitchBend, -1.0f},
  };
  for (const auto& [source, expected] : cases) {
    CAPTURE(static_cast<int>(source));
    ModMatrix matrix;
    matrix.routes[0] = {source, ModDestination::kCutoffCents, 1200.0f};
    REQUIRE(evaluate_mod_matrix(matrix, values).cutoff_cents == 1200.0f * expected);
  }
}

TEST_CASE("aftertouch is the saturating sum of channel and poly", "[midi][synth]") {
  REQUIRE(sonare::midi::synth::combined_aftertouch(0.0f, 0.0f) == 0.0f);
  REQUIRE(sonare::midi::synth::combined_aftertouch(0.25f, 0.5f) == 0.75f);
  // Both at full is still full, not double: the sum saturates.
  REQUIRE(sonare::midi::synth::combined_aftertouch(1.0f, 1.0f) == 1.0f);
}
