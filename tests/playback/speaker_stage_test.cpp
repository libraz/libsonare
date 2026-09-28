#include "playback/speaker_stage.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <random>
#include <vector>

#include "core/fft.h"
#include "mastering/multiband/crossover.h"
#include "rt/fractional_delay.h"
#include "support/audio_fixtures.h"
#include "util/constants.h"
#include "util/db.h"

using sonare::ChannelLayout;
using sonare::SpeakerRole;
using namespace sonare::playback;

namespace {

double rms(const std::vector<float>& samples, std::size_t skip) {
  double sum = 0.0;
  std::size_t count = 0;
  for (std::size_t i = skip; i < samples.size(); ++i) {
    sum += static_cast<double>(samples[i]) * samples[i];
    ++count;
  }
  return count == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(count));
}

/// Delay of @p lagging relative to @p reference (same length, a power of two),
/// measured from the two signals themselves rather than assumed from any
/// interpolator: the unwrapped phase of the cross-spectrum lagging*conj(reference)
/// is linear in bin index for a pure delay, so a least-squares slope over the
/// bins below 0.2 * sample_rate (where a Lagrange-3 tap is still close to an
/// ideal delay) gives the delay directly, independent of how either signal was
/// produced.
double measure_relative_delay_samples(const std::vector<float>& lagging,
                                      const std::vector<float>& reference) {
  const auto n = static_cast<int>(lagging.size());

  // A rectangular window compares two segments that are honestly NOT a
  // circular shift of each other (each holds source samples the other does
  // not, near its edges), and that edge mismatch leaks disproportionately
  // into the low bins this function reads. A Hann taper suppresses exactly
  // that edge content before the transform, which a plain per-bin comparison
  // cannot undo afterwards.
  std::vector<float> windowed_lagging(static_cast<std::size_t>(n));
  std::vector<float> windowed_reference(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    const double w =
        0.5 * (1.0 - std::cos(sonare::constants::kTwoPiD * i / static_cast<double>(n - 1)));
    windowed_lagging[static_cast<std::size_t>(i)] =
        static_cast<float>(w) * lagging[static_cast<std::size_t>(i)];
    windowed_reference[static_cast<std::size_t>(i)] =
        static_cast<float>(w) * reference[static_cast<std::size_t>(i)];
  }

  sonare::FFT fft(n);
  fft.prepare(true, false, false);
  std::vector<std::complex<float>> spec_lagging(static_cast<std::size_t>(fft.n_bins()));
  std::vector<std::complex<float>> spec_reference(static_cast<std::size_t>(fft.n_bins()));
  fft.forward(windowed_lagging.data(), spec_lagging.data());
  fft.forward(windowed_reference.data(), spec_reference.data());

  const int kmax = static_cast<int>(0.2 * n);
  double previous = 0.0;
  double unwrapped = 0.0;
  std::vector<double> phase(static_cast<std::size_t>(kmax) + 1);
  for (int k = 0; k <= kmax; ++k) {
    const auto cross = std::complex<double>(spec_lagging[static_cast<std::size_t>(k)]) *
                       std::conj(std::complex<double>(spec_reference[static_cast<std::size_t>(k)]));
    const double raw = std::atan2(cross.imag(), cross.real());
    double step = raw - previous;
    while (step > sonare::constants::kPiD) step -= sonare::constants::kTwoPiD;
    while (step < -sonare::constants::kPiD) step += sonare::constants::kTwoPiD;
    unwrapped += step;
    previous = raw;
    phase[static_cast<std::size_t>(k)] = unwrapped;
  }

  // Least-squares slope of phase(k) vs k over k = 1..kmax (DC has no phase).
  double sum_k = 0.0, sum_phase = 0.0, sum_kk = 0.0, sum_kphase = 0.0;
  for (int k = 1; k <= kmax; ++k) {
    const double kd = static_cast<double>(k);
    sum_k += kd;
    sum_phase += phase[static_cast<std::size_t>(k)];
    sum_kk += kd * kd;
    sum_kphase += kd * phase[static_cast<std::size_t>(k)];
  }
  const auto count = static_cast<double>(kmax);
  const double mean_k = sum_k / count;
  const double mean_phase = sum_phase / count;
  const double slope =
      (sum_kphase - count * mean_k * mean_phase) / (sum_kk - count * mean_k * mean_k);
  // phase(k) = -2*pi*k*delay/n for a signal delayed by `delay` samples relative
  // to the reference, so delay = -slope * n / (2*pi).
  return -slope * n / sonare::constants::kTwoPiD;
}

}  // namespace

TEST_CASE("speaker trim applies its gain", "[playback][speakers]") {
  constexpr double kRate = 48000.0;
  constexpr int kFrames = 4800;
  SpeakerStage stage;
  std::array<SpeakerPrepare, kSpeakerRoleCount> speakers{};
  stage.prepare(kRate, kFrames, ChannelLayout::Stereo, speakers, BassManagementConfig{});
  std::array<float, kSpeakerRoleCount> trims{};
  trims[static_cast<size_t>(SpeakerRole::L)] = -6.0f;
  stage.set_levels(trims, 10.0f, 0.0f);

  std::vector<float> l(kFrames), r(kFrames);
  for (int i = 0; i < kFrames; ++i) {
    const float s = 0.5f * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(i) /
                                    static_cast<float>(kRate));
    l[static_cast<size_t>(i)] = s;
    r[static_cast<size_t>(i)] = s;
  }
  float* planes[2] = {l.data(), r.data()};
  stage.process(planes, kFrames);

  double e_l = 0.0, e_r = 0.0;
  for (int i = 0; i < kFrames; ++i) {
    e_l += static_cast<double>(l[static_cast<size_t>(i)]) * l[static_cast<size_t>(i)];
    e_r += static_cast<double>(r[static_cast<size_t>(i)]) * r[static_cast<size_t>(i)];
  }
  REQUIRE(e_r > 0.0);
  CHECK(std::abs(10.0 * std::log10(e_l / e_r) - (-6.0)) <= 0.01);
}

TEST_CASE("80 Hz LR4 bass management attenuates a small speaker and feeds the LFE plane",
          "[playback][speakers]") {
  constexpr double kRate = 48000.0;
  constexpr int kFrames = 24000;       // 0.5 s: 10 cycles of the 20 Hz probe tone.
  constexpr std::size_t kSkip = 4800;  // 100 ms transient guard.

  SpeakerStage stage;
  std::array<SpeakerPrepare, kSpeakerRoleCount> speakers{};
  speakers[static_cast<size_t>(SpeakerRole::L)].size = SpeakerSize::Small;
  BassManagementConfig bass;
  bass.enabled = true;
  bass.crossover_hz = 80.0f;
  bass.subwoofer = true;
  stage.prepare(kRate, kFrames, ChannelLayout::FivePointOne, speakers, bass);
  std::array<float, kSpeakerRoleCount> trims{};
  stage.set_levels(trims, 10.0f, 0.0f);

  const std::vector<float> tone = sonare::test::generate_sine(kFrames, 20.0f, 48000, 1.0f);
  std::vector<float> l = tone, r(kFrames, 0.0f), c(kFrames, 0.0f), lfe(kFrames, 0.0f),
                     ls(kFrames, 0.0f), rs(kFrames, 0.0f);
  float* planes[6] = {l.data(), r.data(), c.data(), lfe.data(), ls.data(), rs.data()};
  stage.process(planes, kFrames);

  const double input_rms = rms(tone, kSkip);
  const double l_rms = rms(l, kSkip);
  const double lfe_rms = rms(lfe, kSkip);
  REQUIRE(input_rms > 0.0);
  const double attenuation_db = 20.0 * std::log10(l_rms / input_rms);
  CAPTURE(attenuation_db);
  CHECK(attenuation_db <= -45.0);
  // 20 Hz sits deep in the 80 Hz LR4 passband, so the low band the crossover
  // hands to the LFE plane keeps most of the original energy (well above the
  // -45 dB the speaker's own high-passed output falls to).
  CHECK(20.0 * std::log10(lfe_rms / input_rms) >= -6.0);
}

TEST_CASE("80 Hz LR4 HPF and LPF sum stays flat 20 Hz-20 kHz", "[playback][speakers]") {
  constexpr double kRate = 48000.0;
  constexpr int kFrames = 24000;
  constexpr std::size_t kSkip = 4800;
  const float kFrequencies[] = {20.0f,   50.0f,   80.0f,   120.0f,   200.0f,   500.0f,
                                1000.0f, 2000.0f, 5000.0f, 10000.0f, 15000.0f, 19000.0f};

  for (float freq : kFrequencies) {
    SpeakerStage stage;
    std::array<SpeakerPrepare, kSpeakerRoleCount> speakers{};
    speakers[static_cast<size_t>(SpeakerRole::L)].size = SpeakerSize::Small;
    BassManagementConfig bass;
    bass.enabled = true;
    bass.crossover_hz = 80.0f;
    bass.subwoofer = true;
    stage.prepare(kRate, kFrames, ChannelLayout::FivePointOne, speakers, bass);
    std::array<float, kSpeakerRoleCount> trims{};
    // lfe_gain_db = 0 dB and a silent LFE input make the LFE plane output
    // exactly the low-band sum, so L + LFE reconstructs the crossover input.
    stage.set_levels(trims, 0.0f, 0.0f);

    const std::vector<float> tone = sonare::test::generate_sine(kFrames, freq, 48000, 1.0f);
    std::vector<float> l = tone, r(kFrames, 0.0f), c(kFrames, 0.0f), lfe(kFrames, 0.0f),
                       ls(kFrames, 0.0f), rs(kFrames, 0.0f);
    float* planes[6] = {l.data(), r.data(), c.data(), lfe.data(), ls.data(), rs.data()};
    stage.process(planes, kFrames);

    const double input_rms = rms(tone, kSkip);
    std::vector<float> sum(static_cast<std::size_t>(kFrames));
    for (int i = 0; i < kFrames; ++i) {
      sum[static_cast<std::size_t>(i)] =
          l[static_cast<std::size_t>(i)] + lfe[static_cast<std::size_t>(i)];
    }
    const double sum_rms = rms(sum, kSkip);
    REQUIRE(input_rms > 0.0);
    INFO("frequency " << freq << " Hz");
    CHECK(std::abs(20.0 * std::log10(sum_rms / input_rms)) <= 0.1);
  }
}

TEST_CASE("distance compensation gives the exact requested delay and reported latency",
          "[playback][speakers]") {
  constexpr double kRate = 48000.0;
  constexpr int kFrames = 8192;

  SpeakerStage stage;
  std::array<SpeakerPrepare, kSpeakerRoleCount> speakers{};
  speakers[static_cast<size_t>(SpeakerRole::L)].has_distance = true;
  speakers[static_cast<size_t>(SpeakerRole::L)].distance_m = 1.0f;
  speakers[static_cast<size_t>(SpeakerRole::R)].has_distance = true;
  speakers[static_cast<size_t>(SpeakerRole::R)].distance_m = 5.0f;
  stage.prepare(kRate, kFrames, ChannelLayout::Stereo, speakers, BassManagementConfig{});
  std::array<float, kSpeakerRoleCount> trims{};
  trims[static_cast<size_t>(SpeakerRole::L)] = -6.0f;
  stage.set_levels(trims, 10.0f, 0.0f);

  // Spec formula: delay = (max distance - this speaker's distance) / speed of
  // sound. R sits at the max distance (5 m), so it gets zero delay; L is
  // 4 m closer and gets the full 4 m / c worth of compensation.
  const double delay_seconds = (5.0 - 1.0) / static_cast<double>(sonare::constants::kSoundSpeedMps);
  const auto expected_q8_l = static_cast<int>(std::lround(delay_seconds * kRate * 256.0));

  CHECK(stage.plane_delay_q8(static_cast<int>(SpeakerRole::L)) == expected_q8_l);
  CHECK(stage.plane_delay_q8(static_cast<int>(SpeakerRole::R)) == 0);
  CHECK(stage.latency_samples_q8() == expected_q8_l);
  CHECK(stage.latency_samples() == static_cast<int>(std::lround(expected_q8_l / 256.0)));

  std::vector<float> l = sonare::test::generate_sine(kFrames, 1000.0f, 48000, 0.5f);
  std::vector<float> r = l;
  float* planes[2] = {l.data(), r.data()};
  stage.process(planes, kFrames);

  // Independent reference: apply the same Lagrange-3 kernel the header
  // documents, at the Q8 delay the spec formula gives, to a fresh copy of the
  // input (with the same trim applied first, matching gain-then-delay).
  std::vector<float> input = sonare::test::generate_sine(kFrames, 1000.0f, 48000, 0.5f);
  const float trim_linear = sonare::db_to_linear(-6.0f);
  std::vector<float> reference_buffer(static_cast<std::size_t>(expected_q8_l >> 8) + 16, 0.0f);
  std::size_t write_index = 0;
  std::vector<float> reference(static_cast<std::size_t>(kFrames));
  for (int i = 0; i < kFrames; ++i) {
    reference[static_cast<std::size_t>(i)] = sonare::rt::lagrange3_fractional_delay(
        reference_buffer.data(), reference_buffer.size(), write_index, expected_q8_l,
        trim_linear * input[static_cast<std::size_t>(i)]);
  }

  double max_diff = 0.0;
  for (int i = 0; i < kFrames; ++i) {
    max_diff =
        std::max(max_diff, static_cast<double>(std::abs(l[static_cast<std::size_t>(i)] -
                                                        reference[static_cast<std::size_t>(i)])));
  }
  CHECK(max_diff < 1e-5);
}

TEST_CASE("sub-feed rule adds the LFE gain and the small speakers' low-band sum",
          "[playback][speakers]") {
  constexpr double kRate = 48000.0;
  constexpr int kFrames = 24000;
  constexpr std::size_t kSkip = 4800;
  constexpr float kFreq = 20.0f;  // deep in the 80 Hz LR4 passband

  auto run = [&](bool feed_lfe, bool feed_ls) {
    SpeakerStage stage;
    std::array<SpeakerPrepare, kSpeakerRoleCount> speakers{};
    speakers[static_cast<size_t>(SpeakerRole::Ls)].size = SpeakerSize::Small;
    BassManagementConfig bass;
    bass.enabled = true;
    bass.crossover_hz = 80.0f;
    bass.subwoofer = true;
    stage.prepare(kRate, kFrames, ChannelLayout::FivePointOne, speakers, bass);
    std::array<float, kSpeakerRoleCount> trims{};
    stage.set_levels(trims, 10.0f, 0.0f);  // schema default lfe_gain_db

    const std::vector<float> tone = sonare::test::generate_sine(kFrames, kFreq, 48000, 1.0f);
    std::vector<float> l(kFrames, 0.0f), r(kFrames, 0.0f), c(kFrames, 0.0f), lfe(kFrames, 0.0f),
        ls(kFrames, 0.0f), rs(kFrames, 0.0f);
    if (feed_lfe) lfe = tone;
    if (feed_ls) ls = tone;
    float* planes[6] = {l.data(), r.data(), c.data(), lfe.data(), ls.data(), rs.data()};
    stage.process(planes, kFrames);
    return lfe;
  };

  const std::vector<float> lfe_only = run(true, false);
  const std::vector<float> ls_only = run(false, true);
  const std::vector<float> combined = run(true, true);

  double max_diff = 0.0;
  for (int i = 0; i < kFrames; ++i) {
    const float expected =
        lfe_only[static_cast<std::size_t>(i)] + ls_only[static_cast<std::size_t>(i)];
    max_diff = std::max(
        max_diff, static_cast<double>(std::abs(combined[static_cast<std::size_t>(i)] - expected)));
  }
  CHECK(max_diff < 1e-5);

  // lfe_gain_db = +10 dB must actually have been applied to the LFE-only path.
  const std::vector<float> tone = sonare::test::generate_sine(kFrames, kFreq, 48000, 1.0f);
  const double gain_db = 20.0 * std::log10(rms(lfe_only, kSkip) / rms(tone, kSkip));
  CHECK(std::abs(gain_db - 10.0) <= 0.1);
}

TEST_CASE("subwoofer=false folds the low-band and LFE sum to the large L/R pair",
          "[playback][speakers]") {
  constexpr double kRate = 48000.0;
  constexpr int kFrames = 24000;
  constexpr float kFreq = 40.0f;

  SpeakerStage stage;
  std::array<SpeakerPrepare, kSpeakerRoleCount> speakers{};
  speakers[static_cast<size_t>(SpeakerRole::Ls)].size = SpeakerSize::Small;
  BassManagementConfig bass;
  bass.enabled = true;
  bass.crossover_hz = 80.0f;
  bass.subwoofer = false;
  stage.prepare(kRate, kFrames, ChannelLayout::FivePointOne, speakers, bass);
  std::array<float, kSpeakerRoleCount> trims{};
  const float lfe_mix_db = -6.0f;
  stage.set_levels(trims, 10.0f, lfe_mix_db);

  const std::vector<float> tone = sonare::test::generate_sine(kFrames, kFreq, 48000, 1.0f);
  std::vector<float> l(kFrames, 0.0f), r(kFrames, 0.0f), c(kFrames, 0.0f),
      lfe = tone, ls(kFrames, 0.0f), rs(kFrames, 0.0f);
  float* planes[6] = {l.data(), r.data(), c.data(), lfe.data(), ls.data(), rs.data()};
  stage.process(planes, kFrames);

  // No subwoofer: the discrete LFE plane must end up silent.
  float lfe_peak = 0.0f;
  for (float v : lfe) lfe_peak = std::max(lfe_peak, std::abs(v));
  CHECK(lfe_peak == 0.0f);

  // Independent reference: the same fold-down rule (lfe_mix_db, 120 Hz LR4,
  // -3 dB each) applied to the LFE input alone via the shared crossover.
  sonare::mastering::multiband::CrossoverConfig fold_config;
  fold_config.cutoffs_hz = {120.0f};
  fold_config.slope = sonare::mastering::multiband::CrossoverSlope::LR4;
  fold_config.mode = sonare::mastering::multiband::CrossoverMode::LinkwitzRiley;
  sonare::mastering::multiband::Crossover fold(fold_config);
  fold.prepare(kRate, kFrames, 1);
  sonare::mastering::multiband::CrossoverScratch scratch;
  fold.prepare_scratch(scratch, 1, kFrames);
  std::vector<float> fold_input(static_cast<std::size_t>(kFrames));
  const float mix_linear = sonare::db_to_linear(lfe_mix_db);
  for (int i = 0; i < kFrames; ++i) {
    fold_input[static_cast<std::size_t>(i)] = mix_linear * tone[static_cast<std::size_t>(i)];
  }
  float* fold_ptr = fold_input.data();
  fold.split_into(&fold_ptr, 1, kFrames, scratch);
  const float* low = scratch.band_channels[0][0];

  double max_diff_l = 0.0, max_diff_r = 0.0;
  for (int i = 0; i < kFrames; ++i) {
    const float expected = sonare::constants::kInvSqrt2 * low[i];
    max_diff_l = std::max(max_diff_l,
                          static_cast<double>(std::abs(l[static_cast<std::size_t>(i)] - expected)));
    max_diff_r = std::max(max_diff_r,
                          static_cast<double>(std::abs(r[static_cast<std::size_t>(i)] - expected)));
  }
  CHECK(max_diff_l < 1e-5);
  CHECK(max_diff_r < 1e-5);
}

TEST_CASE("bass management disabled leaves the LFE plane untouched", "[playback][speakers]") {
  constexpr double kRate = 48000.0;
  constexpr int kFrames = 4800;

  SpeakerStage stage;
  std::array<SpeakerPrepare, kSpeakerRoleCount> speakers{};
  speakers[static_cast<size_t>(SpeakerRole::Ls)].size = SpeakerSize::Small;
  stage.prepare(kRate, kFrames, ChannelLayout::FivePointOne, speakers, BassManagementConfig{});
  std::array<float, kSpeakerRoleCount> trims{};
  stage.set_levels(trims, 10.0f, 0.0f);

  const std::vector<float> tone = sonare::test::generate_sine(kFrames, 40.0f, 48000, 1.0f);
  std::vector<float> l(kFrames, 0.0f), r(kFrames, 0.0f), c(kFrames, 0.0f),
      lfe = tone, ls(kFrames, 0.0f), rs(kFrames, 0.0f);
  float* planes[6] = {l.data(), r.data(), c.data(), lfe.data(), ls.data(), rs.data()};
  stage.process(planes, kFrames);

  float max_diff = 0.0f;
  for (int i = 0; i < kFrames; ++i) {
    max_diff = std::max(
        max_diff, std::abs(lfe[static_cast<std::size_t>(i)] - tone[static_cast<std::size_t>(i)]));
  }
  CHECK(max_diff == 0.0f);
}

TEST_CASE("distance delay and 80 Hz crossover agree at 44.1 and 48 kHz", "[playback][speakers]") {
  for (const double rate : {44100.0, 48000.0}) {
    const int frames = static_cast<int>(rate * 0.5);
    SpeakerStage stage;
    std::array<SpeakerPrepare, kSpeakerRoleCount> speakers{};
    speakers[static_cast<size_t>(SpeakerRole::L)].has_distance = true;
    speakers[static_cast<size_t>(SpeakerRole::L)].distance_m = 1.0f;
    speakers[static_cast<size_t>(SpeakerRole::R)].has_distance = true;
    speakers[static_cast<size_t>(SpeakerRole::R)].distance_m = 5.0f;
    speakers[static_cast<size_t>(SpeakerRole::L)].size = SpeakerSize::Small;
    BassManagementConfig bass;
    bass.enabled = true;
    bass.crossover_hz = 80.0f;
    bass.subwoofer = true;
    stage.prepare(rate, frames, ChannelLayout::FivePointOne, speakers, bass);
    std::array<float, kSpeakerRoleCount> trims{};
    stage.set_levels(trims, 10.0f, 0.0f);

    const double delay_seconds =
        (5.0 - 1.0) / static_cast<double>(sonare::constants::kSoundSpeedMps);
    const auto expected_q8 = static_cast<int>(std::lround(delay_seconds * rate * 256.0));
    INFO("rate " << rate);
    CHECK(stage.plane_delay_q8(static_cast<int>(SpeakerRole::L)) == expected_q8);

    // Cross-rate agreement: the same physical distance difference must yield
    // the same delay in seconds regardless of sample rate.
    const double measured_seconds = expected_q8 / 256.0 / rate;
    CHECK(std::abs(measured_seconds - delay_seconds) < 1.0 / (256.0 * rate));

    const std::vector<float> tone =
        sonare::test::generate_sine(frames, 20.0f, static_cast<int>(rate), 1.0f);
    std::vector<float> l = tone, r(static_cast<std::size_t>(frames), 0.0f),
                       c(static_cast<std::size_t>(frames), 0.0f),
                       lfe(static_cast<std::size_t>(frames), 0.0f),
                       ls(static_cast<std::size_t>(frames), 0.0f),
                       rs(static_cast<std::size_t>(frames), 0.0f);
    float* planes[6] = {l.data(), r.data(), c.data(), lfe.data(), ls.data(), rs.data()};
    stage.process(planes, frames);

    const std::size_t skip = static_cast<std::size_t>(rate * 0.1);
    const double input_rms = rms(tone, skip);
    const double output_rms = rms(l, skip);
    const double attenuation_db = 20.0 * std::log10(output_rms / input_rms);
    CHECK(attenuation_db <= -45.0);
  }
}

TEST_CASE("distance delay measured from the output's own cross-spectrum phase slope",
          "[playback][speakers]") {
  constexpr double kRate = 48000.0;
  constexpr int kAnalysisFrames = 65536;  // room for a fine bin grid below 0.2 * fs
  constexpr int kWarmupFrames = 4096;     // several times the delay under test
  // Two Δd values with different fractional parts: comparing against a
  // Lagrange-3 reference (as the other distance test does) cannot tell a
  // correct delay convention from a consistently-wrong one, since both sides
  // would share the same interpolator. This measures the delay purely from
  // the processed output, independent of how speaker_stage.cpp built it.
  for (const double target_delay_samples : {100.25, 150.7}) {
    SpeakerStage stage;
    std::array<SpeakerPrepare, kSpeakerRoleCount> speakers{};
    constexpr float kLDistance = 1.0f;
    const auto r_distance = static_cast<float>(
        kLDistance + target_delay_samples * sonare::constants::kSoundSpeedMps / kRate);
    speakers[static_cast<size_t>(SpeakerRole::L)].has_distance = true;
    speakers[static_cast<size_t>(SpeakerRole::L)].distance_m = kLDistance;
    speakers[static_cast<size_t>(SpeakerRole::R)].has_distance = true;
    speakers[static_cast<size_t>(SpeakerRole::R)].distance_m = r_distance;
    stage.prepare(kRate, kWarmupFrames + kAnalysisFrames, ChannelLayout::Stereo, speakers,
                  BassManagementConfig{});
    std::array<float, kSpeakerRoleCount> trims{};
    stage.set_levels(trims, 10.0f, 0.0f);

    const double expected_delay =
        (static_cast<double>(r_distance) - static_cast<double>(kLDistance)) /
        static_cast<double>(sonare::constants::kSoundSpeedMps) * kRate;

    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> source(static_cast<std::size_t>(kWarmupFrames + kAnalysisFrames));
    for (float& s : source) s = dist(rng);

    // Warm up the delay line on both planes so the analysis window sees
    // steady-state history rather than the initial zero fill.
    std::vector<float> warm_l(source.begin(), source.begin() + kWarmupFrames);
    std::vector<float> warm_r = warm_l;
    float* warm_planes[2] = {warm_l.data(), warm_r.data()};
    stage.process(warm_planes, kWarmupFrames);

    std::vector<float> l(source.begin() + kWarmupFrames, source.end());
    std::vector<float> r = l;
    float* planes[2] = {l.data(), r.data()};
    stage.process(planes, kAnalysisFrames);

    const double measured_delay = measure_relative_delay_samples(l, r);
    const double error = measured_delay - expected_delay;
    CAPTURE(target_delay_samples, expected_delay, measured_delay, error);
    CHECK(std::lround(measured_delay) == std::lround(expected_delay));
    CHECK(std::abs(error) <= 0.1);
  }
}
