#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "playback/renderer.h"

using sonare::ChannelLayout;
using sonare::SpeakerRole;
using namespace sonare::playback;

namespace {

constexpr int kRate = 48000;
constexpr int kBlock = 512;
/// Frames analysed after the latency; the render runs latency + this.
constexpr int kAnalysisFrames = 2400;
constexpr double kFedFloorDb = -60.0;

/// Seeded pink noise at -20 dBFS RMS (Kellet's refined filter).
std::vector<float> pink_noise(size_t frames, uint32_t seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> white(0.0f, 1.0f);
  float b[7] = {};
  std::vector<float> out(frames);
  double power = 0.0;
  for (float& s : out) {
    const float w = white(rng);
    b[0] = 0.99886f * b[0] + w * 0.0555179f;
    b[1] = 0.99332f * b[1] + w * 0.0750759f;
    b[2] = 0.96900f * b[2] + w * 0.1538520f;
    b[3] = 0.86650f * b[3] + w * 0.3104856f;
    b[4] = 0.55000f * b[4] + w * 0.5329522f;
    b[5] = -0.7616f * b[5] - w * 0.0168980f;
    s = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f;
    b[6] = w * 0.115926f;
    power += static_cast<double>(s) * s;
  }
  const double rms = std::sqrt(power / static_cast<double>(frames));
  const float gain = static_cast<float>(0.1 / rms);
  for (float& s : out) s *= gain;
  return out;
}

enum class Target { StereoSpeakers, Speakers51, Speakers71, Headphones };
enum class Bass { Off, OnSub, OnNoSub };

struct Row {
  InputLayout input;
  Target target;
  bool upmix;
  RoomPreset room;
  float night;
  Bass bass;
};

const char* target_name(Target target) {
  switch (target) {
    case Target::StereoSpeakers:
      return "stereo_spk";
    case Target::Speakers51:
      return "spk_5.1";
    case Target::Speakers71:
      return "spk_7.1";
    case Target::Headphones:
      return "headphones";
  }
  return "";
}

std::string describe(const Row& row) {
  return std::string(input_layout_name(row.input)) + " -> " + target_name(row.target) +
         " upmix=" + (row.upmix ? "on" : "off") + " room=" + room_preset_name(row.room) +
         " night=" + std::to_string(row.night) + " bass=" +
         (row.bass == Bass::Off     ? "off"
          : row.bass == Bass::OnSub ? "on_sub"
                                    : "on_nosub");
}

ChannelLayout speaker_layout(Target target) {
  switch (target) {
    case Target::StereoSpeakers:
      return ChannelLayout::Stereo;
    case Target::Speakers51:
      return ChannelLayout::FivePointOne;
    case Target::Speakers71:
    case Target::Headphones:
      return ChannelLayout::SevenPointOne;
  }
  return ChannelLayout::Stereo;
}

bool is_surround_role(SpeakerRole role) {
  return role == SpeakerRole::Ls || role == SpeakerRole::Rs || role == SpeakerRole::Lss ||
         role == SpeakerRole::Rss;
}

/// on_sub marks the surround speakers small (so the sub feed carries their low
/// band); on_nosub keeps every speaker large, as the model's constraint requires.
RendererConfig config_for(const Row& row, InputLayout input) {
  RendererConfig config;
  config.prepare.input_layout = input;
  config.prepare.room_preset = row.room;
  config.realtime.upmix_enabled = row.upmix;
  config.realtime.night_amount = row.night;
  if (row.target == Target::Headphones) return config;
  config.prepare.target_kind = TargetKind::Speakers;
  config.prepare.target_layout = speaker_layout(row.target);
  BassManagementConfig& bass = config.prepare.bass_management;
  bass.enabled = row.bass != Bass::Off;
  bass.subwoofer = row.bass != Bass::OnNoSub;
  if (row.bass == Bass::OnSub) {
    const ChannelLayout layout = config.prepare.target_layout;
    for (int p = 0; p < sonare::channel_count(layout); ++p) {
      const SpeakerRole role = sonare::speaker_roles(layout)[p];
      if (is_surround_role(role)) {
        config.prepare.speakers[static_cast<size_t>(role)].size = SpeakerSize::Small;
      }
    }
  }
  return config;
}

enum class Plane { Fed, Zero };

/// Per-plane expectation from the conversion table of the design, with the
/// bass-management rules applied on top.
std::vector<Plane> expected_planes(const Row& row) {
  if (row.target == Target::Headphones || row.target == Target::StereoSpeakers) {
    return {Plane::Fed, Plane::Fed};
  }
  const ChannelLayout out = speaker_layout(row.target);
  const int count = sonare::channel_count(out);
  const SpeakerRole* roles = sonare::speaker_roles(out);
  std::vector<Plane> planes(static_cast<size_t>(count), Plane::Zero);
  for (int p = 0; p < count; ++p) {
    const SpeakerRole role = roles[p];
    Plane& plane = planes[static_cast<size_t>(p)];
    switch (row.input) {
      case InputLayout::Mono:
        plane = role == SpeakerRole::C ? Plane::Fed : Plane::Zero;
        break;
      case InputLayout::Stereo:
        // Upmixed: every plane but LFE (no `lfe_from_upmix` here) carries sound.
        if (role == SpeakerRole::L || role == SpeakerRole::R) {
          plane = Plane::Fed;
        } else {
          plane = row.upmix && role != SpeakerRole::LFE ? Plane::Fed : Plane::Zero;
        }
        break;
      case InputLayout::FivePointOne:
        plane = out == ChannelLayout::SevenPointOne &&
                        (role == SpeakerRole::Ls || role == SpeakerRole::Rs)
                    ? Plane::Zero
                    : Plane::Fed;
        break;
      case InputLayout::SevenPointOne:
      case InputLayout::Auto:
        plane = Plane::Fed;
        break;
    }
  }
  const int lfe = sonare::lfe_index(out);
  if (row.bass == Bass::OnNoSub) {
    planes[static_cast<size_t>(lfe)] = Plane::Zero;
  } else if (row.bass == Bass::OnSub) {
    bool small_fed = false;
    for (int p = 0; p < count; ++p) {
      small_fed =
          small_fed || (is_surround_role(roles[p]) && planes[static_cast<size_t>(p)] == Plane::Fed);
    }
    if (small_fed) planes[static_cast<size_t>(lfe)] = Plane::Fed;
  }
  return planes;
}

int input_channels(InputLayout input) { return sonare::channel_count(to_channel_layout(input)); }

/// Renders uncorrelated -20 dBFS pink noise (one seed per input plane) and
/// returns the output planes.
std::vector<std::vector<float>> render(const Row& row, InputLayout layout, int* latency) {
  PlaybackRenderer renderer(config_for(row, layout), nullptr, kRate, kBlock);
  *latency = renderer.latency_samples();
  const int in_count = input_channels(row.input);
  const int out_count = renderer.output_channel_count();
  const int total = ((*latency + kAnalysisFrames) / kBlock + 1) * kBlock;
  std::vector<std::vector<float>> in;
  for (int ch = 0; ch < in_count; ++ch) {
    in.push_back(pink_noise(static_cast<size_t>(total), 101u + static_cast<uint32_t>(ch)));
  }
  std::vector<std::vector<float>> out(static_cast<size_t>(out_count),
                                      std::vector<float>(static_cast<size_t>(total), 0.0f));
  std::vector<const float*> in_ptrs(static_cast<size_t>(in_count));
  std::vector<float*> out_ptrs(static_cast<size_t>(out_count));
  for (int start = 0; start < total; start += kBlock) {
    for (int ch = 0; ch < in_count; ++ch) {
      in_ptrs[static_cast<size_t>(ch)] = in[static_cast<size_t>(ch)].data() + start;
    }
    for (int ch = 0; ch < out_count; ++ch) {
      out_ptrs[static_cast<size_t>(ch)] = out[static_cast<size_t>(ch)].data() + start;
    }
    REQUIRE(renderer.process_planar(in_ptrs.data(), in_count, out_ptrs.data(), out_count, kBlock));
  }
  return out;
}

void check_row(const Row& row) {
  INFO(describe(row));
  int latency = 0;
  const auto out = render(row, row.input, &latency);
  const std::vector<Plane> expected = expected_planes(row);
  REQUIRE(out.size() == expected.size());
  for (size_t p = 0; p < out.size(); ++p) {
    INFO("plane " << p);
    const std::vector<float>& plane = out[p];
    bool finite = true;
    double total_energy = 0.0;
    double tail_energy = 0.0;
    for (size_t i = 0; i < plane.size(); ++i) {
      const double s = plane[i];
      finite = finite && std::isfinite(plane[i]);
      total_energy += s * s;
      if (static_cast<int>(i) >= latency) tail_energy += s * s;
    }
    CHECK(finite);
    const double frames = static_cast<double>(plane.size()) - latency;
    const double rms_db = 10.0 * std::log10(tail_energy / frames + 1e-30);
    if (expected[p] == Plane::Fed) {
      CHECK(rms_db > kFedFloorDb);
    } else {
      CHECK(total_energy == 0.0);
    }
  }
}

constexpr InputLayout kInputs[] = {InputLayout::Mono, InputLayout::Stereo,
                                   InputLayout::FivePointOne, InputLayout::SevenPointOne};
constexpr Target kTargets[] = {Target::StereoSpeakers, Target::Speakers51, Target::Speakers71,
                               Target::Headphones};

std::vector<Row> all_pairs() {
  std::vector<Row> rows;
  for (InputLayout input : kInputs) {
    for (Target target : kTargets) {
      const RoomPreset room =
          target == Target::Headphones ? RoomPreset::LivingRoom : RoomPreset::None;
      rows.push_back({input, target, true, room, 0.0f, Bass::Off});
    }
  }
  return rows;
}

// Pairwise (strength 2) cover of input x target x upmix x room x night x
// bass_mgmt under the model's constraints, generated once and fixed here.
const Row kCoverRows[] = {
    {InputLayout::Mono, Target::Headphones, true, RoomPreset::LivingRoom, 1.0f, Bass::Off},
    {InputLayout::SevenPointOne, Target::Speakers51, false, RoomPreset::None, 1.0f, Bass::OnNoSub},
    {InputLayout::FivePointOne, Target::Speakers71, false, RoomPreset::None, 0.0f, Bass::Off},
    {InputLayout::Stereo, Target::Speakers71, true, RoomPreset::None, 0.0f, Bass::OnSub},
    {InputLayout::SevenPointOne, Target::Headphones, true, RoomPreset::LivingRoom, 0.0f, Bass::Off},
    {InputLayout::Stereo, Target::StereoSpeakers, false, RoomPreset::None, 1.0f, Bass::Off},
    {InputLayout::Mono, Target::StereoSpeakers, true, RoomPreset::None, 0.0f, Bass::Off},
    {InputLayout::FivePointOne, Target::Speakers51, true, RoomPreset::None, 1.0f, Bass::OnNoSub},
    {InputLayout::Stereo, Target::Headphones, false, RoomPreset::LivingRoom, 1.0f, Bass::Off},
    {InputLayout::FivePointOne, Target::Headphones, true, RoomPreset::None, 1.0f, Bass::Off},
    {InputLayout::Mono, Target::Speakers51, false, RoomPreset::None, 1.0f, Bass::OnSub},
    {InputLayout::Mono, Target::Speakers71, true, RoomPreset::None, 1.0f, Bass::OnNoSub},
    {InputLayout::Stereo, Target::Speakers51, false, RoomPreset::None, 0.0f, Bass::Off},
    {InputLayout::Stereo, Target::Speakers51, false, RoomPreset::None, 0.0f, Bass::OnNoSub},
    {InputLayout::FivePointOne, Target::StereoSpeakers, true, RoomPreset::None, 0.0f, Bass::Off},
    {InputLayout::SevenPointOne, Target::Speakers71, false, RoomPreset::None, 0.0f, Bass::OnSub},
    {InputLayout::SevenPointOne, Target::StereoSpeakers, false, RoomPreset::None, 0.0f, Bass::Off},
    {InputLayout::FivePointOne, Target::Headphones, false, RoomPreset::LivingRoom, 1.0f, Bass::Off},
    {InputLayout::FivePointOne, Target::Speakers71, true, RoomPreset::None, 0.0f, Bass::OnSub},
};

void check_auto_matches_fixed(const Row& row) {
  INFO(describe(row));
  int fixed_latency = 0;
  int auto_latency = 0;
  const auto fixed = render(row, row.input, &fixed_latency);
  const auto automatic = render(row, InputLayout::Auto, &auto_latency);
  CHECK(fixed_latency == auto_latency);
  REQUIRE(fixed.size() == automatic.size());
  for (size_t p = 0; p < fixed.size(); ++p) {
    INFO("plane " << p);
    CHECK(fixed[p] == automatic[p]);
  }
}

}  // namespace

TEST_CASE("every input x target pair feeds the table's planes", "[playback][matrix]") {
  for (const Row& row : all_pairs()) check_row(row);
}

TEST_CASE("the pairwise option rows feed the table's planes", "[playback][matrix]") {
  for (const Row& row : kCoverRows) check_row(row);
}

TEST_CASE("auto renders every pair exactly like its fixed layout", "[playback][matrix]") {
  for (const Row& row : all_pairs()) check_auto_matches_fixed(row);
}

TEST_CASE("auto renders every pairwise row exactly like its fixed layout", "[playback][matrix]") {
  for (const Row& row : kCoverRows) check_auto_matches_fixed(row);
}

TEST_CASE("output channel count follows the target", "[playback][matrix]") {
  for (const Row& row : all_pairs()) {
    PlaybackRenderer renderer(config_for(row, row.input), nullptr, kRate, kBlock);
    const int expected = row.target == Target::Headphones || row.target == Target::StereoSpeakers
                             ? 2
                             : sonare::channel_count(speaker_layout(row.target));
    CHECK(renderer.output_channel_count() == expected);
    CHECK(renderer.input_channel_count() == input_channels(row.input));
  }
}
