/// @file graph_surround_test.cpp
/// @brief Surround processing in the standalone mixer graph and the project
///        bounce: master built at the output width, buses at their layout width,
///        strip scatter through SurroundPannerProcessor, narrower edges folded by
///        DownmixNode, and the bounce channel-count contract.

#include <sonare/sonare_c_engine.h>
#include <sonare/sonare_c_project_edit.h>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "c_api/mixing_internal.h"
#include "core/channel_layout.h"
#include "mixing/api/scene.h"
#include "mixing/downmix.h"
#include "util/constants.h"
#include "util/exception.h"

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;
constexpr int64_t kTotalFrames = kBlockSize * 40;
constexpr int64_t kSettleFrames = kBlockSize * 10;
constexpr uint32_t kDestination = 77;
constexpr uint32_t kTrackId = 10;
constexpr uint32_t kBusId = 1;
// Sentinel for render_engine_offline: the lane sums straight into the master.
constexpr int kNoBus = -1;
// max|a-b| <= kMetricScale * max(1, max|a|), the design doc's agreement bound.
constexpr double kMetricScale = 1.0e-6;
// Reference RMS floor: an agreement between two silent renders proves nothing.
constexpr double kMinRmsDb = -40.0;

// A lane placed off-centre with an LFE send, so every scatter plane is exercised.
constexpr const char* kSurroundPan =
    R"("surroundPan":{"azimuth":60.0,"divergence":0.25,"lfe":0.3})";

std::string track_strip_body() { return std::string(R"("id":"track",)") + kSurroundPan; }

std::string project_scene_json(const char* master_layout) {
  return std::string(R"({"version":1,"strips":[{)") + track_strip_body() +
         R"(}],"buses":[{"id":"master","role":"master","layout":")" + master_layout +
         R"("}],"connections":[]})";
}

SonareEngineBuiltinSynthConfig engine_synth() {
  SonareEngineBuiltinSynthConfig synth{};
  synth.waveform = 0;
  synth.gain = 1.0f;
  synth.attack_ms = 1.0f;
  synth.decay_ms = 1.0f;
  synth.sustain = 1.0f;
  synth.release_ms = 1.0f;
  synth.polyphony = 1;
  return synth;
}

SonareBuiltinSynthConfig project_synth() {
  SonareBuiltinSynthConfig synth{};
  synth.waveform = 0;
  synth.gain = 1.0f;
  synth.attack_ms = 1.0f;
  synth.decay_ms = 1.0f;
  synth.sustain = 1.0f;
  synth.release_ms = 1.0f;
  synth.polyphony = 1;
  return synth;
}

// Engine offline render of one surround-panned lane, returned interleaved at
// @p channels. With @p bus_layout the lane feeds one default bus of that layout,
// which sums into the master; otherwise it sums into the master directly.
std::vector<float> render_engine_offline(int channels, int bus_layout = kNoBus) {
  SonareRealtimeEngine* engine = nullptr;
  REQUIRE(sonare_engine_create(&engine) == SONARE_OK);
  REQUIRE(sonare_engine_prepare(engine, kSampleRate, kBlockSize, 64, 16) == SONARE_OK);
  uint32_t output_bus = 0;
  if (bus_layout != kNoBus) {
    SonareEngineBus bus{};
    bus.bus_id = kBusId;
    bus.channel_layout = static_cast<uint8_t>(bus_layout);
    REQUIRE(sonare_engine_set_track_buses(engine, &bus, 1) == SONARE_OK);
    output_bus = kBusId;
  }
  const SonareEngineTrackLane lane[] = {{kTrackId, nullptr, 0, output_bus, 1}};
  REQUIRE(sonare_engine_set_track_lanes(engine, lane, 1) == SONARE_OK);
  const std::string strip_json = std::string(R"({"version":1,"strips":[{)") + track_strip_body() +
                                 R"(}],"buses":[],"connections":[]})";
  REQUIRE(sonare_engine_set_track_strip_json(engine, kTrackId, strip_json.c_str()) == SONARE_OK);
  SonareEngineBuiltinSynthConfig synth = engine_synth();
  REQUIRE(sonare_engine_set_builtin_instrument(engine, kDestination, &synth) == SONARE_OK);
  const SonareEngineMidiEvent events[] = {{0, 0x20903C7Fu, 0u, 0u, 0u, 1u, 0u, 0u, 0u}};
  SonareEngineMidiClipSchedule clip{};
  clip.gain = 1.0f;
  clip.id = 1;
  clip.track_id = kTrackId;
  clip.start_sample = 0;
  clip.length_samples = kTotalFrames * 2;
  clip.destination_id = kDestination;
  clip.events = events;
  clip.event_count = std::size(events);
  REQUIRE(sonare_engine_set_midi_clips(engine, &clip, 1) == SONARE_OK);
  REQUIRE(sonare_engine_settle_parameters(engine) == SONARE_OK);

  std::vector<std::vector<float>> planes(static_cast<size_t>(channels),
                                         std::vector<float>(static_cast<size_t>(kTotalFrames)));
  std::vector<float*> ptrs;
  for (auto& plane : planes) ptrs.push_back(plane.data());
  REQUIRE(sonare_engine_render_offline(engine, ptrs.data(), channels, kTotalFrames, kBlockSize) ==
          SONARE_OK);
  sonare_engine_destroy(engine);
  std::vector<float> interleaved(static_cast<size_t>(kTotalFrames * channels));
  for (int64_t f = 0; f < kTotalFrames; ++f) {
    for (int ch = 0; ch < channels; ++ch) {
      interleaved[static_cast<size_t>(f * channels + ch)] =
          planes[static_cast<size_t>(ch)][static_cast<size_t>(f)];
    }
  }
  return interleaved;
}

SonareProject* make_project(const std::string& scene_json) {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, kSampleRate) == SONARE_OK);
  REQUIRE(sonare_project_set_mixer_scene_json(project, scene_json.c_str()) == SONARE_OK);
  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 8.0, &track, &clip) == SONARE_OK);
  const SonareMidiEventPod events[] = {{0.0, 0x20903C7Fu, 0u}};
  REQUIRE(sonare_project_set_midi_events(project, clip, events, std::size(events)) == SONARE_OK);
  REQUIRE(sonare_project_set_track_midi_destination(project, track, kDestination) == SONARE_OK);
  REQUIRE(sonare_project_set_track_route(project, track, "track", "") == SONARE_OK);
  return project;
}

SonareError bounce(SonareProject* project, int channels, std::vector<float>* out_samples) {
  SonareProjectBounceOptions options{};
  options.total_frames = kTotalFrames;
  options.block_size = kBlockSize;
  options.num_channels = channels;
  options.sample_rate = static_cast<int>(kSampleRate);
  SonareBuiltinInstrumentBinding binding{};
  binding.destination_id = kDestination;
  binding.config = project_synth();
  float* out = nullptr;
  size_t out_len = 0;
  const SonareError err = sonare_project_bounce_with_builtin_instruments(
      project, &options, &binding, 1, &out, &out_len);
  if (err == SONARE_OK && out_samples != nullptr) out_samples->assign(out, out + out_len);
  if (out != nullptr) sonare_free_floats(out);
  return err;
}

struct Agreement {
  double max_diff = 0.0;
  double bound = 0.0;
  double rms_db = -300.0;
  bool ok = false;
};

Agreement check_agreement(const std::vector<float>& reference, const std::vector<float>& other,
                          int channels, int64_t settle_frames = kSettleFrames) {
  Agreement result;
  const size_t begin = static_cast<size_t>(settle_frames * channels);
  const size_t n = std::min(reference.size(), other.size());
  double max_ref = 0.0;
  double sum_sq = 0.0;
  for (size_t i = begin; i < n; ++i) {
    result.max_diff =
        std::max(result.max_diff, static_cast<double>(std::abs(reference[i] - other[i])));
    max_ref = std::max(max_ref, static_cast<double>(std::abs(reference[i])));
    sum_sq += static_cast<double>(reference[i]) * reference[i];
  }
  const double count = static_cast<double>(n > begin ? n - begin : 1);
  result.rms_db = 10.0 * std::log10(std::max(sum_sq / count, 1.0e-30));
  result.bound = kMetricScale * std::max(1.0, max_ref);
  result.ok = reference.size() == other.size() && result.max_diff <= result.bound;
  return result;
}

double plane_peak(const std::vector<float>& interleaved, int channels, int plane) {
  double peak = 0.0;
  for (size_t i = static_cast<size_t>(plane); i < interleaved.size();
       i += static_cast<size_t>(channels)) {
    peak = std::max(peak, static_cast<double>(std::abs(interleaved[i])));
  }
  return peak;
}

// Standalone-mixer helpers that drive the internal planar entry.
struct MixerHandle {
  SonareMixer* mixer = nullptr;
  ~MixerHandle() {
    if (mixer != nullptr) sonare_mixer_destroy(mixer);
  }
};

// A 440 Hz tone on L and a 660 Hz tone on R, one block at @p block_index.
void fill_input(int block_index, std::vector<float>* left, std::vector<float>* right) {
  for (int i = 0; i < kBlockSize; ++i) {
    const double t = static_cast<double>(block_index * kBlockSize + i) / kSampleRate;
    (*left)[static_cast<size_t>(i)] =
        static_cast<float>(0.5 * std::sin(sonare::constants::kTwoPiD * 440.0 * t));
    (*right)[static_cast<size_t>(i)] =
        static_cast<float>(0.25 * std::sin(sonare::constants::kTwoPiD * 660.0 * t));
  }
}

// Renders @p blocks blocks through @p scene_json at @p out_channels, planar.
std::vector<std::vector<float>> render_mixer(const std::string& scene_json, int out_channels,
                                             int blocks) {
  MixerHandle handle;
  handle.mixer =
      sonare_mixer_from_scene_json(scene_json.c_str(), static_cast<int>(kSampleRate), kBlockSize);
  REQUIRE(handle.mixer != nullptr);
  sonare_c_mixing_detail::set_output_channels(handle.mixer, out_channels);
  const size_t strips = sonare_mixer_strip_count(handle.mixer);
  std::vector<float> left(kBlockSize);
  std::vector<float> right(kBlockSize);
  std::vector<const float*> in_l(strips, left.data());
  std::vector<const float*> in_r(strips, right.data());
  std::vector<std::vector<float>> planes(
      static_cast<size_t>(out_channels),
      std::vector<float>(static_cast<size_t>(blocks) * kBlockSize));
  std::vector<float*> out(static_cast<size_t>(out_channels));
  for (int b = 0; b < blocks; ++b) {
    fill_input(b, &left, &right);
    for (int ch = 0; ch < out_channels; ++ch) {
      out[static_cast<size_t>(ch)] =
          planes[static_cast<size_t>(ch)].data() + static_cast<size_t>(b) * kBlockSize;
    }
    REQUIRE(sonare_c_mixing_detail::process_planar(handle.mixer, in_l.data(), in_r.data(), strips,
                                                   out.data(), out_channels,
                                                   kBlockSize) == SONARE_OK);
  }
  return planes;
}

}  // namespace

TEST_CASE("a 5.1-master scene bounces at 6 channels and matches the engine's 6-channel render",
          "[graph_surround]") {
  SonareProject* project = make_project(project_scene_json("5.1"));
  std::vector<float> bounced;
  REQUIRE(bounce(project, 6, &bounced) == SONARE_OK);
  sonare_project_destroy(project);
  REQUIRE(bounced.size() == static_cast<size_t>(kTotalFrames * 6));

  const std::vector<float> engine = render_engine_offline(6);
  const Agreement agreement = check_agreement(engine, bounced, 6);
  INFO("engine vs bounce @6ch: max_diff=" << agreement.max_diff << " bound=" << agreement.bound
                                          << " rms_db=" << agreement.rms_db);
  CHECK(agreement.rms_db >= kMinRmsDb);
  CHECK(agreement.ok);
  // Both sides snap the scatter on their first block, so they agree from frame 0.
  const Agreement from_start = check_agreement(engine, bounced, 6, 0);
  INFO("from frame 0: max_diff=" << from_start.max_diff << " bound=" << from_start.bound);
  CHECK(from_start.ok);
  // The scatter reached the planes a front-pair copy would leave silent.
  CHECK(plane_peak(bounced, 6, 2) > 1.0e-3);  // C
  CHECK(plane_peak(bounced, 6, 3) > 1.0e-3);  // LFE
}

TEST_CASE("a 5.1-master scene bounced at 2 channels matches the engine's stereo render",
          "[graph_surround]") {
  SonareProject* project = make_project(project_scene_json("5.1"));
  std::vector<float> bounced;
  REQUIRE(bounce(project, 2, &bounced) == SONARE_OK);
  sonare_project_destroy(project);
  const std::vector<float> engine = render_engine_offline(2);
  const Agreement agreement = check_agreement(engine, bounced, 2);
  INFO("engine vs bounce @2ch: max_diff=" << agreement.max_diff << " bound=" << agreement.bound
                                          << " rms_db=" << agreement.rms_db);
  CHECK(agreement.rms_db >= kMinRmsDb);
  CHECK(agreement.ok);
}

TEST_CASE("project bounce refuses channel counts outside {1,2,6,8} or wider than the master",
          "[graph_surround]") {
  SonareProject* surround = make_project(project_scene_json("5.1"));
  CHECK(bounce(surround, 8, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(bounce(surround, 3, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(bounce(surround, 1, nullptr) == SONARE_OK);
  sonare_project_destroy(surround);

  SonareProject* stereo = make_project(project_scene_json("stereo"));
  CHECK(bounce(stereo, 6, nullptr) == SONARE_ERROR_INVALID_PARAMETER);
  CHECK(bounce(stereo, 2, nullptr) == SONARE_OK);
  sonare_project_destroy(stereo);
}

TEST_CASE("a surround bus folded into a stereo master equals mixing::downmix of its planes",
          "[graph_surround]") {
  // track -> 5.1 bus -> master. At 6 channels the bus reaches the master plane
  // for plane; at 2 channels the same bus passes through a DownmixNode.
  const std::string scene = std::string(R"({"version":1,"strips":[{)") + track_strip_body() +
                            R"(}],"buses":[{"id":"surr","layout":"5.1"},)" +
                            R"({"id":"master","role":"master","layout":"5.1"}],)" +
                            R"("connections":[{"source":"track","destination":"surr"},)" +
                            R"({"source":"surr","destination":"master"}]})";
  constexpr int kBlocks = 8;
  const auto wide = render_mixer(scene, 6, kBlocks);
  const auto narrow = render_mixer(scene, 2, kBlocks);

  const size_t n = static_cast<size_t>(kBlocks) * kBlockSize;
  std::vector<float> folded_l(n);
  std::vector<float> folded_r(n);
  const float* in[6];
  for (int ch = 0; ch < 6; ++ch) in[ch] = wide[static_cast<size_t>(ch)].data();
  float* out[2] = {folded_l.data(), folded_r.data()};
  sonare::mixing::downmix(sonare::ChannelLayout::FivePointOne, sonare::ChannelLayout::Stereo, in,
                          out, n);

  double max_diff = 0.0;
  double max_ref = 0.0;
  for (size_t i = 0; i < n; ++i) {
    max_diff = std::max(max_diff, static_cast<double>(std::abs(narrow[0][i] - folded_l[i])));
    max_diff = std::max(max_diff, static_cast<double>(std::abs(narrow[1][i] - folded_r[i])));
    max_ref = std::max(max_ref, static_cast<double>(std::abs(folded_l[i])));
  }
  INFO("max_diff=" << max_diff << " max_ref=" << max_ref);
  CHECK(max_ref > 1.0e-2);
  CHECK(max_diff <= kMetricScale * std::max(1.0, max_ref));
  // Non-vacuity: the fold is not a front-pair copy (C and surround contributed).
  double front_copy_diff = 0.0;
  for (size_t i = 0; i < n; ++i) {
    front_copy_diff =
        std::max(front_copy_diff, static_cast<double>(std::abs(narrow[0][i] - wide[0][i])));
  }
  CHECK(front_copy_diff > 1.0e-3);
}

TEST_CASE("a stereo bus into a 5.1 master lands on the front pair only", "[graph_surround]") {
  const std::string scene =
      std::string(R"({"version":1,"strips":[{"id":"track"}],)") +
      R"("buses":[{"id":"st"},{"id":"master","role":"master","layout":"5.1"}],)" +
      R"("connections":[{"source":"track","destination":"st"},)" +
      R"({"source":"st","destination":"master"}]})";
  const auto planes = render_mixer(scene, 6, 4);
  double front = 0.0;
  for (float v : planes[0]) front = std::max(front, static_cast<double>(std::abs(v)));
  CHECK(front > 1.0e-2);
  for (int ch = 2; ch < 6; ++ch) {
    for (float v : planes[static_cast<size_t>(ch)]) REQUIRE(v == 0.0f);
  }
}

TEST_CASE("scene validation refuses a non-default width on a surround bus", "[graph_surround]") {
  const std::string surround_width =
      R"({"version":1,"strips":[],"buses":[{"id":"master","role":"master","layout":"5.1","width":1.5}],"connections":[]})";
  CHECK_THROWS_AS(sonare::mixing::api::scene_from_json(surround_width), sonare::SonareException);
  CHECK(sonare_mixer_from_scene_json(surround_width.c_str(), static_cast<int>(kSampleRate),
                                     kBlockSize) == nullptr);
  const std::string stereo_width =
      R"({"version":1,"strips":[],"buses":[{"id":"master","role":"master","width":1.5}],"connections":[]})";
  CHECK_NOTHROW(sonare::mixing::api::scene_from_json(stereo_width));
}

TEST_CASE("the public stereo mixer builds a 5.1-master scene at two channels", "[graph_surround]") {
  // A default strip into a 2-wide master is a plain stereo sum: no scatter, so
  // the output is the input to within the strip's unity gain stage.
  const std::string scene = std::string(R"({"version":1,"strips":[{)") + track_strip_body() +
                            R"(}],"buses":[{"id":"master","role":"master","layout":"5.1"}],)" +
                            R"("connections":[]})";
  MixerHandle handle;
  handle.mixer =
      sonare_mixer_from_scene_json(scene.c_str(), static_cast<int>(kSampleRate), kBlockSize);
  REQUIRE(handle.mixer != nullptr);
  std::vector<float> left(kBlockSize);
  std::vector<float> right(kBlockSize);
  fill_input(0, &left, &right);
  const float* in_l[] = {left.data()};
  const float* in_r[] = {right.data()};
  std::vector<float> out_l(kBlockSize);
  std::vector<float> out_r(kBlockSize);
  REQUIRE(sonare_mixer_process_stereo(handle.mixer, in_l, in_r, 1, out_l.data(), out_r.data(),
                                      kBlockSize) == SONARE_OK);
  for (int i = 0; i < kBlockSize; ++i) {
    REQUIRE(std::abs(out_l[static_cast<size_t>(i)] - left[static_cast<size_t>(i)]) <= kMetricScale);
    REQUIRE(std::abs(out_r[static_cast<size_t>(i)] - right[static_cast<size_t>(i)]) <=
            kMetricScale);
  }
  const sonare::graph::Node* master = handle.mixer->graph.node(handle.mixer->master_id);
  REQUIRE(master != nullptr);
  CHECK(master->num_ports() == 2);
}

TEST_CASE("an unpatched explicit surround bus stays silent at six channels", "[graph_surround]") {
  const std::string scene = std::string(R"({"version":1,"strips":[{)") + track_strip_body() +
                            R"(}],"buses":[{"id":"aux","layout":"5.1"},)" +
                            R"({"id":"master","role":"master","layout":"5.1"}],)" +
                            R"("connections":[{"source":"track","destination":"aux"}]})";
  // Non-vacuity: the strip is fed a real signal, so the silence is the routing's.
  std::vector<float> left(kBlockSize);
  std::vector<float> right(kBlockSize);
  fill_input(1, &left, &right);
  double input_peak = 0.0;
  for (float v : left) input_peak = std::max(input_peak, static_cast<double>(std::abs(v)));
  REQUIRE(input_peak > 0.1);
  const auto planes = render_mixer(scene, 6, 4);
  for (const auto& plane : planes) {
    for (float v : plane) REQUIRE(v == 0.0f);
  }
}

TEST_CASE("a 7.1-master scene bounces at 8 channels and matches the engine's 8-channel render",
          "[graph_surround]") {
  SonareProject* project = make_project(project_scene_json("7.1"));
  std::vector<float> bounced;
  REQUIRE(bounce(project, 8, &bounced) == SONARE_OK);
  sonare_project_destroy(project);
  REQUIRE(bounced.size() == static_cast<size_t>(kTotalFrames * 8));
  const std::vector<float> engine = render_engine_offline(8);
  const Agreement agreement = check_agreement(engine, bounced, 8);
  INFO("engine vs bounce @8ch: max_diff=" << agreement.max_diff << " bound=" << agreement.bound
                                          << " rms_db=" << agreement.rms_db);
  CHECK(agreement.rms_db >= kMinRmsDb);
  CHECK(agreement.ok);
  CHECK(check_agreement(engine, bounced, 8, 0).ok);
}

TEST_CASE("a 7.1 bus into a 5.1 master folds through a DownmixNode and matches the engine",
          "[graph_surround]") {
  // Default pan and width on the surround bus, so the comparison is independent
  // of how either side treats those stages above two planes.
  const std::string scene = std::string(R"({"version":1,"strips":[{)") + track_strip_body() +
                            R"(}],"buses":[{"id":"surr","layout":"7.1"},)" +
                            R"({"id":"master","role":"master","layout":"5.1"}],)" +
                            R"("connections":[{"source":"track","destination":"surr"},)" +
                            R"({"source":"surr","destination":"master"}]})";
  SonareProject* project = make_project(scene);
  std::vector<float> bounced;
  REQUIRE(bounce(project, 6, &bounced) == SONARE_OK);
  sonare_project_destroy(project);
  const std::vector<float> engine =
      render_engine_offline(6, static_cast<int>(sonare::ChannelLayout::SevenPointOne));
  const Agreement agreement = check_agreement(engine, bounced, 6);
  INFO("engine vs bounce, 7.1 bus -> 5.1 master: max_diff="
       << agreement.max_diff << " bound=" << agreement.bound << " rms_db=" << agreement.rms_db);
  CHECK(agreement.rms_db >= kMinRmsDb);
  CHECK(agreement.ok);
}

TEST_CASE("a mixer refuses an output width wider than its master layout", "[graph_surround]") {
  const std::string scene =
      R"({"version":1,"strips":[{"id":"track"}],"buses":[{"id":"master","role":"master"}],"connections":[]})";
  MixerHandle handle;
  handle.mixer =
      sonare_mixer_from_scene_json(scene.c_str(), static_cast<int>(kSampleRate), kBlockSize);
  REQUIRE(handle.mixer != nullptr);
  sonare_c_mixing_detail::set_output_channels(handle.mixer, 6);
  std::vector<float> left(kBlockSize, 0.1f);
  std::vector<float> right(kBlockSize, 0.1f);
  const float* in_l[] = {left.data()};
  const float* in_r[] = {right.data()};
  std::vector<std::vector<float>> planes(6, std::vector<float>(kBlockSize));
  std::vector<float*> out;
  for (auto& plane : planes) out.push_back(plane.data());
  CHECK(sonare_c_mixing_detail::process_planar(handle.mixer, in_l, in_r, 1, out.data(), 6,
                                               kBlockSize) == SONARE_ERROR_INVALID_PARAMETER);
  // The same mixer at the width its layout allows still renders.
  sonare_c_mixing_detail::set_output_channels(handle.mixer, 2);
  CHECK(sonare_c_mixing_detail::process_planar(handle.mixer, in_l, in_r, 1, out.data(), 2,
                                               kBlockSize) == SONARE_OK);
}
