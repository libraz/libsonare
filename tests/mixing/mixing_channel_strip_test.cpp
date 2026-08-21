/// @file mixing_channel_strip_test.cpp
/// @brief Mixing channel strip tests.

#include <algorithm>
#include <limits>

#include "mastering/api/insert_factory.h"
#include "mastering/dynamics/compressor.h"
#include "mixing_test_helpers.h"
#include "rt/delay_line.h"
#include "util/exception.h"

namespace {

// Symmetric hard clipper at +/-kLimit: a pure memoryless nonlinearity, so
// clip(a + b) != clip(a) + clip(b) once |a + b| crosses the limit. Deterministic
// (no smoothing/state), which lets a test pin the summing-order semantics of a
// shared mixer strip exactly.
class HardClipProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit HardClipProcessor(float limit) : limit_(limit) {}
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    for (int ch = 0; ch < num_channels; ++ch) {
      if (channels[ch] == nullptr) continue;
      for (int i = 0; i < num_samples; ++i) {
        channels[ch][i] = std::clamp(channels[ch][i], -limit_, limit_);
      }
    }
  }
  void reset() override {}

 private:
  float limit_ = 1.0f;
};

// Models a latent StereoPairOnly insert: it delays the two planes it receives,
// while the host chain (BusProcessor or ChannelStrip) is responsible for
// aligning the untouched surround planes.
class FixedLatencyStereoProcessor final : public sonare::rt::ProcessorBase {
 public:
  explicit FixedLatencyStereoProcessor(int latency) : latency_(latency) {}

  void prepare(double, int) override {
    for (auto& delay : delays_) delay.prepare(static_cast<size_t>(latency_));
  }
  void process(float* const* channels, int num_channels, int num_samples) override {
    for (int ch = 0; ch < std::min(num_channels, 2); ++ch) {
      if (channels[ch] == nullptr) continue;
      for (int sample = 0; sample < num_samples; ++sample) {
        channels[ch][sample] = delays_[static_cast<size_t>(ch)].process(channels[ch][sample]);
      }
    }
  }
  void reset() override {
    for (auto& delay : delays_) delay.reset();
  }
  int latency_samples() const noexcept override { return latency_; }

 private:
  int latency_ = 0;
  std::array<sonare::rt::DelayLine, 2> delays_{};
};

}  // namespace

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

TEST_CASE("ChannelStrip can omit internal meter state", "[mixing]") {
  sonare::mixing::ChannelStripConfig config;
  config.enable_metering = false;
  sonare::mixing::ChannelStrip strip(config);
  strip.prepare(48000.0, 512);

  std::array<float, 512> left{};
  std::array<float, 512> right{};
  left.fill(0.5f);
  right.fill(0.5f);
  float* channels[] = {left.data(), right.data()};
  strip.process(channels, 2, static_cast<int>(left.size()));

  REQUIRE_FALSE(strip.metering_enabled());
  const auto pre = strip.meter_snapshot(sonare::mixing::TapPoint::PreFader);
  const auto post = strip.meter_snapshot(sonare::mixing::TapPoint::PostFader);
  REQUIRE(pre.seq == 0);
  REQUIRE(post.seq == 0);
  REQUIRE(pre.peak_db[0] == sonare::constants::kFloorDb);
  REQUIRE(post.true_peak_db[0] == sonare::constants::kFloorDb);
}

TEST_CASE("surround bus linked dynamics exclude only the LFE plane from detection",
          "[mixing][surround][dynamics]") {
  constexpr int kFrames = 64;

  const auto render = [](sonare::ChannelLayout layout, int channels, float lfe_level) {
    std::array<std::array<float, kFrames>, 8> planes{};
    std::array<float*, 8> pointers{};
    for (int ch = 0; ch < channels; ++ch) {
      planes[static_cast<size_t>(ch)].fill(0.05f);
      pointers[static_cast<size_t>(ch)] = planes[static_cast<size_t>(ch)].data();
    }
    planes[3].fill(lfe_level);

    sonare::mastering::dynamics::CompressorConfig config;
    config.threshold_db = -20.0f;
    config.ratio = 20.0f;
    config.attack_ms = 0.0f;
    config.release_ms = 0.0f;
    config.detector = sonare::mastering::dynamics::DetectorMode::Peak;
    sonare::mixing::BusProcessor bus(sonare::mixing::BusRole::Subgroup);
    bus.set_channel_layout(layout);
    bus.add_insert(std::make_unique<sonare::mastering::dynamics::Compressor>(config));
    bus.prepare(48000.0, kFrames);
    bus.process(pointers.data(), channels, kFrames);
    return planes;
  };

  // 5.1 and 7.1 use the same canonical LFE index. Raising that plane must not
  // change their shared gain envelope, while the LFE itself still receives the
  // shared gain calculated from the program channels.
  for (const auto& [layout, channels] : {std::pair{sonare::ChannelLayout::FivePointOne, 6},
                                         std::pair{sonare::ChannelLayout::SevenPointOne, 8}}) {
    const auto quiet_lfe = render(layout, channels, 0.0f);
    const auto loud_lfe = render(layout, channels, 0.95f);
    for (int ch = 0; ch < channels; ++ch) {
      if (ch == 3) continue;
      REQUIRE(loud_lfe[static_cast<size_t>(ch)] == quiet_lfe[static_cast<size_t>(ch)]);
    }
  }

  // The same six-plane buffer with the legacy stereo context intentionally
  // detects every plane, proving the surround layout is what supplies the LFE
  // exclusion rather than an unconditional channel-3 special case.
  const auto surround = render(sonare::ChannelLayout::FivePointOne, 6, 0.95f);
  const auto legacy = render(sonare::ChannelLayout::Stereo, 6, 0.95f);
  REQUIRE(surround[0].back() > legacy[0].back() * 2.0f);
}

TEST_CASE("surround bus aligns untouched planes after a latent StereoPairOnly insert",
          "[mixing][surround]") {
  constexpr int kChannels = 6;
  constexpr int kFrames = 12;
  constexpr int kLatency = 4;
  std::array<std::array<float, kFrames>, kChannels> planes{};
  std::array<float*, kChannels> pointers{};
  for (int ch = 0; ch < kChannels; ++ch) {
    planes[static_cast<size_t>(ch)][0] = static_cast<float>(ch + 1);
    pointers[static_cast<size_t>(ch)] = planes[static_cast<size_t>(ch)].data();
  }

  sonare::mixing::BusProcessor bus(sonare::mixing::BusRole::Subgroup);
  bus.set_channel_layout(sonare::ChannelLayout::FivePointOne);
  bus.add_insert(std::make_unique<FixedLatencyStereoProcessor>(kLatency), true);
  bus.prepare(48000.0, kFrames);
  bus.process(pointers.data(), kChannels, kFrames);

  REQUIRE(bus.latency_samples() == kLatency);
  for (int ch = 0; ch < kChannels; ++ch) {
    for (int sample = 0; sample < kLatency; ++sample) {
      REQUIRE_THAT(planes[static_cast<size_t>(ch)][static_cast<size_t>(sample)],
                   WithinAbs(0.0f, 0.0001f));
    }
    REQUIRE_THAT(planes[static_cast<size_t>(ch)][kLatency],
                 WithinAbs(static_cast<float>(ch + 1), 0.0001f));
  }
}

TEST_CASE("BusProcessor keeps a bypassed insert's latency in the signal path",
          "[mixing][surround]") {
  // Same soft-bypass contract as ChannelStrip: latency_samples_q8() counts a
  // bypassed insert, so the bus must still deliver that delay -- on every plane,
  // since a bypassed StereoPairOnly insert leaves the front pair undelayed too.
  constexpr int kChannels = 6;
  constexpr int kFrames = 12;
  constexpr int kLatency = 4;
  std::array<std::array<float, kFrames>, kChannels> planes{};
  std::array<float*, kChannels> pointers{};
  for (int ch = 0; ch < kChannels; ++ch) {
    planes[static_cast<size_t>(ch)][0] = static_cast<float>(ch + 1);
    pointers[static_cast<size_t>(ch)] = planes[static_cast<size_t>(ch)].data();
  }

  sonare::mixing::BusProcessor bus(sonare::mixing::BusRole::Subgroup);
  bus.set_channel_layout(sonare::ChannelLayout::FivePointOne);
  bus.add_insert(std::make_unique<FixedLatencyStereoProcessor>(kLatency), true);
  bus.prepare(48000.0, kFrames);
  REQUIRE(bus.set_insert_bypassed(0, true));
  REQUIRE(bus.latency_samples() == kLatency);
  bus.process(pointers.data(), kChannels, kFrames);

  for (int ch = 0; ch < kChannels; ++ch) {
    for (int sample = 0; sample < kLatency; ++sample) {
      REQUIRE_THAT(planes[static_cast<size_t>(ch)][static_cast<size_t>(sample)],
                   WithinAbs(0.0f, 0.0001f));
    }
    REQUIRE_THAT(planes[static_cast<size_t>(ch)][kLatency],
                 WithinAbs(static_cast<float>(ch + 1), 0.0001f));
  }
}

namespace {

// Renders a rising ramp through a chain block by block, engaging bypass partway.
// The insert is a pure delay, so its output and the bypass substitute's output
// are the same signal: the rendered stream must be indistinguishable from one
// continuous delay. A substitute that started cold would punch a
// latency-length hole at the toggle instead.
template <typename Chain>
void require_continuous_bypass_engage(Chain& chain, int block_size, int blocks, int latency,
                                      int bypass_from_block) {
  std::vector<float> rendered;
  rendered.reserve(static_cast<size_t>(block_size * blocks));
  for (int block = 0; block < blocks; ++block) {
    REQUIRE(chain.set_insert_bypassed(0, block >= bypass_from_block));
    std::vector<float> left(static_cast<size_t>(block_size), 0.0f);
    std::vector<float> right(static_cast<size_t>(block_size), 0.0f);
    for (int i = 0; i < block_size; ++i) {
      left[static_cast<size_t>(i)] = static_cast<float>(block * block_size + i + 1);
      right[static_cast<size_t>(i)] = left[static_cast<size_t>(i)];
    }
    float* channels[] = {left.data(), right.data()};
    chain.process(channels, 2, block_size);
    rendered.insert(rendered.end(), left.begin(), left.end());
  }

  for (int n = 0; n < block_size * blocks; ++n) {
    const float expected = n < latency ? 0.0f : static_cast<float>(n - latency + 1);
    INFO("sample " << n);
    REQUIRE_THAT(rendered[static_cast<size_t>(n)], WithinAbs(expected, 0.0001f));
  }
}

}  // namespace

TEST_CASE("ChannelStrip bypass engages without a dropout", "[mixing]") {
  constexpr int kBlock = 16;
  constexpr int kBlocks = 4;
  constexpr int kLatency = 5;

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_pre_insert(std::make_unique<FixedLatencyStereoProcessor>(kLatency));
  strip.prepare(48000.0, kBlock);
  require_continuous_bypass_engage(strip, kBlock, kBlocks, kLatency, /*bypass_from_block=*/2);
}

TEST_CASE("BusProcessor bypass engages without a dropout", "[mixing]") {
  constexpr int kBlock = 16;
  constexpr int kBlocks = 4;
  constexpr int kLatency = 5;

  sonare::mixing::BusProcessor bus(sonare::mixing::BusRole::Subgroup);
  bus.add_insert(std::make_unique<FixedLatencyStereoProcessor>(kLatency));
  bus.prepare(48000.0, kBlock);
  require_continuous_bypass_engage(bus, kBlock, kBlocks, kLatency, /*bypass_from_block=*/2);
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

TEST_CASE("ChannelStrip channel delay reaches every plane of a wide layout", "[mixing]") {
  // 7.1.4 is twelve planes, three more than the segmented stack path carries.
  // A uniform channel delay has to land on all twelve: delaying only the bed
  // and leaving the height layer early splits correlated material in time.
  constexpr int kPlanes = 12;
  constexpr int kDelay = 480;  // 10 ms at 48 kHz
  constexpr size_t kFrames = 1024;

  auto run = [](int prepared_channels) {
    std::array<std::vector<float>, kPlanes> planes;
    std::array<float*, kPlanes> pointers{};
    for (size_t ch = 0; ch < kPlanes; ++ch) {
      planes[ch].assign(kFrames, 0.0f);
      planes[ch][0] = 1.0f;
      pointers[ch] = planes[ch].data();
    }
    // Unity everywhere else so the only thing acting on the impulse is the
    // channel delay: no pan, no width collapse, no fader ramp.
    sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
    strip.set_width(1.0f);
    strip.set_channel_delay_samples(kDelay);
    if (prepared_channels > 0) strip.set_prepared_channels(prepared_channels);
    strip.prepare(48000.0, static_cast<int>(kFrames));
    strip.settle();
    strip.process(pointers.data(), kPlanes, static_cast<int>(kFrames));
    return std::make_pair(std::move(planes), strip.alignment_channel_overflow());
  };

  auto [aligned, aligned_overflow] = run(kPlanes);
  REQUIRE(aligned_overflow == 0);
  for (size_t ch = 0; ch < kPlanes; ++ch) {
    INFO("plane " << ch);
    REQUIRE_THAT(aligned[ch][0], WithinAbs(0.0f, 0.0001f));
    REQUIRE_THAT(aligned[ch][kDelay - 1], WithinAbs(0.0f, 0.0001f));
    REQUIRE_THAT(aligned[ch][kDelay], WithinAbs(1.0f, 0.0001f));
  }

  // Left at the default width the upper planes run un-delayed -- which is the
  // failure this guards -- and the bank now says so instead of staying silent.
  auto [unprepared, unprepared_overflow] = run(0);
  REQUIRE(unprepared_overflow == kPlanes - 8);
  REQUIRE_THAT(unprepared[0][kDelay], WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(unprepared[kPlanes - 1][0], WithinAbs(1.0f, 0.0001f));
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

TEST_CASE("ChannelStrip StereoPairOnly insert touches only the front pair on a surround buffer",
          "[mixing][surround]") {
  // Six planes, distinct DC per plane (1..6). At 0 dB fader / center pan the
  // only stage that can alter a plane is the insert, so the surround planes
  // (2..5) must come through untouched when the insert is StereoPairOnly.
  constexpr int kCh = 6;
  constexpr int kN = 4;
  std::array<std::array<float, kN>, kCh> planes{};
  std::array<float*, kCh> ptrs{};
  for (int ch = 0; ch < kCh; ++ch) {
    planes[static_cast<size_t>(ch)].fill(static_cast<float>(ch + 1));
    ptrs[static_cast<size_t>(ch)] = planes[static_cast<size_t>(ch)].data();
  }

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_pre_insert(std::make_unique<ScaleProcessor>(2.0f), /*stereo_pair_only=*/true);
  strip.prepare(48000.0, kN);
  strip.process(ptrs.data(), kCh, kN);

  for (int i = 0; i < kN; ++i) {
    // Front pair scaled by the insert.
    REQUIRE_THAT(planes[0][static_cast<size_t>(i)], WithinAbs(2.0f, 0.0001f));
    REQUIRE_THAT(planes[1][static_cast<size_t>(i)], WithinAbs(4.0f, 0.0001f));
    // Surround planes pass through dry (insert never saw them).
    REQUIRE_THAT(planes[2][static_cast<size_t>(i)], WithinAbs(3.0f, 0.0001f));
    REQUIRE_THAT(planes[3][static_cast<size_t>(i)], WithinAbs(4.0f, 0.0001f));
    REQUIRE_THAT(planes[4][static_cast<size_t>(i)], WithinAbs(5.0f, 0.0001f));
    REQUIRE_THAT(planes[5][static_cast<size_t>(i)], WithinAbs(6.0f, 0.0001f));
  }
}

TEST_CASE("ChannelStrip aligns untouched planes after a latent StereoPairOnly insert",
          "[mixing][surround]") {
  // The insert delays only the front pair it is handed, so the strip owes the
  // centre, LFE and surround planes the same delay. Without it a convolution or
  // lookahead insert on a 5.1 strip smears the front image against a centre
  // channel that arrives early.
  constexpr int kChannels = 6;
  constexpr int kFrames = 12;
  constexpr int kLatency = 4;
  std::array<std::array<float, kFrames>, kChannels> planes{};
  std::array<float*, kChannels> pointers{};
  for (int ch = 0; ch < kChannels; ++ch) {
    planes[static_cast<size_t>(ch)][0] = static_cast<float>(ch + 1);
    pointers[static_cast<size_t>(ch)] = planes[static_cast<size_t>(ch)].data();
  }

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_pre_insert(std::make_unique<FixedLatencyStereoProcessor>(kLatency),
                       /*stereo_pair_only=*/true);
  strip.prepare(48000.0, kFrames);
  strip.process(pointers.data(), kChannels, kFrames);

  REQUIRE(strip.latency_samples() == kLatency);
  for (int ch = 0; ch < kChannels; ++ch) {
    for (int sample = 0; sample < kLatency; ++sample) {
      REQUIRE_THAT(planes[static_cast<size_t>(ch)][static_cast<size_t>(sample)],
                   WithinAbs(0.0f, 0.0001f));
    }
    REQUIRE_THAT(planes[static_cast<size_t>(ch)][kLatency],
                 WithinAbs(static_cast<float>(ch + 1), 0.0001f));
  }
}

TEST_CASE("ChannelStrip keeps a bypassed insert's latency in the signal path", "[mixing]") {
  // A bypass toggle must not move the strip in time relative to the rest of the
  // mix: the host's PDC is computed from latency_samples_q8() on the control
  // thread, while bypass flips on the audio thread, so the two only agree if the
  // reported latency and the delivered latency both stay put.
  constexpr int kFrames = 16;
  constexpr int kLatency = 5;

  auto render = [kLatency](bool bypassed) {
    sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
    strip.add_pre_insert(std::make_unique<FixedLatencyStereoProcessor>(kLatency));
    strip.prepare(48000.0, kFrames);
    REQUIRE(strip.set_insert_bypassed(0, bypassed));
    REQUIRE(strip.latency_samples() == kLatency);

    std::array<float, kFrames> left{};
    std::array<float, kFrames> right{};
    left[0] = 1.0f;
    right[0] = 1.0f;
    float* channels[] = {left.data(), right.data()};
    strip.process(channels, 2, kFrames);
    return left;
  };

  const std::array<float, kFrames> active = render(false);
  const std::array<float, kFrames> bypassed = render(true);

  for (int sample = 0; sample < kLatency; ++sample) {
    REQUIRE_THAT(active[static_cast<size_t>(sample)], WithinAbs(0.0f, 0.0001f));
    REQUIRE_THAT(bypassed[static_cast<size_t>(sample)], WithinAbs(0.0f, 0.0001f));
  }
  REQUIRE_THAT(active[kLatency], WithinAbs(1.0f, 0.0001f));
  REQUIRE_THAT(bypassed[kLatency], WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("ChannelStrip aligns untouched planes across both insert chains", "[mixing][surround]") {
  // The pre and post chains share one alignment bank addressed by the combined
  // insert index, so the post chain must pick up its OWN insert's latency rather
  // than the pre chain's. Distinct latencies make a mis-offset bank visible: the
  // surround planes would land at 3+3 instead of 3+5.
  constexpr int kChannels = 6;
  constexpr int kFrames = 24;
  constexpr int kPreLatency = 3;
  constexpr int kPostLatency = 5;
  constexpr int kTotalLatency = kPreLatency + kPostLatency;
  std::array<std::array<float, kFrames>, kChannels> planes{};
  std::array<float*, kChannels> pointers{};
  for (int ch = 0; ch < kChannels; ++ch) {
    planes[static_cast<size_t>(ch)][0] = static_cast<float>(ch + 1);
    pointers[static_cast<size_t>(ch)] = planes[static_cast<size_t>(ch)].data();
  }

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_pre_insert(std::make_unique<FixedLatencyStereoProcessor>(kPreLatency),
                       /*stereo_pair_only=*/true);
  strip.add_post_insert(std::make_unique<FixedLatencyStereoProcessor>(kPostLatency),
                        /*stereo_pair_only=*/true);
  strip.prepare(48000.0, kFrames);
  strip.process(pointers.data(), kChannels, kFrames);

  REQUIRE(strip.latency_samples() == kTotalLatency);
  for (int ch = 0; ch < kChannels; ++ch) {
    for (int sample = 0; sample < kTotalLatency; ++sample) {
      REQUIRE_THAT(planes[static_cast<size_t>(ch)][static_cast<size_t>(sample)],
                   WithinAbs(0.0f, 0.0001f));
    }
    REQUIRE_THAT(planes[static_cast<size_t>(ch)][kTotalLatency],
                 WithinAbs(static_cast<float>(ch + 1), 0.0001f));
  }
}

TEST_CASE("ChannelStrip bypass keeps both insert chains aligned", "[mixing][surround]") {
  // Same two-chain layout, both inserts bypassed: the bypass bank is addressed
  // by the same combined index, so a mis-offset bank would drop the post
  // insert's 5 samples and land everything 5 samples early.
  constexpr int kChannels = 6;
  constexpr int kFrames = 24;
  constexpr int kPreLatency = 3;
  constexpr int kPostLatency = 5;
  constexpr int kTotalLatency = kPreLatency + kPostLatency;
  std::array<std::array<float, kFrames>, kChannels> planes{};
  std::array<float*, kChannels> pointers{};
  for (int ch = 0; ch < kChannels; ++ch) {
    planes[static_cast<size_t>(ch)][0] = static_cast<float>(ch + 1);
    pointers[static_cast<size_t>(ch)] = planes[static_cast<size_t>(ch)].data();
  }

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_pre_insert(std::make_unique<FixedLatencyStereoProcessor>(kPreLatency),
                       /*stereo_pair_only=*/true);
  strip.add_post_insert(std::make_unique<FixedLatencyStereoProcessor>(kPostLatency),
                        /*stereo_pair_only=*/true);
  strip.prepare(48000.0, kFrames);
  // insert_index addresses the combined [pre ... post ...] sequence.
  REQUIRE(strip.set_insert_bypassed(0, true));
  REQUIRE(strip.set_insert_bypassed(1, true));
  REQUIRE(strip.latency_samples() == kTotalLatency);
  strip.process(pointers.data(), kChannels, kFrames);

  for (int ch = 0; ch < kChannels; ++ch) {
    for (int sample = 0; sample < kTotalLatency; ++sample) {
      REQUIRE_THAT(planes[static_cast<size_t>(ch)][static_cast<size_t>(sample)],
                   WithinAbs(0.0f, 0.0001f));
    }
    REQUIRE_THAT(planes[static_cast<size_t>(ch)][kTotalLatency],
                 WithinAbs(static_cast<float>(ch + 1), 0.0001f));
  }
}

TEST_CASE("ChannelStrip surround alignment survives mid-block segmentation", "[mixing][surround]") {
  // An automation event splits the block, routing the chain through
  // process_segment instead of process_unsegmented. The alignment bank has to
  // carry its delay-line state across the segment boundary, so the boundary is
  // placed inside the latency window: the impulse enters in the first segment
  // and must emerge in the second.
  constexpr int kChannels = 6;
  constexpr int kFrames = 24;
  constexpr int kLatency = 8;
  constexpr int kSplit = 4;
  static_assert(kSplit < kLatency, "the boundary must fall inside the latency window");

  std::array<std::array<float, kFrames>, kChannels> planes{};
  std::array<float*, kChannels> pointers{};
  for (int ch = 0; ch < kChannels; ++ch) {
    planes[static_cast<size_t>(ch)][0] = static_cast<float>(ch + 1);
    pointers[static_cast<size_t>(ch)] = planes[static_cast<size_t>(ch)].data();
  }

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_pre_insert(std::make_unique<FixedLatencyStereoProcessor>(kLatency),
                       /*stereo_pair_only=*/true);
  strip.prepare(48000.0, kFrames);
  // Re-targets the fader at its current 0 dB, so the block splits without any
  // gain change to disturb the impulse amplitude.
  REQUIRE(strip.schedule_fader_automation(kSplit, 0.0f, sonare::AutomationCurve::Hold));
  strip.process_at(pointers.data(), kChannels, kFrames, 0);

  for (int ch = 0; ch < kChannels; ++ch) {
    for (int sample = 0; sample < kLatency; ++sample) {
      REQUIRE_THAT(planes[static_cast<size_t>(ch)][static_cast<size_t>(sample)],
                   WithinAbs(0.0f, 0.0001f));
    }
    REQUIRE_THAT(planes[static_cast<size_t>(ch)][kLatency],
                 WithinAbs(static_cast<float>(ch + 1), 0.0001f));
  }
}

TEST_CASE("ChannelStrip bypass keeps every surround plane aligned", "[mixing][surround]") {
  // Bypassing a latent StereoPairOnly insert takes both the insert's own front
  // pair delay and the strip's surround compensation out of the chain at once,
  // so the substitute delay has to cover all six planes uniformly.
  constexpr int kChannels = 6;
  constexpr int kFrames = 12;
  constexpr int kLatency = 4;
  std::array<std::array<float, kFrames>, kChannels> planes{};
  std::array<float*, kChannels> pointers{};
  for (int ch = 0; ch < kChannels; ++ch) {
    planes[static_cast<size_t>(ch)][0] = static_cast<float>(ch + 1);
    pointers[static_cast<size_t>(ch)] = planes[static_cast<size_t>(ch)].data();
  }

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.add_pre_insert(std::make_unique<FixedLatencyStereoProcessor>(kLatency),
                       /*stereo_pair_only=*/true);
  strip.prepare(48000.0, kFrames);
  REQUIRE(strip.set_insert_bypassed(0, true));
  REQUIRE(strip.latency_samples() == kLatency);
  strip.process(pointers.data(), kChannels, kFrames);

  for (int ch = 0; ch < kChannels; ++ch) {
    for (int sample = 0; sample < kLatency; ++sample) {
      REQUIRE_THAT(planes[static_cast<size_t>(ch)][static_cast<size_t>(sample)],
                   WithinAbs(0.0f, 0.0001f));
    }
    REQUIRE_THAT(planes[static_cast<size_t>(ch)][kLatency],
                 WithinAbs(static_cast<float>(ch + 1), 0.0001f));
  }
}

TEST_CASE("ChannelStrip Multichannel insert processes every plane on a surround buffer",
          "[mixing][surround]") {
  constexpr int kCh = 6;
  constexpr int kN = 4;
  std::array<std::array<float, kN>, kCh> planes{};
  std::array<float*, kCh> ptrs{};
  for (int ch = 0; ch < kCh; ++ch) {
    planes[static_cast<size_t>(ch)].fill(static_cast<float>(ch + 1));
    ptrs[static_cast<size_t>(ch)] = planes[static_cast<size_t>(ch)].data();
  }

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  // Default (Multichannel): one full-buffer call scales every plane.
  strip.add_pre_insert(std::make_unique<ScaleProcessor>(2.0f));
  strip.prepare(48000.0, kN);
  strip.process(ptrs.data(), kCh, kN);

  for (int ch = 0; ch < kCh; ++ch) {
    for (int i = 0; i < kN; ++i) {
      REQUIRE_THAT(planes[static_cast<size_t>(ch)][static_cast<size_t>(i)],
                   WithinAbs(2.0f * static_cast<float>(ch + 1), 0.0001f));
    }
  }
}

TEST_CASE("ChannelStrip StereoPairOnly wrapper feeds eq.midSide a 2-plane view (no abort)",
          "[mixing][surround]") {
  // eq.midSide throws on a non-stereo width. The StereoPairOnly wrapper must
  // hand it exactly the front pair so a 5.1 buffer processes cleanly; a
  // Multichannel (unwrapped) build would pass all 6 planes and abort.
  constexpr int kCh = 6;
  constexpr int kN = 8;
  auto make_strip = [](bool spo) {
    auto strip = std::make_unique<sonare::mixing::ChannelStrip>(
        sonare::mixing::ChannelStripConfig{0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
    auto insert = sonare::mastering::api::make_insert("eq.midSide", "{}");
    REQUIRE(insert != nullptr);
    strip->add_pre_insert(std::move(insert), spo);
    strip->prepare(48000.0, kN);
    return strip;
  };
  auto buffer = [] {
    auto planes = std::make_unique<std::array<std::array<float, kN>, kCh>>();
    for (int ch = 0; ch < kCh; ++ch) {
      (*planes)[static_cast<size_t>(ch)].fill(static_cast<float>(ch + 1) * 0.1f);
    }
    return planes;
  };

  auto wrapped = make_strip(/*spo=*/true);
  auto wrapped_planes = buffer();
  std::array<float*, kCh> wrapped_ptrs{};
  for (int ch = 0; ch < kCh; ++ch)
    wrapped_ptrs[static_cast<size_t>(ch)] = (*wrapped_planes)[static_cast<size_t>(ch)].data();
  REQUIRE_NOTHROW(wrapped->process(wrapped_ptrs.data(), kCh, kN));
  // Surround planes pass through dry.
  for (int ch = 2; ch < kCh; ++ch) {
    for (int i = 0; i < kN; ++i) {
      REQUIRE_THAT((*wrapped_planes)[static_cast<size_t>(ch)][static_cast<size_t>(i)],
                   WithinAbs(static_cast<float>(ch + 1) * 0.1f, 0.0001f));
    }
  }

  // Without the wrapper the same processor aborts the 6-channel block.
  auto unwrapped = make_strip(/*spo=*/false);
  auto unwrapped_planes = buffer();
  std::array<float*, kCh> unwrapped_ptrs{};
  for (int ch = 0; ch < kCh; ++ch)
    unwrapped_ptrs[static_cast<size_t>(ch)] = (*unwrapped_planes)[static_cast<size_t>(ch)].data();
  REQUIRE_THROWS_AS(unwrapped->process(unwrapped_ptrs.data(), kCh, kN), sonare::SonareException);
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

TEST_CASE("ChannelStrip pre-fader meter width is independent of automation",
          "[mixing][surround][meter]") {
  // An automation event routes the block through process_segment, which used to
  // drive the pre-fader meter from a two-row send tap while the unsegmented
  // path drove it from the full-width buffer. A 5.1 strip therefore dropped
  // from 6 observed planes to 2 for exactly the blocks an event landed in.
  //
  // The per-plane readings were only half of it. The integrated-LUFS histogram
  // is instance-lifetime state, so a block metered over 2 planes and a block
  // metered over 6 fed one histogram with energies computed over different
  // channel counts, permanently skewing integrated_lufs.
  constexpr int kChannels = 6;
  constexpr int kFrames = 1024;
  // Long enough to fill the 400 ms momentary and 3 s short-term windows and to
  // put several gated blocks into the integrated histogram; a shorter run
  // leaves every loudness field at the floor in BOTH variants, which would make
  // the comparison below vacuous.
  constexpr int kBlocks = 200;

  auto run = [](bool with_automation) {
    sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
    strip.set_prepared_channels(kChannels);
    strip.prepare(48000.0, kFrames);
    strip.settle();
    for (int block = 0; block < kBlocks; ++block) {
      std::array<std::vector<float>, kChannels> planes;
      std::array<float*, kChannels> pointers{};
      for (int ch = 0; ch < kChannels; ++ch) {
        planes[static_cast<size_t>(ch)] = sonare::test::generate_sine_samples(
            220.0f * static_cast<float>(ch + 1), 48000, kFrames, 0.5f);
        pointers[static_cast<size_t>(ch)] = planes[static_cast<size_t>(ch)].data();
      }
      const int64_t block_start = static_cast<int64_t>(block) * kFrames;
      if (with_automation) {
        // Re-targets the fader at its current 0 dB, so the block splits without
        // any gain change: the two runs process identical audio.
        REQUIRE(strip.schedule_fader_automation(block_start + kFrames / 2, 0.0f,
                                                sonare::AutomationCurve::Hold));
      }
      strip.process_at(pointers.data(), kChannels, kFrames, block_start);
    }
    return strip.meter_snapshot(sonare::mixing::TapPoint::PreFader);
  };

  const auto unsplit = run(false);
  const auto split = run(true);

  // The acceptance condition: identical input, identical loudness, whether or
  // not an automation event happened to land in the block.
  REQUIRE_THAT(split.momentary_lufs, WithinAbs(unsplit.momentary_lufs, 0.01f));
  REQUIRE_THAT(split.short_term_lufs, WithinAbs(unsplit.short_term_lufs, 0.01f));
  REQUIRE_THAT(split.integrated_lufs, WithinAbs(unsplit.integrated_lufs, 0.01f));

  // Planes 2..N keep being updated rather than freezing at the meter floor.
  for (int ch = 0; ch < kChannels; ++ch) {
    CAPTURE(ch);
    REQUIRE(split.peak_db[static_cast<size_t>(ch)] > sonare::constants::kFloorDb);
    REQUIRE(split.rms_db[static_cast<size_t>(ch)] > sonare::constants::kFloorDb);
    REQUIRE_THAT(split.peak_db[static_cast<size_t>(ch)],
                 WithinAbs(unsplit.peak_db[static_cast<size_t>(ch)], 0.01f));
    REQUIRE_THAT(split.rms_db[static_cast<size_t>(ch)],
                 WithinAbs(unsplit.rms_db[static_cast<size_t>(ch)], 0.01f));
  }

  // Guard against a vacuous comparison: the upper planes carry different
  // content from the front pair, so agreeing on them is a real check.
  REQUIRE(split.rms_db[5] != split.rms_db[0]);
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

TEST_CASE("ChannelStrip reaches the block-final fader value past the per-block event cap",
          "[mixing]") {
  // More automation breakpoints than kMaxAutomationEventsPerBlock (128) fall in
  // one block. consume_block() advances the SPSC lane tail for every event, so
  // any breakpoint the strip fails to store is gone permanently -- and because
  // the dropped ones are the highest-offset events, the fader used to stay stuck
  // at the value of the 128th breakpoint and never reach its true block-final
  // value. The overflow-collapse policy must still land the parameter on the
  // last breakpoint's value.
  constexpr int kNumEvents = 200;  // > 128 cap
  constexpr int kBlock = 256;
  std::vector<float> left(kBlock, 1.0f);
  std::vector<float> right(kBlock, 1.0f);
  float* channels[] = {left.data(), right.data()};

  sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
  strip.prepare(48000.0, kBlock);
  // Hold curves so no synthetic interpolation events inflate the count: the
  // block contains exactly kNumEvents breakpoints. Every intermediate event
  // parks the fader at -60 dB; only the final one opens it to -3 dB.
  for (int i = 0; i < kNumEvents; ++i) {
    const float db = (i == kNumEvents - 1) ? -3.0f : -60.0f;
    REQUIRE(strip.schedule_fader_automation(i, db, sonare::mixing::AutomationCurveType::Hold));
  }
  strip.process_at(channels, 2, kBlock, 0);

  REQUIRE_THAT(strip.fader_db(), WithinAbs(-3.0f, 0.0001f));
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

TEST_CASE("ChannelStrip rejects non-finite live values without poisoning later updates",
          "[mixing][validation]") {
  sonare::mixing::ChannelStrip strip;
  strip.prepare(48000.0, 8);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  strip.set_fader_db(-6.0f);
  strip.set_fader_db(nan);
  REQUIRE(strip.fader_db() == -6.0f);
  strip.set_pan(0.25f);
  strip.set_pan(inf);
  REQUIRE(strip.pan() == 0.25f);
  strip.set_width(0.75f);
  strip.set_width(nan);
  REQUIRE(strip.width() == 0.75f);

  REQUIRE_FALSE(strip.schedule_fader_automation(0, nan));
  REQUIRE_FALSE(strip.schedule_pan_automation(0, inf));
  REQUIRE_FALSE(strip.schedule_width_automation(0, nan));
  REQUIRE(strip.schedule_fader_automation(0, -3.0f));
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

  REQUIRE(linear_phase_strip.schedule_insert_automation_result(0, 1, 0, 6.0f) ==
          sonare::mixing::InsertAutomationScheduleResult::NotSupported);
  REQUIRE(linear_phase_strip.schedule_insert_automation_result(0, 1000, 0, 6.0f) ==
          sonare::mixing::InsertAutomationScheduleResult::InvalidParameter);
  REQUIRE_FALSE(linear_phase_strip.schedule_insert_automation(0, 1, 0, 6.0f));

  auto equalizer = std::make_unique<sonare::mastering::eq::EqualizerProcessor>();
  sonare::mastering::eq::EqBand linear_band{sonare::mastering::eq::EqBandType::Peak, 1000.0f, 3.0f,
                                            1.0f, true};
  linear_band.phase = sonare::mastering::eq::PhaseMode::LinearPhase;
  equalizer->set_band(0, linear_band);
  sonare::mixing::ChannelStrip equalizer_strip;
  equalizer_strip.add_pre_insert(std::move(equalizer));
  equalizer_strip.prepare(48000.0, 128);

  REQUIRE(equalizer_strip.schedule_insert_automation_result(0, 1, 0, 6.0f) ==
          sonare::mixing::InsertAutomationScheduleResult::NotSupported);
  REQUIRE_FALSE(equalizer_strip.schedule_insert_automation(0, 1, 0, 6.0f));

  sonare::mixing::ChannelStrip maximizer_strip;
  maximizer_strip.add_pre_insert(std::make_unique<sonare::mastering::maximizer::Maximizer>());
  maximizer_strip.prepare(48000.0, 128);

  REQUIRE(maximizer_strip.schedule_insert_automation(0, 0, 0, 3.0f));
  REQUIRE(maximizer_strip.schedule_insert_automation_result(0, 1, 0, -2.0f) ==
          sonare::mixing::InsertAutomationScheduleResult::NotSupported);
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

TEST_CASE("Shared mixer strip sum-then-process diverges from per-track process-then-sum",
          "[mixing]") {
  // A scene strip referenced by several tracks (N tracks -> 1 strip) sums its
  // sources THEN runs the strip inserts once. The offline channel-strip bounce
  // realizes exactly this ("sum-then-process", c_api/project_bounce.cpp
  // bounce_through_mixer). The live TrackMixerRuntime is strictly one track per
  // strip (set_track_lanes rejects duplicate track ids), so it cannot express a
  // shared strip; a naive per-track wiring would instead run each track through
  // its own copy of the insert and sum after ("process-then-sum"). This test
  // pins that the two orders genuinely differ when the strip holds a nonlinear
  // insert, so the divergence is not a rounding artifact -- it is the reason a
  // shared-strip project mix is currently reproducible only offline.
  constexpr int kN = 8;
  constexpr float kA = 0.4f;  // track A DC level
  constexpr float kB = 0.4f;  // track B DC level (kA + kB = 0.8 > limit)
  constexpr float kLimit = 0.5f;

  // sum-then-process: sum the two track stems, run ONE shared strip.
  std::array<float, kN> summed_l{};
  std::array<float, kN> summed_r{};
  summed_l.fill(kA + kB);
  summed_r.fill(kA + kB);
  float* summed[] = {summed_l.data(), summed_r.data()};
  sonare::mixing::ChannelStrip shared(
      {0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});  // unity fader, center pan
  shared.add_pre_insert(std::make_unique<HardClipProcessor>(kLimit));
  shared.prepare(48000.0, kN);
  shared.process(summed, 2, kN);

  // process-then-sum: each track through its own copy of the strip, summed after.
  auto run_single = [&](float level) {
    std::array<float, kN> l{};
    std::array<float, kN> r{};
    l.fill(level);
    r.fill(level);
    float* ch[] = {l.data(), r.data()};
    sonare::mixing::ChannelStrip strip({0.0f, 0.0f, sonare::mixing::PanLaw::Linear0dB, 0.0f});
    strip.add_pre_insert(std::make_unique<HardClipProcessor>(kLimit));
    strip.prepare(48000.0, kN);
    strip.process(ch, 2, kN);
    return l;  // both channels identical for center-pan DC
  };
  const std::array<float, kN> track_a = run_single(kA);
  const std::array<float, kN> track_b = run_single(kB);

  for (int i = 0; i < kN; ++i) {
    // Shared strip clips the 0.8 sum down to the 0.5 limit.
    REQUIRE_THAT(summed_l[static_cast<size_t>(i)], WithinAbs(kLimit, 0.0001f));
    // Per-track: neither 0.4 stem clips, so their sum stays at 0.8.
    const float per_track_sum = track_a[static_cast<size_t>(i)] + track_b[static_cast<size_t>(i)];
    REQUIRE_THAT(per_track_sum, WithinAbs(kA + kB, 0.0001f));
    // The two grouping orders differ by a full 0.3 here -- not a rounding gap.
    REQUIRE(std::abs(per_track_sum - summed_l[static_cast<size_t>(i)]) > 0.25f);
  }
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
