/// @file mixing_meter_panner_capi_test.cpp
/// @brief Mixing meter, panner, and C API tests.

#include "mixing_test_helpers.h"

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

TEST_CASE("Mixing C API removes a strip send", "[mixing][capi]") {
  SonareMixer* mixer = sonare_mixer_create(48000, 8);
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

TEST_CASE("Mixing C API preserves surround pan in scene JSON", "[mixing][capi]") {
  SonareMixer* mixer = sonare_mixer_create(48000, 8);
  REQUIRE(mixer != nullptr);
  SonareStrip* strip = sonare_mixer_add_strip(mixer, "surround");
  REQUIRE(strip != nullptr);

  REQUIRE(sonare_strip_set_surround_pan(nullptr, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  SonareSurroundPan pan{};
  pan.azimuth = -45.0f;
  pan.divergence = 0.25f;
  pan.lfe = 0.5f;
  pan.distance = 1.0f;
  REQUIRE(sonare_strip_set_surround_pan(strip, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  REQUIRE(sonare_strip_set_surround_pan(strip, &pan) == SONARE_OK);

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
  char* restored_json = nullptr;
  REQUIRE(sonare_mixer_to_scene_json(restored, &restored_json) == SONARE_OK);
  REQUIRE(restored_json != nullptr);
  const std::string restored_scene_json(restored_json);
  REQUIRE(restored_scene_json.find("\"azimuth\":-45") != std::string::npos);
  REQUIRE(restored_scene_json.find("\"divergence\":0.25") != std::string::npos);
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

TEST_CASE("Mixing C API set_pan keep-mode sentinel preserves current pan mode", "[mixing][capi]") {
  SonareMixer* mixer = sonare_mixer_create(48000, 8);
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
  // Mode stays StereoPan (panMode:1), not reset to Balance (panMode:0).
  REQUIRE(scene_json.find("\"panMode\":1") != std::string::npos);
  sonare_free_string(json);

  sonare_mixer_destroy(mixer);
}

TEST_CASE("Mixing C API reports invalid scene JSON through last error", "[mixing][capi]") {
  SonareMixer* mixer = sonare_mixer_from_scene_json("{\"strips\":[", 48000, 8);
  REQUIRE(mixer == nullptr);
  REQUIRE(std::string(sonare_last_error_message()).find("expected") != std::string::npos);
}
