/// @file reverb_tail_test.cpp
/// @brief Reverb/delay decay-tail reporting so offline bounces are not
///        truncated, plus the convolution empty-IR latency contract.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <complex>
#include <vector>

#include "core/fft.h"
#include "effects/common/dc_blocker.h"
#include "effects/delay/stereo_delay.h"
#include "effects/filter/vowel_filter.h"
#include "effects/modulation/auto_wah.h"
#include "effects/modulation/chorus.h"
#include "effects/modulation/ensemble.h"
#include "effects/modulation/flanger.h"
#include "effects/modulation/phaser.h"
#include "effects/modulation/pitch_shifter.h"
#include "effects/modulation/rotary.h"
#include "effects/modulation/wah.h"
#include "effects/reverb/convolution_reverb.h"
#include "effects/reverb/dattorro_reverb.h"
#include "effects/reverb/fdn_reverb.h"
#include "effects/reverb/velvet_reverb.h"
#include "mastering/dynamics/deesser.h"
#include "mastering/eq/cut_filter.h"
#include "mastering/eq/dynamic_eq.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/graphic_eq.h"
#include "mastering/eq/parametric.h"
#include "mastering/eq/pultec.h"
#include "mastering/multiband/multiband_compressor.h"
#include "mastering/repair/decrackle_streaming.h"
#include "mastering/repair/dehum_streaming.h"
#include "mastering/repair/denoise_streaming.h"
#include "mastering/repair/dereverb_streaming.h"
#include "mastering/saturation/amp_sim.h"
#include "mastering/saturation/exciter.h"
#include "mastering/saturation/hard_clipper.h"
#include "mastering/saturation/pedal.h"
#include "mastering/saturation/soft_clipper.h"
#include "mastering/saturation/tape.h"
#include "mastering/saturation/tube.h"
#include "mastering/saturation/waveshaper.h"
#include "mastering/spectral/air_band.h"
#include "mastering/spectral/low_end_focus.h"
#include "mastering/spectral/presence_enhancer.h"
#include "mastering/spectral/spectral_shaper.h"
#include "mastering/stereo/binaural_panner.h"
#include "mastering/stereo/imager.h"
#include "mastering/stereo/mono_maker.h"
#include "rt/processor_base.h"
#include "rt/tail_budget.h"
#include "util/constants.h"

using sonare::effects::delay::StereoDelay;
using sonare::effects::delay::StereoDelayConfig;
using sonare::effects::reverb::ConvolutionReverb;
using sonare::effects::reverb::ConvolutionReverbConfig;
using sonare::effects::reverb::DattorroReverb;
using sonare::effects::reverb::DattorroReverbConfig;
using sonare::effects::reverb::FdnReverb;
using sonare::effects::reverb::FdnReverbConfig;
using sonare::effects::reverb::VelvetReverb;
using sonare::effects::reverb::VelvetReverbConfig;
using sonare::rt::ProcessorBase;

namespace {

/// @brief Renders an impulse through a fully wet VelvetReverb, block by block,
///        for exactly the window it declares as its tail.
/// @return The RMS of each of @p num_segments equal slices, in dB relative to
///         the first slice. A slice with no energy at all reports -600 dB
///         rather than -inf so a failure prints a readable number.
std::vector<double> velvet_tail_segments_db(VelvetReverbConfig config, double sample_rate,
                                            int num_segments) {
  constexpr int kBlockSize = 512;
  config.dry_wet = 1.0f;
  VelvetReverb reverb(config);
  reverb.prepare(sample_rate, kBlockSize);

  const int total = reverb.tail_samples();
  std::vector<float> out(static_cast<size_t>(total), 0.0f);
  out[0] = 1.0f;
  for (int offset = 0; offset < total;) {
    const int count = std::min(kBlockSize, total - offset);
    float* channels[] = {out.data() + offset};
    reverb.process(channels, 1, count);
    offset += count;
  }

  const int segment = total / num_segments;
  std::vector<double> segments_db;
  segments_db.reserve(static_cast<size_t>(num_segments));
  double reference = 0.0;
  for (int s = 0; s < num_segments; ++s) {
    double sum = 0.0;
    for (int i = s * segment; i < (s + 1) * segment; ++i) {
      const double sample = out[static_cast<size_t>(i)];
      sum += sample * sample;
    }
    const double rms = std::sqrt(sum / static_cast<double>(segment));
    if (s == 0) reference = rms > 0.0 ? rms : 1.0;
    segments_db.push_back(rms > 0.0 ? 20.0 * std::log10(rms / reference) : -600.0);
  }
  return segments_db;
}

/// @brief Renders a stereo impulse of @p amplitude followed by silence, in blocks, and
///        returns the last sample index on either channel whose magnitude exceeds @p floor.
int last_audible_after_impulse(ProcessorBase& processor, int length, float amplitude, float floor) {
  constexpr int kBlockSize = 256;
  std::vector<float> left(static_cast<size_t>(length), 0.0f);
  std::vector<float> right(static_cast<size_t>(length), 0.0f);
  left[0] = amplitude;
  right[0] = amplitude;
  for (int offset = 0; offset < length; offset += kBlockSize) {
    const int count = std::min(kBlockSize, length - offset);
    float* channels[] = {left.data() + offset, right.data() + offset};
    processor.process(channels, 2, count);
  }
  int last = -1;
  for (int i = 0; i < length; ++i) {
    if (std::fabs(left[static_cast<size_t>(i)]) > floor ||
        std::fabs(right[static_cast<size_t>(i)]) > floor) {
      last = i;
    }
  }
  return last;
}

/// @brief Requires every output past latency + tail to stay at or under @p floor, rendering
///        well past the reported span so an under-report has room to show.
void require_tail_contains_output(ProcessorBase& processor, float amplitude = 1.0f,
                                  float floor = 1.0e-5f) {
  const int span = processor.latency_samples() + processor.tail_samples();
  const int length = 2 * span + 4096;
  const int last = last_audible_after_impulse(processor, length, amplitude, floor);
  INFO("latency " << processor.latency_samples() << ", tail " << processor.tail_samples()
                  << ", last audible " << last);
  REQUIRE(last >= 0);
  REQUIRE(last <= span);
}

}  // namespace

TEST_CASE("Modulated delays report the tail their lines emit", "[effects][tail]") {
  const double rate = GENERATE(44100.0, 48000.0);
  SECTION("chorus") {
    sonare::effects::modulation::ChorusConfig config;
    config.dry_wet = 1.0f;
    config.feedback = GENERATE(0.0f, 0.6f);
    sonare::effects::modulation::Chorus chorus(config);
    chorus.prepare(rate, 256);
    require_tail_contains_output(chorus);
  }
  SECTION("flanger") {
    sonare::effects::modulation::FlangerConfig config;
    config.dry_wet = 1.0f;
    config.feedback = GENERATE(0.0f, -0.8f);
    sonare::effects::modulation::Flanger flanger(config);
    flanger.prepare(rate, 256);
    require_tail_contains_output(flanger);
  }
  SECTION("ensemble") {
    sonare::effects::modulation::EnsembleConfig config;
    config.dry_wet = 1.0f;
    config.pre_delay_dev_ms = 4.0f;
    sonare::effects::modulation::Ensemble ensemble(config);
    ensemble.prepare(rate, 256);
    require_tail_contains_output(ensemble);
  }
  SECTION("pitch shifter") {
    sonare::effects::modulation::PitchShifterConfig config;
    config.semitones = 7.0f;
    config.feedback = 0.5f;
    config.pre_delay_ms = 10.0f;
    config.anti_alias = true;
    sonare::effects::modulation::PitchShifter shifter(config);
    shifter.prepare(rate, 256);
    require_tail_contains_output(shifter);
  }
}

TEST_CASE("Delay and reverb tails cover degenerate settings", "[effects][tail]") {
  SECTION("stereo delay recirculating through a zero-length line") {
    StereoDelayConfig config;
    config.delay_time_l_ms = 0.0f;
    config.delay_time_r_ms = 0.0f;
    config.feedback = 0.95f;
    config.dry_wet = 1.0f;
    StereoDelay delay(config);
    delay.prepare(48000.0, 256);
    // 0.95^n reaches -60 dB after 135 passes of one sample each.
    REQUIRE(delay.tail_samples() >= 135);
    require_tail_contains_output(delay);
  }
  SECTION("dattorro with the tank loop closed") {
    DattorroReverbConfig config;
    config.decay = 0.0f;
    config.dry_wet = 1.0f;
    DattorroReverb reverb(config);
    reverb.prepare(48000.0, 256);
    REQUIRE(reverb.tail_samples() > 0);
    require_tail_contains_output(reverb);
  }
  SECTION("dattorro with a recirculating tank") {
    DattorroReverbConfig config;
    config.decay = 0.5f;
    config.dry_wet = 1.0f;
    DattorroReverb reverb(config);
    reverb.prepare(48000.0, 256);
    require_tail_contains_output(reverb);
  }
  SECTION("fdn with the shortest decay") {
    FdnReverbConfig config;
    config.decay = 0.0f;
    config.dry_wet = 1.0f;
    FdnReverb reverb(config);
    reverb.prepare(48000.0, 256);
    require_tail_contains_output(reverb);
  }
}

TEST_CASE("Binaural speaker output reports its crossover ring", "[effects][tail]") {
  sonare::mastering::stereo::BinauralPannerConfig config;
  config.output = sonare::mastering::stereo::BinauralOutput::kSpeakers;
  sonare::mastering::stereo::BinauralPanner panner(config);
  panner.prepare(48000.0, 256);
  require_tail_contains_output(panner);
}

TEST_CASE("Oversampled clippers report the FIR ring past their latency", "[effects][tail]") {
  constexpr float kRingFloor = 1.0e-7f;
  const auto aliasing = sonare::rt::AliasingControl::Oversample4x;
  SECTION("hard clipper") {
    sonare::mastering::saturation::HardClipperConfig config;
    config.aliasing = aliasing;
    sonare::mastering::saturation::HardClipper clipper(config);
    clipper.prepare(48000.0, 256);
    require_tail_contains_output(clipper, 0.5f, kRingFloor);
  }
  SECTION("soft clipper") {
    sonare::mastering::saturation::SoftClipperConfig config;
    config.aliasing = aliasing;
    sonare::mastering::saturation::SoftClipper clipper(config);
    clipper.prepare(48000.0, 256);
    require_tail_contains_output(clipper, 0.5f, kRingFloor);
  }
  SECTION("waveshaper") {
    sonare::mastering::saturation::WaveshaperConfig config;
    config.aliasing = aliasing;
    sonare::mastering::saturation::Waveshaper shaper(config);
    shaper.prepare(48000.0, 256);
    require_tail_contains_output(shaper, 0.5f, kRingFloor);
  }
  SECTION("tube") {
    sonare::mastering::saturation::Tube tube;
    tube.prepare(48000.0, 256);
    require_tail_contains_output(tube, 0.5f, kRingFloor);
  }
  SECTION("air band") {
    sonare::mastering::spectral::AirBand air;
    air.prepare(48000.0, 256);
    require_tail_contains_output(air, 0.5f, kRingFloor);
  }
}

TEST_CASE("FdnReverb reports a non-zero decay tail", "[effects][reverb][fdn]") {
  FdnReverbConfig config;
  config.decay = 0.6f;
  FdnReverb reverb(config);
  reverb.prepare(48000.0, 512);
  // decay 0.6 -> T60_lf = 6 s -> ~288000 samples at 48 kHz.
  REQUIRE(reverb.tail_samples() > 48000);
}

TEST_CASE("FdnReverb tail bounds the measured unit-impulse decay without overshooting",
          "[effects][reverb][fdn]") {
  // Pairwise cover of decay x damping x rate x wet (see rt/tail_budget.h: a tail ends where a
  // unit impulse's residual falls below kTailFloor).
  struct Row {
    float decay;
    float damping;
    double rate;
    float wet;
  };
  const std::vector<Row> rows = {
      {1.5f, 0.5f, 48000, 0.35f},   {0.55f, 0.5f, 44100, 1.0f},  {1.5f, 1.0f, 96000, 1.0f},
      {0.02f, 0.25f, 44100, 0.35f}, {0.1f, 0.0f, 96000, 0.35f},  {1.5f, 1.0f, 44100, 0.35f},
      {0.02f, 0.0f, 48000, 1.0f},   {0.02f, 0.5f, 96000, 0.35f}, {0.55f, 0.0f, 48000, 0.35f},
      {1.0f, 0.25f, 96000, 1.0f},   {0.1f, 0.5f, 48000, 1.0f},   {0.55f, 1.0f, 48000, 1.0f},
      {1.0f, 0.0f, 48000, 1.0f},    {0.1f, 0.0f, 44100, 1.0f},   {1.0f, 0.5f, 44100, 0.35f},
      {1.0f, 1.0f, 96000, 1.0f},    {0.02f, 1.0f, 44100, 0.35f}, {1.5f, 0.0f, 48000, 1.0f},
      {0.55f, 0.25f, 96000, 1.0f},  {1.0f, 0.25f, 48000, 1.0f},  {0.1f, 1.0f, 96000, 1.0f},
      {0.1f, 0.25f, 96000, 1.0f},   {1.5f, 0.25f, 96000, 1.0f},
  };
  double max_ratio = 0.0;
  for (const Row& row : rows) {
    FdnReverbConfig config;
    config.decay = row.decay;
    config.hf_damping = row.damping;
    config.dry_wet = row.wet;
    FdnReverb reverb(config);
    reverb.prepare(row.rate, 512);
    const int tail = reverb.tail_samples();
    constexpr int kBlock = 512;
    const int total = tail + static_cast<int>(row.rate);
    std::vector<float> left(static_cast<size_t>(total), 0.0f);
    std::vector<float> right(static_cast<size_t>(total), 0.0f);
    left[0] = right[0] = 1.0f;
    for (int pos = 0; pos < total; pos += kBlock) {
      float* block[2] = {left.data() + pos, right.data() + pos};
      reverb.process(block, 2, std::min(kBlock, total - pos));
    }
    int measured = 0;
    for (int i = 1; i < total; ++i) {
      const size_t k = static_cast<size_t>(i);
      if (std::fabs(left[k]) > sonare::rt::kTailFloor ||
          std::fabs(right[k]) > sonare::rt::kTailFloor) {
        measured = i;
      }
    }
    INFO("decay=" << row.decay << " damping=" << row.damping << " rate=" << row.rate
                  << " wet=" << row.wet << " tail=" << tail << " measured=" << measured);
    REQUIRE(measured > 0);
    CHECK(tail >= measured);
    const double ratio = static_cast<double>(tail) / measured;
    max_ratio = std::max(max_ratio, ratio);
    // A decay of a fraction of a second is mostly the line delay and the blocker's ring, which
    // the bound adds rather than overlaps.
    CHECK(ratio <= (row.decay >= 0.1f ? 1.3 : 2.0));
  }
  INFO("max reported/measured ratio = " << max_ratio);
  CHECK(max_ratio > 1.0);
}

TEST_CASE("VelvetReverb reports a non-zero decay tail", "[effects][reverb][velvet]") {
  VelvetReverbConfig config;
  config.reverb_time_s = 1.5f;
  config.decay = 0.45f;
  VelvetReverb reverb(config);
  reverb.prepare(48000.0, 512);
  // rt60 = 1.5 * (0.5 + 0.45) ~= 1.425 s -> ~68400 samples.
  REQUIRE(reverb.tail_samples() > 48000);
}

TEST_CASE("VelvetReverb impulse response has no comb from a periodic tap table",
          "[effects][reverb][velvet]") {
  // Velvet noise has a flat expected spectrum; a periodic sign or position
  // sequence shows up as bins tens of dB above their neighbourhood.
  constexpr int kFftLength = 32768;
  for (const double rate : {44100.0, 48000.0}) {
    VelvetReverbConfig config;
    config.dry_wet = 1.0f;
    config.reverb_time_s = 0.4f;
    config.enable_shelf = false;
    VelvetReverb reverb(config);
    reverb.prepare(rate, kFftLength);
    std::vector<float> left(kFftLength, 0.0f);
    std::vector<float> right(kFftLength, 0.0f);
    left[0] = 1.0f;
    right[0] = 1.0f;
    float* channels[] = {left.data(), right.data()};
    reverb.process(channels, 2, kFftLength);

    sonare::FFT plan(kFftLength);
    std::vector<std::complex<float>> spectrum(static_cast<size_t>(plan.n_bins()));
    for (std::vector<float>* channel : {&left, &right}) {
      CAPTURE(rate, channel == &left);
      plan.forward(channel->data(), spectrum.data());
      std::vector<double> power(spectrum.size());
      for (size_t k = 0; k < power.size(); ++k) power[k] = std::norm(spectrum[k]);
      const double bin_hz = rate / kFftLength;
      double worst_db = -1000.0;
      for (size_t k = static_cast<size_t>(200.0 / bin_hz); k * bin_hz < 8000.0; ++k) {
        // Median power over a sixth of an octave either side.
        const double hz = static_cast<double>(k) * bin_hz;
        const auto lo = static_cast<size_t>(hz * std::pow(2.0, -1.0 / 6.0) / bin_hz);
        const auto hi = static_cast<size_t>(hz * std::pow(2.0, 1.0 / 6.0) / bin_hz);
        std::vector<double> window(power.begin() + static_cast<std::ptrdiff_t>(lo),
                                   power.begin() + static_cast<std::ptrdiff_t>(hi) + 1);
        std::nth_element(window.begin(),
                         window.begin() + static_cast<std::ptrdiff_t>(window.size() / 2),
                         window.end());
        const double median = window[window.size() / 2];
        worst_db = std::max(worst_db, 10.0 * std::log10(power[k] / median));
      }
      CAPTURE(worst_db);
      // An exponentially distributed bin exceeds 15 dB over its median with odds
      // near 3e-10; the periodic table put 1 kHz about 47 dB over.
      CHECK(worst_db < 15.0);
    }
  }
}

TEST_CASE("VelvetReverb bounds excessive reverb time and tap work", "[effects][reverb][velvet]") {
  VelvetReverbConfig config;
  config.reverb_time_s = 40.0f;
  config.decay = 1.0f;
  config.density_hz = 3000.0f;
  VelvetReverb reverb(config);
  reverb.prepare(48000.0, 512);

  // 12 s base time × the maximum 1.5 decay factor, rather than the requested 40 s,
  // followed by the 20 Hz DC blocker's ring (under 100 ms).
  REQUIRE(reverb.tail_samples() <= 18 * 48000 + 48000 / 10);

  std::vector<float> samples(512, 0.0f);
  samples[0] = 1.0f;
  float* channels[] = {samples.data()};
  reverb.process(channels, 1, static_cast<int>(samples.size()));
  for (const float sample : samples) REQUIRE(std::isfinite(sample));
}

TEST_CASE("VelvetReverb decays smoothly across the whole tail it declares",
          "[effects][reverb][velvet]") {
  constexpr int kSegments = 20;
  // The tap gains span 60 dB over the declared T60, so each 5 % slice sits
  // 3 dB below the one before it.
  constexpr double kStepDb = -60.0 / kSegments;

  // 16 kHz rather than 48 kHz: the pulse count follows density * T60 while the
  // grid step follows sample_rate / density, so the fraction of the tail a
  // saturated tap table covers is the same at every rate, and the FFT tail
  // render is quadratic in the T60 sample count.
  constexpr double kSampleRate = 16000.0;

  VelvetReverbConfig config;
  SECTION("decaySec = 8 s") {
    // insert_factory maps decaySec onto reverb_time_s * (0.5 + decay).
    config.decay = 0.45f;
    config.reverb_time_s = 8.0f / (0.5f + 0.45f);
    config.density_hz = 2000.0f;
  }
  SECTION("reverb time above the ceiling") {
    config.decay = 1.0f;
    config.reverb_time_s = 40.0f;  // clamped to the 12 s base, i.e. an 18 s T60
    config.density_hz = 3000.0f;
  }

  const std::vector<double> segments_db = velvet_tail_segments_db(config, kSampleRate, kSegments);

  for (int s = 0; s < kSegments; ++s) {
    INFO("segment " << s << " = " << segments_db[static_cast<size_t>(s)] << " dB");
    // Every slice tracks the exponential envelope. A tap table that stops short
    // of the declared tail leaves the trailing slices far below this floor.
    REQUIRE(segments_db[static_cast<size_t>(s)] > kStepDb * s - 8.0);
    REQUIRE(segments_db[static_cast<size_t>(s)] < kStepDb * s + 4.0);
    // ...and it gets there smoothly: no step big enough to be an edge.
    if (s > 0) {
      REQUIRE(segments_db[static_cast<size_t>(s)] - segments_db[static_cast<size_t>(s - 1)] >
              -15.0);
    }
  }
  // The end of the declared window has reached the -60 dB target without
  // collapsing into silence before it.
  INFO("last segment = " << segments_db.back() << " dB");
  REQUIRE(segments_db.back() < -45.0);
  REQUIRE(segments_db.back() > -70.0);
}

TEST_CASE("VelvetReverb hybrid tap reconstruction is independent of host block boundaries",
          "[effects][reverb][velvet]") {
  constexpr int kSamples = 4096;
  VelvetReverbConfig config;
  config.dry_wet = 1.0f;
  config.enable_shelf = false;
  std::vector<float> whole(kSamples, 0.0f);
  std::vector<float> split(kSamples, 0.0f);
  whole[0] = 1.0f;
  split[0] = 1.0f;

  VelvetReverb whole_fx(config);
  whole_fx.prepare(48000.0, kSamples);
  float* whole_channels[] = {whole.data()};
  whole_fx.process(whole_channels, 1, kSamples);

  VelvetReverb split_fx(config);
  split_fx.prepare(48000.0, 257);
  for (int offset = 0; offset < kSamples;) {
    const int count = std::min(37 + (offset % 211), kSamples - offset);
    float* split_channels[] = {split.data() + offset};
    split_fx.process(split_channels, 1, count);
    offset += count;
  }

  for (int i = 0; i < kSamples; ++i) {
    REQUIRE(whole[static_cast<size_t>(i)] ==
            Catch::Approx(split[static_cast<size_t>(i)]).margin(1e-5));
  }
}

TEST_CASE("ConvolutionReverb reports the IR length as its tail", "[effects][reverb][convolution]") {
  ConvolutionReverbConfig config;
  config.decay_sec = 1.0f;
  ConvolutionReverb reverb(config);
  reverb.prepare(48000.0, 512);
  // A synthesized decaying-noise IR spans roughly the requested decay window.
  REQUIRE(reverb.tail_samples() > 0);
  REQUIRE(reverb.tail_samples() == reverb.ir_size());
}

TEST_CASE("ConvolutionReverb with an empty IR reports zero latency and tail",
          "[effects][reverb][convolution]") {
  ConvolutionReverb reverb;  // default-constructed, no IR loaded
  reverb.load_ir(nullptr, 0);
  reverb.prepare(48000.0, 512);
  // process() is a true no-op with no IR, so it must not claim partition latency
  // nor a decay tail.
  REQUIRE(reverb.ir_size() == 0);
  REQUIRE(reverb.latency_samples() == 0);
  REQUIRE(reverb.tail_samples() == 0);
}

TEST_CASE("StereoDelay reports a feedback-extended tail", "[effects][delay][stereo]") {
  StereoDelayConfig config;
  config.delay_time_l_ms = 250.0f;
  config.delay_time_r_ms = 250.0f;
  config.feedback = 0.5f;
  StereoDelay delay(config);
  delay.prepare(48000.0, 512);
  const int one_delay = static_cast<int>(0.250 * 48000.0);
  // Feedback keeps echoes ringing past a single delay period until they hit
  // -60 dB, so the tail must exceed one delay length.
  REQUIRE(delay.tail_samples() > one_delay);
}

TEST_CASE("StereoDelay with no feedback reports one delay length", "[effects][delay][stereo]") {
  StereoDelayConfig config;
  config.delay_time_l_ms = 100.0f;
  config.delay_time_r_ms = 300.0f;
  config.feedback = 0.0f;
  StereoDelay delay(config);
  delay.prepare(48000.0, 512);
  const int longest = static_cast<int>(0.300 * 48000.0);
  // Without feedback the tail is a single pass through the longer delay line.
  REQUIRE(delay.tail_samples() >= longest);
  REQUIRE(delay.tail_samples() < 2 * longest);
}

TEST_CASE("Delay and reverb tails are zero for dry-only configurations",
          "[effects][reverb][delay][tail]") {
  SECTION("stereo delay") {
    StereoDelayConfig config;
    config.dry_wet = 0.0f;
    StereoDelay effect(config);
    effect.prepare(48000.0, 512);
    REQUIRE(effect.tail_samples() == 0);
  }
  SECTION("Dattorro") {
    DattorroReverbConfig config;
    config.dry_wet = 0.0f;
    DattorroReverb effect(config);
    effect.prepare(48000.0, 512);
    REQUIRE(effect.tail_samples() == 0);
  }
  SECTION("FDN") {
    FdnReverbConfig config;
    config.dry_wet = 0.0f;
    FdnReverb effect(config);
    effect.prepare(48000.0, 512);
    REQUIRE(effect.tail_samples() == 0);
  }
  SECTION("velvet") {
    VelvetReverbConfig config;
    config.dry_wet = 0.0f;
    VelvetReverb effect(config);
    effect.prepare(48000.0, 512);
    REQUIRE(effect.tail_samples() == 0);
  }
  SECTION("convolution") {
    ConvolutionReverbConfig config;
    config.dry_wet = 0.0f;
    ConvolutionReverb effect(config);
    effect.prepare(48000.0, 512);
    REQUIRE(effect.latency_samples() > 0);
    REQUIRE(effect.tail_samples() == 0);
  }
}

TEST_CASE("ConvolutionReverb reloaded after an empty IR starts from silence",
          "[effects][reverb][convolution]") {
  std::vector<float> ir(64, 0.0f);
  ir[0] = 1.0f;
  ir[40] = 0.5f;
  ConvolutionReverb reverb;
  reverb.load_ir(ir);
  reverb.prepare(48000.0, 512);

  std::vector<float> block(512, 0.7f);
  float* channels[] = {block.data()};
  reverb.process(channels, 1, 512);

  reverb.load_ir(std::vector<float>{});
  std::vector<float> silence(1024, 0.0f);
  channels[0] = silence.data();
  reverb.process(channels, 1, 1024);

  reverb.load_ir(ir);
  std::vector<float> after(256, 0.0f);
  channels[0] = after.data();
  reverb.process(channels, 1, 256);
  for (const float sample : after) REQUIRE(sample == 0.0f);
}

TEST_CASE("DattorroReverb reset returns the output gate to its prepared state",
          "[effects][reverb][dattorro]") {
  DattorroReverbConfig config;
  config.dry_wet = 1.0f;
  config.gate_threshold_db = -40.0f;
  config.gate_hold_ms = 50.0f;
  config.gate_type = sonare::effects::reverb::DattorroGateType::kSweep1;

  const auto impulse_response = [](DattorroReverb& reverb) {
    std::vector<float> left(4096, 0.0f);
    std::vector<float> right(4096, 0.0f);
    left[0] = 1.0f;
    right[0] = 1.0f;
    float* channels[] = {left.data(), right.data()};
    reverb.process(channels, 2, 4096);
    left.insert(left.end(), right.begin(), right.end());
    return left;
  };

  DattorroReverb fresh(config);
  fresh.prepare(48000.0, 4096);
  DattorroReverb used(config);
  used.prepare(48000.0, 4096);
  impulse_response(used);  // opens the gate and moves its sweep
  used.reset();

  const std::vector<float> expected = impulse_response(fresh);
  const std::vector<float> actual = impulse_response(used);
  REQUIRE(actual == expected);
}

TEST_CASE("IIR stages report the ring their sections leave", "[effects][tail]") {
  namespace eq = sonare::mastering::eq;
  constexpr double kRate = 48000.0;
  SECTION("parametric low resonant band") {
    eq::ParametricEq parametric;
    parametric.set_band(0, {eq::EqBandType::Peak, 40.0f, 12.0f, 8.0f, true});
    parametric.prepare(kRate, 256);
    require_tail_contains_output(parametric, 0.5f);
  }
  SECTION("graphic band") {
    eq::GraphicEq graphic;
    graphic.prepare(kRate, 256);
    graphic.set_gain_for_frequency(63.0f, 12.0f);
    require_tail_contains_output(graphic, 0.5f);
  }
  SECTION("steep high-pass") {
    eq::CutFilter cut;
    cut.prepare(kRate, 256);
    cut.set_high_pass(30.0f, sonare::constants::kButterworthQ, eq::CutFilterSlope::Db48PerOct);
    require_tail_contains_output(cut, 0.5f);
  }
  SECTION("equalizer processor") {
    eq::EqualizerProcessor equalizer;
    equalizer.prepare(kRate, 256);
    equalizer.set_band(0, {eq::EqBandType::Peak, 50.0f, 9.0f, 6.0f, true});
    require_tail_contains_output(equalizer, 0.5f);
  }
  SECTION("pultec") {
    eq::PultecEq pultec;
    pultec.prepare(kRate, 256);
    pultec.set_low_boost(8.0f);
    require_tail_contains_output(pultec, 0.5f);
  }
  SECTION("wah") {
    sonare::effects::modulation::Wah wah;
    wah.prepare(kRate, 256);
    require_tail_contains_output(wah, 0.5f);
  }
  SECTION("auto wah") {
    sonare::effects::modulation::AutoWah wah;
    wah.prepare(kRate, 256);
    require_tail_contains_output(wah, 0.5f);
  }
  SECTION("phaser") {
    sonare::effects::modulation::PhaserConfig config;
    config.feedback = 0.7f;
    sonare::effects::modulation::Phaser phaser(config);
    phaser.prepare(kRate, 256);
    require_tail_contains_output(phaser, 0.5f);
  }
  SECTION("rotary") {
    sonare::effects::modulation::Rotary rotary;
    rotary.prepare(kRate, 256);
    require_tail_contains_output(rotary, 0.5f);
  }
  SECTION("vowel filter") {
    sonare::effects::filter::VowelFilter vowel;
    vowel.prepare(kRate, 256);
    require_tail_contains_output(vowel, 0.5f);
  }
  SECTION("dc blocker") {
    sonare::effects::common::DcBlocker blocker;
    blocker.prepare(kRate, 256);
    require_tail_contains_output(blocker, 0.5f);
  }
  SECTION("de-esser") {
    sonare::mastering::dynamics::DeEsser deesser;
    deesser.prepare(kRate, 256);
    require_tail_contains_output(deesser, 0.5f);
  }
  SECTION("multiband compressor") {
    sonare::mastering::multiband::MultibandCompressor compressor;
    compressor.prepare(kRate, 256);
    require_tail_contains_output(compressor, 0.5f);
  }
  SECTION("mono maker") {
    sonare::mastering::stereo::MonoMaker mono;
    mono.prepare(kRate, 256);
    require_tail_contains_output(mono, 0.5f);
  }
  SECTION("low end focus") {
    sonare::mastering::spectral::LowEndFocus focus;
    focus.prepare(kRate, 256);
    require_tail_contains_output(focus, 0.5f);
  }
  SECTION("spectral shaper") {
    sonare::mastering::spectral::SpectralShaper shaper;
    shaper.prepare(kRate, 256);
    require_tail_contains_output(shaper, 0.5f);
  }
  SECTION("exciter and presence") {
    sonare::mastering::saturation::Exciter exciter;
    exciter.prepare(kRate, 256);
    require_tail_contains_output(exciter, 0.5f);
    sonare::mastering::spectral::PresenceEnhancer presence;
    presence.prepare(kRate, 256);
    require_tail_contains_output(presence, 0.5f);
  }
  SECTION("tape and overdrive") {
    sonare::mastering::saturation::Tape tape;
    tape.prepare(kRate, 256);
    require_tail_contains_output(tape, 0.5f);
    sonare::mastering::saturation::Overdrive overdrive;
    overdrive.prepare(kRate, 256);
    require_tail_contains_output(overdrive, 0.5f);
  }
  SECTION("amp sim") {
    sonare::mastering::saturation::AmpSimConfig config;
    config.drive = 0.4f;
    sonare::mastering::saturation::AmpSim amp(config);
    amp.prepare(kRate, 256);
    require_tail_contains_output(amp, 0.5f);
  }
}

TEST_CASE("Dynamic EQ and the streaming repair stages report the ring they leave",
          "[effects][tail]") {
  namespace repair = sonare::mastering::repair;
  constexpr double kRate = 48000.0;
  SECTION("dynamic eq") {
    sonare::mastering::eq::DynamicEqBand band;
    band.frequency_hz = 60.0f;
    band.static_gain_db = 9.0f;
    band.q = 6.0f;
    band.enabled = true;
    sonare::mastering::eq::DynamicEq eq;
    eq.prepare(kRate, 256);
    eq.set_band(0, band);
    require_tail_contains_output(eq, 0.5f);
  }
  SECTION("denoise") {
    repair::DenoiseClassicalConfig config;
    config.noise_estimator = repair::DenoiseNoiseEstimator::Spp;
    repair::StreamingDenoise denoise(config);
    denoise.prepare(kRate, 256);
    require_tail_contains_output(denoise, 0.5f);
  }
  SECTION("dereverb") {
    repair::StreamingDereverb dereverb(repair::DereverbClassicalConfig{});
    dereverb.prepare(kRate, 256);
    require_tail_contains_output(dereverb, 0.5f);
  }
  SECTION("decrackle") {
    repair::StreamingDecrackle decrackle(repair::DecrackleConfig{});
    decrackle.prepare(kRate, 256);
    require_tail_contains_output(decrackle, 0.5f);
  }
  SECTION("dehum, every mode") {
    for (const auto mode : {repair::DehumMode::Notch, repair::DehumMode::Subtract}) {
      for (const bool adaptive : {false, true}) {
        repair::DehumConfig config;
        config.mode = mode;
        config.adaptive = adaptive;
        repair::StreamingDehum dehum(config);
        dehum.prepare(kRate, 256);
        INFO("mode " << static_cast<int>(mode) << " adaptive " << adaptive);
        require_tail_contains_output(dehum, 0.5f);
      }
    }
  }
}
