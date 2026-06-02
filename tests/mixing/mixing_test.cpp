#include "mixing/mixing.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "mastering/dynamics/compressor.h"
#include "mastering/dynamics/sidechain_router.h"
#include "mastering/eq/equalizer.h"
#include "mastering/eq/linear_phase.h"
#include "mastering/eq/parametric.h"
#include "mastering/maximizer/maximizer.h"
#include "metering/lufs.h"
#include "mixing/meter.h"
#include "sonare_c.h"
#include "util/constants.h"
#include "util/db.h"
#include "util/exception.h"

using Catch::Matchers::WithinAbs;
using sonare::constants::kTwoPi;

namespace {

float energy(float left, float right) { return left * left + right * right; }

float rms_tail(const std::vector<float>& samples, size_t skip) {
  double sum = 0.0;
  size_t count = 0;
  for (size_t i = std::min(skip, samples.size()); i < samples.size(); ++i) {
    sum += static_cast<double>(samples[i]) * samples[i];
    ++count;
  }
  return count == 0 ? 0.0f : static_cast<float>(std::sqrt(sum / static_cast<double>(count)));
}

double lagrange3_magnitude_db(double fractional_delay, double normalized_to_nyquist) {
  const double mu = fractional_delay;
  const std::array<double, 4> coeffs{
      -mu * (mu - 1.0) * (mu - 2.0) / 6.0,
      (mu + 1.0) * (mu - 1.0) * (mu - 2.0) / 2.0,
      -(mu + 1.0) * mu * (mu - 2.0) / 2.0,
      (mu + 1.0) * mu * (mu - 1.0) / 6.0,
  };
  const double omega = normalized_to_nyquist * sonare::constants::kPiD;
  std::complex<double> response{0.0, 0.0};
  for (size_t tap = 0; tap < coeffs.size(); ++tap) {
    response += coeffs[tap] *
                std::exp(std::complex<double>{0.0, -omega * (static_cast<double>(tap) - 1.0)});
  }
  return 20.0 * std::log10(std::abs(response));
}

}  // namespace

class TestLatencyProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit TestLatencyProcessor(int latency) : latency_(latency) {}
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  int latency_samples() const noexcept override { return latency_; }

 private:
  int latency_ = 0;
};

class TestQ8LatencyProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit TestQ8LatencyProcessor(int latency_q8) : latency_q8_(latency_q8) {}
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  int latency_samples_q8() const noexcept override { return latency_q8_; }

 private:
  int latency_q8_ = 0;
};

TEST_CASE("MeterProcessor 44.1kHz LUFS matches offline BS.1770 path", "[mixing]") {
  constexpr int kSampleRate = 44100;
  constexpr int kFrames = kSampleRate * 3;
  constexpr int kBlock = 256;

  std::vector<float> left(kFrames);
  std::vector<float> right(kFrames);
  std::vector<float> interleaved(static_cast<size_t>(kFrames) * 2);
  for (int i = 0; i < kFrames; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
    left[i] = 0.25f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 440.0f * t);
    right[i] = 0.20f * std::sin(2.0f * static_cast<float>(sonare::constants::kPiD) * 660.0f * t);
    interleaved[static_cast<size_t>(2 * i)] = left[i];
    interleaved[static_cast<size_t>(2 * i + 1)] = right[i];
  }

  sonare::mixing::MeterProcessor meter;
  meter.prepare(kSampleRate, kBlock);
  for (int offset = 0; offset < kFrames; offset += kBlock) {
    const int frames = std::min(kBlock, kFrames - offset);
    float* channels[] = {left.data() + offset, right.data() + offset};
    meter.process(channels, 2, frames);
  }

  const auto streaming = meter.snapshot();
  const auto offline =
      sonare::metering::lufs_interleaved(interleaved.data(), kFrames, 2, kSampleRate);
  REQUIRE_THAT(streaming.integrated_lufs, WithinAbs(offline.integrated_lufs, 0.2f));
}

class ScaleProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit ScaleProcessor(float scale) : scale_(scale) {}
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] == nullptr) {
        continue;
      }
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] *= scale_;
      }
    }
  }
  void reset() override {}

  // Param 0 = linear scale, so insert automation can drive this processor.
  bool set_parameter(unsigned int param_id, float value) override {
    if (param_id == 0) {
      scale_ = value;
      return true;
    }
    return false;
  }

 private:
  float scale_ = 1.0f;
};

class AddProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit AddProcessor(float offset) : offset_(offset) {}
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] == nullptr) {
        continue;
      }
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] += offset_;
      }
    }
  }
  void reset() override {}

 private:
  float offset_ = 0.0f;
};

// Reports a gain reduction proportional to the loudest sample seen in the most
// recent process() call (GR = -10 * max|sample|). Used to verify the segmented
// process path aggregates the block-representative (most-negative) GR across
// segments rather than only reflecting the final segment's value.
class PeakGainReductionProcessor final : public sonare::rt::ProcessorBase {
 public:
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    float peak = 0.0f;
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] == nullptr) {
        continue;
      }
      for (int i = 0; i < num_samples; ++i) {
        peak = std::max(peak, std::abs(channels[ch][i]));
      }
    }
    last_gr_db_ = -10.0f * peak;
  }
  void reset() override { last_gr_db_ = 0.0f; }
  float last_gain_reduction_db() const override { return last_gr_db_; }

 private:
  float last_gr_db_ = 0.0f;
};

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
  sonare::mixing::PannerProcessor panner(
      {0.0f, sonare::mixing::PanLaw::Linear0dB, 5.0f, sonare::mixing::PanMode::DualPan});
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

TEST_CASE("Mixing C API processes stereo strips and exposes meters", "[mixing][capi]") {
  SonareMixer* mixer = sonare_mixer_create(48000, 8);
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

  sonare_mixer_destroy(mixer);
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
  SonareMixer* mixer = sonare_mixer_create(48000, 8);
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

TEST_CASE("Mixing C API preserves dual pan in scene JSON", "[mixing][capi]") {
  SonareMixer* mixer = sonare_mixer_create(48000, 8);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "dual");
  REQUIRE(strip != nullptr);
  REQUIRE(sonare_strip_set_dual_pan(strip, 1.0f, -0.5f) == SONARE_OK);

  char* json = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &json) == SONARE_OK);
  REQUIRE(json != nullptr);
  const std::string scene_json(json);
  REQUIRE(scene_json.find("\"panMode\":2") != std::string::npos);
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
  REQUIRE(restored_scene_json.find("\"panMode\":2") != std::string::npos);
  REQUIRE(restored_scene_json.find("\"dualPanLeft\":1") != std::string::npos);
  REQUIRE(restored_scene_json.find("\"dualPanRight\":-0.5") != std::string::npos);
  REQUIRE(restored_scene_json.find("\"pan\":0.25") != std::string::npos);
  sonare_free_string(restored_json);
  sonare_mixer_destroy(restored);
}

TEST_CASE("Mixing C API preserves pan mode set via set_pan in scene JSON", "[mixing][capi]") {
  SonareMixer* mixer = sonare_mixer_create(48000, 8);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "panned");
  REQUIRE(strip != nullptr);
  // sonare_strip_set_pan must mirror the pan mode into the scene strip so it
  // survives scene serialization (regression: previously only the pan position
  // was written, leaving panMode at the default 0).
  REQUIRE(sonare_strip_set_pan(strip, 0.25f, SONARE_PAN_MODE_STEREO_PAN) == SONARE_OK);

  char* json = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(mixer, &json) == SONARE_OK);
  REQUIRE(json != nullptr);
  const std::string scene_json(json);
  REQUIRE(scene_json.find("\"panMode\":1") != std::string::npos);
  sonare_free_string(json);

  // Round-trips through from_scene_json with the mode intact.
  SonareMixer* restored = sonare_mixer_from_scene_json(scene_json.c_str(), 48000, 8);
  REQUIRE(restored != nullptr);
  char* restored_json = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(restored, &restored_json) == SONARE_OK);
  REQUIRE(restored_json != nullptr);
  REQUIRE(std::string(restored_json).find("\"panMode\":1") != std::string::npos);
  sonare_free_string(restored_json);
  sonare_mixer_destroy(restored);

  // Invalid pan modes are rejected without mutating state.
  REQUIRE(sonare_strip_set_pan(strip, 0.0f, 99) == SONARE_ERROR_INVALID_PARAMETER);
  sonare_mixer_destroy(mixer);
}

TEST_CASE("Mixing C API reports invalid scene JSON through last error", "[mixing][capi]") {
  SonareMixer* mixer = sonare_mixer_from_scene_json("{\"strips\":[", 48000, 8);
  REQUIRE(mixer == nullptr);
  REQUIRE(std::string(sonare_last_error_message()).find("expected") != std::string::npos);
}

TEST_CASE("AutomationLane enforces bounded monotonic SPSC events", "[mixing]") {
  sonare::mixing::AutomationLane lane(2);
  sonare::mixing::AutomationEvent first;
  first.sample_pos = 10;
  first.value = 0.25f;
  first.target = {sonare::mixing::AutomationTargetKind::Fader, 1, 0, 0};
  sonare::mixing::AutomationEvent second = first;
  second.sample_pos = 10;
  second.value = 0.5f;
  second.curve = sonare::mixing::AutomationCurveType::Exponential;
  sonare::mixing::AutomationEvent third = first;
  third.sample_pos = 12;
  third.value = 0.75f;

  REQUIRE(lane.capacity() == 2);
  REQUIRE(lane.push(first));
  REQUIRE(lane.push(second));
  REQUIRE_FALSE(lane.push(third));

  std::vector<sonare::mixing::AutomationBlockEvent> consumed;
  const size_t count =
      lane.consume_block(8, 4, [&](const auto& event) { consumed.push_back(event); });

  REQUIRE(count == 2);
  REQUIRE(consumed.size() == 2);
  REQUIRE(consumed[0].offset == 2);
  REQUIRE(consumed[1].offset == 2);
  REQUIRE_THAT(consumed[0].event.value, WithinAbs(0.25f, 0.0001f));
  REQUIRE_THAT(consumed[1].event.value, WithinAbs(0.5f, 0.0001f));
  REQUIRE(consumed[1].event.curve == sonare::mixing::AutomationCurveType::Exponential);
  REQUIRE(lane.empty());

  REQUIRE(lane.push(third));
  sonare::mixing::AutomationEvent stale = first;
  stale.sample_pos = 11;
  REQUIRE_FALSE(lane.push(stale));
}

TEST_CASE("AutomationLane consumes only the requested block and drops stale events", "[mixing]") {
  sonare::mixing::AutomationLane lane(4);
  for (int64_t sample : {4, 8, 16}) {
    sonare::mixing::AutomationEvent event;
    event.sample_pos = sample;
    event.value = static_cast<float>(sample);
    REQUIRE(lane.push(event));
  }

  std::vector<sonare::mixing::AutomationBlockEvent> first_block;
  REQUIRE(lane.consume_block(8, 4, [&](const auto& event) { first_block.push_back(event); }) == 4);
  REQUIRE(first_block.size() == 4);
  REQUIRE(first_block[0].offset == 0);
  REQUIRE_THAT(first_block[0].event.value, WithinAbs(8.0f, 0.0001f));
  REQUIRE(first_block[3].offset == 3);
  REQUIRE_THAT(first_block[3].event.value, WithinAbs(11.0f, 0.0001f));

  std::vector<sonare::mixing::AutomationBlockEvent> second_block;
  REQUIRE(lane.consume_block(12, 4, [&](const auto& event) { second_block.push_back(event); }) ==
          4);
  REQUIRE(second_block.size() == 4);
  REQUIRE_FALSE(lane.empty());

  REQUIRE(lane.consume_block(16, 1, [&](const auto& event) { second_block.push_back(event); }) ==
          1);
  REQUIRE(second_block.size() == 5);
  REQUIRE(second_block.back().offset == 0);

  lane.clear();
  REQUIRE(lane.empty());
  sonare::mixing::AutomationEvent reset_event;
  reset_event.sample_pos = 1;
  REQUIRE(lane.push(reset_event));
}

TEST_CASE("AutomationLane emits exponential curve events between positive breakpoints",
          "[mixing]") {
  sonare::mixing::AutomationLane lane(8);
  sonare::mixing::AutomationEvent first;
  first.sample_pos = 10;
  first.value = 1.0f;
  first.curve = sonare::mixing::AutomationCurveType::Exponential;
  first.target = {sonare::mixing::AutomationTargetKind::Width, 1, 0, 0};
  sonare::mixing::AutomationEvent second = first;
  second.sample_pos = 14;
  second.value = 4.0f;

  REQUIRE(lane.push(first));
  REQUIRE(lane.push(second));

  std::vector<sonare::mixing::AutomationBlockEvent> consumed;
  const size_t count =
      lane.consume_block(8, 8, [&](const auto& event) { consumed.push_back(event); });

  REQUIRE(count == 5);
  REQUIRE(consumed.size() == 5);
  REQUIRE(consumed[0].offset == 2);
  REQUIRE(consumed[1].offset == 3);
  REQUIRE(consumed[2].offset == 4);
  REQUIRE(consumed[3].offset == 5);
  REQUIRE(consumed[4].offset == 6);
  REQUIRE_THAT(consumed[2].event.value, WithinAbs(2.0f, 0.0001f));
}

TEST_CASE("AutomationLane supports linear hold and s-curve interpolation", "[mixing]") {
  using sonare::mixing::AutomationBlockEvent;
  using sonare::mixing::AutomationCurveType;
  using sonare::mixing::AutomationEvent;
  using sonare::mixing::AutomationLane;
  using sonare::mixing::AutomationTargetKind;

  auto collect_curve = [](AutomationCurveType curve) {
    AutomationLane lane(8);
    AutomationEvent first;
    first.sample_pos = 0;
    first.value = 0.0f;
    first.curve = curve;
    first.target = {AutomationTargetKind::Fader, 1, 0, 0};
    AutomationEvent second = first;
    second.sample_pos = 4;
    second.value = 1.0f;
    REQUIRE(lane.push(first));
    REQUIRE(lane.push(second));
    std::vector<AutomationBlockEvent> consumed;
    lane.consume_block(0, 5, [&](const auto& event) { consumed.push_back(event); });
    return consumed;
  };

  const auto linear = collect_curve(AutomationCurveType::Linear);
  REQUIRE(linear.size() == 5);
  REQUIRE_THAT(linear[2].event.value, WithinAbs(0.5f, 0.0001f));

  const auto hold = collect_curve(AutomationCurveType::Hold);
  REQUIRE(hold.size() == 2);
  REQUIRE_THAT(hold[0].event.value, WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(hold[1].event.value, WithinAbs(1.0f, 0.0001f));

  const auto s_curve = collect_curve(AutomationCurveType::SCurve);
  REQUIRE(s_curve.size() == 5);
  REQUIRE_THAT(s_curve[1].event.value, WithinAbs(0.15625f, 0.0001f));
  REQUIRE_THAT(s_curve[2].event.value, WithinAbs(0.5f, 0.0001f));
  REQUIRE_THAT(s_curve[3].event.value, WithinAbs(0.84375f, 0.0001f));
}

TEST_CASE("AutomationLane sees a producer push before peeking the next curve event", "[mixing]") {
  sonare::mixing::AutomationLane lane(4);
  sonare::mixing::AutomationEvent first;
  first.sample_pos = 0;
  first.value = 1.0f;
  first.curve = sonare::mixing::AutomationCurveType::Exponential;
  first.target = {sonare::mixing::AutomationTargetKind::Fader, 1, 0, 0};
  sonare::mixing::AutomationEvent second = first;
  second.sample_pos = 8;
  second.value = 16.0f;

  REQUIRE(lane.push(first));

  bool pushed_second = false;
  std::vector<sonare::mixing::AutomationBlockEvent> consumed;
  const size_t count = lane.consume_block(0, 8, [&](const auto& event) {
    consumed.push_back(event);
    if (!pushed_second && event.event.sample_pos == 0) {
      pushed_second = true;
      REQUIRE(lane.push(second));
    }
  });

  REQUIRE(pushed_second);
  REQUIRE(count == 8);
  REQUIRE(consumed.size() == 8);
  REQUIRE(consumed.front().offset == 0);
  REQUIRE(consumed.back().offset == 7);
  REQUIRE_THAT(consumed[4].event.value, WithinAbs(4.0f, 0.0001f));
}

TEST_CASE("AutomationLane continues exponential curves across blocks", "[mixing]") {
  sonare::mixing::AutomationLane lane(4);
  sonare::mixing::AutomationEvent first;
  first.sample_pos = 0;
  first.value = 1.0f;
  first.curve = sonare::mixing::AutomationCurveType::Exponential;
  first.target = {sonare::mixing::AutomationTargetKind::Fader, 1, 0, 0};
  sonare::mixing::AutomationEvent second = first;
  second.sample_pos = 100;
  second.value = 16.0f;

  REQUIRE(lane.push(first));
  REQUIRE(lane.push(second));

  std::vector<sonare::mixing::AutomationBlockEvent> first_block;
  REQUIRE(lane.consume_block(0, 8, [&](const auto& event) { first_block.push_back(event); }) == 8);
  REQUIRE(first_block.front().offset == 0);
  REQUIRE(first_block.back().offset == 7);

  std::vector<sonare::mixing::AutomationBlockEvent> second_block;
  REQUIRE(lane.consume_block(48, 5, [&](const auto& event) { second_block.push_back(event); }) ==
          5);
  REQUIRE(second_block.front().offset == 0);
  REQUIRE(second_block.back().offset == 4);
  REQUIRE_THAT(second_block[2].event.value, WithinAbs(4.0f, 0.0001f));
}

TEST_CASE("Mixing Scene JSON round-trips pure data without graph dependency", "[mixing]") {
  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip vocal;
  vocal.id = "vocal";
  vocal.input_trim_db = 2.0f;
  vocal.fader_db = -3.0f;
  vocal.pan = -0.25f;
  vocal.width = 1.2f;
  vocal.muted = false;
  vocal.soloed = true;
  vocal.solo_safe = false;
  vocal.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PreFader, "dynamics.compressor", "{\"thresholdDb\":-18}"});
  vocal.inserts.push_back({sonare::mixing::api::InsertSlot::PostFader, "eq.equalizer",
                           "{\"band0.externalSidechain\":1,\"band0.gainDb\":-3}", "kick"});
  vocal.sends.push_back({"verb-send", "verb", -12.0f, sonare::mixing::api::SendTiming::PostFader});
  scene.strips.push_back(vocal);
  sonare::mixing::api::Bus verb_bus;
  verb_bus.id = "verb";
  verb_bus.role = "aux";
  verb_bus.inserts.push_back(
      {sonare::mixing::api::InsertSlot::PostFader, "effects.reverb.plate", "{\"decaySec\":1.2}"});
  scene.buses.push_back(verb_bus);
  scene.vca_groups.push_back({"lead", -1.5f, {"vocal"}});
  scene.connections.push_back({"vocal", "master"});

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  const auto parsed = sonare::mixing::api::scene_from_json(json);

  REQUIRE(parsed.version == 1);
  REQUIRE(parsed.strips.size() == 1);
  REQUIRE(parsed.strips[0].id == "vocal");
  REQUIRE_THAT(parsed.strips[0].input_trim_db, WithinAbs(2.0f, 0.0001f));
  REQUIRE_THAT(parsed.strips[0].fader_db, WithinAbs(-3.0f, 0.0001f));
  REQUIRE_THAT(parsed.strips[0].pan, WithinAbs(-0.25f, 0.0001f));
  REQUIRE_THAT(parsed.strips[0].width, WithinAbs(1.2f, 0.0001f));
  REQUIRE(parsed.strips[0].soloed);
  REQUIRE(parsed.strips[0].inserts.size() == 2);
  REQUIRE(parsed.strips[0].inserts[0].slot == sonare::mixing::api::InsertSlot::PreFader);
  REQUIRE(parsed.strips[0].inserts[0].processor_name == "dynamics.compressor");
  REQUIRE(parsed.strips[0].inserts[0].params_json == "{\"thresholdDb\":-18}");
  REQUIRE(parsed.strips[0].inserts[1].processor_name == "eq.equalizer");
  REQUIRE(parsed.strips[0].inserts[1].params_json.find("externalSidechain") != std::string::npos);
  REQUIRE(parsed.strips[0].inserts[1].sidechain_key == "kick");
  REQUIRE(parsed.strips[0].sends.size() == 1);
  REQUIRE(parsed.strips[0].sends[0].destination_bus_id == "verb");
  REQUIRE(parsed.strips[0].sends[0].timing == sonare::mixing::api::SendTiming::PostFader);
  REQUIRE(parsed.buses[0].role == "aux");
  REQUIRE(parsed.buses[0].inserts.size() == 1);
  REQUIRE(parsed.buses[0].inserts[0].processor_name == "effects.reverb.plate");
  REQUIRE(parsed.vca_groups[0].members[0] == "vocal");
  REQUIRE(parsed.connections[0].destination == "master");
}

TEST_CASE("Mixing Scene JSON parses exponent-form numbers it can emit", "[mixing]") {
  const std::string json =
      "{\"version\":1,\"strips\":[{\"id\":\"tiny\",\"inputTrimDb\":1e-05,\"faderDb\":-3E+00,"
      "\"pan\":0,\"width\":1,\"muted\":false,\"soloed\":false,\"soloSafe\":false,"
      "\"panMode\":0,\"dualPanLeft\":-1.25e-01,\"dualPanRight\":1.25E-01,"
      "\"polarityInvertLeft\":false,\"polarityInvertRight\":false,\"panLaw\":0,"
      "\"channelDelaySamples\":0,\"inserts\":[],\"sends\":[]}],\"buses\":[],"
      "\"vcaGroups\":[],\"connections\":[]}";

  const auto parsed = sonare::mixing::api::scene_from_json(json);

  REQUIRE(parsed.strips.size() == 1);
  REQUIRE_THAT(parsed.strips[0].input_trim_db, WithinAbs(1e-05f, 1e-09f));
  REQUIRE_THAT(parsed.strips[0].fader_db, WithinAbs(-3.0f, 0.0001f));
  REQUIRE_THAT(parsed.strips[0].dual_pan_left, WithinAbs(-0.125f, 0.0001f));
  REQUIRE_THAT(parsed.strips[0].dual_pan_right, WithinAbs(0.125f, 0.0001f));

  const auto reparsed =
      sonare::mixing::api::scene_from_json(sonare::mixing::api::scene_to_json(parsed));
  REQUIRE(reparsed.strips.size() == 1);
  REQUIRE_THAT(reparsed.strips[0].input_trim_db, WithinAbs(1e-05f, 1e-09f));
}

TEST_CASE("Mixing Scene JSON round-trips all extended strip fields", "[mixing]") {
  // Every field added to api::Strip beyond the original set must survive a
  // scene_to_json -> scene_from_json round-trip. Build a strip whose extended
  // fields all carry non-default values so a dropped field would change the
  // observed value.
  sonare::mixing::api::Scene scene;
  sonare::mixing::api::Strip strip;
  strip.id = "extended";
  strip.pan_mode = 2;  // DualPan
  strip.dual_pan_left = -0.4f;
  strip.dual_pan_right = 0.7f;
  strip.polarity_invert_left = true;
  strip.polarity_invert_right = true;
  strip.pan_law = 3;  // Linear0dB
  strip.channel_delay_samples = 17;
  scene.strips.push_back(strip);

  const std::string json = sonare::mixing::api::scene_to_json(scene);
  const auto parsed = sonare::mixing::api::scene_from_json(json);

  REQUIRE(parsed.strips.size() == 1);
  const auto& out = parsed.strips[0];
  REQUIRE(out.pan_mode == 2);
  REQUIRE_THAT(out.dual_pan_left, WithinAbs(-0.4f, 0.0001f));
  REQUIRE_THAT(out.dual_pan_right, WithinAbs(0.7f, 0.0001f));
  REQUIRE(out.polarity_invert_left);
  REQUIRE(out.polarity_invert_right);
  REQUIRE(out.pan_law == 3);
  REQUIRE(out.channel_delay_samples == 17);
}

TEST_CASE("Mixing Scene JSON ignores unknown forward-compat fields", "[mixing]") {
  // A scene authored by a newer version may carry fields this parser does not
  // know. Unknown scalars, arrays, and nested objects at both the scene level
  // and inside a strip object must be skipped without throwing, while known
  // fields still parse correctly.
  const std::string json =
      "{"
      "\"version\":1,"
      "\"futureScalar\":42,"
      "\"unknownArray\":[1,2,3],"
      "\"futureField\":{\"nested\":{\"deep\":[true,false],\"s\":\"x\"}},"
      "\"strips\":[{"
      "\"id\":\"vox\","
      "\"faderDb\":-4.5,"
      "\"panLaw\":2,"
      "\"channelDelaySamples\":9,"
      "\"futureStripScalar\":\"hello\","
      "\"futureStripObject\":{\"a\":1,\"b\":[{\"c\":2}]},"
      "\"futureStripArray\":[{\"x\":1},{\"y\":2}]"
      "}],"
      "\"buses\":[],"
      "\"vcaGroups\":[],"
      "\"connections\":[]"
      "}";

  sonare::mixing::api::Scene parsed;
  REQUIRE_NOTHROW(parsed = sonare::mixing::api::scene_from_json(json));
  REQUIRE(parsed.version == 1);
  REQUIRE(parsed.strips.size() == 1);
  REQUIRE(parsed.strips[0].id == "vox");
  REQUIRE_THAT(parsed.strips[0].fader_db, WithinAbs(-4.5f, 0.0001f));
  REQUIRE(parsed.strips[0].pan_law == 2);
  REQUIRE(parsed.strips[0].channel_delay_samples == 9);
}

TEST_CASE("Mixing Scene JSON rejects unsupported version", "[mixing]") {
  const std::string json = "{\"version\":2,\"strips\":[],\"buses\":[]}";
  REQUIRE_THROWS_AS(sonare::mixing::api::scene_from_json(json), sonare::SonareException);
}

TEST_CASE("Mixing scene presets expose planned templates and JSON round-trip", "[mixing]") {
  const auto names = sonare::mixing::api::scene_preset_names();
  REQUIRE(names.size() == 3);
  REQUIRE(names[0] == "vocalReverbSend");
  REQUIRE(names[1] == "drumBusSubgroup");
  REQUIRE(names[2] == "commentaryDucking");

  for (const auto& name : names) {
    CAPTURE(name);
    const auto preset = sonare::mixing::api::scene_preset_from_string(name);
    REQUIRE(std::string(sonare::mixing::api::scene_preset_to_string(preset)) == name);
    const auto scene = sonare::mixing::api::scene_preset(preset);
    REQUIRE_FALSE(scene.strips.empty());
    REQUIRE_FALSE(scene.buses.empty());
    const auto parsed =
        sonare::mixing::api::scene_from_json(sonare::mixing::api::scene_to_json(scene));
    REQUIRE(parsed.strips.size() == scene.strips.size());
    REQUIRE(parsed.buses.size() == scene.buses.size());
  }

  REQUIRE_THROWS_AS(sonare::mixing::api::scene_preset_from_string("missing"),
                    sonare::SonareException);
}

TEST_CASE("Mixing scene presets contain expected routing intent", "[mixing]") {
  const auto vocal =
      sonare::mixing::api::scene_preset(sonare::mixing::api::ScenePreset::VocalReverbSend);
  REQUIRE(vocal.strips[0].id == "vocal");
  REQUIRE(vocal.strips[0].sends[0].destination_bus_id == "vocal-verb");
  REQUIRE(vocal.strips[0].inserts[0].processor_name == "eq.parametric");

  const auto drums =
      sonare::mixing::api::scene_preset(sonare::mixing::api::ScenePreset::DrumBusSubgroup);
  REQUIRE(drums.vca_groups.size() == 1);
  REQUIRE(drums.vca_groups[0].members.size() == 4);
  REQUIRE(drums.connections[0].destination == "drum-bus");

  const auto commentary =
      sonare::mixing::api::scene_preset(sonare::mixing::api::ScenePreset::CommentaryDucking);
  REQUIRE(commentary.strips[2].id == "music-bed");
  REQUIRE(commentary.strips[2].inserts[0].processor_name == "dynamics.sidechainRouter");
  REQUIRE(commentary.vca_groups[0].id == "voices");
}

TEST_CASE("BusProcessor sums multiple stereo inputs", "[mixing]") {
  std::array<float, 4> in1_l{1.0f, 2.0f, 3.0f, 4.0f};
  std::array<float, 4> in1_r{0.5f, 1.0f, 1.5f, 2.0f};
  std::array<float, 4> in2_l{-0.25f, 0.25f, -0.25f, 0.25f};
  std::array<float, 4> in2_r{2.0f, 1.0f, 0.0f, -1.0f};
  std::array<float, 4> out_l{};
  std::array<float, 4> out_r{};

  float* input1[] = {in1_l.data(), in1_r.data()};
  float* input2[] = {in2_l.data(), in2_r.data()};
  float* output[] = {out_l.data(), out_r.data()};

  const sonare::mixing::BusProcessor bus(sonare::mixing::BusRole::Master);
  bus.sum_inputs({input1, input2}, output, 2, 4);

  REQUIRE_THAT(out_l[0], WithinAbs(0.75f, 0.0001f));
  REQUIRE_THAT(out_l[1], WithinAbs(2.25f, 0.0001f));
  REQUIRE_THAT(out_r[0], WithinAbs(2.5f, 0.0001f));
  REQUIRE_THAT(out_r[3], WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("BusProcessor publishes post-insert meter snapshot", "[mixing]") {
  std::array<float, 8> left{};
  std::array<float, 8> right{};
  left.fill(0.25f);
  right.fill(0.25f);
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::BusProcessor bus(sonare::mixing::BusRole::Subgroup);
  bus.prepare(48000.0, static_cast<int>(left.size()));
  bus.process(channels, 2, static_cast<int>(left.size()));

  const auto snapshot = bus.meter_snapshot();
  REQUIRE(snapshot.seq == 1);
  REQUIRE_THAT(snapshot.correlation, WithinAbs(1.0f, 0.0001f));
  REQUIRE(snapshot.peak_db[0] > sonare::constants::kFloorDb);
}

TEST_CASE("GainProcessor applies fader and VCA offset", "[mixing]") {
  std::array<float, 4> samples{1.0f, 1.0f, 1.0f, 1.0f};
  float* channels[] = {samples.data()};

  sonare::mixing::GainProcessor gain({-6.0f, 0.0f});
  gain.set_vca_offset_db(6.0f);
  gain.prepare(48000.0, 4);
  gain.process(channels, 1, 4);

  for (float sample : samples) {
    REQUIRE_THAT(sample, WithinAbs(1.0f, 0.0001f));
  }
}

TEST_CASE("GainProcessor zero smoothing applies target without a second ramp", "[mixing]") {
  std::array<float, 4> samples{1.0f, 1.0f, 1.0f, 1.0f};
  float* channels[] = {samples.data()};

  sonare::mixing::GainProcessor gain({0.0f, 0.0f});
  gain.prepare(48000.0, 4);
  gain.set_gain_db(-6.0206f);
  gain.process(channels, 1, 4);

  for (float sample : samples) {
    REQUIRE_THAT(sample, WithinAbs(0.5f, 0.0001f));
  }
}

TEST_CASE("ChannelStrip applies fader then pan", "[mixing]") {
  std::array<float, 4> left{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> right{1.0f, 1.0f, 1.0f, 1.0f};
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 1.0f, sonare::mixing::PanLaw::Const3dB, 0.0f});
  strip.prepare(48000.0, 4);
  strip.process(channels, 2, 4);

  for (float sample : left) {
    REQUIRE_THAT(sample, WithinAbs(0.0f, 0.0001f));
  }
  for (float sample : right) {
    REQUIRE_THAT(sample, WithinAbs(1.0f, 0.0001f));
  }
}

TEST_CASE("ChannelStrip input trim is independent from fader", "[mixing]") {
  std::array<float, 4> left{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> right{1.0f, 1.0f, 1.0f, 1.0f};
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({-6.0206f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.set_input_trim_db(6.0206f);
  strip.prepare(48000.0, 4);
  strip.process(channels, 2, 4);

  REQUIRE_THAT(strip.input_trim_db(), WithinAbs(6.0206f, 0.0001f));
  REQUIRE_THAT(strip.fader_db(), WithinAbs(-6.0206f, 0.0001f));
  for (int i = 0; i < 4; ++i) {
    REQUIRE_THAT(left[static_cast<size_t>(i)], WithinAbs(1.0f, 0.0002f));
    REQUIRE_THAT(right[static_cast<size_t>(i)], WithinAbs(1.0f, 0.0002f));
  }
}

TEST_CASE("ChannelStrip applies polarity delay width and dual meter taps", "[mixing]") {
  std::array<float, 4> left{1.0f, 2.0f, 3.0f, 4.0f};
  std::array<float, 4> right{10.0f, 20.0f, 30.0f, 40.0f};
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.set_polarity_invert(true, false);
  strip.set_width(0.0f);
  strip.set_channel_delay_samples(1);
  strip.prepare(48000.0, 4);
  strip.process(channels, 2, 4);

  REQUIRE(strip.polarity_invert_left());
  REQUIRE_FALSE(strip.polarity_invert_right());
  REQUIRE(strip.channel_delay_samples() == 1);
  REQUIRE(strip.latency_samples() == 1);

  // After polarity and one-sample delay, the first nonzero stereo frame is (-1, 10).
  // Width 0 collapses the post-fader signal to mono, so both channels become 4.5.
  REQUIRE_THAT(left[0], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(right[0], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(left[1], WithinAbs(4.5f, 0.0001f));
  REQUIRE_THAT(right[1], WithinAbs(4.5f, 0.0001f));

  const auto pre = strip.meter_snapshot(sonare::mixing::TapPoint::PreFader);
  const auto post = strip.meter_snapshot(sonare::mixing::TapPoint::PostFader);
  REQUIRE(pre.seq == 1);
  REQUIRE(post.seq == 1);
  REQUIRE(pre.peak_db[1] > post.peak_db[1]);
}

TEST_CASE("ChannelStrip pre and post inserts wrap fader pan and width", "[mixing]") {
  std::array<float, 4> left{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> right{1.0f, 1.0f, 1.0f, 1.0f};
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({-6.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_pre_insert(std::make_unique<ScaleProcessor>(2.0f));
  strip.add_post_insert(std::make_unique<ScaleProcessor>(3.0f));
  strip.prepare(48000.0, 4);
  const size_t pre_send = strip.add_send({0.0f, sonare::mixing::SendTiming::PreFader, 0.0f});
  strip.process(channels, 2, 4);

  std::array<float, 4> send_l{};
  std::array<float, 4> send_r{};
  float* send[] = {send_l.data(), send_r.data()};
  strip.mix_send(pre_send, send, 2, 4);

  const float fader_gain = std::pow(10.0f, -6.0f / 20.0f);
  REQUIRE(strip.num_pre_inserts() == 1);
  REQUIRE(strip.num_post_inserts() == 1);
  for (int i = 0; i < 4; ++i) {
    REQUIRE_THAT(send_l[static_cast<size_t>(i)], WithinAbs(2.0f, 0.0001f));
    REQUIRE_THAT(left[static_cast<size_t>(i)], WithinAbs(2.0f * fader_gain * 3.0f, 0.0001f));
    REQUIRE_THAT(right[static_cast<size_t>(i)], WithinAbs(2.0f * fader_gain * 3.0f, 0.0001f));
  }
}

TEST_CASE("ChannelStrip input trim starts the fixed strip order", "[mixing]") {
  std::array<float, 4> left{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> right{1.0f, 1.0f, 1.0f, 1.0f};
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({-6.0206f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.set_input_trim_db(6.0206f);
  strip.add_pre_insert(std::make_unique<AddProcessor>(3.0f));
  strip.add_post_insert(std::make_unique<ScaleProcessor>(5.0f));
  strip.prepare(48000.0, 4);
  const size_t pre_send = strip.add_send({0.0f, sonare::mixing::SendTiming::PreFader, 0.0f});
  strip.process(channels, 2, 4);

  std::array<float, 4> send_l{};
  std::array<float, 4> send_r{};
  float* send[] = {send_l.data(), send_r.data()};
  strip.mix_send(pre_send, send, 2, 4);

  for (int i = 0; i < 4; ++i) {
    REQUIRE_THAT(send_l[static_cast<size_t>(i)], WithinAbs(5.0f, 0.0002f));
    REQUIRE_THAT(send_r[static_cast<size_t>(i)], WithinAbs(5.0f, 0.0002f));
    REQUIRE_THAT(left[static_cast<size_t>(i)], WithinAbs(12.5f, 0.001f));
    REQUIRE_THAT(right[static_cast<size_t>(i)], WithinAbs(12.5f, 0.001f));
  }
  REQUIRE(strip.meter_snapshot(sonare::mixing::TapPoint::PreFader).seq == 1);
  REQUIRE(strip.meter_snapshot(sonare::mixing::TapPoint::PostFader).seq == 1);
}

TEST_CASE("ChannelStrip aggregates Q8 latency across delay and inserts", "[mixing]") {
  sonare::mixing::ChannelStrip strip;

  strip.set_channel_delay_samples(2);
  strip.add_pre_insert(std::make_unique<TestQ8LatencyProcessor>(3 << 8));
  strip.add_post_insert(std::make_unique<TestQ8LatencyProcessor>((5 << 8) + 128));

  REQUIRE(strip.latency_samples_q8() == ((10 << 8) + 128));
  REQUIRE(strip.latency_samples() == 10);
}

TEST_CASE("ChannelStrip applies fader automation at block sample offsets", "[mixing]") {
  std::array<float, 6> left{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 6> right = left;
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, 6);
  REQUIRE(strip.schedule_fader_automation(102, -6.0206f));
  strip.process_at(channels, 2, 6, 100);

  for (int i = 0; i < 2; ++i) {
    REQUIRE_THAT(left[static_cast<size_t>(i)], WithinAbs(1.0f, 0.0001f));
    REQUIRE_THAT(right[static_cast<size_t>(i)], WithinAbs(1.0f, 0.0001f));
  }
  for (int i = 2; i < 6; ++i) {
    REQUIRE_THAT(left[static_cast<size_t>(i)], WithinAbs(0.5f, 0.0001f));
    REQUIRE_THAT(right[static_cast<size_t>(i)], WithinAbs(0.5f, 0.0001f));
  }
  REQUIRE_THAT(strip.fader_db(), WithinAbs(-6.0206f, 0.0001f));
  REQUIRE(strip.meter_snapshot().seq == 1);
}

TEST_CASE("ChannelStrip segmented path reports block-max gain reduction", "[mixing]") {
  // With sample-accurate automation, process_at() splits the block into
  // segments. The pre-insert's last_gain_reduction_db() reflects only the most
  // recently processed segment, so the reported aggregate must track the
  // block-representative (most-negative) GR across all segments, not just the
  // final one. Here the loud leading segment (GR -10 dB) precedes a quiet
  // trailing segment (GR -1 dB); the snapshot must report -10 dB.
  std::array<float, 6> left{1.0f, 1.0f, 0.1f, 0.1f, 0.1f, 0.1f};
  std::array<float, 6> right = left;
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_pre_insert(std::make_unique<PeakGainReductionProcessor>());
  strip.prepare(48000.0, 6);
  // A width automation event at offset 2 forces a segment boundary there.
  REQUIRE(strip.schedule_width_automation(102, 1.0f));
  strip.process_at(channels, 2, 6, 100);

  // First segment peak 1.0 -> GR -10 dB; final segment peak 0.1 -> GR -1 dB.
  // The reported GR must be the block max (-10), not the last segment (-1).
  REQUIRE_THAT(strip.meter_snapshot().gain_reduction_db, WithinAbs(-10.0f, 0.0001f));
}

TEST_CASE("ChannelStrip reset clears pending automation lanes", "[mixing]") {
  std::array<float, 4> left{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> right = left;
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, 4);
  REQUIRE(strip.schedule_fader_automation(100, -6.0206f));
  strip.reset();
  strip.process_at(channels, 2, 4, 100);

  for (int i = 0; i < 4; ++i) {
    REQUIRE_THAT(left[static_cast<size_t>(i)], WithinAbs(1.0f, 0.0001f));
    REQUIRE_THAT(right[static_cast<size_t>(i)], WithinAbs(1.0f, 0.0001f));
  }
}

TEST_CASE("ChannelStrip applies pan and width automation in sample order", "[mixing]") {
  std::array<float, 6> left{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 6> right{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, 6);
  REQUIRE(strip.schedule_width_automation(102, 0.0f));
  REQUIRE(strip.schedule_pan_automation(104, 1.0f));
  strip.process_at(channels, 2, 6, 100);

  REQUIRE_THAT(left[0], WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(right[0], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(left[2], WithinAbs(0.5f, 0.0001f));
  REQUIRE_THAT(right[2], WithinAbs(0.5f, 0.0001f));
  REQUIRE_THAT(left[4], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(right[4], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(strip.width(), WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(strip.pan(), WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("ChannelStrip drives insert parameter automation at sample offsets", "[mixing]") {
  // schedule_insert_automation must dispatch to the insert's set_parameter at the
  // scheduled sample. ScaleProcessor::set_parameter(0, v) sets its linear gain, so
  // a single step event flips the gain mid-block at a known boundary.
  std::array<float, 6> left{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 6> right = left;
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_pre_insert(std::make_unique<ScaleProcessor>(2.0f));
  strip.prepare(48000.0, 6);
  REQUIRE(strip.schedule_insert_automation(0, 0, 102, 4.0f));
  strip.process_at(channels, 2, 6, 100);

  for (int i = 0; i < 2; ++i) {
    REQUIRE_THAT(left[static_cast<size_t>(i)], WithinAbs(2.0f, 0.0001f));
    REQUIRE_THAT(right[static_cast<size_t>(i)], WithinAbs(2.0f, 0.0001f));
  }
  for (int i = 2; i < 6; ++i) {
    REQUIRE_THAT(left[static_cast<size_t>(i)], WithinAbs(4.0f, 0.0001f));
    REQUIRE_THAT(right[static_cast<size_t>(i)], WithinAbs(4.0f, 0.0001f));
  }
}

TEST_CASE("ChannelStrip keeps independent insert automation lanes sample-ordered", "[mixing]") {
  std::array<float, 4> left{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> right = left;
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_pre_insert(std::make_unique<ScaleProcessor>(2.0f));
  strip.add_pre_insert(std::make_unique<ScaleProcessor>(3.0f));
  strip.prepare(48000.0, 4);

  REQUIRE(strip.schedule_insert_automation(1, 0, 101, 5.0f));
  REQUIRE(strip.schedule_insert_automation(0, 0, 102, 4.0f));
  strip.process_at(channels, 2, 4, 100);

  REQUIRE_THAT(left[0], WithinAbs(6.0f, 0.0001f));
  REQUIRE_THAT(left[1], WithinAbs(10.0f, 0.0001f));
  REQUIRE_THAT(left[2], WithinAbs(20.0f, 0.0001f));
  REQUIRE_THAT(left[3], WithinAbs(20.0f, 0.0001f));
}

TEST_CASE("ChannelStrip insert automation boosts a parametric EQ band", "[mixing][eq]") {
  // Proves set_parameter reaches a real mastering insert (ParametricEq): band 0
  // gain lives at param id 1 (block-of-3 layout). Automating it from 0 dB to
  // +12 dB must lift the band's output energy, which a no-op set_parameter could
  // not do.
  static constexpr int kN = 4096;
  auto make_sine = [] {
    std::vector<float> out(kN);
    for (int i = 0; i < kN; ++i) {
      out[static_cast<size_t>(i)] =
          0.5f * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(i) / 48000.0f);
    }
    return out;
  };

  auto make_band0_eq = [] {
    auto eq = std::make_unique<sonare::mastering::eq::ParametricEq>();
    sonare::mastering::eq::EqBand band;
    band.type = sonare::mastering::eq::EqBandType::Peak;
    band.frequency_hz = 1000.0f;
    band.gain_db = 0.0f;
    band.q = sonare::constants::kButterworthQ;
    band.enabled = true;
    eq->set_band(0, band);
    return eq;
  };

  std::vector<float> flat_l = make_sine();
  std::vector<float> flat_r = flat_l;
  float* flat[] = {flat_l.data(), flat_r.data()};
  sonare::mixing::ChannelStrip flat_strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  flat_strip.add_pre_insert(make_band0_eq());
  flat_strip.prepare(48000.0, kN);
  flat_strip.process(flat, 2, kN);

  std::vector<float> auto_l = make_sine();
  std::vector<float> auto_r = auto_l;
  float* automated[] = {auto_l.data(), auto_r.data()};
  sonare::mixing::ChannelStrip auto_strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  auto_strip.add_pre_insert(make_band0_eq());
  auto_strip.prepare(48000.0, kN);
  REQUIRE(auto_strip.schedule_insert_automation(0, 1, 0, 12.0f));
  auto_strip.process_at(automated, 2, kN, 0);

  REQUIRE(rms_tail(auto_l, 512) > rms_tail(flat_l, 512) * 1.5f);
}

TEST_CASE("ChannelStrip rejects non-RT-safe insert automation", "[mixing]") {
  sonare::mixing::ChannelStrip linear_phase_strip;
  linear_phase_strip.add_pre_insert(std::make_unique<sonare::mastering::eq::LinearPhaseEq>());
  linear_phase_strip.prepare(48000.0, 128);

  REQUIRE_FALSE(linear_phase_strip.schedule_insert_automation(0, 1, 0, 6.0f));

  auto equalizer = std::make_unique<sonare::mastering::eq::EqualizerProcessor>();
  sonare::mastering::eq::EqBand linear_band{sonare::mastering::eq::EqBandType::Peak, 1000.0f, 3.0f,
                                            1.0f, true};
  linear_band.phase = sonare::mastering::eq::PhaseMode::LinearPhase;
  equalizer->set_band(0, linear_band);
  sonare::mixing::ChannelStrip equalizer_strip;
  equalizer_strip.add_pre_insert(std::move(equalizer));
  equalizer_strip.prepare(48000.0, 128);

  REQUIRE_FALSE(equalizer_strip.schedule_insert_automation(0, 1, 0, 6.0f));

  sonare::mixing::ChannelStrip maximizer_strip;
  maximizer_strip.add_pre_insert(std::make_unique<sonare::mastering::maximizer::Maximizer>());
  maximizer_strip.prepare(48000.0, 128);

  REQUIRE(maximizer_strip.schedule_insert_automation(0, 0, 0, 3.0f));
  REQUIRE_FALSE(maximizer_strip.schedule_insert_automation(0, 1, 0, -2.0f));
  REQUIRE(maximizer_strip.schedule_insert_automation(0, 2, 0, 20.0f));
}

TEST_CASE("ChannelStrip reuses mastering EQ as a pre-fader insert", "[mixing]") {
  static constexpr int kN = 512;
  auto make_input = [] {
    std::vector<float> out(kN);
    for (int i = 0; i < kN; ++i) {
      out[static_cast<size_t>(i)] =
          0.5f * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(i) / 48000.0f);
    }
    return out;
  };

  std::vector<float> plain_l = make_input();
  std::vector<float> plain_r = plain_l;
  float* plain[] = {plain_l.data(), plain_r.data()};
  sonare::mixing::ChannelStrip plain_strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  plain_strip.prepare(48000.0, kN);
  plain_strip.process(plain, 2, kN);

  auto eq = std::make_unique<sonare::mastering::eq::ParametricEq>();
  sonare::mastering::eq::EqBand band;
  band.type = sonare::mastering::eq::EqBandType::Peak;
  band.frequency_hz = 1000.0f;
  band.gain_db = 12.0f;
  band.q = sonare::constants::kButterworthQ;
  band.enabled = true;
  eq->set_band(0, band);

  std::vector<float> eq_l = make_input();
  std::vector<float> eq_r = eq_l;
  float* eq_channels[] = {eq_l.data(), eq_r.data()};
  sonare::mixing::ChannelStrip eq_strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  eq_strip.add_pre_insert(std::move(eq));
  eq_strip.prepare(48000.0, kN);
  eq_strip.process(eq_channels, 2, kN);

  REQUIRE(rms_tail(eq_l, 128) > rms_tail(plain_l, 128) * 1.5f);
}

TEST_CASE("ChannelStrip reuses mastering compressor as a post-fader insert", "[mixing]") {
  constexpr int kN = 48000;
  std::vector<float> left(kN, 0.8f);
  std::vector<float> right(kN, 0.8f);
  float* channels[] = {left.data(), right.data()};

  sonare::mastering::dynamics::CompressorConfig config;
  config.threshold_db = -30.0f;
  config.ratio = 8.0f;
  config.attack_ms = 0.0f;
  config.release_ms = 20.0f;
  config.detector = sonare::mastering::dynamics::DetectorMode::Peak;
  auto compressor = std::make_unique<sonare::mastering::dynamics::Compressor>(config);
  auto* compressor_ptr = compressor.get();

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_post_insert(std::move(compressor));
  strip.prepare(48000.0, 1024);
  strip.process(channels, 2, kN);

  REQUIRE(rms_tail(left, 4096) < 0.25f);
  REQUIRE(compressor_ptr->last_gain_reduction_db() < -10.0f);
  REQUIRE(strip.meter_snapshot().gain_reduction_db < -10.0f);
}

TEST_CASE("ChannelStrip accepts mastering SidechainRouter insert with external key", "[mixing]") {
  constexpr int kN = 48000;
  std::vector<float> left(kN, 0.01f);
  std::vector<float> right(kN, 0.01f);
  float* channels[] = {left.data(), right.data()};

  sonare::mastering::dynamics::SidechainRouterConfig config;
  config.threshold_db = -30.0f;
  config.ratio = 4.0f;
  config.attack_ms = 0.0f;
  config.release_ms = 20.0f;
  config.range_db = 18.0f;
  auto router = std::make_unique<sonare::mastering::dynamics::SidechainRouter>(config);
  auto* router_ptr = router.get();

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_post_insert(std::move(router));
  strip.prepare(48000.0, 1024);

  std::vector<float> sidechain(kN, 0.8f);
  const float* sidechain_channels[] = {sidechain.data()};
  router_ptr->set_sidechain(sidechain_channels, 1, static_cast<int>(sidechain.size()));
  strip.process(channels, 2, kN);

  REQUIRE(rms_tail(left, 4096) < 0.0025f);
  REQUIRE(router_ptr->last_gain_reduction_db() < -10.0f);
}

TEST_CASE("SendProcessor exposes pre and post fader timing", "[mixing]") {
  sonare::mixing::SendProcessor send({-3.0f, sonare::mixing::SendTiming::PreFader, 0.0f});

  REQUIRE(send.timing() == sonare::mixing::SendTiming::PreFader);
  send.set_timing(sonare::mixing::SendTiming::PostFader);
  REQUIRE(send.timing() == sonare::mixing::SendTiming::PostFader);
}

TEST_CASE("VcaGroup applies relative gain offset to members", "[mixing]") {
  sonare::mixing::GainProcessor first({0.0f, 0.0f});
  sonare::mixing::GainProcessor second({-6.0f, 0.0f});
  sonare::mixing::VcaGroup vca;

  REQUIRE(vca.add_member(&first));
  REQUIRE(vca.add_member(&second));
  vca.set_vca_gain_db(-3.0f);

  REQUIRE_THAT(first.vca_offset_db(), WithinAbs(-3.0f, 0.0001f));
  REQUIRE_THAT(second.vca_offset_db(), WithinAbs(-3.0f, 0.0001f));
}

TEST_CASE("Manual VCA trim and group membership accumulate independently", "[mixing]") {
  // Regression: a direct set_vca_offset_db() used to overwrite the strip's whole
  // offset, discarding every VCA group's accumulated contribution. The manual
  // trim and the group offset must now sum without either stomping the other.
  sonare::mixing::GainProcessor gain({0.0f, 0.0f});

  sonare::mixing::VcaGroup group_a;
  sonare::mixing::VcaGroup group_b;
  group_a.set_vca_gain_db(-3.0f);
  group_b.set_vca_gain_db(-2.0f);
  REQUIRE(group_a.add_member(&gain));
  REQUIRE(group_b.add_member(&gain));
  REQUIRE_THAT(gain.vca_offset_db(), WithinAbs(-5.0f, 0.0001f));  // -3 + -2

  // A direct manual trim adds on top without disturbing the group total.
  gain.set_vca_offset_db(1.5f);
  REQUIRE_THAT(gain.vca_trim_offset_db(), WithinAbs(1.5f, 0.0001f));
  REQUIRE_THAT(gain.vca_group_offset_db(), WithinAbs(-5.0f, 0.0001f));
  REQUIRE_THAT(gain.vca_offset_db(), WithinAbs(-3.5f, 0.0001f));  // 1.5 + (-5)

  // Removing one group only subtracts its own contribution; the manual trim and
  // the other group remain intact.
  REQUIRE(group_b.remove_member(&gain));
  REQUIRE_THAT(gain.vca_offset_db(), WithinAbs(-1.5f, 0.0001f));  // 1.5 + (-3)
}

TEST_CASE("MixerController computes solo implied mute outside audio thread", "[mixing]") {
  sonare::mixing::ChannelStrip vocal;
  sonare::mixing::ChannelStrip drums;
  sonare::mixing::ChannelStrip reverb;
  sonare::mixing::MixerController controller;

  REQUIRE(controller.add_strip(&vocal));
  REQUIRE(controller.add_strip(&drums));
  REQUIRE(controller.add_strip(&reverb));
  controller.set_solo_safe(reverb, true);
  controller.set_solo(vocal, true);

  REQUIRE_FALSE(vocal.effectively_muted());
  REQUIRE(drums.effectively_muted());
  REQUIRE_FALSE(reverb.effectively_muted());
}

TEST_CASE("AlignmentDelay reports and applies integer latency", "[mixing]") {
  std::array<float, 4> mono{1.0f, 2.0f, 3.0f, 4.0f};
  float* channels[] = {mono.data()};
  sonare::mixing::AlignmentDelay delay(2);

  delay.prepare(48000.0, 4);
  delay.process(channels, 1, 4);

  REQUIRE(delay.latency_samples() == 2);
  REQUIRE_THAT(mono[0], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(mono[1], WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(mono[2], WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("AlignmentDelay clamps pathological delays so the Q8 shift never overflows", "[mixing]") {
  // delay_samples_q8_ is delay_samples << 8; an unclamped huge request would
  // overflow signed int and wrap to a negative Q8 latency. The clamp keeps both
  // the integer and Q8 reports non-negative and monotonic.
  sonare::mixing::AlignmentDelay delay(std::numeric_limits<int>::max());
  REQUIRE(delay.delay_samples() >= 0);
  REQUIRE(delay.delay_samples_q8() >= 0);
  REQUIRE(delay.delay_samples_q8() == (delay.delay_samples() << 8));

  delay.set_delay_samples(std::numeric_limits<int>::max());
  REQUIRE(delay.delay_samples() >= 0);
  REQUIRE(delay.delay_samples_q8() >= 0);
  REQUIRE(delay.delay_samples_q8() == (delay.delay_samples() << 8));
}

TEST_CASE("AlignmentDelay reports Q8 fractional latency and interpolates impulse", "[mixing]") {
  std::array<float, 6> mono{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  float* channels[] = {mono.data()};
  sonare::mixing::AlignmentDelay delay;

  delay.set_delay_samples_q8((1 << 8) + 128);
  delay.prepare(48000.0, 6);
  delay.process(channels, 1, 6);

  REQUIRE(delay.delay_samples() == 1);
  REQUIRE(delay.delay_samples_q8() == ((1 << 8) + 128));
  REQUIRE(delay.latency_samples_q8() == ((1 << 8) + 128));
  REQUIRE(delay.fractional_mode() == sonare::mixing::FractionalDelayMode::Lagrange3);

  float abs_sum = 0.0f;
  int nonzero = 0;
  for (float sample : mono) {
    abs_sum += std::abs(sample);
    if (std::abs(sample) > 0.001f) {
      ++nonzero;
    }
  }
  REQUIRE(abs_sum > 0.5f);
  REQUIRE(nonzero >= 2);
}

TEST_CASE("AlignmentDelay Lagrange3 magnitude droop is documented by fixture values", "[mixing]") {
  for (double fractional_delay : {0.25, 0.5, 0.75}) {
    REQUIRE(lagrange3_magnitude_db(fractional_delay, 0.25) > -0.1);
  }

  REQUIRE_THAT(lagrange3_magnitude_db(0.25, 0.8), WithinAbs(-3.4543, 0.01));
  REQUIRE_THAT(lagrange3_magnitude_db(0.5, 0.8), WithinAbs(-6.9595, 0.01));
  REQUIRE_THAT(lagrange3_magnitude_db(0.75, 0.8), WithinAbs(-3.4543, 0.01));

  REQUIRE(lagrange3_magnitude_db(0.5, 1.0) < -200.0);
}

TEST_CASE("StereoWidthProcessor collapses to mono at zero width", "[mixing]") {
  std::array<float, 2> left{1.0f, 0.0f};
  std::array<float, 2> right{0.0f, 1.0f};
  float* channels[] = {left.data(), right.data()};
  sonare::mixing::StereoWidthProcessor width(0.0f);

  width.process(channels, 2, 2);

  REQUIRE_THAT(left[0], WithinAbs(0.5f, 0.0001f));
  REQUIRE_THAT(right[0], WithinAbs(0.5f, 0.0001f));
  REQUIRE_THAT(left[1], WithinAbs(0.5f, 0.0001f));
  REQUIRE_THAT(right[1], WithinAbs(0.5f, 0.0001f));
}

TEST_CASE("FxBus owns ordered inserts and sums latency", "[mixing]") {
  sonare::mixing::FxBus fx_bus;

  fx_bus.add_insert(std::make_unique<TestLatencyProcessor>(3));
  fx_bus.add_insert(std::make_unique<TestLatencyProcessor>(5));

  REQUIRE(fx_bus.num_inserts() == 2);
  REQUIRE(fx_bus.latency_samples() == 8);
  REQUIRE(fx_bus.latency_samples_q8() == (8 << 8));
}

TEST_CASE("BusProcessor applies post-sum inserts", "[mixing]") {
  std::array<float, 4> a_l{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> a_r{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> b_l{2.0f, 2.0f, 2.0f, 2.0f};
  std::array<float, 4> b_r{2.0f, 2.0f, 2.0f, 2.0f};
  float* input_a[] = {a_l.data(), a_r.data()};
  float* input_b[] = {b_l.data(), b_r.data()};
  std::array<float, 4> out_l{};
  std::array<float, 4> out_r{};
  float* output[] = {out_l.data(), out_r.data()};

  sonare::mixing::BusProcessor bus(sonare::mixing::BusRole::Subgroup);
  bus.add_insert(std::make_unique<ScaleProcessor>(2.0f));
  bus.prepare(48000.0, 4);
  bus.sum_inputs({input_a, input_b}, output, 2, 4);
  bus.process(output, 2, 4);

  REQUIRE(bus.num_inserts() == 1);
  for (int i = 0; i < 4; ++i) {
    REQUIRE_THAT(out_l[static_cast<size_t>(i)], WithinAbs(6.0f, 0.0001f));
    REQUIRE_THAT(out_r[static_cast<size_t>(i)], WithinAbs(6.0f, 0.0001f));
  }
}

TEST_CASE("MeterProcessor publishes peak rms and correlation snapshot", "[mixing]") {
  std::array<float, 4> left{0.5f, -0.5f, 0.5f, -0.5f};
  std::array<float, 4> right{0.5f, -0.5f, 0.5f, -0.5f};
  float* channels[] = {left.data(), right.data()};
  sonare::mixing::MeterProcessor meter;

  meter.prepare(48000.0, 4);
  meter.process(channels, 2, 4);
  const auto snapshot = meter.snapshot();

  REQUIRE(snapshot.seq == 1);
  REQUIRE_THAT(snapshot.peak_db[0], WithinAbs(-6.0206f, 0.001f));
  REQUIRE_THAT(snapshot.rms_db[1], WithinAbs(-6.0206f, 0.001f));
  REQUIRE_THAT(snapshot.correlation, WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(snapshot.gain_reduction_db, WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(snapshot.max_true_peak_db, WithinAbs(sonare::constants::kFloorDb, 0.001f));
}

TEST_CASE("MeterProcessor optionally publishes true peak", "[mixing]") {
  std::array<float, 3> mono{0.0f, 0.8f, 0.0f};
  float* channels[] = {mono.data()};
  sonare::mixing::MeterConfig config;
  config.measure_lufs = false;
  config.measure_true_peak = true;
  config.true_peak_oversample = 4;
  sonare::mixing::MeterProcessor meter(config);

  meter.prepare(48000.0, 3);
  meter.process(channels, 1, 3);
  const auto snapshot = meter.snapshot();

  REQUIRE(snapshot.max_true_peak_db >= snapshot.peak_db[0]);
  REQUIRE_THAT(snapshot.true_peak_db[0], WithinAbs(snapshot.max_true_peak_db, 0.0001f));
  REQUIRE_THAT(snapshot.true_peak_db[1], WithinAbs(sonare::constants::kFloorDb, 0.001f));
  REQUIRE(snapshot.max_true_peak_db > -3.0f);
}

TEST_CASE("MeterProcessor true peak is consistent across block sizes (boundary history)",
          "[mixing]") {
  // Regression for the stateless true-peak path: it reconstructed ~half-a-tap
  // worth of samples against zeros at the start AND end of every block and kept
  // no cross-block history, so the measured true peak depended on the block
  // size and phase. The history-preserving path must measure (nearly) the same
  // true peak whether the signal is fed in one block or in many small blocks,
  // and must still see the inter-sample over.
  constexpr int kSampleRate = 48000;
  constexpr int kN = 1020;  // multiple of the 6-sample pattern below
  // A repeating half-band pattern near full scale has a strong inter-sample
  // peak: its bandlimited reconstruction overshoots well above the sample
  // values (same family as the existing "bandlimited inter-sample overs" test).
  const std::array<float, 6> pattern{0.0f, 0.95f, 0.95f, 0.0f, -0.95f, -0.95f};
  std::vector<float> signal(static_cast<size_t>(kN));
  for (int i = 0; i < kN; ++i) {
    signal[static_cast<size_t>(i)] = pattern[static_cast<size_t>(i % 6)];
  }
  const float sample_peak = 0.95f;

  auto measure = [&](int block) {
    sonare::mixing::MeterConfig config;
    config.measure_lufs = false;
    config.measure_true_peak = true;
    config.true_peak_oversample = 4;
    sonare::mixing::MeterProcessor meter(config);
    meter.prepare(static_cast<double>(kSampleRate), block);

    float max_tp_db = sonare::constants::kFloorDb;
    for (int offset = 0; offset < kN; offset += block) {
      const int n = std::min(block, kN - offset);
      float* chan = signal.data() + offset;
      float* channels[] = {chan};
      meter.process(channels, 1, n);
      max_tp_db = std::max(max_tp_db, meter.snapshot().max_true_peak_db);
    }
    return max_tp_db;
  };

  const float tp_one_block = measure(kN);
  const float tp_small_blocks = measure(64);  // 15 full + a 60-sample tail block
  const float tp_tiny_blocks = measure(13);   // ragged blocks, many boundaries

  // The inter-sample over must be detected (true peak strictly above the sample
  // peak) regardless of block size.
  const float sample_peak_db = sonare::linear_to_db(sample_peak);
  REQUIRE(tp_one_block > sample_peak_db);
  REQUIRE(tp_small_blocks > sample_peak_db);
  REQUIRE(tp_tiny_blocks > sample_peak_db);

  // And the result must be block-size independent within a tight tolerance. The
  // old stateless path zero-padded the FIR at every block edge and kept no
  // history, so this measurement drifted with the block size; the cross-block
  // history path makes the small/tiny-block runs track the single-block
  // reference closely.
  REQUIRE_THAT(tp_small_blocks, WithinAbs(tp_one_block, 0.3f));
  REQUIRE_THAT(tp_tiny_blocks, WithinAbs(tp_one_block, 0.3f));
}

TEST_CASE("GoniometerBuffer returns latest scope points", "[mixing]") {
  sonare::mixing::GoniometerBuffer<3> buffer;
  std::array<sonare::mixing::GoniometerPoint, 3> points{};

  buffer.push(1.0f, 0.0f);
  buffer.push(0.0f, 1.0f);
  buffer.push(-1.0f, 0.0f);
  buffer.push(0.0f, -1.0f);
  const size_t count = buffer.read_latest(points.data(), points.size());

  REQUIRE(count == 3);
  REQUIRE_THAT(points[0].left, WithinAbs(0.0f, 0.0001f));
  REQUIRE_THAT(points[0].right, WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(points[2].right, WithinAbs(-1.0f, 0.0001f));
}

TEST_CASE("ChannelStrip publishes post-fader goniometer points", "[mixing]") {
  std::array<float, 4> left{1.0f, 0.5f, -0.5f, -1.0f};
  std::array<float, 4> right{-1.0f, -0.5f, 0.5f, 1.0f};
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({-6.0206f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, 4);
  strip.process(channels, 2, 4);

  std::array<sonare::mixing::GoniometerPoint, 4> points{};
  const size_t count = strip.read_goniometer_latest(points.data(), points.size());

  REQUIRE(count == 4);
  for (size_t i = 0; i < points.size(); ++i) {
    REQUIRE_THAT(points[i].left, WithinAbs(left[i], 0.0001f));
    REQUIRE_THAT(points[i].right, WithinAbs(right[i], 0.0001f));
  }
  REQUIRE(strip.meter_snapshot().max_true_peak_db > sonare::constants::kFloorDb);

  strip.reset();
  REQUIRE(strip.read_goniometer_latest(points.data(), points.size()) == 0);
}

TEST_CASE("StereoWidthProcessor converges to steady-state mid/side", "[mixing]") {
  constexpr int kN = 4096;
  constexpr float kL0 = 0.8f;
  constexpr float kR0 = 0.2f;
  const float mid0 = 0.5f * (kL0 + kR0);
  const float side0 = 0.5f * (kL0 - kR0);

  SECTION("width=0 collapses to mid after convergence") {
    std::vector<float> left(kN, kL0);
    std::vector<float> right(kN, kR0);
    float* channels[] = {left.data(), right.data()};

    sonare::mixing::StereoWidthProcessor width(0.0f);
    width.prepare(48000.0, kN);
    width.process(channels, 2, kN);

    REQUIRE_THAT(left[kN - 1], WithinAbs(mid0, 0.001f));
    REQUIRE_THAT(right[kN - 1], WithinAbs(mid0, 0.001f));
  }

  SECTION("width=2 doubles the side and preserves the mid after convergence") {
    std::vector<float> left(kN, kL0);
    std::vector<float> right(kN, kR0);
    float* channels[] = {left.data(), right.data()};

    sonare::mixing::StereoWidthProcessor width(2.0f);
    width.prepare(48000.0, kN);
    width.process(channels, 2, kN);

    // Standard M/S width law: mid is left untouched, only the side scales with width.
    REQUIRE_THAT(left[kN - 1], WithinAbs(mid0 + 2.0f * side0, 0.001f));
    REQUIRE_THAT(right[kN - 1], WithinAbs(mid0 - 2.0f * side0, 0.001f));

    // The recovered mid (channel sum / 2) must equal the input mid: widening must
    // not attenuate the center/mono component.
    const float out_mid = 0.5f * (left[kN - 1] + right[kN - 1]);
    REQUIRE_THAT(out_mid, WithinAbs(mid0, 0.001f));
  }

  SECTION("width does not attenuate a centered/mono source") {
    constexpr float kCenter = 0.7f;
    std::vector<float> left(kN, kCenter);
    std::vector<float> right(kN, kCenter);  // mono: side == 0
    float* channels[] = {left.data(), right.data()};

    sonare::mixing::StereoWidthProcessor width(2.0f);
    width.prepare(48000.0, kN);
    width.process(channels, 2, kN);

    // With side == 0, raising width must leave a mono source completely unchanged.
    REQUIRE_THAT(left[kN - 1], WithinAbs(kCenter, 0.001f));
    REQUIRE_THAT(right[kN - 1], WithinAbs(kCenter, 0.001f));
  }

  SECTION("width=2 doubles a pure side signal") {
    std::vector<float> left(kN, 1.0f);
    std::vector<float> right(kN, -1.0f);  // mid == 0, side == 1
    float* channels[] = {left.data(), right.data()};

    sonare::mixing::StereoWidthProcessor width(2.0f);
    width.prepare(48000.0, kN);
    width.process(channels, 2, kN);

    // Pure side scales directly with width: |L| = |R| = side * w = 1 * 2 = 2.
    REQUIRE_THAT(std::abs(left[kN - 1]), WithinAbs(2.0f, 0.001f));
    REQUIRE_THAT(std::abs(right[kN - 1]), WithinAbs(2.0f, 0.001f));
  }
}

TEST_CASE("StereoWidthProcessor smooths width changes (no zipper)", "[mixing]") {
  constexpr int kN = 64;
  std::vector<float> left(kN, 1.0f);
  std::vector<float> right(kN, -1.0f);  // pure side signal
  float* channels[] = {left.data(), right.data()};

  // Default smoothing (5 ms) and starting width=1.
  sonare::mixing::StereoWidthProcessor width(1.0f);
  width.prepare(48000.0, kN);

  // Request a collapse to mono, then process a single small block.
  width.set_width(0.0f);
  width.process(channels, 2, kN);

  // The side must not collapse instantaneously: the first sample should still
  // retain a substantial fraction of the original side (|L - R| was 2.0).
  const float side_diff = std::abs(left[0] - right[0]);
  REQUIRE(side_diff > 0.1f);
  REQUIRE(side_diff < 2.0f);
}

TEST_CASE("MeterProcessor seqlock increments per block and stays consistent", "[mixing]") {
  constexpr int kN = 256;
  std::vector<float> left(kN);
  std::vector<float> right(kN);
  for (int i = 0; i < kN; ++i) {
    const float s =
        0.5f * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(i) / 48000.0f);
    left[i] = s;
    right[i] = s;
  }
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::MeterProcessor meter;
  meter.prepare(48000.0, kN);
  meter.process(channels, 2, kN);
  meter.process(channels, 2, kN);
  meter.process(channels, 2, kN);

  const auto snapshot = meter.snapshot();
  REQUIRE(snapshot.seq == 3);
  REQUIRE(std::isfinite(snapshot.peak_db[0]));
  REQUIRE(std::isfinite(snapshot.rms_db[0]));
  REQUIRE(snapshot.peak_db[0] > sonare::constants::kFloorDb);
  REQUIRE(snapshot.rms_db[0] > sonare::constants::kFloorDb);
}

namespace {

// Feeds a steady stereo sine of the given amplitude through the meter in blocks and
// returns the final snapshot. The buffer is also captured for an optional offline check.
sonare::mixing::MeterSnapshot drive_meter_sine(sonare::mixing::MeterProcessor& meter,
                                               float amplitude, double sample_rate,
                                               double duration_sec, int block_size,
                                               std::vector<float>* interleaved = nullptr) {
  const int total = static_cast<int>(sample_rate * duration_sec);
  std::vector<float> left(static_cast<size_t>(block_size));
  std::vector<float> right(static_cast<size_t>(block_size));
  float* channels[] = {left.data(), right.data()};
  if (interleaved != nullptr) {
    interleaved->clear();
    interleaved->reserve(static_cast<size_t>(total) * 2);
  }

  int n = 0;
  while (n < total) {
    const int block = std::min(block_size, total - n);
    for (int i = 0; i < block; ++i) {
      const float s =
          amplitude * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(n + i) /
                               static_cast<float>(sample_rate));
      left[static_cast<size_t>(i)] = s;
      right[static_cast<size_t>(i)] = s;
      if (interleaved != nullptr) {
        interleaved->push_back(s);
        interleaved->push_back(s);
      }
    }
    meter.process(channels, 2, block);
    n += block;
  }
  return meter.snapshot();
}

}  // namespace

TEST_CASE("MeterProcessor streaming LUFS obeys the energy doubling law", "[mixing]") {
  constexpr double kSr = 48000.0;
  constexpr int kBlock = 512;
  constexpr float kA = 0.25f;

  // Before the momentary window (400 ms) is full, momentary LUFS stays at the floor.
  {
    sonare::mixing::MeterProcessor warmup;
    warmup.prepare(kSr, kBlock);
    std::vector<float> l(kBlock);
    std::vector<float> r(kBlock);
    for (int i = 0; i < kBlock; ++i) {
      const float s = kA * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(i) /
                                    static_cast<float>(kSr));
      l[static_cast<size_t>(i)] = s;
      r[static_cast<size_t>(i)] = s;
    }
    float* ch[] = {l.data(), r.data()};
    // Two blocks ~= 21 ms, far short of the 400 ms momentary window.
    warmup.process(ch, 2, kBlock);
    warmup.process(ch, 2, kBlock);
    const auto early = warmup.snapshot();
    REQUIRE_THAT(early.momentary_lufs, WithinAbs(sonare::constants::kFloorDb, 0.001f));
  }

  std::vector<float> buffer;
  sonare::mixing::MeterProcessor meter_a;
  meter_a.prepare(kSr, kBlock);
  const auto snap_a = drive_meter_sine(meter_a, kA, kSr, 3.5, kBlock, &buffer);

  // Once filled, momentary/short-term are finite and well above the absolute gate.
  REQUIRE(std::isfinite(snap_a.momentary_lufs));
  REQUIRE(snap_a.momentary_lufs > -70.0f);
  REQUIRE(std::isfinite(snap_a.short_term_lufs));
  REQUIRE(snap_a.short_term_lufs > -70.0f);

  // For a steady tone the integrated loudness tracks momentary closely.
  REQUIRE(std::isfinite(snap_a.integrated_lufs));
  REQUIRE_THAT(snap_a.integrated_lufs, WithinAbs(snap_a.momentary_lufs, 1.0f));

  // Doubling the amplitude raises loudness by 20*log10(2) ~= 6.0206 dB, independent
  // of the K-weighting gain (a pure energy ratio on identical filtering).
  sonare::mixing::MeterProcessor meter_b;
  meter_b.prepare(kSr, kBlock);
  const auto snap_b = drive_meter_sine(meter_b, 2.0f * kA, kSr, 3.5, kBlock);

  REQUIRE_THAT(snap_b.momentary_lufs - snap_a.momentary_lufs, WithinAbs(6.0206f, 0.1f));

  // Optional reference-free cross-check against the offline meter on the same buffer.
  // metering/lufs.cpp is part of sonare_core, so this links without CMake changes.
  const auto offline = sonare::metering::lufs_interleaved(buffer.data(), buffer.size() / 2, 2,
                                                          static_cast<int>(kSr));
  REQUIRE_THAT(snap_a.momentary_lufs, WithinAbs(offline.momentary_lufs, 0.7f));
}

TEST_CASE("ChannelStrip EQ alters signal and position matters", "[mixing]") {
  static constexpr int kN = 256;
  auto make_input = [](std::vector<float>& l, std::vector<float>& r) {
    for (int i = 0; i < kN; ++i) {
      const float s =
          0.5f * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(i) / 48000.0f);
      l[static_cast<size_t>(i)] = s;
      r[static_cast<size_t>(i)] = s;
    }
  };

  SECTION("an enabled EQ band changes the output energy") {
    std::vector<float> bypass_l(kN);
    std::vector<float> bypass_r(kN);
    make_input(bypass_l, bypass_r);
    float* bypass[] = {bypass_l.data(), bypass_r.data()};

    sonare::mixing::ChannelStrip plain({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
    plain.prepare(48000.0, kN);
    plain.process(bypass, 2, kN);

    std::vector<float> eq_l(kN);
    std::vector<float> eq_r(kN);
    make_input(eq_l, eq_r);
    float* eqd[] = {eq_l.data(), eq_r.data()};

    sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
    sonare::mastering::eq::EqBand band;
    band.type = sonare::mastering::eq::EqBandType::Peak;
    band.frequency_hz = 1000.0f;
    band.gain_db = 12.0f;
    band.q = sonare::constants::kButterworthQ;
    band.enabled = true;
    strip.set_eq_band(0, band);
    strip.prepare(48000.0, kN);
    strip.process(eqd, 2, kN);

    float diff_energy = 0.0f;
    for (int i = 0; i < kN; ++i) {
      diff_energy += (eq_l[static_cast<size_t>(i)] - bypass_l[static_cast<size_t>(i)]) *
                     (eq_l[static_cast<size_t>(i)] - bypass_l[static_cast<size_t>(i)]);
    }
    REQUIRE(diff_energy > 1e-4f);
  }

  SECTION("EqPosition is configurable and routes the EQ relative to the fader") {
    sonare::mastering::eq::EqBand band;
    band.type = sonare::mastering::eq::EqBandType::Peak;
    band.frequency_hz = 1000.0f;
    band.gain_db = 12.0f;
    band.q = sonare::constants::kButterworthQ;
    band.enabled = true;

    std::vector<float> pre_l(kN);
    std::vector<float> pre_r(kN);
    make_input(pre_l, pre_r);
    float* pre[] = {pre_l.data(), pre_r.data()};
    sonare::mixing::ChannelStrip pre_strip({-6.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f,
                                            sonare::mixing::EqPosition::PreFader});
    REQUIRE(pre_strip.eq_position() == sonare::mixing::EqPosition::PreFader);
    pre_strip.set_eq_band(0, band);
    pre_strip.prepare(48000.0, kN);
    pre_strip.process(pre, 2, kN);

    std::vector<float> post_l(kN);
    std::vector<float> post_r(kN);
    make_input(post_l, post_r);
    float* post[] = {post_l.data(), post_r.data()};
    sonare::mixing::ChannelStrip post_strip({-6.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f,
                                             sonare::mixing::EqPosition::PostFader});
    REQUIRE(post_strip.eq_position() == sonare::mixing::EqPosition::PostFader);
    post_strip.set_eq_band(0, band);
    post_strip.prepare(48000.0, kN);
    post_strip.process(post, 2, kN);

    // The EQ stage is a linear biquad and the fader is a scalar gain; an LTI filter
    // commutes with scalar multiplication, so fader*EQ(x) == EQ(fader*x). Both orderings
    // therefore produce the same output here. (Position would matter once a nonlinear or
    // amplitude-dependent stage is inserted between EQ and fader.) Verify exact agreement
    // so this assertion catches any accidental non-commuting change in the routing.
    float max_diff = 0.0f;
    for (int i = 0; i < kN; ++i) {
      max_diff = std::max(max_diff,
                          std::abs(pre_l[static_cast<size_t>(i)] - post_l[static_cast<size_t>(i)]));
    }
    REQUIRE_THAT(max_diff, WithinAbs(0.0f, 1e-4f));
  }
}

TEST_CASE("ChannelStrip aux sends tap the correct stage", "[mixing]") {
  static constexpr int kN = 128;
  auto make_input = [](std::vector<float>& l, std::vector<float>& r) {
    for (int i = 0; i < kN; ++i) {
      const float s =
          0.5f * std::sin(sonare::constants::kTwoPi * 1000.0f * static_cast<float>(i) / 48000.0f);
      l[static_cast<size_t>(i)] = s;
      r[static_cast<size_t>(i)] = s;
    }
  };

  SECTION("post-fader send at 0 dB equals the post-fader output") {
    std::vector<float> in_l(kN);
    std::vector<float> in_r(kN);
    make_input(in_l, in_r);
    float* channels[] = {in_l.data(), in_r.data()};

    sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
    strip.prepare(48000.0, kN);
    const size_t idx = strip.add_send({0.0f, sonare::mixing::SendTiming::PostFader, 0.0f});
    strip.process(channels, 2, kN);

    std::vector<float> dest_l(kN, 0.0f);
    std::vector<float> dest_r(kN, 0.0f);
    float* dest[] = {dest_l.data(), dest_r.data()};
    strip.mix_send(idx, dest, 2, kN);

    for (int i = 0; i < kN; ++i) {
      REQUIRE_THAT(dest_l[static_cast<size_t>(i)], WithinAbs(in_l[static_cast<size_t>(i)], 1e-3f));
    }
  }

  SECTION("post-fader send at -6.0206 dB scales the output by 0.5") {
    std::vector<float> in_l(kN);
    std::vector<float> in_r(kN);
    make_input(in_l, in_r);
    float* channels[] = {in_l.data(), in_r.data()};

    sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
    strip.prepare(48000.0, kN);
    const size_t idx = strip.add_send({-6.0206f, sonare::mixing::SendTiming::PostFader, 0.0f});
    strip.process(channels, 2, kN);

    std::vector<float> dest_l(kN, 0.0f);
    std::vector<float> dest_r(kN, 0.0f);
    float* dest[] = {dest_l.data(), dest_r.data()};
    strip.mix_send(idx, dest, 2, kN);

    for (int i = 0; i < kN; ++i) {
      REQUIRE_THAT(dest_l[static_cast<size_t>(i)],
                   WithinAbs(0.5f * in_l[static_cast<size_t>(i)], 1e-3f));
    }
  }

  SECTION("pre-fader send taps the signal before the fader gain") {
    std::vector<float> input(kN);
    {
      std::vector<float> tmp(kN);
      make_input(input, tmp);
    }

    std::vector<float> in_l = input;
    std::vector<float> in_r = input;
    float* channels[] = {in_l.data(), in_r.data()};

    // -6 dB fader so pre-fader and post-fader taps differ. No EQ bands.
    sonare::mixing::ChannelStrip strip({-6.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
    strip.prepare(48000.0, kN);
    const size_t idx = strip.add_send({0.0f, sonare::mixing::SendTiming::PreFader, 0.0f});
    strip.process(channels, 2, kN);

    std::vector<float> dest_l(kN, 0.0f);
    std::vector<float> dest_r(kN, 0.0f);
    float* dest[] = {dest_l.data(), dest_r.data()};
    strip.mix_send(idx, dest, 2, kN);

    const float fader_gain = std::pow(10.0f, -6.0f / 20.0f);
    for (int i = 0; i < kN; ++i) {
      // Pre-fader tap equals the original input (no EQ, tapped before the fader).
      REQUIRE_THAT(dest_l[static_cast<size_t>(i)], WithinAbs(input[static_cast<size_t>(i)], 1e-3f));
      // Post-fader output (left, captured in channels) is the input scaled by the fader.
      REQUIRE_THAT(in_l[static_cast<size_t>(i)],
                   WithinAbs(fader_gain * input[static_cast<size_t>(i)], 1e-3f));
    }
    // The pre-fader send therefore differs from the post-fader output.
    float max_diff = 0.0f;
    for (int i = 0; i < kN; ++i) {
      max_diff = std::max(max_diff,
                          std::abs(dest_l[static_cast<size_t>(i)] - in_l[static_cast<size_t>(i)]));
    }
    REQUIRE(max_diff > 1e-3f);
  }
}

TEST_CASE("ChannelStrip applies send automation during mix_send_at", "[mixing]") {
  std::array<float, 6> left{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 6> right = left;
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, 6);
  const size_t send = strip.add_send({0.0f, sonare::mixing::SendTiming::PostFader, 0.0f});
  REQUIRE(strip.schedule_send_automation(send, 102, -6.0206f));
  REQUIRE_FALSE(strip.schedule_send_automation(send + 1, 102, -6.0206f));

  strip.process_at(channels, 2, 6, 100);

  std::array<float, 6> send_l{};
  std::array<float, 6> send_r{};
  float* dest[] = {send_l.data(), send_r.data()};
  strip.mix_send_at(send, dest, 2, 6, 100);

  for (int i = 0; i < 2; ++i) {
    REQUIRE_THAT(send_l[static_cast<size_t>(i)], WithinAbs(1.0f, 0.0001f));
    REQUIRE_THAT(send_r[static_cast<size_t>(i)], WithinAbs(1.0f, 0.0001f));
  }
  for (int i = 2; i < 6; ++i) {
    REQUIRE_THAT(send_l[static_cast<size_t>(i)], WithinAbs(0.5f, 0.0001f));
    REQUIRE_THAT(send_r[static_cast<size_t>(i)], WithinAbs(0.5f, 0.0001f));
  }
}

TEST_CASE("ChannelStrip discards stale send automation even when send is not mixed", "[mixing]") {
  std::array<float, 1> left{1.0f};
  std::array<float, 1> right{1.0f};
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, 1);
  const size_t send = strip.add_send({0.0f, sonare::mixing::SendTiming::PostFader, 0.0f});

  for (int i = 0; i < 1024; ++i) {
    REQUIRE(strip.schedule_send_automation(send, i, -6.0f));
  }
  REQUIRE_FALSE(strip.schedule_send_automation(send, 1024, -6.0f));

  strip.process_at(channels, 2, 1, 2048);
  REQUIRE(strip.schedule_send_automation(send, 2048, -3.0f));
}

// ============================================================================
// P1 regression test: ScopedNoDenormals guard on BusProcessor::process
// ============================================================================

TEST_CASE("BusProcessor silent input through IIR insert produces exact-zero output",
          "[mixing][bus][rt-safety]") {
  // Regression guard for the P1 fix that wraps BusProcessor::process in
  // rt::ScopedNoDenormals. An IIR insert (parametric low-shelf EQ here) fed a
  // long block of silence must produce an exact-zero output rather than
  // accumulating denormal floats — denormals would manifest as tiny non-zero
  // tail samples on x86 without DAZ/FTZ, and 10-100x CPU spikes in audio
  // callbacks. Mirrors the C-1 test for the voice changer (commit 4d34bbe).
  constexpr int kSampleRate = 48000;
  constexpr int kBlockSize = 4096;

  auto eq = std::make_unique<sonare::mastering::eq::ParametricEq>();
  sonare::mastering::eq::EqBand band;
  band.type = sonare::mastering::eq::EqBandType::LowShelf;
  band.frequency_hz = 100.0f;
  band.gain_db = 6.0f;
  band.q = sonare::constants::kButterworthQ;
  band.enabled = true;
  eq->set_band(0, band);

  sonare::mixing::BusProcessor bus(sonare::mixing::BusRole::Subgroup);
  bus.add_insert(std::move(eq));
  bus.prepare(static_cast<double>(kSampleRate), kBlockSize);

  std::array<float, kBlockSize> left{};
  std::array<float, kBlockSize> right{};
  left.fill(0.0f);
  right.fill(0.0f);
  float* channels[] = {left.data(), right.data()};

  bus.process(channels, 2, kBlockSize);

  for (int i = 0; i < kBlockSize; ++i) {
    REQUIRE(left[static_cast<size_t>(i)] == 0.0f);
    REQUIRE(right[static_cast<size_t>(i)] == 0.0f);
  }
}

TEST_CASE("ChannelStrip segmented pre and post meters integrate the same window", "[mixing]") {
  // Regression: in the segmented automation path of process_at(), the pre-fader
  // meter was driven with clamped_samples (min(num_samples, max_block_size_))
  // while the post-fader meter was driven with the full num_samples. When
  // num_samples > max_block_size_ the two meters integrated different lengths,
  // so their RMS/LUFS readings disagreed for the SAME block. The fix clamps the
  // post meter to the same window as the pre meter (pre_tap_ is only
  // max_block_size_ wide, so the pre meter can never see more than that).
  //
  // Build a block that is LOUD only over the first max_block_size_ samples and
  // SILENT afterward. RMS depends on the integration window, so:
  //   - pre meter integrates clamped_samples (the loud region) -> rms = loud
  //   - post meter, when consistent, integrates the same window -> rms = loud
  // If the post meter instead integrated the full num_samples (loud + silence)
  // its RMS would be measurably LOWER. Asserting pre.rms_db == post.rms_db
  // therefore proves both meters use the same window length.
  constexpr int kMaxBlock = 64;
  constexpr int kNumSamples = 256;  // > kMaxBlock, so the windows differed pre-fix.
  static_assert(kNumSamples > kMaxBlock, "test requires num_samples > max_block_size_");

  // Unity strip: 0 dB fader, center Linear0dB pan, width 1 — so the post-fader
  // output equals the pre-fader tap sample-for-sample and the two meters see the
  // same signal. Any window mismatch is then the ONLY source of an RMS gap.
  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, kMaxBlock);

  std::vector<float> left(kNumSamples, 0.0f);
  std::vector<float> right(kNumSamples, 0.0f);
  constexpr float kLoud = 0.5f;
  for (int i = 0; i < kMaxBlock; ++i) {
    left[static_cast<size_t>(i)] = kLoud;
    right[static_cast<size_t>(i)] = kLoud;
  }
  float* channels[] = {left.data(), right.data()};

  // Force the segmented path with a no-op fader automation event (target equals
  // the current 0 dB value, so the gain stays unity and the signal is unchanged)
  // scheduled within the block.
  REQUIRE(strip.schedule_fader_automation(0, 0.0f));
  strip.process_at(channels, 2, kNumSamples, 0);

  const auto pre = strip.meter_snapshot(sonare::mixing::TapPoint::PreFader);
  const auto post = strip.meter_snapshot(sonare::mixing::TapPoint::PostFader);
  REQUIRE(pre.seq == 1);
  REQUIRE(post.seq == 1);

  // The pre meter reflects the loud region only (it can never integrate more
  // than max_block_size_). The expected RMS of a constant kLoud over its window
  // is exactly kLoud.
  const float expected_rms_db = 20.0f * std::log10(kLoud);
  REQUIRE_THAT(pre.rms_db[0], WithinAbs(expected_rms_db, 0.01f));
  REQUIRE_THAT(pre.rms_db[1], WithinAbs(expected_rms_db, 0.01f));

  // Same window: the post meter must report the same RMS. (Pre-fix it would have
  // been ~6 dB lower because the silent tail diluted the full-block average.)
  REQUIRE_THAT(post.rms_db[0], WithinAbs(pre.rms_db[0], 0.01f));
  REQUIRE_THAT(post.rms_db[1], WithinAbs(pre.rms_db[1], 0.01f));

  // Peak is window-length insensitive here (the loud region dominates), so it
  // matches on both meters as a sanity check that the signals are identical.
  REQUIRE_THAT(post.peak_db[0], WithinAbs(pre.peak_db[0], 0.01f));
}
