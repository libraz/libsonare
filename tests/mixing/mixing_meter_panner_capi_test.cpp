/// @file mixing_meter_panner_capi_test.cpp
/// @brief Mixing meter, panner, and C API tests.

#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <cstring>

#include "c_api/mixing_internal.h"
#include "mixing_test_helpers.h"
#include "util/constants.h"

TEST_CASE("Const3dB pan law preserves stereo power", "[mixing]") {
  for (float pan : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
    const auto gains = sonare::mixing::compute_pan_gains(pan, sonare::mixing::PanLaw::Const3dB);

    REQUIRE_THAT(energy(gains.left, gains.right), WithinAbs(1.0f, 0.0001f));
  }
}

TEST_CASE("Panner supports selectable pan laws", "[mixing]") {
  const auto const3 = sonare::mixing::compute_pan_gains(0.0f, sonare::mixing::PanLaw::Const3dB);
  const auto const45 = sonare::mixing::compute_pan_gains(0.0f, sonare::mixing::PanLaw::Const4p5dB);
  const auto const6 = sonare::mixing::compute_pan_gains(0.0f, sonare::mixing::PanLaw::Const6dB);
  const auto linear = sonare::mixing::compute_pan_gains(0.0f, sonare::mixing::PanLaw::Linear0dB);

  REQUIRE_THAT(const3.left, WithinAbs(std::sqrt(0.5f), 0.0001f));
  REQUIRE_THAT(const3.right, WithinAbs(std::sqrt(0.5f), 0.0001f));
  REQUIRE_THAT(20.0f * std::log10(const45.left), WithinAbs(-4.5f, 0.05f));
  REQUIRE_THAT(const45.left, WithinAbs(const45.right, 0.0001f));
  REQUIRE_THAT(const6.left, WithinAbs(0.5f, 0.0001f));
  REQUIRE_THAT(const6.right, WithinAbs(0.5f, 0.0001f));
  REQUIRE_THAT(linear.left, WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(linear.right, WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("Panner mono center follows the pan law's raw energy by design", "[mixing]") {
  // A mono strip has no stereo image to balance, so it applies the pan law's
  // literal combined energy sqrt(l^2 + r^2) at center. A stereo strip's Balance
  // path instead normalizes the near channel to unity for every law. The two
  // therefore agree at center only under the constant-power default; the
  // difference under the other laws is intentional and this test locks it in.
  struct Case {
    sonare::mixing::PanLaw law;
    float expected_mono_center;
  };
  const Case cases[] = {
      {sonare::mixing::PanLaw::Const3dB, 1.0f},              // l == r == 1/sqrt(2) -> 1.0
      {sonare::mixing::PanLaw::Const6dB, std::sqrt(0.5f)},   // l == r == 0.5      -> sqrt(0.5)
      {sonare::mixing::PanLaw::Linear0dB, std::sqrt(2.0f)},  // l == r == 1.0     -> sqrt(2)
  };
  for (const auto& c : cases) {
    std::array<float, 2> mono{1.0f, 1.0f};
    float* mono_ch[] = {mono.data()};
    sonare::mixing::PannerProcessor mono_panner(
        {0.0f, c.law, 0.0f, sonare::mixing::PanMode::Balance});
    mono_panner.prepare(48000.0, 1);
    mono_panner.process(mono_ch, 1, 2);
    REQUIRE_THAT(mono[0], WithinAbs(c.expected_mono_center, 0.0001f));

    // The stereo balance path keeps a centered signal at unity for every law.
    std::array<float, 2> left{1.0f, 1.0f};
    std::array<float, 2> right{1.0f, 1.0f};
    float* stereo_ch[] = {left.data(), right.data()};
    sonare::mixing::PannerProcessor stereo_panner(
        {0.0f, c.law, 0.0f, sonare::mixing::PanMode::Balance});
    stereo_panner.prepare(48000.0, 2);
    stereo_panner.process(stereo_ch, 2, 2);
    REQUIRE_THAT(left[0], WithinAbs(1.0f, 0.0001f));
    REQUIRE_THAT(right[0], WithinAbs(1.0f, 0.0001f));
  }
}

TEST_CASE("Panner supports balance stereo-pan and dual-pan modes", "[mixing]") {
  SECTION("Balance preserves independent stereo channels") {
    std::array<float, 2> left{1.0f, 1.0f};
    std::array<float, 2> right{0.25f, 0.25f};
    float* channels[] = {left.data(), right.data()};

    sonare::mixing::PannerProcessor panner(
        {1.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f, sonare::mixing::PanMode::Balance});
    panner.prepare(48000.0, 2);
    panner.process(channels, 2, 2);

    REQUIRE_THAT(left[0], WithinAbs(0.0f, 0.0001f));
    REQUIRE_THAT(right[0], WithinAbs(0.25f, 0.0001f));
  }

  SECTION("StereoPan collapses stereo input to a panned mono image") {
    std::array<float, 2> left{1.0f, 1.0f};
    std::array<float, 2> right{0.0f, 0.0f};
    float* channels[] = {left.data(), right.data()};

    sonare::mixing::PannerProcessor panner(
        {1.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f, sonare::mixing::PanMode::StereoPan});
    panner.prepare(48000.0, 2);
    panner.process(channels, 2, 2);

    REQUIRE_THAT(left[0], WithinAbs(0.0f, 0.0001f));
    REQUIRE_THAT(right[0], WithinAbs(0.5f, 0.0001f));
  }

  SECTION("DualPan routes left and right inputs independently") {
    std::array<float, 2> left{1.0f, 1.0f};
    std::array<float, 2> right{2.0f, 2.0f};
    float* channels[] = {left.data(), right.data()};

    sonare::mixing::PannerProcessor panner(
        {0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f, sonare::mixing::PanMode::DualPan});
    panner.set_dual_pan(1.0f, -1.0f);
    panner.prepare(48000.0, 2);
    panner.process(channels, 2, 2);

    REQUIRE_THAT(left[0], WithinAbs(2.0f, 0.0001f));
    REQUIRE_THAT(right[0], WithinAbs(1.0f, 0.0001f));
  }
}

TEST_CASE("Panner DualPan ramps coefficient changes without a single-sample step", "[mixing]") {
  // Regression: DualPan must smooth its 2x2 routing matrix the same way the
  // other pan modes smooth their gains. With the old instantaneous behavior the
  // first sample of the block after set_dual_pan() would jump straight to the
  // new target. A 5 ms one-pole smoother instead ramps over many samples.
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 256;

  // Start fully separated: left input -> left out, right input -> right out
  // (DualPan defaults to dual_pan_left = -1, dual_pan_right = +1). Feed a DC
  // signal only on the left input so the left output reflects the left->left
  // coefficient (ll) directly.
  // The -6 dB law's left gain starts falling the moment the position leaves the
  // hard-left end; Linear0dB holds it at unity until the position crosses centre.
  sonare::mixing::PannerProcessor panner(
      {0.0f, sonare::mixing::PanLaw::Const6dB, 5.0f, sonare::mixing::PanMode::DualPan});
  panner.prepare(kSr, kBlock);

  std::vector<float> left(kBlock, 1.0f);
  std::vector<float> right(kBlock, 0.0f);
  float* channels[] = {left.data(), right.data()};

  // Settle the initial state: left out should equal the input (ll == 1).
  panner.process(channels, 2, kBlock);
  REQUIRE_THAT(left[kBlock - 1], WithinAbs(1.0f, 0.01f));
  const float settled = left[kBlock - 1];

  // Swap the routing: now left input should go to the right output, so the
  // left->left coefficient (ll) must ramp from 1 toward 0.
  panner.set_dual_pan(1.0f, -1.0f);
  std::fill(left.begin(), left.end(), 1.0f);
  std::fill(right.begin(), right.end(), 0.0f);
  panner.process(channels, 2, kBlock);

  // (1) Continuity across the block boundary: the first sample of the new block
  // stays close to the previous block's last sample. A single-sample step to
  // the new target (ll == 0) would put left[0] near 0, failing this bound.
  REQUIRE_THAT(left[0], WithinAbs(settled, 0.05f));

  // (2) The coefficient ramps gradually: each of the first few samples moves
  // only a little and the sequence is monotonically decreasing toward 0. A
  // single-sample step would make samples 1..N identical (all 0), not a ramp.
  REQUIRE(left[0] > left[1]);
  REQUIRE(left[1] > left[2]);
  REQUIRE(left[4] > 0.5f);  // still well above target after ~0.1 ms

  // (3) After the full block (~5.3 ms, ~1 time constant) it has converged well
  // toward the new target of 0.
  REQUIRE(left[kBlock - 1] < 0.4f);
}

TEST_CASE("Meter snapshot exposes mono compatibility fields", "[mixing]") {
  std::array<float, 4> left{1.0f, 0.75f, -0.5f, -0.25f};
  std::array<float, 4> right = left;
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::MeterProcessor meter({false, false, 4, 0.0f});
  meter.prepare(48000.0, static_cast<int>(left.size()));
  meter.process(channels, 2, static_cast<int>(left.size()));
  auto snapshot = meter.snapshot();

  REQUIRE_THAT(snapshot.correlation, WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(snapshot.mono_compat_width, WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(snapshot.mono_compat_side_rms, WithinAbs(0.0f, 0.0001f));
  REQUIRE(snapshot.likely_mono_compatible);

  for (size_t i = 0; i < left.size(); ++i) {
    right[i] = -left[i];
  }
  meter.process(channels, 2, static_cast<int>(left.size()));
  snapshot = meter.snapshot();

  REQUIRE_THAT(snapshot.correlation, WithinAbs(-1.0f, 0.0001f));
  // Anti-phase (mid -> 0, side present) drives the raw width ratio to +Inf; the
  // meter must clamp it to a large but FINITE sentinel so it stays serializable
  // through the C ABI / JSON telemetry rather than emitting +Inf.
  REQUIRE(snapshot.mono_compat_width > 1000.0f);
  REQUIRE(std::isfinite(snapshot.mono_compat_width));
  REQUIRE_THAT(snapshot.mono_compat_peak, WithinAbs(0.0f, 0.0001f));
  REQUIRE_FALSE(snapshot.likely_mono_compatible);
}

TEST_CASE("Meter stereo statistics match a direct double-precision reference",
          "[mixing][meter-stereo-accumulation]") {
  constexpr int kSamples = 4096;
  std::vector<float> left(kSamples);
  std::vector<float> right(kSamples);
  uint32_t state = 0x2468ace1u;
  const auto next = [&]() {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state >> 8) / static_cast<float>(1u << 24) - 0.5f;
  };
  for (int i = 0; i < kSamples; ++i) {
    left[static_cast<size_t>(i)] = next();
    right[static_cast<size_t>(i)] = 0.6f * left[static_cast<size_t>(i)] + 0.3f * next();
  }
  double sum_l = 0.0, sum_r = 0.0, sum_lr = 0.0, sum_mid = 0.0, sum_side = 0.0, side_rms_sum = 0.0;
  for (int i = 0; i < kSamples; ++i) {
    const double l = left[static_cast<size_t>(i)];
    const double r = right[static_cast<size_t>(i)];
    sum_l += l * l;
    sum_r += r * r;
    sum_lr += l * r;
    sum_mid += (l + r) * (l + r) * 0.5;
    sum_side += (l - r) * (l - r) * 0.5;
    side_rms_sum += 0.25 * (l - r) * (l - r);
  }

  float* channels[] = {left.data(), right.data()};
  sonare::mixing::MeterProcessor meter({false, false, 4, 0.0f});
  meter.prepare(48000.0, kSamples);
  meter.process(channels, 2, kSamples);
  const auto snapshot = meter.snapshot();

  REQUIRE(snapshot.correlation == Catch::Approx(sum_lr / std::sqrt(sum_l * sum_r)).epsilon(1e-6));
  REQUIRE(snapshot.mono_compat_side_rms ==
          Catch::Approx(std::sqrt(side_rms_sum / kSamples)).epsilon(1e-6));
  REQUIRE(snapshot.mono_compat_width == Catch::Approx(std::sqrt(sum_side / sum_mid)).epsilon(1e-6));
  REQUIRE(snapshot.rms_db[0] ==
          Catch::Approx(20.0 * std::log10(std::sqrt(sum_l / kSamples))).epsilon(1e-6));
}

TEST_CASE("Mixing C API processes stereo strips and exposes meters", "[mixing][capi]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 8, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip_a = sonare_mixer_add_strip(mixer, "a");
  SonareStrip* strip_b = sonare_mixer_add_strip(mixer, "b");
  REQUIRE(strip_a != nullptr);
  REQUIRE(strip_b != nullptr);
  REQUIRE(sonare_strip_set_fader_db(strip_b, -6.0f) == SONARE_OK);
  REQUIRE(sonare_strip_set_pan(strip_a, 1.0f, SONARE_PAN_MODE_BALANCE) == SONARE_OK);
  REQUIRE(sonare_strip_set_muted(strip_a, 1) == SONARE_OK);

  std::array<float, 4> a_l{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> a_r{0.0f, 0.0f, 0.0f, 0.0f};
  std::array<float, 4> b_l{0.0f, 0.0f, 0.0f, 0.0f};
  std::array<float, 4> b_r{1.0f, 1.0f, 1.0f, 1.0f};
  const float* inputs_l[] = {a_l.data(), b_l.data()};
  const float* inputs_r[] = {a_r.data(), b_r.data()};
  std::array<float, 4> out_l{};
  std::array<float, 4> out_r{};

  REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, inputs_r, 2, out_l.data(), out_r.data(),
                                      out_l.size()) == SONARE_OK);

  REQUIRE_THAT(out_l[0], WithinAbs(0.0f, 0.0001f));
  REQUIRE(out_r[0] > std::pow(10.0f, -6.0f / 20.0f));
  REQUIRE(out_r[0] < 1.0f);

  SonareMixMeterSnapshot snapshot{};
  REQUIRE(sonare_strip_meter(strip_b, &snapshot) == SONARE_OK);
  REQUIRE(snapshot.seq > 0);
  REQUIRE(snapshot.likely_mono_compatible == 1);

  // Per-plane meters expose the surround planes; a stereo strip reports two
  // planes whose [0]/[1] mirror the *_l/*_r stereo convenience fields.
  REQUIRE(snapshot.num_channels == 2);
  REQUIRE(snapshot.peak_db[0] == snapshot.peak_db_l);
  REQUIRE(snapshot.peak_db[1] == snapshot.peak_db_r);
  REQUIRE(snapshot.rms_db[0] == snapshot.rms_db_l);
  REQUIRE(snapshot.rms_db[1] == snapshot.rms_db_r);
  REQUIRE(snapshot.true_peak_dbtp[0] == snapshot.true_peak_dbtp_l);
  REQUIRE(snapshot.true_peak_dbtp[1] == snapshot.true_peak_dbtp_r);

  sonare_mixer_destroy(mixer);
}

TEST_CASE("Mixing C API rejects a non-finite process block without touching the mix",
          "[mixing][capi]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 8, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "a");
  REQUIRE(strip != nullptr);

  std::array<float, 4> in_l{0.5f, 0.5f, 0.5f, 0.5f};
  std::array<float, 4> in_r{0.5f, 0.5f, 0.5f, 0.5f};
  const float* inputs_l[] = {in_l.data()};
  const float* inputs_r[] = {in_r.data()};
  std::array<float, 4> out_l{};
  std::array<float, 4> out_r{};

  // Positive control: the identical call with finite input must succeed and
  // produce an audible mix, so a later rejection cannot be an unrelated failure.
  REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, inputs_r, 1, out_l.data(), out_r.data(),
                                      out_l.size()) == SONARE_OK);
  REQUIRE(out_l[0] > 0.1f);
  SonareMixMeterSnapshot mixed{};
  REQUIRE(sonare_strip_meter(strip, &mixed) == SONARE_OK);
  REQUIRE(mixed.seq > 0);

  // A zero-frame block stays a documented no-op (see the EMPTY-BLOCK POLICY in
  // sonare_mixer_process_stereo); sonare_mixer_drain_tail_stereo relies on it.
  REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, inputs_r, 1, out_l.data(), out_r.data(),
                                      0) == SONARE_OK);

  SECTION("NaN in the left channel") {
    std::array<float, 4> nan_l = in_l;
    nan_l[2] = std::numeric_limits<float>::quiet_NaN();
    const float* bad_l[] = {nan_l.data()};
    const std::array<float, 4> sentinel{-7.0f, -7.0f, -7.0f, -7.0f};
    out_l = sentinel;
    out_r = sentinel;

    REQUIRE(sonare_mixer_process_stereo(mixer, bad_l, inputs_r, 1, out_l.data(), out_r.data(),
                                        out_l.size()) == SONARE_ERROR_INVALID_PARAMETER);
    // The rejection happens before any mixer state changes: the output buffers
    // are left untouched and the meters do not advance, so a NaN can never be
    // read back as a plausible mix.
    REQUIRE(out_l == sentinel);
    REQUIRE(out_r == sentinel);
    SonareMixMeterSnapshot after{};
    REQUIRE(sonare_strip_meter(strip, &after) == SONARE_OK);
    REQUIRE(after.seq == mixed.seq);
  }

  SECTION("Inf in the right channel") {
    std::array<float, 4> inf_r = in_r;
    inf_r[0] = std::numeric_limits<float>::infinity();
    const float* bad_r[] = {inf_r.data()};
    REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, bad_r, 1, out_l.data(), out_r.data(),
                                        out_l.size()) == SONARE_ERROR_INVALID_PARAMETER);
  }

  sonare_mixer_destroy(mixer);
}

TEST_CASE("Mixing C API reports the state a strip discarded after a non-finite sample",
          "[mixing][capi][non_finite]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 64, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "a");
  REQUIRE(strip != nullptr);

  // Pre-set to a value the entry must overwrite, so a read that never writes
  // cannot pass as a zero count.
  uint32_t count = 0xDEADu;
  REQUIRE(sonare_strip_non_finite_discard_count(strip, &count) == SONARE_OK);
  REQUIRE(count == 0u);

  // A strip with no enabled band and no insert holds no recursive state at all,
  // so its count would stay at zero for a reason unrelated to the entry. One
  // peaking band gives it two cells per channel to lose.
  strip->strip.set_eq_band(
      0, sonare::mastering::eq::EqBand(sonare::mastering::eq::EqBandType::Peak, 1000.0f, 6.0f, 2.0f,
                                       /*is_enabled=*/true));

  constexpr int kBlock = 32;
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* channels[] = {left.data(), right.data()};
  const auto fill = [&](float value) {
    for (int i = 0; i < kBlock; ++i) {
      left[static_cast<size_t>(i)] = 0.25f;
      right[static_cast<size_t>(i)] = 0.25f;
    }
    left[4] = value;
    right[4] = value;
  };

  // Control: an ordinary block leaves the count where it was, so the increment
  // below is attributable to the poison rather than to processing at all.
  fill(0.25f);
  strip->strip.process(channels, 2, kBlock);
  REQUIRE(sonare_strip_non_finite_discard_count(strip, &count) == SONARE_OK);
  REQUIRE(count == 0u);

  // The poison is applied through the strip itself rather than through
  // sonare_mixer_process_stereo, which refuses a non-finite input block
  // outright (see the rejection case above). What reaches a strip's recursive
  // state is therefore always something the chain produced, which is exactly
  // what this count is for.
  fill(std::numeric_limits<float>::quiet_NaN());
  strip->strip.process(channels, 2, kBlock);
  REQUIRE(sonare_strip_non_finite_discard_count(strip, &count) == SONARE_OK);
  // Both channels lost their cells and the block still adds one. Moving by two
  // here would report a stereo strip as twice as degraded as a mono one for the
  // same defect, over a width the caller passed rather than asked for.
  REQUIRE(count == 1u);
  REQUIRE(count == strip->strip.non_finite_discard_count());

  REQUIRE(sonare_strip_non_finite_discard_count(nullptr, &count) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(count == 1u);
  REQUIRE(sonare_strip_non_finite_discard_count(strip, nullptr) == SONARE_ERROR_INVALID_PARAMETER);

  sonare_mixer_destroy(mixer);
}

TEST_CASE("A strip's discard count reaches a meter that met a non-finite sample",
          "[mixing][capi][non_finite]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 64, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "a");
  REQUIRE(strip != nullptr);

  // No band is enabled and no insert is added, so the EQ's own discard branch
  // cannot run and the inserts do not exist. Anything this count records is the
  // meter's, which is what makes the meter's contribution attributable rather
  // than merely present.
  constexpr int kBlock = 32;
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* channels[] = {left.data(), right.data()};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = 0.25f;
    right[static_cast<size_t>(i)] = 0.25f;
  }

  uint32_t count = 0xDEADu;
  strip->strip.process(channels, 2, kBlock);
  REQUIRE(sonare_strip_non_finite_discard_count(strip, &count) == SONARE_OK);
  REQUIRE(count == 0u);
  REQUIRE(strip->strip.eq().non_finite_discard_count() == 0u);

  left[4] = std::numeric_limits<float>::quiet_NaN();
  right[4] = std::numeric_limits<float>::quiet_NaN();
  strip->strip.process(channels, 2, kBlock);

  // The meters count the non-finite sample (as silence) in the block it arrives.
  REQUIRE(sonare_strip_non_finite_discard_count(strip, &count) == SONARE_OK);
  REQUIRE(count == 1u);
  // The EQ stayed out of it, so the strip's count moved for the meters alone.
  REQUIRE(strip->strip.eq().non_finite_discard_count() == 0u);

  // Nothing was carried into the meters' state, so a clean block adds nothing.
  left[4] = 0.25f;
  right[4] = 0.25f;
  strip->strip.process(channels, 2, kBlock);
  REQUIRE(sonare_strip_non_finite_discard_count(strip, &count) == SONARE_OK);
  REQUIRE(count == 1u);

  sonare_mixer_destroy(mixer);
}

TEST_CASE("Mixing C API reports the state a bus discarded after a non-finite sample",
          "[mixing][capi][non_finite]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 64, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  REQUIRE(sonare_mixer_add_strip(mixer, "a") != nullptr);
  REQUIRE(sonare_mixer_add_bus(mixer, "verb", "aux") == SONARE_OK);

  uint32_t count = 0xDEADu;
  // A declared bus has no DSP record until a compile builds one, so it is
  // reported as unknown rather than as a zero that would read as "clean".
  REQUIRE(sonare_mixer_bus_non_finite_discard_count(mixer, "verb", &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_mixer_compile(mixer) == SONARE_OK);
  REQUIRE(sonare_mixer_bus_non_finite_discard_count(mixer, "verb", &count) == SONARE_OK);
  REQUIRE(count == 0u);

  REQUIRE(sonare_mixer_bus_non_finite_discard_count(mixer, "nosuchbus", &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_mixer_bus_non_finite_discard_count(nullptr, "verb", &count) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_mixer_bus_non_finite_discard_count(mixer, "verb", nullptr) ==
          SONARE_ERROR_INVALID_PARAMETER);

  sonare::mixing::FxBus* fx = nullptr;
  for (const auto& entry : mixer->bus_dsp) {
    if (entry && entry->id == "verb") fx = &entry->fx;
  }
  REQUIRE(fx != nullptr);

  constexpr int kBlock = 32;
  std::array<float, kBlock> left{};
  std::array<float, kBlock> right{};
  float* channels[] = {left.data(), right.data()};
  for (int i = 0; i < kBlock; ++i) {
    left[static_cast<size_t>(i)] = 0.25f;
    right[static_cast<size_t>(i)] = 0.25f;
  }

  fx->process(channels, 2, kBlock);
  REQUIRE(sonare_mixer_bus_non_finite_discard_count(mixer, "verb", &count) == SONARE_OK);
  REQUIRE(count == 0u);

  left[4] = std::numeric_limits<float>::quiet_NaN();
  right[4] = std::numeric_limits<float>::quiet_NaN();
  fx->process(channels, 2, kBlock);
  // The bus meter counts the non-finite sample (as silence) in the block it arrives. An aux bus a
  // send destination names carries no inserts, so this count is the meter's alone.
  REQUIRE(sonare_mixer_bus_non_finite_discard_count(mixer, "verb", &count) == SONARE_OK);
  REQUIRE(count == 1u);
  left[4] = 0.25f;
  right[4] = 0.25f;
  fx->process(channels, 2, kBlock);
  REQUIRE(sonare_mixer_bus_non_finite_discard_count(mixer, "verb", &count) == SONARE_OK);
  // Nothing was carried into the meter's state, so a clean block adds nothing.
  REQUIRE(count == 1u);

  sonare_mixer_destroy(mixer);
}

// A NULL per-strip channel pointer is the one rejection this entry used to make
// after zero-filling the caller's output. The other three return before writing,
// and the non-finite case above asserts that a rejected block leaves the buffers
// alone - a host that treats an error as "keep the previous block" got a hard
// dropout from this path instead of a rejection.
TEST_CASE("Mixing C API rejects a NULL strip input without touching the output", "[mixing][capi]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 8, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  REQUIRE(sonare_mixer_add_strip(mixer, "a") != nullptr);
  REQUIRE(sonare_mixer_add_strip(mixer, "b") != nullptr);

  std::array<float, 4> in_l{0.5f, 0.5f, 0.5f, 0.5f};
  std::array<float, 4> in_r{0.5f, 0.5f, 0.5f, 0.5f};
  std::array<float, 4> out_l{};
  std::array<float, 4> out_r{};

  // Positive control: two valid strips mix and the output is written.
  const float* ok_l[] = {in_l.data(), in_l.data()};
  const float* ok_r[] = {in_r.data(), in_r.data()};
  REQUIRE(sonare_mixer_process_stereo(mixer, ok_l, ok_r, 2, out_l.data(), out_r.data(),
                                      out_l.size()) == SONARE_OK);
  REQUIRE(out_l[0] > 0.1f);

  // Second strip's left channel missing: rejected, and the caller's buffers
  // still hold what they held on entry.
  const std::array<float, 4> sentinel{-7.0f, -7.0f, -7.0f, -7.0f};
  out_l = sentinel;
  out_r = sentinel;
  const float* null_l[] = {in_l.data(), nullptr};
  REQUIRE(sonare_mixer_process_stereo(mixer, null_l, ok_r, 2, out_l.data(), out_r.data(),
                                      out_l.size()) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out_l == sentinel);
  REQUIRE(out_r == sentinel);

  // Same for the right channel.
  const float* null_r[] = {in_r.data(), nullptr};
  out_l = sentinel;
  out_r = sentinel;
  REQUIRE(sonare_mixer_process_stereo(mixer, ok_l, null_r, 2, out_l.data(), out_r.data(),
                                      out_l.size()) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(out_l == sentinel);
  REQUIRE(out_r == sentinel);

  // The mixer is still usable afterwards.
  REQUIRE(sonare_mixer_process_stereo(mixer, ok_l, ok_r, 2, out_l.data(), out_r.data(),
                                      out_l.size()) == SONARE_OK);
  REQUIRE(out_l[0] > 0.1f);
  sonare_mixer_destroy(mixer);
}

// ChannelStripConfig::enable_metering was announced as the escape hatch for the
// strips whose snapshots are never read, but nothing that builds a strip could
// set it. The configuration is fixed at construction, so it reaches a strip
// either through the scene document or through the _ex entry point.
TEST_CASE("Mixing C API builds strips with a configurable meter", "[mixing][capi]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 512, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  // enable_metering, measure_lufs, measure_true_peak, true_peak_oversample.
  SonareStrip* full = sonare_mixer_add_strip_ex(mixer, "full", 1, 1, 1, 0);
  SonareStrip* peak_only = sonare_mixer_add_strip_ex(mixer, "peakOnly", 1, 0, 0, 0);
  SonareStrip* silent = sonare_mixer_add_strip_ex(mixer, "silent", 0, 0, 0, 0);
  REQUIRE(full != nullptr);
  REQUIRE(peak_only != nullptr);
  REQUIRE(silent != nullptr);
  // An out-of-range factor is refused rather than silently resolved.
  REQUIRE(sonare_mixer_add_strip_ex(mixer, "bad", 1, 1, 1, 99) == nullptr);
  // In-range factors the realtime meter does not implement are refused too, not resolved to one.
  for (int factor : {1, 2, 3, 5, 16}) {
    INFO("factor " << factor);
    REQUIRE(sonare_mixer_add_strip_ex(mixer, "bad", 1, 1, 1, factor) == nullptr);
    REQUIRE(std::string(sonare_last_error_message()) ==
            "true_peak_oversample must be one of 0, 4, 8");
  }

  std::array<float, 512> block{};
  for (std::size_t i = 0; i < block.size(); ++i) {
    block[i] = 0.5f * std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(i) / 48000.0f);
  }
  const float* inputs[] = {block.data(), block.data(), block.data()};
  std::array<float, 512> out_l{};
  std::array<float, 512> out_r{};
  // Long enough to fill the momentary LUFS window (400 ms = 19200 frames at
  // 48 kHz), so the fully-metered control reports a real loudness rather than
  // the floor it would still be at after a few blocks.
  for (int pass = 0; pass < 48; ++pass) {
    REQUIRE(sonare_mixer_process_stereo(mixer, inputs, inputs, 3, out_l.data(), out_r.data(),
                                        block.size()) == SONARE_OK);
  }

  SonareMixMeterSnapshot full_snap{};
  SonareMixMeterSnapshot peak_snap{};
  SonareMixMeterSnapshot silent_snap{};
  REQUIRE(sonare_strip_meter(full, &full_snap) == SONARE_OK);
  REQUIRE(sonare_strip_meter(peak_only, &peak_snap) == SONARE_OK);
  REQUIRE(sonare_strip_meter(silent, &silent_snap) == SONARE_OK);

  // The fully-metered strip is the control: the same signal reaches all three.
  CHECK(full_snap.peak_db_l > -40.0f);
  CHECK(full_snap.momentary_lufs > -70.0f);
  CHECK(full_snap.true_peak_dbtp_l > -40.0f);

  // Peak / RMS keep working with the loudness and true-peak work switched off,
  // and those two fields stay at the floor.
  CHECK(peak_snap.peak_db_l > -40.0f);
  CHECK(peak_snap.momentary_lufs == sonare::constants::kFloorDb);
  CHECK(peak_snap.true_peak_dbtp_l == sonare::constants::kFloorDb);

  // A strip with metering off publishes no snapshot at all.
  CHECK(silent_snap.seq == 0);

  sonare_mixer_destroy(mixer);
}

namespace {

// One stereo strip at unity carrying a 997 Hz tone at -6 dBFS, so the master
// output is a known signal.
struct MasterMeterRig {
  static constexpr int kBlock = 512;
  SonareMixer* mixer = nullptr;
  std::array<float, kBlock> tone{};
  std::array<float, kBlock> out_l{};
  std::array<float, kBlock> out_r{};
  int64_t frame = 0;

  MasterMeterRig() {
    REQUIRE(sonare_mixer_create(48000, kBlock, &mixer) == SONARE_OK);
    REQUIRE(mixer != nullptr);
    REQUIRE(sonare_mixer_add_strip(mixer, "a") != nullptr);
  }
  ~MasterMeterRig() { sonare_mixer_destroy(mixer); }

  void process(float amplitude) {
    for (int i = 0; i < kBlock; ++i) {
      tone[static_cast<std::size_t>(i)] =
          amplitude * std::sin(kTwoPi * 997.0f * static_cast<float>(frame + i) / 48000.0f);
    }
    frame += kBlock;
    const float* inputs[] = {tone.data()};
    REQUIRE(sonare_mixer_process_stereo(mixer, inputs, inputs, 1, out_l.data(), out_r.data(),
                                        kBlock) == SONARE_OK);
  }
};

void require_same_meter(const SonareMixMeterSnapshot& a, const sonare::mixing::MeterSnapshot& b) {
  SonareMixMeterSnapshot expected{};
  sonare_c_mixing_detail::copy_meter_snapshot(b, &expected);
  CHECK(a.num_channels == expected.num_channels);
  for (int ch = 0; ch < 2; ++ch) {
    CHECK_THAT(a.peak_db[ch], WithinAbs(expected.peak_db[ch], 1e-4f));
    CHECK_THAT(a.rms_db[ch], WithinAbs(expected.rms_db[ch], 1e-4f));
    CHECK_THAT(a.true_peak_dbtp[ch], WithinAbs(expected.true_peak_dbtp[ch], 1e-4f));
  }
  CHECK_THAT(a.correlation, WithinAbs(expected.correlation, 1e-5f));
  CHECK_THAT(a.momentary_lufs, WithinAbs(expected.momentary_lufs, 1e-4f));
  CHECK_THAT(a.short_term_lufs, WithinAbs(expected.short_term_lufs, 1e-4f));
  CHECK_THAT(a.integrated_lufs, WithinAbs(expected.integrated_lufs, 1e-4f));
  CHECK_THAT(a.max_true_peak_dbtp, WithinAbs(expected.max_true_peak_dbtp, 1e-4f));
}

}  // namespace

TEST_CASE("sonare_mixer_master_meter_config_init sets every documented default", "[mixing][capi]") {
  SonareMixerMasterMeterConfig config;
  std::memset(&config, 0x7f, sizeof(config));
  sonare_mixer_master_meter_config_init(&config);
  CHECK(config.struct_size == sizeof(SonareMixerMasterMeterConfig));
  CHECK(config.enabled == 1);
  CHECK(config.measure_lufs == 1);
  CHECK(config.measure_true_peak == 1);
  CHECK(config.true_peak_oversample == 4);
  sonare_mixer_master_meter_config_init(nullptr);
}

TEST_CASE("The master meter is read only once enabled and refuses a config it cannot run",
          "[mixing][capi]") {
  MasterMeterRig rig;
  SonareMixMeterSnapshot snap{};
  CHECK(sonare_mixer_master_meter(rig.mixer, &snap) == SONARE_ERROR_INVALID_STATE);
  CHECK(sonare_mixer_master_meter(nullptr, &snap) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_mixer_master_meter(rig.mixer, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(sonare_mixer_configure_master_meter(nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);

  SonareMixerMasterMeterConfig config{};
  CHECK(sonare_mixer_configure_master_meter(rig.mixer, &config) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK_THAT(std::string(sonare_last_error_message()),
             Catch::Matchers::ContainsSubstring(
                 "config.struct_size must be set by sonare_mixer_master_meter_config_init"));
  sonare_mixer_master_meter_config_init(&config);
  config.struct_size = sizeof(config) - 1;
  CHECK(sonare_mixer_configure_master_meter(rig.mixer, &config) == SONARE_ERROR_INVALID_PARAMETER);

  // A 0 is a factor like any other, not a request for the default.
  for (int factor : {0, 1, 2, 16}) {
    INFO("factor " << factor);
    sonare_mixer_master_meter_config_init(&config);
    config.true_peak_oversample = factor;
    CHECK(sonare_mixer_configure_master_meter(rig.mixer, &config) ==
          SONARE_ERROR_INVALID_PARAMETER);
  }
  // Nothing was enabled by the refused calls.
  rig.process(0.5f);
  CHECK(sonare_mixer_master_meter(rig.mixer, &snap) == SONARE_ERROR_INVALID_STATE);
}

TEST_CASE("The master meter reads what a meter over the master output reads", "[mixing][capi]") {
  for (int factor : {4, 8}) {
    INFO("factor " << factor);
    MasterMeterRig rig;
    SonareMixerMasterMeterConfig config{};
    sonare_mixer_master_meter_config_init(&config);
    config.true_peak_oversample = factor;
    REQUIRE(sonare_mixer_configure_master_meter(rig.mixer, &config) == SONARE_OK);

    sonare::mixing::MeterConfig reference_config = sonare::mixing::kPublishedMeterConfig;
    reference_config.true_peak_oversample = factor;
    sonare::mixing::MeterProcessor reference(reference_config);
    reference.prepare(48000.0, MasterMeterRig::kBlock);
    // Past the momentary LUFS window, so loudness is a real reading.
    for (int pass = 0; pass < 48; ++pass) {
      rig.process(0.5f);
      float* planes[] = {rig.out_l.data(), rig.out_r.data()};
      reference.process(planes, 2, MasterMeterRig::kBlock);
    }
    SonareMixMeterSnapshot snap{};
    REQUIRE(sonare_mixer_master_meter(rig.mixer, &snap) == SONARE_OK);
    CHECK(snap.peak_db_l > -7.0f);
    CHECK(snap.momentary_lufs > -70.0f);
    require_same_meter(snap, reference.snapshot());

    REQUIRE(sonare_mixer_flush_meters(rig.mixer) == SONARE_OK);
    reference.flush_true_peak();
    REQUIRE(sonare_mixer_master_meter(rig.mixer, &snap) == SONARE_OK);
    require_same_meter(snap, reference.snapshot());
  }
}

TEST_CASE("Disabling the master meter freezes its reading and enabling resets it",
          "[mixing][capi]") {
  MasterMeterRig rig;
  REQUIRE(sonare_mixer_configure_master_meter(rig.mixer, nullptr) == SONARE_OK);
  rig.process(0.25f);
  SonareMixMeterSnapshot quiet{};
  REQUIRE(sonare_mixer_master_meter(rig.mixer, &quiet) == SONARE_OK);
  REQUIRE(quiet.seq > 0);

  SonareMixerMasterMeterConfig config{};
  sonare_mixer_master_meter_config_init(&config);
  config.enabled = 0;
  REQUIRE(sonare_mixer_configure_master_meter(rig.mixer, &config) == SONARE_OK);
  rig.process(1.0f);
  SonareMixMeterSnapshot frozen{};
  REQUIRE(sonare_mixer_master_meter(rig.mixer, &frozen) == SONARE_OK);
  CHECK(frozen.seq == quiet.seq);
  CHECK(frozen.peak_db_l == quiet.peak_db_l);

  // Enabling again starts a new reading: the loud block before it is not in it.
  REQUIRE(sonare_mixer_configure_master_meter(rig.mixer, nullptr) == SONARE_OK);
  rig.process(0.25f);
  SonareMixMeterSnapshot again{};
  REQUIRE(sonare_mixer_master_meter(rig.mixer, &again) == SONARE_OK);
  CHECK_THAT(again.peak_db_l, WithinAbs(quiet.peak_db_l, 0.5f));
  CHECK(again.max_true_peak_dbtp < -6.0f);
}

TEST_CASE("Master meter switches leave their measurements at the floor", "[mixing][capi]") {
  MasterMeterRig rig;
  SonareMixerMasterMeterConfig config{};
  sonare_mixer_master_meter_config_init(&config);
  config.measure_lufs = 0;
  config.measure_true_peak = 0;
  REQUIRE(sonare_mixer_configure_master_meter(rig.mixer, &config) == SONARE_OK);
  for (int pass = 0; pass < 48; ++pass) rig.process(0.5f);
  SonareMixMeterSnapshot snap{};
  REQUIRE(sonare_mixer_master_meter(rig.mixer, &snap) == SONARE_OK);
  CHECK(snap.peak_db_l > -7.0f);
  CHECK(snap.momentary_lufs == sonare::constants::kFloorDb);
  CHECK(snap.true_peak_dbtp_l == sonare::constants::kFloorDb);
}

TEST_CASE("Mixing C API exposes scene preset JSON", "[mixing][capi]") {
  const char* names = sonare_mixing_scene_preset_names();
  REQUIRE(names != nullptr);
  REQUIRE(std::string(names).find("vocalReverbSend") != std::string::npos);

  char* json = nullptr;
  REQUIRE(sonare_mixing_scene_preset_json("vocalReverbSend", &json) == SONARE_OK);
  REQUIRE(json != nullptr);
  REQUIRE(std::string(json).find("\"strips\"") != std::string::npos);
  sonare_free_string(json);
}

TEST_CASE("Mixing C API round-trips mixer scene JSON", "[mixing][capi]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 8, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "vocal");
  REQUIRE(strip != nullptr);
  REQUIRE(sonare_strip_set_input_trim_db(strip, 6.0f) == SONARE_OK);
  REQUIRE(sonare_strip_set_fader_db(strip, -3.0f) == SONARE_OK);
  REQUIRE(sonare_strip_set_pan(strip, 0.25f, SONARE_PAN_MODE_BALANCE) == SONARE_OK);
  REQUIRE(sonare_strip_set_width(strip, 1.2f) == SONARE_OK);
  size_t send_index = 999;
  REQUIRE(sonare_strip_add_send(strip, "vocal-to-verb", "verb", -12.0f,
                                SONARE_SEND_TIMING_POST_FADER, &send_index) == SONARE_OK);
  REQUIRE(send_index == 0);
  REQUIRE(sonare_strip_set_send_db(strip, send_index, -10.0f) == SONARE_OK);

  char* json = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &json) == SONARE_OK);
  REQUIRE(json != nullptr);
  const std::string scene_json(json);
  REQUIRE(scene_json.find("\"vocal\"") != std::string::npos);
  REQUIRE(scene_json.find("\"inputTrimDb\":6") != std::string::npos);
  REQUIRE(scene_json.find("\"vocal-to-verb\"") != std::string::npos);
  REQUIRE(scene_json.find("\"sendDb\":-10") != std::string::npos);
  sonare_free_string(json);
  sonare_mixer_destroy(mixer);

  SonareMixer* restored = sonare_mixer_from_scene_json(scene_json.c_str(), 48000, 8);
  REQUIRE(restored != nullptr);

  std::array<float, 4> input_l{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> input_r{0.0f, 0.0f, 0.0f, 0.0f};
  const float* inputs_l[] = {input_l.data()};
  const float* inputs_r[] = {input_r.data()};
  std::array<float, 4> out_l{};
  std::array<float, 4> out_r{};
  REQUIRE(sonare_mixer_process_stereo(restored, inputs_l, inputs_r, 1, out_l.data(), out_r.data(),
                                      out_l.size()) == SONARE_OK);
  // The restored strip applies +6 dB input trim and -3 dB fader (+3 dB net,
  // ~1.41x) before a slight-right balance pan. With the Balance pan law no longer
  // applying a spurious -3 dB center attenuation, the left output is legitimately
  // boosted above unity (~1.3), confirming the round-tripped trim/fader/pan.
  REQUIRE(out_l[0] > 1.0f);
  REQUIRE(out_l[0] < 2.0f);
  sonare_mixer_destroy(restored);
}

TEST_CASE("Mixing C API refuses pan and width outside their domains and keeps the strip",
          "[mixing][capi]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 8, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "vocal");
  REQUIRE(strip != nullptr);
  REQUIRE(sonare_strip_set_pan(strip, -1.0f, SONARE_PAN_MODE_BALANCE) == SONARE_OK);
  REQUIRE(sonare_strip_set_width(strip, 2.0f) == SONARE_OK);
  char* json = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &json) == SONARE_OK);
  const std::string before(json);
  sonare_free_string(json);

  REQUIRE(sonare_strip_set_pan(strip, 1.5f, SONARE_PAN_MODE_KEEP) ==
          SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_width(strip, 2.5f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_width(strip, -0.5f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_dual_pan(strip, -1.0f, 1.25f) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_mixer_to_scene_json(mixer, &json) == SONARE_OK);
  CHECK(std::string(json) == before);
  sonare_free_string(json);
  sonare_mixer_destroy(mixer);
}

TEST_CASE("Mixing C API removes a strip send", "[mixing][capi]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 8, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "vocal");
  REQUIRE(strip != nullptr);

  size_t to_verb = 999;
  size_t to_delay = 999;
  REQUIRE(sonare_strip_add_send(strip, "vocal-to-verb", "verb", -12.0f,
                                SONARE_SEND_TIMING_POST_FADER, &to_verb) == SONARE_OK);
  REQUIRE(sonare_strip_add_send(strip, "vocal-to-delay", "delay", -9.0f,
                                SONARE_SEND_TIMING_POST_FADER, &to_delay) == SONARE_OK);
  REQUIRE(to_verb == 0);
  REQUIRE(to_delay == 1);

  // Out-of-range removal is rejected.
  REQUIRE(sonare_strip_remove_send(strip, 5) == SONARE_ERROR_INVALID_PARAMETER);

  // Remove the first send; the second must shift down to index 0 and the dropped
  // send must disappear from both the live strip and the scene mirror.
  REQUIRE(sonare_strip_remove_send(strip, 0) == SONARE_OK);

  char* json = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &json) == SONARE_OK);
  REQUIRE(json != nullptr);
  const std::string scene_json(json);
  sonare_free_string(json);
  REQUIRE(scene_json.find("\"vocal-to-verb\"") == std::string::npos);
  REQUIRE(scene_json.find("\"vocal-to-delay\"") != std::string::npos);

  // The surviving send is now addressable at index 0 (the shifted position);
  // index 1 is out of range after the removal.
  REQUIRE(sonare_strip_set_send_db(strip, 0, -6.0f) == SONARE_OK);
  REQUIRE(sonare_strip_set_send_db(strip, 1, -6.0f) == SONARE_ERROR_INVALID_PARAMETER);
  sonare_mixer_destroy(mixer);
}

TEST_CASE("Mixing C API preserves dual pan in scene JSON", "[mixing][capi]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 8, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "dual");
  REQUIRE(strip != nullptr);
  REQUIRE(sonare_strip_set_dual_pan(strip, 1.0f, -0.5f) == SONARE_OK);

  char* json = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &json) == SONARE_OK);
  REQUIRE(json != nullptr);
  const std::string scene_json(json);
  REQUIRE(scene_json.find("\"panMode\":\"dualPan\"") != std::string::npos);
  REQUIRE(scene_json.find("\"dualPanLeft\":1") != std::string::npos);
  REQUIRE(scene_json.find("\"dualPanRight\":-0.5") != std::string::npos);
  // The exported nominal pan must reflect the computed 0.5*(L+R) = 0.25 that
  // set_dual_pan cached, not the live ChannelStrip pan_ (which set_dual_pan
  // never updates and would export as the stale default 0).
  REQUIRE(scene_json.find("\"pan\":0.25") != std::string::npos);
  sonare_free_string(json);
  sonare_mixer_destroy(mixer);

  SonareMixer* restored = sonare_mixer_from_scene_json(scene_json.c_str(), 48000, 8);
  REQUIRE(restored != nullptr);
  char* restored_json = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(restored, &restored_json) == SONARE_OK);
  REQUIRE(restored_json != nullptr);
  const std::string restored_scene_json(restored_json);
  REQUIRE(restored_scene_json.find("\"panMode\":\"dualPan\"") != std::string::npos);
  REQUIRE(restored_scene_json.find("\"dualPanLeft\":1") != std::string::npos);
  REQUIRE(restored_scene_json.find("\"dualPanRight\":-0.5") != std::string::npos);
  REQUIRE(restored_scene_json.find("\"pan\":0.25") != std::string::npos);
  sonare_free_string(restored_json);
  sonare_mixer_destroy(restored);
}

TEST_CASE("Mixing C API preserves surround pan in scene JSON", "[mixing][capi]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 8, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "surround");
  REQUIRE(strip != nullptr);

  REQUIRE(sonare_strip_set_surround_pan(nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  SonareSurroundPan pan{};
  pan.azimuth = -45.0f;
  pan.divergence = 0.25f;
  pan.lfe = 0.5f;
  REQUIRE(sonare_strip_set_surround_pan(strip, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_surround_pan(strip, &pan) == SONARE_OK);
  const auto live_pan = strip->strip.surround_pan_params();
  REQUIRE(live_pan.azimuth == -45.0f);
  REQUIRE(live_pan.divergence == 0.25f);
  REQUIRE(live_pan.lfe == 0.5f);

  char* json = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &json) == SONARE_OK);
  REQUIRE(json != nullptr);
  const std::string scene_json(json);
  REQUIRE(scene_json.find("\"surroundPan\"") != std::string::npos);
  REQUIRE(scene_json.find("\"azimuth\":-45") != std::string::npos);
  REQUIRE(scene_json.find("\"lfe\":0.5") != std::string::npos);
  sonare_free_string(json);
  sonare_mixer_destroy(mixer);

  SonareMixer* restored = sonare_mixer_from_scene_json(scene_json.c_str(), 48000, 8);
  REQUIRE(restored != nullptr);
  REQUIRE(restored->strips.size() == 1);
  const auto restored_live_pan = restored->strips[0]->strip.surround_pan_params();
  REQUIRE(restored_live_pan.azimuth == -45.0f);
  REQUIRE(restored_live_pan.divergence == 0.25f);
  char* restored_json = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(restored, &restored_json) == SONARE_OK);
  REQUIRE(restored_json != nullptr);
  const std::string restored_scene_json(restored_json);
  REQUIRE(restored_scene_json.find("\"azimuth\":-45") != std::string::npos);
  REQUIRE(restored_scene_json.find("\"divergence\":0.25") != std::string::npos);
  sonare_free_string(restored_json);
  sonare_mixer_destroy(restored);
}

TEST_CASE("Mixing C API surround pan refuses an out-of-range field and keeps the previous pan",
          "[mixing][capi]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 8, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "surround");
  REQUIRE(strip != nullptr);

  SonareSurroundPan good{};
  good.azimuth = -45.0f;
  good.divergence = 0.25f;
  good.lfe = 0.5f;
  REQUIRE(sonare_strip_set_surround_pan(strip, &good) == SONARE_OK);

  const auto refused = [&](SonareSurroundPan bad) {
    REQUIRE(sonare_strip_set_surround_pan(strip, &bad) == SONARE_ERROR_INVALID_PARAMETER);
    const auto live = strip->strip.surround_pan_params();
    REQUIRE(live.azimuth == -45.0f);
    REQUIRE(live.divergence == 0.25f);
    REQUIRE(live.lfe == 0.5f);
    REQUIRE(strip->scene_strip.surround_pan.azimuth == -45.0f);
    REQUIRE(strip->scene_strip.surround_pan.divergence == 0.25f);
    REQUIRE(strip->scene_strip.surround_pan.lfe == 0.5f);
  };
  SonareSurroundPan bad = good;
  bad.azimuth = 181.0f;
  refused(bad);
  bad = good;
  bad.azimuth = -180.5f;
  refused(bad);
  bad = good;
  bad.divergence = 1.5f;
  refused(bad);
  bad = good;
  bad.divergence = -0.1f;
  refused(bad);
  bad = good;
  bad.lfe = 2.0f;
  refused(bad);

  // The domain edges are accepted.
  SonareSurroundPan edge = good;
  edge.azimuth = 180.0f;
  edge.divergence = 1.0f;
  edge.lfe = 0.0f;
  REQUIRE(sonare_strip_set_surround_pan(strip, &edge) == SONARE_OK);
  sonare_mixer_destroy(mixer);
}

TEST_CASE("Mixing C API preserves pan mode set via set_pan in scene JSON", "[mixing][capi]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 8, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "panned");
  REQUIRE(strip != nullptr);
  // sonare_strip_set_pan must mirror the pan mode into the scene strip so it
  // survives scene serialization (regression: previously only the pan position
  // was written, leaving panMode at the default balance).
  REQUIRE(sonare_strip_set_pan(strip, 0.25f, SONARE_PAN_MODE_STEREO_PAN) == SONARE_OK);

  char* json = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &json) == SONARE_OK);
  REQUIRE(json != nullptr);
  const std::string scene_json(json);
  REQUIRE(scene_json.find("\"panMode\":\"stereoPan\"") != std::string::npos);
  sonare_free_string(json);

  // Round-trips through from_scene_json with the mode intact.
  SonareMixer* restored = sonare_mixer_from_scene_json(scene_json.c_str(), 48000, 8);
  REQUIRE(restored != nullptr);
  char* restored_json = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(restored, &restored_json) == SONARE_OK);
  REQUIRE(restored_json != nullptr);
  REQUIRE(std::string(restored_json).find("\"panMode\":\"stereoPan\"") != std::string::npos);
  sonare_free_string(restored_json);
  sonare_mixer_destroy(restored);

  // Invalid pan modes are rejected without mutating state.
  REQUIRE(sonare_strip_set_pan(strip, 0.0f, 99) == SONARE_ERROR_INVALID_PARAMETER);
  sonare_mixer_destroy(mixer);
}

TEST_CASE("Mixing C API set_pan keep-mode sentinel preserves current pan mode", "[mixing][capi]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 8, &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "panned");
  REQUIRE(strip != nullptr);

  // Establish a non-default (StereoPan) mode.
  REQUIRE(sonare_strip_set_pan(strip, 0.25f, SONARE_PAN_MODE_STEREO_PAN) == SONARE_OK);

  // Nudging the pan with SONARE_PAN_MODE_KEEP (< 0) must NOT reset the mode back
  // to Balance — only the pan position moves.
  REQUIRE(sonare_strip_set_pan(strip, -0.5f, SONARE_PAN_MODE_KEEP) == SONARE_OK);

  char* json = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &json) == SONARE_OK);
  REQUIRE(json != nullptr);
  const std::string scene_json(json);
  // Mode stays stereoPan, not reset to balance.
  REQUIRE(scene_json.find("\"panMode\":\"stereoPan\"") != std::string::npos);
  sonare_free_string(json);

  sonare_mixer_destroy(mixer);
}

TEST_CASE("Mixing C API reports invalid scene JSON through last error", "[mixing][capi]") {
  SonareMixer* mixer = sonare_mixer_from_scene_json("{\"strips\":[", 48000, 8);
  REQUIRE(mixer == nullptr);
  REQUIRE(std::string(sonare_last_error_message()).find("expected") != std::string::npos);
}

TEST_CASE("Mixing C API one-shot true peak reads the signal's last samples once flushed",
          "[mixing][capi]") {
  // A one-shot mix is a single block. The meter reads one group delay behind it, so a peak in
  // the final samples is only measured once the caller ends the signal.
  constexpr size_t kLength = 256;
  std::array<float, kLength> left{};
  std::array<float, kLength> right{};
  left[kLength - 3] = 0.9f;
  left[kLength - 2] = 0.9f;
  left[kLength - 1] = -0.9f;
  float offline_db = 0.0f;
  REQUIRE(sonare_metering_true_peak_dbtp(left.data(), left.size(), 48000, 4, &offline_db) ==
          SONARE_OK);

  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, static_cast<int>(kLength), &mixer) == SONARE_OK);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "a");
  REQUIRE(strip != nullptr);
  REQUIRE(sonare_strip_set_pan(strip, 0.0f, SONARE_PAN_MODE_BALANCE) == SONARE_OK);
  const float* inputs_l[] = {left.data()};
  const float* inputs_r[] = {right.data()};
  std::array<float, kLength> out_l{};
  std::array<float, kLength> out_r{};
  REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, inputs_r, 1, out_l.data(), out_r.data(),
                                      kLength) == SONARE_OK);

  SonareMixMeterSnapshot before{};
  REQUIRE(sonare_strip_meter(strip, &before) == SONARE_OK);
  REQUIRE(before.true_peak_dbtp_l < offline_db - 0.01f);
  SonareMixMeterSnapshot master_before{};
  REQUIRE(sonare_mixer_bus_meter(mixer, "master", &master_before) == SONARE_OK);
  REQUIRE(master_before.true_peak_dbtp_l < offline_db - 0.01f);

  REQUIRE(sonare_mixer_flush_meters(mixer) == SONARE_OK);
  SonareMixMeterSnapshot after{};
  REQUIRE(sonare_strip_meter(strip, &after) == SONARE_OK);
  REQUIRE_THAT(after.true_peak_dbtp_l, WithinAbs(offline_db, 1.0e-4));
  REQUIRE_THAT(after.max_true_peak_dbtp, WithinAbs(offline_db, 1.0e-4));
  // The master bus meter publishes the same quantity and is flushed with the strips.
  SonareMixMeterSnapshot master_after{};
  REQUIRE(sonare_mixer_bus_meter(mixer, "master", &master_after) == SONARE_OK);
  REQUIRE_THAT(master_after.true_peak_dbtp_l, WithinAbs(offline_db, 1.0e-4));
  REQUIRE_THAT(master_after.max_true_peak_dbtp, WithinAbs(offline_db, 1.0e-4));

  REQUIRE(sonare_mixer_flush_meters(nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  sonare_mixer_destroy(mixer);
}

TEST_CASE("Mixing C API bus meters read an inter-sample over above the sample peak",
          "[mixing][capi][meter]") {
  // A quarter-rate sine sampled 45 degrees off its crest: every sample sits at 0.5 / sqrt(2),
  // while the waveform between them reaches 0.5, about +3 dB over the sample peak.
  for (const int sample_rate : {44100, 48000}) {
    CAPTURE(sample_rate);
    constexpr size_t kLength = 1024;
    std::array<float, kLength> left{};
    std::array<float, kLength> right{};
    for (size_t i = 0; i < kLength; ++i) {
      left[i] = 0.5f * std::sin(sonare::constants::kHalfPi * static_cast<float>(i) +
                                0.5f * sonare::constants::kHalfPi);
    }
    SonareMixer* mixer = nullptr;
    REQUIRE(sonare_mixer_create(sample_rate, static_cast<int>(kLength), &mixer) == SONARE_OK);
    REQUIRE(mixer != nullptr);
    SonareStrip* strip = sonare_mixer_add_strip(mixer, "a");
    REQUIRE(strip != nullptr);
    REQUIRE(sonare_strip_set_pan(strip, 0.0f, SONARE_PAN_MODE_BALANCE) == SONARE_OK);
    const float* inputs_l[] = {left.data()};
    const float* inputs_r[] = {right.data()};
    std::array<float, kLength> out_l{};
    std::array<float, kLength> out_r{};
    REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, inputs_r, 1, out_l.data(), out_r.data(),
                                        kLength) == SONARE_OK);
    REQUIRE(sonare_mixer_flush_meters(mixer) == SONARE_OK);

    SonareMixMeterSnapshot master{};
    REQUIRE(sonare_mixer_bus_meter(mixer, "master", &master) == SONARE_OK);
    INFO("peak " << master.peak_db_l << " dB, true peak " << master.true_peak_dbtp_l << " dB");
    REQUIRE(master.peak_db_l > -10.0f);
    REQUIRE(master.true_peak_dbtp_l >= master.peak_db_l);
    REQUIRE(master.max_true_peak_dbtp >= master.peak_db_l);
    // The reconstruction must actually find the over, not merely tie the sample peak.
    REQUIRE(master.true_peak_dbtp_l > master.peak_db_l + 2.0f);
    sonare_mixer_destroy(mixer);
  }
}

TEST_CASE("Bus meter refusals name the refused argument", "[mixing][capi]") {
  SonareMixer* mixer = nullptr;
  REQUIRE(sonare_mixer_create(48000, 64, &mixer) == SONARE_OK);
  REQUIRE(sonare_mixer_add_strip(mixer, "a") != nullptr);
  SonareMixMeterSnapshot snapshot{};

  CHECK(sonare_mixer_bus_meter(nullptr, "master", &snapshot) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "mixer must not be NULL");
  CHECK(sonare_mixer_bus_meter(mixer, nullptr, &snapshot) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "bus_id must not be NULL");
  CHECK(sonare_mixer_bus_meter(mixer, "", &snapshot) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "bus_id must not be empty");
  CHECK(sonare_mixer_bus_meter(mixer, "master", nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "out must not be NULL");
  // Two NULL arguments name the first one in the signature.
  CHECK(sonare_mixer_bus_meter(nullptr, nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "mixer must not be NULL");

  std::array<float, 64> silence{};
  std::array<float, 64> out_l{};
  std::array<float, 64> out_r{};
  const float* inputs_l[] = {silence.data()};
  const float* inputs_r[] = {silence.data()};
  REQUIRE(sonare_mixer_process_stereo(mixer, inputs_l, inputs_r, 1, out_l.data(), out_r.data(),
                                      silence.size()) == SONARE_OK);
  CHECK(sonare_mixer_bus_meter(mixer, "missing", &snapshot) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "bus_id must be the id of a bus in this mixer");
  // A strip id names a node that is not a bus.
  CHECK(sonare_mixer_bus_meter(mixer, "a", &snapshot) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(std::string(sonare_last_error_message()) == "bus_id must be the id of a bus in this mixer");
  sonare_mixer_destroy(mixer);
}

TEST_CASE("Mixing assistant suggest refusals name the refused argument", "[mixing][capi]") {
  const std::array<float, 8> samples{};
  const float* left[] = {samples.data()};
  const char* ids[] = {"t"};
  const size_t lengths[] = {samples.size()};
  char* json = nullptr;
  const auto refusal = [] { return std::string(sonare_last_error_message()); };

  CHECK(sonare_mixing_assistant_suggest(nullptr, nullptr, ids, nullptr, lengths, nullptr, 1, 48000,
                                        nullptr, 0, &json) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(refusal() == "input_left must not be NULL when input_count is not 0");
  CHECK(sonare_mixing_assistant_suggest(left, nullptr, nullptr, nullptr, lengths, nullptr, 1, 48000,
                                        nullptr, 0, &json) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(refusal() == "track_ids must not be NULL when input_count is not 0");
  CHECK(sonare_mixing_assistant_suggest(left, nullptr, ids, nullptr, nullptr, nullptr, 1, 48000,
                                        nullptr, 0, &json) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(refusal() == "track_lengths must not be NULL when input_count is not 0");

  const float* null_left[] = {nullptr};
  CHECK(sonare_mixing_assistant_suggest(null_left, nullptr, ids, nullptr, lengths, nullptr, 1,
                                        48000, nullptr, 0,
                                        &json) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(refusal() == "input_left[0] must not be NULL when track_lengths[0] is not 0");
  const char* null_ids[] = {nullptr};
  CHECK(sonare_mixing_assistant_suggest(left, nullptr, null_ids, nullptr, lengths, nullptr, 1,
                                        48000, nullptr, 0,
                                        &json) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(refusal() == "track_ids[0] must not be NULL");
  const int bad_rates[] = {0};
  CHECK(sonare_mixing_assistant_suggest(left, nullptr, ids, nullptr, lengths, bad_rates, 1, 48000,
                                        nullptr, 0, &json) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(refusal() == "track_sample_rates[0] must be an integer in [1, 2147483647]");
  CHECK(sonare_mixing_assistant_suggest(left, nullptr, ids, nullptr, lengths, nullptr, 1, 0,
                                        nullptr, 0, &json) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(refusal() == "sample_rate must be an integer in [1, 2147483647]");
  CHECK(sonare_mixing_assistant_suggest(left, nullptr, ids, nullptr, lengths, nullptr, 1, 48000,
                                        nullptr, 1, &json) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(refusal() == "params must not be NULL when param_count is not 0");
  CHECK(sonare_mixing_assistant_suggest(left, nullptr, ids, nullptr, lengths, nullptr, 1, 48000,
                                        nullptr, 0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(refusal() == "json_out must not be NULL");
  // A bad argument is named before the NULL out.
  CHECK(sonare_mixing_assistant_suggest(left, nullptr, ids, nullptr, lengths, nullptr, 1, 0,
                                        nullptr, 0, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(refusal() == "sample_rate must be an integer in [1, 2147483647]");
}
