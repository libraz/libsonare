/// @file sf2_fidelity_test.cpp
/// @brief SF2 full-fidelity layer (build-plan P3): resonant lowpass from
///        initialFilterFc/Q with mod-env / mod-LFO sweeps, vibrato LFO,
///        default modulators (CC1 vibrato, CC7/CC11 gain, CC10 pan,
///        velocity -> brightness, pitch bend with RPN0 range) and zipper-free
///        filter sweeps. Uses the synthetic in-memory SF2 fixtures.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "midi/midi_event.h"
#include "midi/synth/sf2_file.h"
#include "midi/synth/sf2_player.h"
#include "midi/ump.h"
#include "support/sf2_builder.h"

namespace {

using Catch::Approx;
using sonare::midi::MidiEvent;
using sonare::midi::synth::Sf2File;
using sonare::midi::synth::Sf2Player;
using sonare::midi::synth::Sf2PlayerConfig;
using sonare::test::Sf2Builder;

constexpr double kOutRate = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;

MidiEvent event(const sonare::midi::Ump& ump) {
  MidiEvent e;
  e.ump = ump;
  return e;
}

/// Builds a harmonically rich (square-ish) looped sample so filtering is
/// audible, plus presets exercising specific generators:
///   program 0: plain bright loop (no filter gens)
///   program 1: filtered (initialFilterFc 1200 Hz-ish, Q boost)
///   program 2: vibrato (vibLfoToPitch 100 cents, 6 Hz)
///   program 3: filter swept by the mod envelope
std::shared_ptr<Sf2File> make_fidelity_fixture() {
  Sf2Builder b;

  // Band-limited square at 500 Hz recorded at 32 kHz: period 64, harmonics
  // 1,3,5,7,9 -> energy well above and below typical filter cutoffs.
  std::vector<float> square(128);
  for (size_t i = 0; i < square.size(); ++i) {
    double v = 0.0;
    for (int h = 1; h <= 9; h += 2) {
      v += std::sin(kTwoPi * h * static_cast<double>(i) / 64.0) / h;
    }
    square[i] = 0.6f * static_cast<float>(v);
  }
  const int sq_id = b.add_sample("square500", square, 32000, 60, 0, 128);

  Sf2Builder::ZoneSpec plain;
  plain.gens.push_back({54 /*sampleModes*/, 1});
  plain.target = sq_id;
  const int plain_inst = b.add_instrument("plain", {plain});

  Sf2Builder::ZoneSpec filtered = plain;
  // initialFilterFc: 1200 Hz ~ 8.176 * 2^(c/1200) -> c ~ 8637; Q +6 dB.
  filtered.gens.push_back({8 /*initialFilterFc*/, 8637});
  filtered.gens.push_back({9 /*initialFilterQ*/, 60});
  const int filtered_inst = b.add_instrument("filtered", {filtered});

  Sf2Builder::ZoneSpec vibrato = plain;
  vibrato.gens.push_back({6 /*vibLfoToPitch*/, 100});
  // freqVibLfo: 6 Hz ~ 8.176*2^(c/1200) -> c ~ -535.
  vibrato.gens.push_back({24 /*freqVibLfo*/, -535});
  const int vibrato_inst = b.add_instrument("vibrato", {vibrato});

  Sf2Builder::ZoneSpec swept = plain;
  swept.gens.push_back({8, 7000});                        // start dark (~470 Hz)
  swept.gens.push_back({11 /*modEnvToFilterFc*/, 4800});  // +4 octaves at peak
  swept.gens.push_back({26 /*attackModEnv*/, -2786});     // ~200 ms attack
  swept.gens.push_back({29 /*sustainModEnv*/, 0});        // hold at full
  const int swept_inst = b.add_instrument("swept", {swept});

  auto preset = [&](const char* name, uint16_t prog, int inst) {
    Sf2Builder::ZoneSpec pz;
    pz.target = inst;
    b.add_preset(name, 0, prog, {pz});
  };
  preset("Plain", 0, plain_inst);
  preset("Filtered", 1, filtered_inst);
  preset("Vibrato", 2, vibrato_inst);
  preset("Swept", 3, swept_inst);

  const auto bytes = b.build();
  auto sf2 = std::make_shared<Sf2File>();
  std::string error;
  REQUIRE(sf2->parse(bytes.data(), bytes.size(), &error));
  return sf2;
}

Sf2Player make_player(std::shared_ptr<Sf2File> sf2) {
  Sf2PlayerConfig cfg;
  cfg.gain = 1.0f;
  Sf2Player player(cfg);
  player.set_soundfont(std::move(sf2));
  player.prepare(kOutRate, 256);
  return player;
}

std::vector<float> render_mono(Sf2Player& player, int num_samples) {
  std::vector<float> left(static_cast<size_t>(num_samples), 0.0f);
  std::vector<float> right(static_cast<size_t>(num_samples), 0.0f);
  float* chans[2] = {left.data(), right.data()};
  player.process(chans, 2, num_samples);
  return left;
}

/// Goertzel single-bin energy at @p freq over buf[from..).
double band_energy(const std::vector<float>& buf, size_t from, double freq) {
  const double w = kTwoPi * freq / kOutRate;
  const double coeff = 2.0 * std::cos(w);
  double s0 = 0.0, s1 = 0.0, s2 = 0.0;
  size_t n = 0;
  for (size_t i = from; i < buf.size(); ++i) {
    s0 = static_cast<double>(buf[i]) + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
    ++n;
  }
  if (n == 0) return 0.0;
  const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
  return power / static_cast<double>(n * n);
}

float rms(const std::vector<float>& buf, size_t from) {
  double acc = 0.0;
  size_t n = 0;
  for (size_t i = from; i < buf.size(); ++i) {
    acc += static_cast<double>(buf[i]) * buf[i];
    ++n;
  }
  return n > 0 ? static_cast<float>(std::sqrt(acc / static_cast<double>(n))) : 0.0f;
}

/// Instantaneous frequency spread from rising zero-crossing periods.
void crossing_period_range(const std::vector<float>& buf, size_t from, double* min_hz,
                           double* max_hz) {
  *min_hz = 1.0e9;
  *max_hz = 0.0;
  double prev = -1.0;
  for (size_t i = from + 1; i < buf.size(); ++i) {
    if (buf[i - 1] < 0.0f && buf[i] >= 0.0f) {
      const double frac =
          static_cast<double>(buf[i - 1]) / (static_cast<double>(buf[i - 1]) - buf[i]);
      const double t = static_cast<double>(i - 1) + frac;
      if (prev >= 0.0 && t > prev) {
        const double hz = kOutRate / (t - prev);
        *min_hz = std::min(*min_hz, hz);
        *max_hz = std::max(*max_hz, hz);
      }
      prev = t;
    }
  }
}

}  // namespace

TEST_CASE("Sf2 filter generator darkens upper harmonics", "[midi][sf2][fidelity]") {
  // Plain: strong 3rd/5th harmonics. Filtered at ~1.2 kHz: 500 Hz passes,
  // 2.5 kHz (5th) strongly attenuated.
  Sf2Player plain = make_player(make_fidelity_fixture());
  plain.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const auto plain_out = render_mono(plain, 24000);

  Sf2Player filtered = make_player(make_fidelity_fixture());
  filtered.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
  filtered.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const auto filt_out = render_mono(filtered, 24000);

  const double plain_fund = band_energy(plain_out, 4800, 500.0);
  const double plain_h5 = band_energy(plain_out, 4800, 2500.0);
  const double filt_fund = band_energy(filt_out, 4800, 500.0);
  const double filt_h5 = band_energy(filt_out, 4800, 2500.0);

  REQUIRE(plain_fund > 0.0);
  REQUIRE(plain_h5 > 0.0);
  // Fundamental survives the 1.2 kHz lowpass.
  REQUIRE(filt_fund > plain_fund * 0.25);
  // The 5th harmonic is cut by an order of magnitude relative to plain.
  REQUIRE(filt_h5 / filt_fund < 0.1 * (plain_h5 / plain_fund));
}

TEST_CASE("Sf2 velocity darkens timbre via the default modulator", "[midi][sf2][fidelity]") {
  // Use the filtered preset (Fc ~1.2 kHz) so the velocity offset (-2400 cents
  // at vel 0) moves the cutoff through the note's harmonics.
  Sf2Player loud = make_player(make_fidelity_fixture());
  loud.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
  loud.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const auto loud_out = render_mono(loud, 24000);

  Sf2Player soft = make_player(make_fidelity_fixture());
  soft.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 1)));
  soft.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 20)));
  const auto soft_out = render_mono(soft, 24000);

  // Compare harmonic balance (5th harmonic relative to fundamental) so the
  // overall velocity gain cancels out.
  const double loud_ratio =
      band_energy(loud_out, 4800, 2500.0) / band_energy(loud_out, 4800, 500.0);
  const double soft_ratio =
      band_energy(soft_out, 4800, 2500.0) / band_energy(soft_out, 4800, 500.0);
  REQUIRE(soft_ratio < loud_ratio * 0.5);
}

TEST_CASE("Sf2 vibrato LFO modulates pitch", "[midi][sf2][fidelity]") {
  Sf2Player player = make_player(make_fidelity_fixture());
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 2)));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const auto out = render_mono(player, 48000);

  double min_hz = 0.0, max_hz = 0.0;
  crossing_period_range(out, 9600, &min_hz, &max_hz);
  // 100 cents peak vibrato around 500 Hz: ~471..530 Hz. Require a clearly
  // audible spread and containment within the depth bounds.
  REQUIRE(max_hz - min_hz > 20.0);
  REQUIRE(min_hz > 440.0);
  REQUIRE(max_hz < 565.0);

  // The plain preset has no measurable spread.
  Sf2Player plain = make_player(make_fidelity_fixture());
  plain.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const auto plain_out = render_mono(plain, 48000);
  crossing_period_range(plain_out, 9600, &min_hz, &max_hz);
  REQUIRE(max_hz - min_hz < 5.0);
}

TEST_CASE("Sf2 CC1 mod wheel adds vibrato (default modulator)", "[midi][sf2][fidelity]") {
  Sf2Player player = make_player(make_fidelity_fixture());
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 1, 127)));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const auto out = render_mono(player, 48000);
  double min_hz = 0.0, max_hz = 0.0;
  crossing_period_range(out, 9600, &min_hz, &max_hz);
  // 50 cents of wheel vibrato at 500 Hz: ~486..515 Hz spread.
  REQUIRE(max_hz - min_hz > 10.0);
}

TEST_CASE("Sf2 pitch bend follows the RPN0 bend range", "[midi][sf2][fidelity]") {
  // Default range (2 semitones): full bend up -> +200 cents (500 -> ~561 Hz).
  Sf2Player player = make_player(make_fidelity_fixture());
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  player.on_event(0, event(sonare::midi::make_midi1_pitch_bend(0, 0, 16383)));
  const auto out = render_mono(player, 24000);
  double min_hz = 0.0, max_hz = 0.0;
  crossing_period_range(out, 4800, &min_hz, &max_hz);
  REQUIRE(min_hz == Approx(561.0).margin(6.0));

  // RPN 0 set to 12 semitones: half bend up -> +600 cents (~707 Hz).
  Sf2Player wide = make_player(make_fidelity_fixture());
  wide.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 101, 0)));
  wide.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 100, 0)));
  wide.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 6, 12)));
  wide.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  wide.on_event(0, event(sonare::midi::make_midi1_pitch_bend(0, 0, 12288)));
  const auto wide_out = render_mono(wide, 24000);
  crossing_period_range(wide_out, 4800, &min_hz, &max_hz);
  REQUIRE(min_hz == Approx(707.0).margin(8.0));
}

TEST_CASE("Sf2 CC7/CC11 scale loudness with the square-law curve", "[midi][sf2][fidelity]") {
  Sf2Player full = make_player(make_fidelity_fixture());
  full.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 7, 127)));
  full.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const float full_rms = rms(render_mono(full, 24000), 4800);

  Sf2Player half = make_player(make_fidelity_fixture());
  half.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 7, 64)));
  half.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const float half_rms = rms(render_mono(half, 24000), 4800);

  // (64/127)^2 ~ 0.254.
  REQUIRE(half_rms / full_rms == Approx(0.254).margin(0.05));

  Sf2Player expr = make_player(make_fidelity_fixture());
  expr.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 7, 127)));
  expr.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 11, 64)));
  expr.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const float expr_rms = rms(render_mono(expr, 24000), 4800);
  REQUIRE(expr_rms / full_rms == Approx(0.254).margin(0.05));
}

TEST_CASE("Sf2 CC10 pans playing voices", "[midi][sf2][fidelity]") {
  Sf2Player player = make_player(make_fidelity_fixture());
  player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 10, 127)));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  std::vector<float> left(24000, 0.0f), right(24000, 0.0f);
  float* chans[2] = {left.data(), right.data()};
  player.process(chans, 2, 24000);
  REQUIRE(rms(right, 4800) > 10.0f * rms(left, 4800));
}

TEST_CASE("Sf2 mod envelope sweeps the filter without zipper noise", "[midi][sf2][fidelity]") {
  Sf2Player player = make_player(make_fidelity_fixture());
  player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 3)));
  player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 127)));
  const auto out = render_mono(player, 48000);

  // Brightness rises as the mod envelope opens the filter: the 5th-harmonic
  // share grows from the first ~60 ms (envelope still low) to steady state.
  const auto head = std::vector<float>(out.begin() + 480, out.begin() + 3360);
  const auto tail = std::vector<float>(out.begin() + 28800, out.end());
  const double head_ratio = band_energy(head, 0, 2500.0) / band_energy(head, 0, 500.0);
  const double tail_ratio = band_energy(tail, 0, 2500.0) / band_energy(tail, 0, 500.0);
  REQUIRE(tail_ratio > 2.5 * head_ratio);

  // Zipper check: no sample-to-sample discontinuity beyond what the waveform
  // itself produces (the square's own slew bounds the legitimate jumps).
  float max_step = 0.0f;
  for (size_t i = 1; i < out.size(); ++i) {
    max_step = std::max(max_step, std::fabs(out[i] - out[i - 1]));
  }
  REQUIRE(std::isfinite(max_step));
  REQUIRE(max_step < 0.6f);
}

TEST_CASE("Sf2 fidelity path renders bit-identically", "[midi][sf2][fidelity]") {
  auto run = [] {
    Sf2Player player = make_player(make_fidelity_fixture());
    player.on_event(0, event(sonare::midi::make_midi1_program_change(0, 0, 3)));
    player.on_event(0, event(sonare::midi::make_midi1_control_change(0, 0, 1, 96)));
    player.on_event(0, event(sonare::midi::make_midi1_note_on(0, 0, 60, 100)));
    player.on_event(0, event(sonare::midi::make_midi1_pitch_bend(0, 0, 10000)));
    return render_mono(player, 24000);
  };
  REQUIRE(run() == run());
}
