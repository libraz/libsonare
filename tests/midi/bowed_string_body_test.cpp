/// @file bowed_string_body_test.cpp
/// @brief The bowed family's corpus stage: the tilt of the body resonator's dry
///        floor and the scaling of its mode bank. Cases carry [bowed][body] and
///        are invoked as an AND, because [body] alone reaches other tests.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

#include "midi/bowed_string_probe.h"
#include "midi/midi_event.h"
#include "midi/synth/body_resonator.h"
#include "midi/synth/gm_fallback_map.h"
#include "midi/synth/native_synth.h"
#include "midi/ump.h"
#include "support/audio_fixtures.h"
#include "support/golden_hash.h"
#include "support/midi_render.h"
#include "util/constants.h"

using sonare::midi::synth::BodyResonator;
using sonare::midi::synth::BodyType;
using sonare::test::fnv1a_quantized;

namespace {

constexpr double kSr = 48000.0;

/// Left channel of @p patch holding @p note at @p velocity for @p num_samples.
std::vector<float> render_patch_left(const sonare::midi::synth::NativeSynthPatch& patch,
                                     uint8_t note, uint8_t velocity, int num_samples) {
  sonare::midi::synth::NativeSynthConfig cfg;
  cfg.patch = patch;
  sonare::midi::synth::NativeSynth synth(cfg);
  synth.prepare(sonare::test::kRate, 256);
  synth.on_event(0, sonare::test::event(sonare::midi::make_midi1_note_on(0, 0, note, velocity)));
  return sonare::test::render_left(synth, num_samples);
}

/// Steady-state RMS of @p body's response to a @p freq_hz sine, discarding the
/// leading half of @p periods as the one-pole's own settling time.
double steady_rms_at(BodyResonator& body, double freq_hz, double sr, int periods) {
  const auto total = static_cast<int>(static_cast<double>(periods) * sr / freq_hz);
  const int settle = total / 2;
  double acc = 0.0;
  int count = 0;
  for (int i = 0; i < total; ++i) {
    const auto x = static_cast<float>(
        std::sin(sonare::constants::kTwoPiD * freq_hz * static_cast<double>(i) / sr));
    const float y = body.process(x);
    if (i >= settle) {
      acc += static_cast<double>(y) * static_cast<double>(y);
      ++count;
    }
  }
  return count > 0 ? std::sqrt(acc / static_cast<double>(count)) : 0.0;
}

/// Frequency of the strongest bin in [@p lo_hz, @p hi_hz) of @p body's impulse
/// response — the bank's spectral peak, read off the whole linear system
/// rather than guessed from a single mode's table entry. A unit impulse (not
/// noise) is the excitation so the result is exactly the transfer function's
/// magnitude, with no excitation-spectrum artifact to confound it.
double peak_hz_in_band(BodyResonator& body, double lo_hz, double hi_hz) {
  constexpr int kLen = 32768;
  std::vector<float> out(kLen, 0.0f);
  for (int i = 0; i < kLen; ++i)
    out[static_cast<std::size_t>(i)] = body.process(i == 0 ? 1.0f : 0.0f);

  const int fft = sonare::test::bowed::probe_fft_size(out.size());
  const std::vector<double> power = sonare::test::bowed::probe_power_spectrum(
      out, sonare::test::bowed::probe_window_start(out.size(), fft), fft);
  const double bin_hz = kSr / static_cast<double>(fft);
  const int lo_bin = std::max(1, static_cast<int>(lo_hz / bin_hz));
  const int hi_bin = std::min(static_cast<int>(power.size()) - 1, static_cast<int>(hi_hz / bin_hz));
  int best = lo_bin;
  for (int b = lo_bin; b <= hi_bin; ++b) {
    if (power[static_cast<std::size_t>(b)] > power[static_cast<std::size_t>(best)]) best = b;
  }
  return static_cast<double>(best) * bin_hz;
}

}  // namespace

TEST_CASE("bowed string corpus tilt rolls off the dry floor above a few kHz",
          "[midi][synth][bowed][body]") {
  // BodyType::kNone with mix = 0 isolates the dry floor: no modes, so
  // process() returns exactly the (possibly tilted) floor.
  BodyResonator flat;
  flat.start(BodyType::kNone, kSr, 0.0f, 0.0f, 1.0f, 0.0f);
  const double flat_200 = steady_rms_at(flat, 200.0, kSr, 100);
  flat.reset();
  const double flat_8k = steady_rms_at(flat, 8000.0, kSr, 100);
  INFO("bypass (corpus_tilt_hz=0): 200 Hz rms " << flat_200 << ", 8 kHz rms " << flat_8k);
  CHECK(flat_200 == Catch::Approx(flat_8k).margin(1e-4));

  BodyResonator tilted;
  tilted.start(BodyType::kNone, kSr, 0.0f, 0.0f, 1.0f, 2000.0f);
  const double tilt_200 = steady_rms_at(tilted, 200.0, kSr, 100);
  tilted.reset();
  const double tilt_8k = steady_rms_at(tilted, 8000.0, kSr, 100);
  REQUIRE(tilt_8k > 0.0);
  const double db_down = 20.0 * std::log10(tilt_200 / tilt_8k);
  INFO("corpus_tilt_hz=2000: 200 Hz rms " << tilt_200 << ", 8 kHz rms " << tilt_8k << " ("
                                          << db_down << " dB down)");
  CHECK(db_down >= 9.0);
}

TEST_CASE("bowed string corpus scale halves the bank's peak position",
          "[midi][synth][bowed][body]") {
  BodyResonator unscaled;
  unscaled.start(BodyType::kViolin, kSr, 261.63f, 1.0f, 1.0f, 0.0f);
  const double peak1 = peak_hz_in_band(unscaled, 50.0, 1000.0);

  BodyResonator scaled;
  scaled.start(BodyType::kViolin, kSr, 261.63f, 1.0f, 0.5f, 0.0f);
  const double peak2 = peak_hz_in_band(scaled, 50.0, 1000.0);

  INFO("bank peak: scale=1.0 at " << peak1 << " Hz, scale=0.5 at " << peak2 << " Hz");
  CHECK(peak2 == Catch::Approx(0.5 * peak1).epsilon(0.1));
}

TEST_CASE("bowed string corpus fields at default reproduce the control hashes",
          "[.][midi][synth][bowed][body][golden]") {
  INFO(sonare::test::kGoldenDigestProvenance);
  // Same deterministic excitation as the probe control case, reproduced here so
  // this [body]-tagged test verifies the identity on its own rather than
  // depending on [probe] having run first.
  std::vector<float> excitation(12000, 0.0f);
  uint32_t state = 0x1234567u;
  for (float& sample : excitation) {
    state = state * 1664525u + 1013904223u;
    sample = static_cast<float>(static_cast<int32_t>(state >> 8) % 2001 - 1000) / 1000.0f;
  }

  BodyResonator violin_body;
  violin_body.start(BodyType::kViolin, kSr, 261.63f, 0.28f);
  REQUIRE(violin_body.active());
  std::vector<float> violin_out(excitation.size(), 0.0f);
  for (std::size_t i = 0; i < excitation.size(); ++i)
    violin_out[i] = violin_body.process(excitation[i]);
  CHECK(fnv1a_quantized(violin_out) == 0x301db9e2991bf7bfull);

  const BodyResonator::Spec shell[3] = {
      {220.0f, 0.080f, 1.00f}, {330.0f, 0.050f, 0.60f}, {480.0f, 0.035f, 0.35f}};
  BodyResonator shell_body;
  shell_body.start_specs(shell, 3, kSr, 0.5f);
  REQUIRE(shell_body.active());
  std::vector<float> shell_out(excitation.size(), 0.0f);
  for (std::size_t i = 0; i < excitation.size(); ++i)
    shell_out[i] = shell_body.process(excitation[i]);
  CHECK(fnv1a_quantized(shell_out) == 0x72eced39118ca79aull);
}

namespace {

using sonare::midi::synth::BowedCorpus;

/// RMS of a 1/k harmonic series at @p f0_hz through @p body, over the steady half of the run.
double series_rms_through(BodyResonator& body, double f0_hz, int harmonics, double sr,
                          bool through) {
  const int total = static_cast<int>(2.0 * sr);
  double acc = 0.0;
  int count = 0;
  for (int i = 0; i < total; ++i) {
    double x = 0.0;
    for (int k = 1; k <= harmonics; ++k)
      x += std::sin(sonare::constants::kTwoPiD * f0_hz * k * static_cast<double>(i) / sr) / k;
    const float y = through ? body.process(static_cast<float>(x)) : static_cast<float>(x);
    if (i >= total / 2) {
      acc += static_cast<double>(y) * static_cast<double>(y);
      ++count;
    }
  }
  return std::sqrt(acc / static_cast<double>(count));
}

}  // namespace

TEST_CASE("the series body rolls off fourth-order below the air mode and peaks in its hump",
          "[midi][synth][bowed][body]") {
  // Cello bank, A0 at 100 Hz: an octave below the corner costs at least a Butterworth
  // fourth-order's 24 dB less the skirt the modes add, and the hump sits over the corner.
  BodyResonator cello;
  cello.start_series(BowedCorpus::kCello, kSr, 65.4f, 1.0f, 1.0f, 0.0f);
  REQUIRE(cello.active());
  const double at_50 = steady_rms_at(cello, 50.0, kSr, 200);
  cello.reset();
  cello.start_series(BowedCorpus::kCello, kSr, 65.4f, 1.0f, 1.0f, 0.0f);
  const double at_25 = steady_rms_at(cello, 25.0, kSr, 200);
  REQUIRE(at_25 > 0.0);
  const double octave_db = 20.0 * std::log10(at_50 / at_25);
  INFO("50 Hz over 25 Hz: " << octave_db << " dB per octave under the air mode");
  CHECK(octave_db > 18.0);

  cello.reset();
  cello.start_series(BowedCorpus::kCello, kSr, 65.4f, 1.0f, 1.0f, 0.0f);
  const double peak = peak_hz_in_band(cello, 30.0, 2000.0);
  INFO("cello series peak at " << peak << " Hz");
  CHECK(peak >= 100.0);
  CHECK(peak <= 330.0);
}

TEST_CASE("each member's series corpus peaks at its own register", "[midi][synth][bowed][body]") {
  const BowedCorpus members[] = {BowedCorpus::kContrabass, BowedCorpus::kCello, BowedCorpus::kViola,
                                 BowedCorpus::kViolin};
  double last = 0.0;
  for (const BowedCorpus corpus : members) {
    BodyResonator body;
    body.start_series(corpus, kSr, 220.0f, 1.0f, 1.0f, 0.0f);
    REQUIRE(body.active());
    const double peak = peak_hz_in_band(body, 30.0, 2000.0);
    INFO("corpus " << static_cast<int>(corpus) << " peaks at " << peak << " Hz, previous " << last);
    CHECK(peak > last);
    last = peak;
  }
}

TEST_CASE("the series body holds a harmonic series at its level on every note",
          "[midi][synth][bowed][body]") {
  // The stage is levelled per note for a 1/k series, so a note whose fundamental sits under
  // the air mode and one on the hump come through within a couple of decibels of the dry RMS.
  for (const double f0 : {65.4, 130.8, 261.6, 523.3}) {
    BodyResonator body;
    body.start_series(BowedCorpus::kCello, kSr, static_cast<float>(f0), 1.0f, 1.0f, 3000.0f);
    const double dry = series_rms_through(body, f0, 16, kSr, false);
    body.reset();
    body.start_series(BowedCorpus::kCello, kSr, static_cast<float>(f0), 1.0f, 1.0f, 3000.0f);
    const double wet = series_rms_through(body, f0, 16, kSr, true);
    const double delta_db = 20.0 * std::log10(wet / dry);
    INFO("f0 " << f0 << " Hz: series body changes the RMS by " << delta_db << " dB");
    CHECK(std::fabs(delta_db) < 2.5);
  }
}

TEST_CASE("a bowed patch naming no series corpus renders the parallel bank bit for bit",
          "[midi][synth][bowed][body]") {
  // The field defaults to kNone, and the cello's shipped render is the control: the series
  // stage must not touch it, and naming the cello corpus must move it.
  using sonare::midi::synth::NativeSynthPatch;
  const NativeSynthPatch shipped = sonare::midi::synth::gm_fallback_patch(0, 42);
  REQUIRE(shipped.bowed_string.corpus_series == BowedCorpus::kNone);
  const std::vector<float> control = render_patch_left(shipped, 48, 100, 24000);
  NativeSynthPatch named = shipped;
  named.bowed_string.corpus_series = BowedCorpus::kNone;
  CHECK(render_patch_left(named, 48, 100, 24000) == control);
  named.bowed_string.corpus_series = BowedCorpus::kCello;
  named.body_mix = 1.0f;
  const std::vector<float> series = render_patch_left(named, 48, 100, 24000);
  CHECK(series != control);
  CHECK(std::all_of(series.begin(), series.end(), [](float s) { return std::isfinite(s); }));
}
