/// @file midi_clip_engine_test.cpp
/// @brief MIDI clip gain/fade envelope applied to actual
///        rendered audio: both engine branches (source-track-aware and the
///        destination-level fallback), the fast per-block resolver against
///        the pure per-sample evaluator, a transport loop wrap inside a
///        single block, and the project-level gain/fade setters reaching
///        `sonare_project_bounce`.

#include <sonare/sonare_c_project_core.h>
#include <sonare/sonare_c_project_edit.h>
#include <sonare/sonare_c_project_instruments.h>
#include <sonare/sonare_c_project_midi.h>

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "core/fade_curve.h"
#include "engine/clip_player.h"
#include "engine/realtime_engine.h"
#include "engine/track_mixer.h"
#include "midi/instrument.h"
#include "midi/midi_clip.h"
#include "midi/midi_clip_envelope.h"
#include "midi/midi_event.h"
#include "rt/command.h"

#if defined(SONARE_WITH_MIXING)

namespace {

using sonare::engine::RealtimeEngine;
using sonare::engine::TrackLaneConfig;
using sonare::midi::MidiClipSchedule;
using sonare::midi::MidiInstrumentSourceOutput;

constexpr double kSampleRate = 48000.0;

MidiClipSchedule make_clip(uint32_t id, uint32_t destination_id, int64_t start_sample,
                           int64_t length_samples, float gain, int64_t fade_in_samples = 0,
                           int64_t fade_out_samples = 0,
                           sonare::FadeCurve fade_in_curve = sonare::FadeCurve::Linear,
                           sonare::FadeCurve fade_out_curve = sonare::FadeCurve::Linear) {
  MidiClipSchedule clip;
  clip.id = id;
  clip.destination_id = destination_id;
  clip.start_sample = start_sample;
  clip.length_samples = length_samples;
  clip.gain = gain;
  clip.fade_in_samples = fade_in_samples;
  clip.fade_out_samples = fade_out_samples;
  clip.fade_in_curve = fade_in_curve;
  clip.fade_out_curve = fade_out_curve;
  return clip;
}

double rms(const std::vector<float>& signal) {
  double sum = 0.0;
  for (float v : signal) sum += static_cast<double>(v) * static_cast<double>(v);
  return std::sqrt(sum / std::max<size_t>(1, signal.size()));
}

// -40 dBFS reference floor, so a diff of two near-silent renders cannot pass as agreement.
constexpr double kMinReferenceRms = 0.01;

// A destination-level instrument that never overrides process_source_tracks,
// forcing the fallback branch (realtime_engine.cpp's plain instrument->process
// path) rather than the source-track-aware one.
class ConstantInstrument final : public sonare::midi::MidiInstrument {
 public:
  explicit ConstantInstrument(float level) : level_(level) {}
  void prepare(double, int) override {}
  void process(float* const* channels, int num_channels, int num_samples) override {
    for (int c = 0; c < num_channels; ++c) {
      if (channels[c] == nullptr) continue;
      for (int i = 0; i < num_samples; ++i) channels[c][i] += level_;
    }
  }
  void reset() override {}
  void on_event(uint32_t, const sonare::midi::MidiEvent&) noexcept override {}

 private:
  float level_ = 0.0f;
};

// A source-track-aware instrument emitting a distinct constant level per
// source_track_id (0 = the source-track-less fallback target), so a shared
// multitimbral destination's several source outputs can be told apart.
class PerSourceInstrument final : public sonare::midi::MidiInstrument {
 public:
  explicit PerSourceInstrument(std::map<uint32_t, float> levels) : levels_(std::move(levels)) {}
  void prepare(double, int) override {}
  void process(float* const*, int, int) override {}
  void reset() override {}
  void on_event(uint32_t, const sonare::midi::MidiEvent&) noexcept override {}
  bool supports_source_track_rendering() const noexcept override { return true; }
  bool process_source_tracks(const MidiInstrumentSourceOutput* outputs, size_t output_count,
                             int num_channels, int num_samples) noexcept override {
    for (size_t s = 0; s < output_count; ++s) {
      const auto it = levels_.find(outputs[s].source_track_id);
      const float level = it != levels_.end() ? it->second : 0.0f;
      for (int c = 0; c < num_channels; ++c) {
        float* buffer = outputs[s].channels[c];
        if (buffer == nullptr) continue;
        for (int i = 0; i < num_samples; ++i) buffer[i] += level;
      }
    }
    return true;
  }

 private:
  std::map<uint32_t, float> levels_;
};

// Captures the source-track-aware branch's per-(destination, source_track_id)
// audio directly -- the same seam project_bounce_stems.cpp reads -- so a test
// observes exactly what reaches that sink instead of routing through lanes.
class CapturingSourceSink final : public sonare::engine::InstrumentSourceRenderSink {
 public:
  void on_instrument_source_audio(uint32_t destination_id, uint32_t source_track_id,
                                  float* const* channels, int, int num_frames,
                                  int64_t) noexcept override {
    std::vector<float>& buffer = buffers_[{destination_id, source_track_id}];
    buffer.insert(buffer.end(), channels[0], channels[0] + num_frames);
  }
  const std::vector<float>& buffer_for(uint32_t destination_id, uint32_t source_track_id) const {
    static const std::vector<float> kEmpty;
    const auto it = buffers_.find({destination_id, source_track_id});
    return it == buffers_.end() ? kEmpty : it->second;
  }

 private:
  std::map<std::pair<uint32_t, uint32_t>, std::vector<float>> buffers_;
};

// Renders `clips` through the fallback (destination-level) branch: a plain
// ConstantInstrument bound to `destination_id`, offline, returning channel 0.
std::vector<float> render_fallback(std::vector<MidiClipSchedule> clips, uint32_t destination_id,
                                   float level, int64_t total_frames, int block_size) {
  RealtimeEngine engine;
  engine.prepare(kSampleRate, block_size);
  engine.set_midi_clips(std::move(clips));
  ConstantInstrument instrument(level);
  REQUIRE(engine.set_midi_instrument(destination_id, &instrument));
  std::vector<float> left(static_cast<size_t>(total_frames), 0.0f);
  std::vector<float> right(static_cast<size_t>(total_frames), 0.0f);
  float* channels[] = {left.data(), right.data()};
  engine.render_offline(channels, 2, total_frames, block_size);
  engine.set_midi_instrument(destination_id, nullptr);
  return left;
}

}  // namespace

// ---------------------------------------------------------------------------
// Both branches: gain 0.5 halves the rendered output.
// ---------------------------------------------------------------------------

TEST_CASE("MIDI clip gain 0.5 halves the fallback-branch instrument output", "[midi_clip_engine]") {
  constexpr int64_t kFrames = 1024;
  constexpr int kBlock = 128;
  constexpr uint32_t kDest = 41;
  constexpr float kLevel = 0.5f;

  const std::vector<float> unity =
      render_fallback({make_clip(1, kDest, 0, kFrames, 1.0f)}, kDest, kLevel, kFrames, kBlock);
  const std::vector<float> half =
      render_fallback({make_clip(1, kDest, 0, kFrames, 0.5f)}, kDest, kLevel, kFrames, kBlock);

  REQUIRE(rms(unity) >= kMinReferenceRms);
  for (int64_t i = 0; i < kFrames; ++i) {
    CAPTURE(i);
    REQUIRE(half[static_cast<size_t>(i)] ==
            Catch::Approx(unity[static_cast<size_t>(i)] * 0.5f).epsilon(1.0e-6));
  }
}

TEST_CASE("MIDI clip gain 0.5 halves every source-track output of a shared destination",
          "[midi_clip_engine]") {
  constexpr int64_t kFrames = 1024;
  constexpr int kBlock = 128;
  constexpr uint32_t kDest = 42;
  const std::map<uint32_t, float> levels = {{0u, 0.3f}, {101u, 0.5f}, {102u, 0.7f}};

  const auto render = [&](float gain) {
    RealtimeEngine engine;
    engine.prepare(kSampleRate, kBlock);
    REQUIRE(engine.set_track_lanes({TrackLaneConfig{101}, TrackLaneConfig{102}}));
    engine.set_midi_clips({make_clip(1, kDest, 0, kFrames, gain)});
    PerSourceInstrument instrument(levels);
    REQUIRE(engine.set_midi_instrument(kDest, &instrument));
    CapturingSourceSink sink;
    engine.set_instrument_source_render_sink(&sink);
    std::vector<float> left(static_cast<size_t>(kFrames), 0.0f);
    std::vector<float> right(static_cast<size_t>(kFrames), 0.0f);
    float* channels[] = {left.data(), right.data()};
    engine.render_offline(channels, 2, kFrames, kBlock);
    engine.set_instrument_source_render_sink(nullptr);
    engine.set_midi_instrument(kDest, nullptr);
    return sink;
  };

  const CapturingSourceSink unity = render(1.0f);
  const CapturingSourceSink half = render(0.5f);

  for (const auto& [source_track_id, level] : levels) {
    (void)level;
    const std::vector<float>& u = unity.buffer_for(kDest, source_track_id);
    const std::vector<float>& h = half.buffer_for(kDest, source_track_id);
    REQUIRE(u.size() == static_cast<size_t>(kFrames));
    REQUIRE(h.size() == static_cast<size_t>(kFrames));
    REQUIRE(rms(u) >= kMinReferenceRms);
    for (size_t i = 0; i < u.size(); ++i) {
      CAPTURE(source_track_id, i);
      REQUIRE(h[i] == Catch::Approx(u[i] * 0.5f).epsilon(1.0e-6));
    }
  }
}

// ---------------------------------------------------------------------------
// Fade curves, hold-after-end, and a contained clip -- all against the
// already-trusted pure evaluator as the oracle for what the rendered audio
// should be.
// ---------------------------------------------------------------------------

TEST_CASE("Fallback-branch instrument output follows clip_fade_gain for every curve",
          "[midi_clip_engine]") {
  constexpr int64_t kFrames = 800;
  constexpr int kBlock = 64;
  constexpr uint32_t kDest = 43;
  constexpr int64_t kFadeIn = 100;
  constexpr int64_t kFadeOut = 150;
  constexpr float kLevel = 0.6f;
  const sonare::FadeCurve curve =
      GENERATE(sonare::FadeCurve::Linear, sonare::FadeCurve::EqualPower,
               sonare::FadeCurve::Exponential, sonare::FadeCurve::Logarithmic);

  const std::vector<MidiClipSchedule> clips = {
      make_clip(1, kDest, 0, kFrames, 1.0f, kFadeIn, kFadeOut, curve, curve)};
  const std::vector<float> out = render_fallback(clips, kDest, kLevel, kFrames, kBlock);

  REQUIRE(rms(out) >= kMinReferenceRms);
  for (int64_t i = 0; i < kFrames; ++i) {
    const float expected =
        kLevel * sonare::clip_fade_gain(i, kFrames, kFadeIn, kFadeOut, curve, curve);
    CAPTURE(i, static_cast<int>(curve));
    REQUIRE(out[static_cast<size_t>(i)] == Catch::Approx(expected).margin(1.0e-6));
  }
}

TEST_CASE("Rendered audio holds the envelope at clip end exactly like midi_clip_envelope_gain",
          "[midi_clip_engine]") {
  constexpr int64_t kFrames = 900;
  constexpr int kBlock = 90;
  constexpr uint32_t kDest = 44;
  constexpr float kLevel = 0.5f;

  SECTION("fade-out present: tail holds at 0") {
    const std::vector<MidiClipSchedule> clips = {
        make_clip(1, kDest, 100, 400, 0.8f, /*fade_in=*/0, /*fade_out=*/50)};
    const std::vector<float> out = render_fallback(clips, kDest, kLevel, kFrames, kBlock);
    for (int64_t t = 0; t < kFrames; ++t) {
      const float expected = kLevel * sonare::midi::midi_clip_envelope_gain(clips, kDest, t);
      CAPTURE(t);
      REQUIRE(out[static_cast<size_t>(t)] == Catch::Approx(expected).margin(1.0e-6));
    }
  }

  SECTION("no fade-out: tail holds at the clip's own gain") {
    const std::vector<MidiClipSchedule> clips = {
        make_clip(1, kDest, 100, 400, 0.8f, /*fade_in=*/0, /*fade_out=*/0)};
    const std::vector<float> out = render_fallback(clips, kDest, kLevel, kFrames, kBlock);
    for (int64_t t = 0; t < kFrames; ++t) {
      const float expected = kLevel * sonare::midi::midi_clip_envelope_gain(clips, kDest, t);
      CAPTURE(t);
      REQUIRE(out[static_cast<size_t>(t)] == Catch::Approx(expected).margin(1.0e-6));
    }
  }
}

TEST_CASE("Rendered audio follows the latest-start-wins rule for a contained clip",
          "[midi_clip_engine]") {
  constexpr int64_t kFrames = 800;
  constexpr int kBlock = 80;
  constexpr uint32_t kDest = 45;
  constexpr float kLevel = 0.5f;
  // A = [0, 800) unity, unfaded. B = [200, 300) contained in A, fading out
  // over its own tail -- A must resume once B ends.
  const std::vector<MidiClipSchedule> clips = {
      make_clip(1, kDest, 0, kFrames, 1.0f),
      make_clip(2, kDest, 200, 100, 0.4f, /*fade_in=*/0, /*fade_out=*/30)};
  const std::vector<float> out = render_fallback(clips, kDest, kLevel, kFrames, kBlock);

  REQUIRE(rms(out) >= kMinReferenceRms);
  for (int64_t t = 0; t < kFrames; ++t) {
    const float expected = kLevel * sonare::midi::midi_clip_envelope_gain(clips, kDest, t);
    CAPTURE(t);
    REQUIRE(out[static_cast<size_t>(t)] == Catch::Approx(expected).margin(1.0e-6));
  }
}

// ---------------------------------------------------------------------------
// Transport loop wrap inside a single block.
// ---------------------------------------------------------------------------

TEST_CASE(
    "The envelope reads the wrapped timeline position, not a naive running "
    "device-frame count, across a transport loop wrap inside one block",
    "[midi_clip_engine]") {
  constexpr int kBlock = 128;
  constexpr int kBlockCount = 6;
  constexpr int64_t kRunFrames = static_cast<int64_t>(kBlock) * kBlockCount;
  // 120 BPM / 48 kHz: one quarter note is 24000 samples, so these PPQ bounds
  // map to exact timeline samples 6000 and 6750.
  constexpr double kLoopStartPpq = 0.25;
  constexpr double kLoopEndPpq = 0.28125;
  constexpr int64_t kSeekDeviceFrame = 3 * kBlock;
  constexpr int64_t kSeekTargetTimeline = 6500;
  constexpr uint32_t kDest = 46;
  constexpr float kLevel = 1.0f;

  // Timeline samples of interest: pre-wrap sustain, pre-wrap held-at-0 (past
  // the clip's fade-out), and post-wrap sustain again -- the transition a
  // wrap-unaware evaluation (continuing to count device frames past the wrap)
  // would get wrong, since post-wrap device frames are only slightly further
  // along than the pre-wrap held-zero region.
  constexpr int64_t kPreWrapSustain = 6640;
  constexpr int64_t kPreWrapHeld = 6720;
  constexpr int64_t kPostWrapSustain = 6005;
  constexpr float kClipGain = 0.4f;

  // Length ends just before kPreWrapHeld, so that sample is already past the
  // fade-out (held at 0) while kPreWrapSustain and kPostWrapSustain both fall
  // in the sustain region before it.
  const std::vector<MidiClipSchedule> clips = {
      make_clip(1, kDest, 0, kPreWrapHeld - 20, kClipGain, /*fade_in=*/0, /*fade_out=*/50)};

  const auto drive_transport = [&](RealtimeEngine& engine) {
    engine.prepare(kSampleRate, kBlock);
    engine.set_tempo(120.0);
    engine.set_loop(kLoopStartPpq, kLoopEndPpq, true);
    sonare::rt::Command play{};
    play.type = sonare::rt::CommandType::kTransportPlay;
    play.sample_time = 0;
    REQUIRE(engine.push_command(play));
    sonare::rt::Command seek{};
    seek.type = sonare::rt::CommandType::kTransportSeekSample;
    seek.sample_time = kSeekDeviceFrame;
    seek.arg.i = kSeekTargetTimeline;
    REQUIRE(engine.push_command(seek));
  };

  const auto render_blocks = [&](RealtimeEngine& engine) {
    std::vector<float> out(static_cast<size_t>(kRunFrames), 0.0f);
    std::vector<float> left(static_cast<size_t>(kBlock), 0.0f);
    std::vector<float> right(static_cast<size_t>(kBlock), 0.0f);
    float* io[] = {left.data(), right.data()};
    for (int b = 0; b < kBlockCount; ++b) {
      std::fill(left.begin(), left.end(), 0.0f);
      std::fill(right.begin(), right.end(), 0.0f);
      engine.process(io, 2, kBlock);
      std::copy(left.begin(), left.end(), out.begin() + static_cast<size_t>(b) * kBlock);
    }
    return out;
  };

  // Oracle pass: an audio-clip impulse at each timeline sample of interest.
  // ClipPlayer positions it purely from the transport's timeline sample
  // position (the same mapping the envelope must use), so its landing device
  // frame -- read back below -- needs no hand-derived wrap arithmetic.
  std::vector<int64_t> impulse_device_frames;
  {
    RealtimeEngine engine;
    drive_transport(engine);
    auto storage = std::make_shared<sonare::engine::ClipAudioStorage>();
    constexpr size_t kStorageLength = 7000;
    storage->channels = {std::vector<float>(kStorageLength, 0.0f),
                         std::vector<float>(kStorageLength, 0.0f)};
    for (int64_t t : {kPreWrapSustain, kPreWrapHeld, kPostWrapSustain}) {
      storage->channels[0][static_cast<size_t>(t)] = 1.0f;
      storage->channels[1][static_cast<size_t>(t)] = 1.0f;
    }
    storage->channel_ptrs = {storage->channels[0].data(), storage->channels[1].data()};
    sonare::engine::ClipSchedule impulse_clip;
    impulse_clip.id = 1;
    impulse_clip.buffer.channels = storage->channel_ptrs.data();
    impulse_clip.buffer.num_channels = 2;
    impulse_clip.buffer.num_samples = static_cast<int64_t>(kStorageLength);
    impulse_clip.start_sample = 0;
    impulse_clip.length_samples = static_cast<int64_t>(kStorageLength);
    impulse_clip.gain = 1.0f;
    impulse_clip.storage = std::move(storage);
    engine.set_clips({impulse_clip});

    const std::vector<float> oracle = render_blocks(engine);
    for (size_t i = 0; i < oracle.size(); ++i) {
      if (std::abs(oracle[i]) > 0.5f) impulse_device_frames.push_back(static_cast<int64_t>(i));
    }
  }
  REQUIRE(impulse_device_frames.size() == 3);

  // Content pass: the same transport script, now with a MIDI clip whose
  // gain/fade-out lands its held-at-0 tail inside the pre-wrap region and its
  // sustain region on both sides of the wrap.
  std::vector<float> content;
  {
    RealtimeEngine engine;
    drive_transport(engine);
    engine.set_midi_clips(clips);
    ConstantInstrument instrument(kLevel);
    REQUIRE(engine.set_midi_instrument(kDest, &instrument));
    content = render_blocks(engine);
  }

  for (size_t i = 0; i < impulse_device_frames.size(); ++i) {
    const int64_t t = std::array<int64_t, 3>{kPreWrapSustain, kPreWrapHeld, kPostWrapSustain}[i];
    const float expected = kLevel * sonare::midi::midi_clip_envelope_gain(clips, kDest, t);
    CAPTURE(t, impulse_device_frames[i]);
    REQUIRE(content[static_cast<size_t>(impulse_device_frames[i])] ==
            Catch::Approx(expected).margin(1.0e-6));
  }
  // The key assertion in prose: sustain, then held-at-0, then sustain AGAIN
  // immediately after the wrap -- not a continued 0 from a wrap-unaware
  // device-frame count.
  REQUIRE(content[static_cast<size_t>(impulse_device_frames[0])] ==
          Catch::Approx(kLevel * kClipGain).margin(1.0e-6));
  REQUIRE(content[static_cast<size_t>(impulse_device_frames[1])] ==
          Catch::Approx(0.0f).margin(1.0e-6));
  REQUIRE(content[static_cast<size_t>(impulse_device_frames[2])] ==
          Catch::Approx(kLevel * kClipGain).margin(1.0e-6));
}

// ---------------------------------------------------------------------------
// The fast per-block resolver vs. the pure per-sample evaluator.
// ---------------------------------------------------------------------------

TEST_CASE(
    "resolve_midi_clip_envelope + apply_midi_clip_envelope match "
    "midi_clip_envelope_gain sample-for-sample",
    "[midi_clip_engine]") {
  using sonare::midi::apply_midi_clip_envelope;
  using sonare::midi::midi_clip_envelope_gain;
  using sonare::midi::MidiClipEnvelopeBlock;
  using sonare::midi::resolve_midi_clip_envelope;

  constexpr uint32_t kDest = 7;
  constexpr int64_t kBlockStart = 1000;
  constexpr int64_t kBlockLength = 2000;

  const auto check = [&](const std::vector<MidiClipSchedule>& clips, bool expect_overflow) {
    MidiClipEnvelopeBlock block;
    resolve_midi_clip_envelope(clips, kDest, kBlockStart, kBlockLength, &block);
    REQUIRE(block.overflowed == expect_overflow);

    std::vector<float> resolved(static_cast<size_t>(kBlockLength), 1.0f);
    float* channels[] = {resolved.data()};
    apply_midi_clip_envelope(block, clips, kDest, kBlockStart, channels, 1,
                             static_cast<int>(kBlockLength));

    for (int64_t i = 0; i < kBlockLength; ++i) {
      const int64_t t = kBlockStart + i;
      const float expected = midi_clip_envelope_gain(clips, kDest, t);
      CAPTURE(t);
      REQUIRE(resolved[static_cast<size_t>(i)] == Catch::Approx(expected).margin(1.0e-6));
    }
  };

  SECTION("representative mix: background, contained, partial overlap, open-ended") {
    const std::vector<MidiClipSchedule> clips = {
        make_clip(1, kDest, 0, 5000, 1.0f),
        make_clip(2, kDest, 1200, 200, 0.3f, /*fade_in=*/20, /*fade_out=*/40),
        make_clip(3, kDest, 1800, 500, 0.6f),
        make_clip(4, kDest, 2600, /*length_samples=*/0, 0.9f, /*fade_in=*/50)};
    check(clips, /*expect_overflow=*/false);
  }

  SECTION("no clips on the destination: identity") { check({}, /*expect_overflow=*/false); }

  SECTION("dense overlapping clips overflow the run capacity and fall back exactly") {
    std::vector<MidiClipSchedule> clips;
    for (uint32_t i = 0; i < 40; ++i) {
      clips.push_back(make_clip(i + 1, kDest, kBlockStart + static_cast<int64_t>(i) * 10, 5,
                                0.5f + 0.01f * static_cast<float>(i)));
    }
    check(clips, /*expect_overflow=*/true);
  }
}

// ---------------------------------------------------------------------------
// Project-level gain/fade setters reaching sonare_project_bounce.
// ---------------------------------------------------------------------------

TEST_CASE("sonare_project_set_clip_gain and set_clip_fade change a MIDI clip's bounced audio",
          "[midi_clip_engine]") {
  SonareProject* project = nullptr;
  REQUIRE(sonare_project_create(&project) == SONARE_OK);
  REQUIRE(sonare_project_set_sample_rate(project, kSampleRate) == SONARE_OK);

  uint32_t track = 0;
  uint32_t clip = 0;
  REQUIRE(sonare_project_add_midi_clip(project, 0.0, 8.0, &track, &clip) == SONARE_OK);
  const SonareMidiEventPod events[] = {{0.0, 0x20903C7Fu, 0u}};  // note-on, note 60, vel 127
  REQUIRE(sonare_project_set_midi_events(project, clip, events, std::size(events)) == SONARE_OK);
  constexpr uint32_t kDestination = 5;
  REQUIRE(sonare_project_set_track_midi_destination(project, track, kDestination) == SONARE_OK);

  SonareProjectBounceOptions options{};
  options.total_frames = static_cast<int64_t>(kSampleRate);  // 1 second: well within the note.
  options.block_size = 256;
  options.num_channels = 2;
  options.sample_rate = static_cast<int>(kSampleRate);

  SonareBuiltinInstrumentBinding binding{};
  binding.destination_id = kDestination;
  binding.config.waveform = 0;
  binding.config.gain = 1.0f;
  binding.config.attack_ms = 1.0f;
  binding.config.decay_ms = 1.0f;
  binding.config.sustain = 1.0f;
  binding.config.release_ms = 1.0f;
  binding.config.polyphony = 1;

  const auto bounce = [&]() {
    float* out = nullptr;
    size_t out_len = 0;
    REQUIRE(sonare_project_bounce_with_builtin_instruments(project, &options, &binding, 1, &out,
                                                           &out_len) == SONARE_OK);
    std::vector<float> result(out, out + out_len);
    sonare_free_floats(out);
    return result;
  };

  const std::vector<float> baseline = bounce();
  REQUIRE(rms(baseline) >= kMinReferenceRms);

  REQUIRE(sonare_project_set_clip_gain(project, clip, 0.4f) == SONARE_OK);
  const std::vector<float> gain_changed = bounce();
  double gain_diff = 0.0;
  for (size_t i = 0; i < baseline.size(); ++i) {
    gain_diff = std::max(gain_diff, static_cast<double>(std::fabs(baseline[i] - gain_changed[i])));
  }
  REQUIRE(gain_diff > 1.0e-4);

  SonareProjectClipFade fade_in{};
  fade_in.length_ppq = 2.0;
  fade_in.curve = 0;
  SonareProjectClipFade fade_out{};
  fade_out.length_ppq = 2.0;
  fade_out.curve = 0;
  REQUIRE(sonare_project_set_clip_fade(project, clip, &fade_in, &fade_out) == SONARE_OK);
  const std::vector<float> fade_changed = bounce();
  double fade_diff = 0.0;
  for (size_t i = 0; i < gain_changed.size(); ++i) {
    fade_diff =
        std::max(fade_diff, static_cast<double>(std::fabs(gain_changed[i] - fade_changed[i])));
  }
  REQUIRE(fade_diff > 1.0e-4);

  sonare_project_destroy(project);
}

#endif  // SONARE_WITH_MIXING
