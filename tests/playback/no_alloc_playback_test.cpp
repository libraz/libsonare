#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

#include "playback/front_end.h"
#include "playback/layout_convert.h"
#include "playback/renderer.h"
#include "support/alloc_guard.h"
#include "util/constants.h"

using sonare::ChannelLayout;
using sonare::SpeakerRole;
using namespace sonare::playback;

namespace {

constexpr int kRate = 48000;
constexpr int kBlock = 480;

/// Drives one renderer through auto layout switches, counting allocations
/// inside process_* and set_head_orientation only; set_config (control thread,
/// allowed to allocate) runs between the guarded calls.
class Driver {
 public:
  explicit Driver(PlaybackRenderer& renderer)
      : renderer_(renderer),
        in_(8, std::vector<float>(kBlock, 0.0f)),
        interleaved_in_(8 * kBlock, 0.0f),
        out_(static_cast<size_t>(renderer.output_channel_count()),
             std::vector<float>(kBlock, 0.0f)),
        interleaved_out_(static_cast<size_t>(renderer.output_channel_count()) * kBlock, 0.0f) {
    for (int ch = 0; ch < 8; ++ch) in_ptrs_[ch] = in_[static_cast<size_t>(ch)].data();
    for (size_t ch = 0; ch < out_.size(); ++ch) out_ptrs_.push_back(out_[ch].data());
  }

  void block(int channels, bool interleaved) {
    for (int ch = 0; ch < channels; ++ch) {
      for (int i = 0; i < kBlock; ++i) {
        const float s = 0.25f * std::sin(sonare::constants::kTwoPi * 440.0f *
                                         static_cast<float>(n_ + i) / static_cast<float>(kRate));
        in_[static_cast<size_t>(ch)][static_cast<size_t>(i)] = s;
        interleaved_in_[static_cast<size_t>(i * channels + ch)] = s;
      }
    }
    const int out_count = renderer_.output_channel_count();
    bool ok = false;
    {
      sonare::test::AllocationGuard guard;
      renderer_.set_head_orientation(static_cast<float>(n_ / kBlock % 90), 5.0f, 0.0f);
      ok = interleaved
               ? renderer_.process_interleaved(interleaved_in_.data(), channels,
                                               interleaved_out_.data(), out_count, kBlock)
               : renderer_.process_planar(in_ptrs_, channels, out_ptrs_.data(), out_count, kBlock);
      allocations_ += guard.count();
    }
    all_ok_ = all_ok_ && ok;
    const float* first = interleaved ? interleaved_out_.data() : out_[0].data();
    const int stride = interleaved ? out_count : 1;
    for (int i = 0; i < kBlock; ++i) {
      const double s = first[static_cast<size_t>(i * stride)];
      energy_ += s * s;
    }
    n_ += kBlock;
  }

  size_t allocations() const { return allocations_; }
  double energy() const { return energy_; }
  bool all_ok() const { return all_ok_; }

 private:
  PlaybackRenderer& renderer_;
  std::vector<std::vector<float>> in_;
  const float* in_ptrs_[8] = {};
  std::vector<float> interleaved_in_;
  std::vector<std::vector<float>> out_;
  std::vector<float*> out_ptrs_;
  std::vector<float> interleaved_out_;
  size_t allocations_ = 0;
  double energy_ = 0.0;
  bool all_ok_ = true;
  int n_ = 0;
};

int longest_drain(const RendererConfig& config) {
  int longest = 0;
  for (ChannelLayout layout : {ChannelLayout::Mono, ChannelLayout::Stereo,
                               ChannelLayout::FivePointOne, ChannelLayout::SevenPointOne}) {
    FrontEnd front_end;
    front_end.prepare(kRate, kBlock, layout, config.prepare, make_output_bus(config.prepare));
    longest = std::max(longest, front_end.drain_frames());
  }
  return longest;
}

void run_switch_sequence(const RendererConfig& config) {
  PlaybackRenderer renderer(config, nullptr, kRate, kBlock);
  // Each run outlasts every drain, so each switch passes a completed drain and
  // the dormant reset that follows it.
  const int blocks_per_run = longest_drain(config) / kBlock + 2;
  Driver driver(renderer);
  RendererConfig changed = config;
  int run = 0;
  for (int channels : {1, 2, 6, 8, 2}) {
    for (int b = 0; b < blocks_per_run; ++b) driver.block(channels, b % 2 == 1);
    // A realtime-only change, adopted by the next guarded block.
    changed.realtime.night_amount = run % 2 == 0 ? 1.0f : 0.0f;
    changed.realtime.upmix_enabled = run % 2 != 0;
    changed.realtime.dialogue_level_db = run % 2 == 0 ? 3.0f : 0.0f;
    changed.realtime.limiter_enabled = run % 2 != 0;
    renderer.set_config(changed);
    ++run;
  }
  // Back to 5.1 and straight back to stereo: a truncated drain.
  driver.block(6, false);
  driver.block(2, false);
  for (int b = 0; b < 4; ++b) driver.block(2, false);

  const RendererDiagnostics d = renderer.diagnostics();
  CHECK(driver.all_ok());
  CHECK(d.layout_switches == 7u);
  CHECK(d.truncated_drains == 1u);
  // Non-vacuity: the guarded path rendered sound.
  CHECK(driver.energy() > 0.0);
  CHECK(driver.allocations() == 0u);
}

}  // namespace

TEST_CASE("headphone rendering and layout switches do not allocate", "[playback][no-alloc]") {
  {
    // The counter is live in this binary.
    sonare::test::AllocationGuard probe;
    std::vector<float> scratch(64, 1.0f);
    const size_t counted = probe.count();
    CHECK(scratch.back() == 1.0f);
    CHECK(counted >= 1u);
  }
  RendererConfig config;
  config.prepare.input_layout = InputLayout::Auto;
  run_switch_sequence(config);
}

TEST_CASE("speaker rendering with bass management and distances does not allocate",
          "[playback][no-alloc]") {
  RendererConfig config;
  config.prepare.input_layout = InputLayout::Auto;
  config.prepare.target_kind = TargetKind::Speakers;
  config.prepare.target_layout = ChannelLayout::SevenPointOne;
  config.prepare.bass_management.enabled = true;
  for (SpeakerRole role : {SpeakerRole::Ls, SpeakerRole::Rs, SpeakerRole::Lss, SpeakerRole::Rss}) {
    SpeakerPrepare& speaker = config.prepare.speakers[static_cast<size_t>(role)];
    speaker.size = SpeakerSize::Small;
    speaker.has_distance = true;
    speaker.distance_m = 1.7f;
  }
  config.prepare.speakers[static_cast<size_t>(SpeakerRole::L)].has_distance = true;
  config.prepare.speakers[static_cast<size_t>(SpeakerRole::L)].distance_m = 3.1f;
  run_switch_sequence(config);
}
