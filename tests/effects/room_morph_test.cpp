#include "effects/acoustic/room_morph.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "acoustic/image_source.h"
#include "acoustic/material.h"
#include "acoustic/rir_synthesizer.h"
#include "acoustic/room_model.h"
#include "analysis/acoustic_analyzer.h"
#include "core/audio.h"
#include "effects/reverb/convolution_reverb.h"
#include "metering/lufs.h"
#include "rt/biquad_design.h"
#include "util/constants.h"
#include "util/exception.h"

using namespace sonare;
using namespace sonare::effects::acoustic;
using sonare::acoustic::Material;
using sonare::acoustic::ShoeboxRoom;
using sonare::acoustic::SourceListener;
using sonare::acoustic::synthesize_rir;
using sonare::acoustic::uniform_material;

namespace {

ShoeboxRoom uniform_room(float length, float width, float height, float absorption) {
  ShoeboxRoom room;
  room.dims = {length, width, height};
  for (Material& w : room.walls) w = uniform_material(absorption, 0.0f);
  return room;
}

double energy(const Audio& a) {
  double e = 0.0;
  for (size_t i = 0; i < a.size(); ++i) e += static_cast<double>(a[i]) * a[i];
  return e;
}

// Deterministic white noise: a full-band excitation, so a measured wet level
// reflects the impulse response's own energy rather than the test signal's
// spectral overlap with it.
std::vector<float> noise(size_t count) {
  std::vector<float> samples(count);
  std::uint32_t state = 0x1234567u;
  for (size_t i = 0; i < count; ++i) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    samples[i] = (static_cast<float>(state) / 2147483648.0f - 1.0f) * 0.25f;
  }
  return samples;
}

template <typename Processor>
std::vector<float> render(Processor& processor, std::vector<float> buffer, int block) {
  for (size_t offset = 0; offset + static_cast<size_t>(block) <= buffer.size();
       offset += static_cast<size_t>(block)) {
    float* data = buffer.data() + offset;
    processor.process(&data, 1, block);
  }
  return buffer;
}

float integrated_lufs(const std::vector<float>& samples, int sample_rate) {
  return sonare::metering::lufs(Audio::from_vector(std::vector<float>(samples), sample_rate))
      .integrated_lufs;
}

constexpr int kStereoRate = 48000;

// The reference room of the stereo criteria: 8x6x3.5 m, uniform absorption 0.15.
RoomMorphConfig reference_stereo_config() {
  RoomMorphConfig cfg;
  cfg.target = uniform_room(8.0f, 6.0f, 3.5f, 0.15f);
  cfg.placement = {{2.0f, 3.0f, 1.2f}, {6.0f, 3.0f, 1.5f}};
  cfg.seed = 1u;
  cfg.wet = 1.0f;
  cfg.source_tail_suppression = 0.0f;
  cfg.receiver_spacing_m = 0.5f;
  return cfg;
}

// 4th-order Butterworth highpass at 1 kHz: two cascaded RBJ sections.
std::vector<double> highpass_1khz(const Audio& audio) {
  using sonare::constants::kPiD;
  std::vector<double> out(audio.size());
  for (size_t i = 0; i < audio.size(); ++i) out[i] = static_cast<double>(audio[i]);
  for (const int k : {1, 3}) {
    rt::BiquadStateD section;
    section.set(rt::rbj_highpass_d(1000.0, static_cast<double>(audio.sample_rate()),
                                   1.0 / (2.0 * std::cos(k * kPiD / 8.0))));
    for (double& x : out) x = section.process(x);
  }
  return out;
}

double correlation(const std::vector<double>& a, const std::vector<double>& b) {
  double ab = 0.0, aa = 0.0, bb = 0.0, ma = 0.0, mb = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    ma += a[i];
    mb += b[i];
  }
  ma /= static_cast<double>(a.size());
  mb /= static_cast<double>(b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    ab += (a[i] - ma) * (b[i] - mb);
    aa += (a[i] - ma) * (a[i] - ma);
    bb += (b[i] - mb) * (b[i] - mb);
  }
  return ab / std::sqrt(aa * bb);
}

}  // namespace

TEST_CASE("room_morph moves the reverberation toward the target room",
          "[.][slow][effects][acoustic][room_morph]") {
  const int sr = 48000;
  // A small, fairly dead source room (short RT60) recorded as its own RIR, and
  // a large, live target room (long RT60).
  const ShoeboxRoom source_room = uniform_room(4.0f, 3.0f, 2.5f, 0.4f);
  const ShoeboxRoom target_room = uniform_room(12.0f, 9.0f, 5.0f, 0.08f);
  const SourceListener src_pl{{1.0f, 1.0f, 1.2f}, {2.5f, 2.0f, 1.5f}};
  const SourceListener tgt_pl{{2.0f, 2.0f, 1.5f}, {8.0f, 6.0f, 1.7f}};

  const Audio recording = synthesize_rir(source_room, src_pl, sr).rir;
  const Audio target_rir = synthesize_rir(target_room, tgt_pl, sr).rir;
  REQUIRE(recording.size() > 0);
  REQUIRE(target_rir.size() > 0);

  RoomMorphConfig cfg;
  cfg.target = target_room;
  cfg.placement = tgt_pl;
  cfg.wet = 0.6f;
  cfg.source_tail_suppression = 0.5f;

  const Audio morphed = room_morph(recording, cfg).audio;
  REQUIRE(morphed.size() > recording.size());  // target reverb tail was appended

  const AcousticParameters src = detect_acoustic(recording);
  const AcousticParameters tgt = detect_acoustic(target_rir);
  const AcousticParameters morph = detect_acoustic(morphed);
  REQUIRE(src.rt60 > 0.0f);
  REQUIRE(tgt.rt60 > src.rt60);  // the target room really is more reverberant

  // The morph must lengthen the source decay and land closer to the target than
  // the untouched recording does (directional, not exact -- no dereverb).
  REQUIRE(morph.rt60 > src.rt60);
  REQUIRE(std::abs(morph.rt60 - tgt.rt60) < std::abs(src.rt60 - tgt.rt60));

  // Clarity moves the same way: adding the larger room's reverberation reduces
  // C50, so the morph's clarity must drop below the (dead) recording's, toward
  // the more reverberant target. (The target RIR's own broadband C50 is not
  // asserted against -- the analyzer reports it blind for a pure RIR -- but the
  // direction of travel from the source is the meaningful check.)
  REQUIRE(std::isfinite(src.c50));
  REQUIRE(std::isfinite(morph.c50));
  REQUIRE(morph.c50 < src.c50);
}

TEST_CASE("room_morph is a passthrough at zero wet and zero suppression",
          "[effects][acoustic][room_morph]") {
  const int sr = 48000;
  const ShoeboxRoom target_room = uniform_room(8.0f, 6.0f, 3.5f, 0.15f);

  std::vector<float> samples(2000, 0.0f);
  samples[0] = 1.0f;
  for (int i = 1; i < 400; ++i) samples[i] = 0.6f * std::exp(-static_cast<float>(i) / 120.0f);
  const Audio rec = Audio::from_vector(std::vector<float>(samples), sr);

  RoomMorphConfig cfg;
  cfg.target = target_room;
  cfg.placement = {{1.0f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};
  cfg.wet = 0.0f;                      // no target room added
  cfg.source_tail_suppression = 0.0f;  // suppressor bypassed

  const Audio out = room_morph(rec, cfg).audio;
  REQUIRE(out.size() == rec.size());
  // Dry-only, latency-compensated: the leading recording is reproduced exactly.
  for (size_t i = 0; i < rec.size(); ++i) {
    REQUIRE(std::abs(out[i] - rec[i]) < 1e-5f);
  }
}

TEST_CASE("room_morph suppression reduces the source tail energy",
          "[effects][acoustic][room_morph]") {
  const int sr = 48000;
  // A decaying tail after a transient -- the reverberant content the suppressor
  // should pull down. Compare wet=0 (suppression only) at full vs zero amount.
  std::vector<float> samples(8000, 0.0f);
  samples[0] = 1.0f;
  for (int i = 1; i < 6000; ++i) samples[i] = 0.5f * std::exp(-static_cast<float>(i) / 1500.0f);
  const Audio rec = Audio::from_vector(std::vector<float>(samples), sr);

  RoomMorphConfig base;
  base.target = uniform_room(8.0f, 6.0f, 3.5f, 0.15f);
  base.placement = {{1.0f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};
  base.wet = 0.0f;  // isolate the suppressor

  RoomMorphConfig none = base;
  none.source_tail_suppression = 0.0f;
  RoomMorphConfig full = base;
  full.source_tail_suppression = 1.0f;

  const Audio out_full = room_morph(rec, full).audio;
  const double e_none = energy(room_morph(rec, none).audio);
  const double e_full = energy(out_full);
  REQUIRE(e_full < e_none);  // the tail was attenuated
  REQUIRE(e_full > 0.0);     // but not removed (the transient survives)

  // The expander is light, not a gate: even at full suppression the direct
  // onset is preserved near unity (smoothing holds the transient gain high).
  float out_peak = 0.0f;
  for (size_t i = 0; i < out_full.size(); ++i) out_peak = std::max(out_peak, std::abs(out_full[i]));
  REQUIRE(out_peak >= 0.8f);  // input onset was 1.0
}

TEST_CASE("room_morph silence does not pull the suppressor gain down before an onset",
          "[effects][acoustic][room_morph]") {
  const int sr = 48000;
  RoomMorphConfig config;
  config.target = uniform_room(8.0f, 6.0f, 3.5f, 0.15f);
  config.placement = {{1.0f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};
  config.wet = 0.0f;  // isolate the suppressor
  config.source_tail_suppression = 1.0f;

  const auto onset_after = [&](std::vector<float> lead_in) {
    const size_t onset = lead_in.size();
    lead_in.resize(onset + 1000, 0.0f);
    lead_in[onset] = 1.0f;
    const Audio out = room_morph(Audio::from_vector(std::move(lead_in), sr), config).audio;
    return out[onset];
  };
  const float fresh = onset_after({});
  REQUIRE(fresh > 0.99f);
  // One second of silence, with and without a decaying tail ahead of it.
  REQUIRE(std::abs(onset_after(std::vector<float>(sr, 0.0f)) - fresh) < 1.0e-3f);
  std::vector<float> tail(static_cast<size_t>(sr) * 2, 0.0f);
  for (int i = 0; i < 6000; ++i) tail[static_cast<size_t>(i)] = 0.5f * std::exp(-i / 1500.0f);
  REQUIRE(std::abs(onset_after(tail) - fresh) < 1.0e-3f);
}

TEST_CASE("room_morph rejects an invalid target room before processing",
          "[effects][acoustic][room_morph]") {
  const int sr = 48000;
  ShoeboxRoom target = uniform_room(8.0f, 6.0f, 3.5f, 0.15f);
  std::vector<float> samples(1000, 0.0f);
  samples[0] = 1.0f;
  const Audio rec = Audio::from_vector(std::vector<float>(samples), sr);

  RoomMorphConfig cfg;
  cfg.target = target;
  // Source placed outside the room => validate_shoebox errors => empty RIR.
  cfg.placement = {{99.0f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};

  REQUIRE_THROWS_AS(room_morph(rec, cfg), SonareException);
}

TEST_CASE("room_morph returns the target synthesis diagnostics rather than dropping them",
          "[effects][acoustic][room_morph]") {
  constexpr int sr = 48000;
  std::vector<float> samples(2000, 0.0f);
  samples[0] = 1.0f;
  const Audio rec = Audio::from_vector(std::move(samples), sr);

  RoomMorphConfig cfg;
  cfg.target = uniform_room(8.0f, 6.0f, 3.5f, 0.3f);
  cfg.placement = {{2.0f, 2.0f, 1.5f}, {6.0f, 4.0f, 1.7f}};
  cfg.max_seconds = 0.3f;

  const auto clamped_order = [](const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& d) {
      return d.code == "acoustic.ism_order_clamped" && d.severity == Diagnostic::Severity::Warning;
    });
  };

  // The same clamp synthesize_rir reports. It used to be read for its Errors and
  // then discarded, so a morph through a room the caller did not ask for came
  // back indistinguishable from one through the room they did.
  cfg.ism_order = sonare::acoustic::kMaxImageSourceOrder + 1;
  const RoomMorphResult clamped = room_morph(rec, cfg);
  REQUIRE_FALSE(clamped.audio.empty());
  REQUIRE(clamped_order(clamped.diagnostics));

  // The highest order the synthesizer honours is not clamped, so the case above
  // fails for the clamp rather than for any order being reported.
  cfg.ism_order = sonare::acoustic::kMaxImageSourceOrder;
  const RoomMorphResult exact = room_morph(rec, cfg);
  REQUIRE_FALSE(exact.audio.empty());
  REQUIRE_FALSE(clamped_order(exact.diagnostics));
}

TEST_CASE("room_morph rejects non-finite and out-of-range controls",
          "[effects][acoustic][room_morph][numeric]") {
  std::vector<float> samples(1000, 0.0f);
  samples[0] = 1.0f;
  const Audio rec = Audio::from_vector(std::move(samples), 48000);

  RoomMorphConfig base;
  base.target = uniform_room(8.0f, 6.0f, 3.5f, 0.15f);
  base.placement = {{1.0f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};

  for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity(), -0.1f, 1.1f}) {
    RoomMorphConfig cfg = base;
    cfg.wet = invalid;
    REQUIRE_THROWS_AS(room_morph(rec, cfg), SonareException);
  }

  RoomMorphConfig timing = base;
  timing.max_seconds = std::numeric_limits<float>::quiet_NaN();
  REQUIRE_THROWS_AS(room_morph(rec, timing), SonareException);

  // Air absorption is only validated while enabled -- an implausible value
  // left in a disabled block must not fail the whole config.
  RoomMorphConfig disabled_air = base;
  disabled_air.air_absorption_enabled = false;
  disabled_air.air = sonare::acoustic::AirAbsorption{-500.0f, 250.0f};
  REQUIRE_NOTHROW(room_morph(rec, disabled_air));

  for (const sonare::acoustic::AirAbsorption& invalid_air :
       {sonare::acoustic::AirAbsorption{-500.0f, 50.0f},
        sonare::acoustic::AirAbsorption{20.0f, 250.0f},
        sonare::acoustic::AirAbsorption{std::numeric_limits<float>::quiet_NaN(), 50.0f}}) {
    RoomMorphConfig enabled_air = base;
    enabled_air.air_absorption_enabled = true;
    enabled_air.air = invalid_air;
    REQUIRE_THROWS_AS(room_morph(rec, enabled_air), SonareException);
  }
}

TEST_CASE("RoomMorphProcessor::prepare reports a refused target synthesis",
          "[effects][acoustic][room_morph][numeric]") {
  // The host sample rate is the one synthesis input the constructor cannot see.
  // A rate synthesis refuses returns an Error diagnostic plus an empty target
  // RIR, which the processor used to load, preparing a silently inert insert.
  RoomMorphConfig cfg;
  cfg.target = uniform_room(8.0f, 6.0f, 3.5f, 0.15f);
  cfg.placement = {{1.0f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};

  RoomMorphProcessor processor(cfg);
  REQUIRE_THROWS_AS(processor.prepare(100.0, 256), SonareException);
  try {
    processor.prepare(100.0, 256);
  } catch (const SonareException& error) {
    REQUIRE(error.code() == ErrorCode::InvalidParameter);
    REQUIRE(std::string(error.what()).find("acoustic.invalid_sample_rate") != std::string::npos);
  }
  REQUIRE_NOTHROW(processor.prepare(48000.0, 256));
}

TEST_CASE("room morph wet level matches the convolution reverb at the same mix",
          "[effects][acoustic][room_morph]") {
  // `wet` must mean the same audible mix depth as the convolution reverb's
  // dryWet: the target RIR carries a physical 1/(4*pi*d) attenuation, so loading
  // it unscaled left room-morph users needing an undocumented makeup gain.
  const int sr = 48000;
  constexpr int kBlock = 256;
  const std::vector<float> input = noise(static_cast<size_t>(sr) * 2);

  RoomMorphConfig cfg;
  cfg.target = uniform_room(8.0f, 6.0f, 3.5f, 0.15f);
  cfg.placement = {{1.0f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};
  cfg.wet = 1.0f;                      // target room only
  cfg.source_tail_suppression = 0.0f;  // isolate the target-room level
  cfg.max_seconds = 1.0f;
  RoomMorphProcessor morph(cfg);
  morph.prepare(static_cast<double>(sr), kBlock);
  const std::vector<float> wet_morph = render(morph, input, kBlock);

  sonare::effects::reverb::ConvolutionReverbConfig conv_config;
  conv_config.decay_sec = 1.0f;
  conv_config.dry_wet = 1.0f;
  sonare::effects::reverb::ConvolutionReverb convolution(conv_config);
  convolution.prepare(static_cast<double>(sr), kBlock);
  const std::vector<float> wet_convolution = render(convolution, input, kBlock);

  REQUIRE(std::fabs(integrated_lufs(wet_morph, sr) - integrated_lufs(wet_convolution, sr)) < 3.0f);
}

TEST_CASE("room_morph is deterministic", "[effects][acoustic][room_morph]") {
  const int sr = 48000;
  std::vector<float> samples(1500, 0.0f);
  samples[0] = 1.0f;
  samples[200] = 0.3f;
  const Audio rec = Audio::from_vector(std::vector<float>(samples), sr);

  RoomMorphConfig cfg;
  cfg.target = uniform_room(7.0f, 5.0f, 3.0f, 0.15f);
  cfg.placement = {{1.5f, 1.0f, 1.2f}, {5.0f, 4.0f, 1.7f}};

  const Audio a = room_morph(rec, cfg).audio;
  const Audio b = room_morph(rec, cfg).audio;
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
}

TEST_CASE("room_morph exposes the late-tail synthesis controls",
          "[effects][acoustic][room_morph]") {
  using sonare::acoustic::ReverbModel;
  const int sr = 48000;
  std::vector<float> samples(4000, 0.0f);
  samples[0] = 1.0f;
  const Audio rec = Audio::from_vector(std::vector<float>(samples), sr);

  // An absorptive target makes Sabine and Eyring diverge, so swapping late_model
  // (now plumbed through RoomMorphConfig) must change the rendered target tail.
  RoomMorphConfig sabine;
  sabine.target = uniform_room(12.0f, 9.0f, 5.0f, 0.4f);
  sabine.placement = {{2.0f, 2.0f, 1.5f}, {8.0f, 6.0f, 1.7f}};
  sabine.wet = 1.0f;
  sabine.source_tail_suppression = 0.0f;
  sabine.max_seconds = 0.3f;
  sabine.late_model = ReverbModel::Sabine;

  RoomMorphConfig eyring = sabine;
  eyring.late_model = ReverbModel::Eyring;

  const Audio a = room_morph(rec, sabine).audio;
  const Audio b = room_morph(rec, eyring).audio;
  bool differs = a.size() != b.size();
  const size_t common = std::min(a.size(), b.size());
  for (size_t i = 0; i < common && !differs; ++i) differs = (a[i] != b[i]);
  REQUIRE(differs);

  // The mixing-time / crossfade overrides are accepted and produce a usable,
  // deterministic render (exercises the new pass-through fields end to end).
  RoomMorphConfig pinned = eyring;
  pinned.mixing_time_ms = 35.0f;
  pinned.crossfade_ms = 12.0f;
  const Audio p1 = room_morph(rec, pinned).audio;
  const Audio p2 = room_morph(rec, pinned).audio;
  REQUIRE(p1.size() == p2.size());
  REQUIRE(p1.size() > rec.size());
  for (size_t i = 0; i < p1.size(); ++i) REQUIRE(p1[i] == p2[i]);
}

TEST_CASE("room_morph exposes air absorption controls", "[effects][acoustic][room_morph]") {
  const int sr = 48000;
  std::vector<float> samples(4000, 0.0f);
  samples[0] = 1.0f;
  const Audio rec = Audio::from_vector(std::vector<float>(samples), sr);

  RoomMorphConfig without_air;
  without_air.target = uniform_room(30.0f, 24.0f, 15.0f, 0.2f);
  without_air.placement = {{3.0f, 3.0f, 1.5f}, {10.0f, 8.0f, 1.7f}};
  without_air.wet = 1.0f;
  without_air.source_tail_suppression = 0.0f;
  without_air.seed = 3u;
  // A short cap keeps this a cheap wiring/reachability check; the RT60
  // magnitude itself is covered by the dedicated rir_synthesizer_test.cpp and
  // insert_factory_extra_test.cpp cases.
  without_air.max_seconds = 0.3f;

  RoomMorphConfig with_air = without_air;
  with_air.air_absorption_enabled = true;
  with_air.air = sonare::acoustic::AirAbsorption{};  // ISO reference climate

  const Audio a = room_morph(rec, without_air).audio;
  const Audio b = room_morph(rec, with_air).audio;
  bool differs = a.size() != b.size();
  const size_t common = std::min(a.size(), b.size());
  for (size_t i = 0; i < common && !differs; ++i) differs = (a[i] != b[i]);
  REQUIRE(differs);
}

TEST_CASE("room_morph_stereo decorrelates the added target room above 1 kHz",
          "[.][slow][effects][acoustic][room_morph][stereo]") {
  const Audio input = Audio::from_vector(noise(static_cast<size_t>(kStereoRate) * 3), kStereoRate);
  const RoomMorphStereoResult out = room_morph_stereo(input, input, reference_stereo_config());
  REQUIRE(out.left.size() == out.right.size());
  REQUIRE(out.left.size() > input.size());

  const double rho = correlation(highpass_1khz(out.left), highpass_1khz(out.right));
  INFO("correlation above 1 kHz " << rho);
  REQUIRE(std::abs(rho) < 0.3);
}

TEST_CASE("room_morph_stereo links the suppressor gain across the channels",
          "[effects][acoustic][room_morph][stereo]") {
  constexpr size_t kLength = kStereoRate;  // 1 s
  constexpr size_t kOnset = kStereoRate * 3 / 10;
  std::vector<float> right(kLength);
  for (size_t i = 0; i < kLength; ++i) {
    right[i] = 0.1f * static_cast<float>(std::sin(2.0 * sonare::constants::kPiD * 1000.0 *
                                                  static_cast<double>(i) / kStereoRate));
  }
  std::vector<float> left(kLength, 0.0f);
  const std::vector<float> tail = noise(kLength - kOnset);
  for (size_t i = kOnset; i < kLength; ++i) {
    const float decay =
        std::exp(-static_cast<float>(i - kOnset) / (0.3f * static_cast<float>(kStereoRate)));
    left[i] = tail[i - kOnset] * decay;  // a tail 12 dB under the onset peak
  }
  left[kOnset] = 1.0f;

  RoomMorphConfig cfg = reference_stereo_config();
  cfg.source_tail_suppression = 1.0f;
  cfg.wet = 0.0f;
  const RoomMorphStereoResult out =
      room_morph_stereo(Audio::from_vector(left, kStereoRate),
                        Audio::from_vector(std::vector<float>(right), kStereoRate), cfg);

  const size_t from = kOnset + static_cast<size_t>(kStereoRate) * 20 / 1000;
  const size_t to = kOnset + static_cast<size_t>(kStereoRate) * 200 / 1000;
  const auto rms_ratio = [&](const Audio& output, const std::vector<float>& input) {
    double e_out = 0.0;
    double e_in = 0.0;
    for (size_t i = from; i < to; ++i) {
      e_out += static_cast<double>(output[i]) * output[i];
      e_in += static_cast<double>(input[i]) * input[i];
    }
    return std::sqrt(e_out / e_in);
  };
  const double left_ratio = rms_ratio(out.left, left);
  const double right_ratio = rms_ratio(out.right, right);
  INFO("output/input RMS ratio: left " << left_ratio << ", right " << right_ratio);
  // Non-vacuity: the tail in the driving channel is suppressed at all, and the right channel
  // on its own (what a per-channel suppressor saw) is left untouched.
  const double alone_ratio = rms_ratio(
      room_morph(Audio::from_vector(std::vector<float>(right), kStereoRate), cfg).audio, right);
  INFO("right channel suppressed on its own " << alone_ratio);
  REQUIRE(left_ratio < 0.9);
  REQUIRE(alone_ratio >= 0.999);
  REQUIRE(right_ratio < 0.9);
}

TEST_CASE("a one-channel RoomMorphProcessor renders exactly what room_morph renders",
          "[effects][acoustic][room_morph][stereo]") {
  constexpr int kBlock = 256;
  RoomMorphConfig cfg = reference_stereo_config();
  cfg.wet = 0.6f;
  cfg.source_tail_suppression = 0.5f;
  cfg.max_seconds = 0.3f;
  const Audio rec = Audio::from_vector(noise(kStereoRate / 4), kStereoRate);

  const Audio offline = room_morph(rec, cfg).audio;

  // The insert's layout loads the pair too; one channel must still run the mono RIR.
  RoomMorphProcessor processor(cfg);
  processor.prepare(static_cast<double>(kStereoRate), kBlock);
  const size_t latency = static_cast<size_t>(processor.latency_samples());
  const size_t total = offline.size() + latency;
  std::vector<float> buf(total, 0.0f);
  std::copy(rec.begin(), rec.end(), buf.begin());
  for (size_t off = 0; off < total; off += kBlock) {
    float* blk = buf.data() + off;
    processor.process(&blk, 1, static_cast<int>(std::min<size_t>(kBlock, total - off)));
  }
  for (size_t i = 0; i < offline.size(); ++i) {
    INFO("sample " << i);
    REQUIRE(buf[i + latency] == offline[i]);
  }
}

TEST_CASE("room_morph_stereo wet level matches the mono wet level",
          "[.][slow][effects][acoustic][room_morph][stereo]") {
  const Audio input = Audio::from_vector(noise(static_cast<size_t>(kStereoRate)), kStereoRate);
  const RoomMorphConfig cfg = reference_stereo_config();
  const double e_mono = energy(room_morph(input, cfg).audio);
  const RoomMorphStereoResult stereo = room_morph_stereo(input, input, cfg);
  const double e_pair = 0.5 * (energy(stereo.left) + energy(stereo.right));
  const double db = 10.0 * std::log10(e_pair / e_mono);
  INFO("pair minus mono wet energy " << db << " dB");
  REQUIRE(std::abs(db) < 1.0);
}

TEST_CASE("a non-finite sample in one channel does not reach the other through the linked gain",
          "[effects][acoustic][room_morph][stereo][numeric]") {
  constexpr int kBlock = 256;
  RoomMorphConfig cfg = reference_stereo_config();
  cfg.wet = 0.5f;
  cfg.source_tail_suppression = 1.0f;
  cfg.max_seconds = 0.2f;
  for (const float poison :
       {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    DYNAMIC_SECTION("poison " << poison) {
      RoomMorphProcessor processor(cfg);
      processor.prepare(static_cast<double>(kStereoRate), kBlock);
      std::vector<float> left = noise(static_cast<size_t>(kBlock) * 16);
      std::vector<float> right = left;
      left[static_cast<size_t>(kBlock) * 2 + 10] = poison;
      for (size_t off = 0; off < left.size(); off += kBlock) {
        float* blk[2] = {left.data() + off, right.data() + off};
        processor.process(blk, 2, kBlock);
      }
      for (size_t i = 0; i < right.size(); ++i) {
        INFO("sample " << i);
        REQUIRE(std::isfinite(right[i]));
      }
    }
  }
}

TEST_CASE("room_morph_stereo refuses mismatched channels and an invalid receiver pair",
          "[effects][acoustic][room_morph][stereo][numeric]") {
  const RoomMorphConfig cfg = reference_stereo_config();
  const Audio a = Audio::from_vector(std::vector<float>(1000, 0.0f), kStereoRate);
  const auto refuses = [](const auto& call) {
    try {
      call();
    } catch (const SonareException& error) {
      return error.code() == ErrorCode::InvalidParameter;
    }
    return false;
  };
  REQUIRE(refuses([&] {
    room_morph_stereo(a, Audio::from_vector(std::vector<float>(999, 0.0f), kStereoRate), cfg);
  }));
  REQUIRE(refuses([&] {
    room_morph_stereo(a, Audio::from_vector(std::vector<float>(1000, 0.0f), 44100), cfg);
  }));
  RoomMorphConfig wide = cfg;
  wide.receiver_spacing_m = 4.5f;
  REQUIRE(refuses([&] { room_morph_stereo(a, a, wide); }));
  // The mono entry point never reads the pair spacing.
  REQUIRE_NOTHROW(room_morph(a, wide));
}
