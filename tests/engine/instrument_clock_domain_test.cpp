/// @file instrument_clock_domain_test.cpp
/// @brief Every MidiEvent an instrument receives must be stamped in ONE clock
///        domain. The engine feeds an instrument from several paths (compiled
///        clips, live MIDI input, queued commands, loop/seek hang-note releases);
///        this exercises them together across a seek and a loop wrap and checks
///        that `event.render_frame - TransportState::render_frame` lands inside
///        the block the instrument is about to render.

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/realtime_engine.h"
#include "host/midi_io.h"
#include "midi/instrument.h"
#include "midi/midi_clip.h"
#include "midi/midi_event.h"
#include "midi/ump.h"
#include "rt/command.h"
#include "transport/transport_state.h"

namespace {

using sonare::engine::RealtimeEngine;
using sonare::midi::MidiEvent;
using sonare::midi::MidiInstrument;

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
constexpr int kBlockCount = 12;
constexpr int64_t kRunFrames = static_cast<int64_t>(kBlock) * kBlockCount;

// Timeline sample positions carrying a coincident pair: one audio-clip impulse
// and one MIDI note-on. The clip impulse is placed by the engine's clip player
// from the transport's timeline position and never touches the MIDI dispatch
// path, so it is an independent oracle for where the note-on's audio belongs.
constexpr int64_t kPairedTimeline[] = {300, 6100, 6600};

// Live MIDI input is stamped in absolute DEVICE render frames by the host, so
// these indices are directly the output-buffer indices their audio must land on.
constexpr int64_t kLiveDeviceFrame[] = {500, 800};

// Device render frame at which the transport seeks; chosen to be a block head so
// the seek's hang-note releases open the block they belong to.
constexpr int64_t kSeekDeviceFrame = 5 * kBlock;
constexpr int64_t kSeekTargetTimeline = 6500;

// Loop region as PPQ. At 120 BPM / 48 kHz one quarter is 24000 samples, so these
// map to timeline samples 6000 and 6750 exactly (0.25 and 0.28125 are both exact
// binary fractions).
constexpr double kLoopStartPpq = 0.25;
constexpr double kLoopEndPpq = 0.28125;

/// Instrument that implements the documented placement contract literally: it
/// takes the block's first frame from the transport snapshot the engine pushes
/// before process(), subtracts it from each queued event's render_frame, and
/// writes a unit impulse at the resulting intra-block offset. Every placement is
/// recorded so a test can see the offsets an instrument actually computes.
class ClockDomainProbe final : public MidiInstrument {
 public:
  struct Placement {
    int64_t block_first_frame = 0;
    int64_t event_frame = 0;
    int64_t offset = 0;
    int block_samples = 0;
    bool note_on = false;
  };

  void prepare(double, int) override {}
  void reset() override {}

  void set_transport(const sonare::transport::TransportState& state) noexcept override {
    block_first_frame_ = state.render_frame;
  }

  void on_event(uint32_t, const MidiEvent& event) noexcept override {
    if (queued_ < queue_.size()) queue_[queued_++] = event;
  }

  void process(float* const* channels, int num_channels, int num_samples) override {
    for (size_t i = 0; i < queued_; ++i) {
      const int64_t offset = queue_[i].render_frame - block_first_frame_;
      placements.push_back({block_first_frame_, queue_[i].render_frame, offset, num_samples,
                            queue_[i].ump.is_note_on()});
      if (offset >= 0 && offset < num_samples) {
        for (int c = 0; c < num_channels; ++c) {
          channels[c][offset] += 1.0f;
        }
      }
    }
    queued_ = 0;
  }

  // Read between blocks by the test only.
  std::vector<Placement> placements;

 private:
  std::array<MidiEvent, 64> queue_{};
  size_t queued_ = 0;
  int64_t block_first_frame_ = 0;
};

/// A stereo clip carrying one unit impulse per paired timeline position.
sonare::engine::ClipSchedule paired_impulse_clip(int64_t length) {
  auto storage = std::make_shared<sonare::engine::ClipAudioStorage>();
  storage->channels = {std::vector<float>(static_cast<size_t>(length), 0.0f),
                       std::vector<float>(static_cast<size_t>(length), 0.0f)};
  for (int64_t position : kPairedTimeline) {
    storage->channels[0][static_cast<size_t>(position)] = 1.0f;
    storage->channels[1][static_cast<size_t>(position)] = 1.0f;
  }
  storage->channel_ptrs = {storage->channels[0].data(), storage->channels[1].data()};
  sonare::engine::ClipSchedule clip;
  clip.id = 1;
  clip.buffer.channels = storage->channel_ptrs.data();
  clip.buffer.num_channels = 2;
  clip.buffer.num_samples = length;
  clip.start_sample = 0;
  clip.length_samples = length;
  clip.gain = 1.0f;
  clip.storage = std::move(storage);
  return clip;
}

/// A MIDI clip whose note-ons coincide with the audio clip's impulses. Distinct
/// note numbers keep the sequencer's active-note bookkeeping unambiguous.
std::vector<sonare::midi::MidiClipSchedule> paired_note_clip() {
  sonare::midi::MidiClipSchedule clip;
  clip.id = 1;
  clip.start_sample = 0;
  clip.length_samples = 1 << 20;
  clip.destination_id = 0;
  uint8_t note = 60;
  for (int64_t position : kPairedTimeline) {
    clip.events.push_back(MidiEvent{position, sonare::midi::make_midi1_note_on(0, 0, note, 100)});
    ++note;
  }
  std::sort(clip.events.begin(), clip.events.end(),
            [](const MidiEvent& a, const MidiEvent& b) { return a.render_frame < b.render_frame; });
  return {clip};
}

/// Drives the identical transport script for both runs: play from the head, seek
/// mid-run, loop enabled throughout (the playhead only reaches loop_end after the
/// seek). `instrument` may be null to render the clip-only oracle pass.
std::vector<float> run_script(MidiInstrument* instrument,
                              sonare::host::FixedMidiInputSource<8>* input) {
  RealtimeEngine engine;
  engine.prepare(kSampleRate, kBlock);
  engine.set_tempo(120.0);
  engine.set_clips({paired_impulse_clip(8192)});
  engine.set_midi_clips(paired_note_clip());
  engine.set_loop(kLoopStartPpq, kLoopEndPpq, true);
  if (instrument != nullptr) REQUIRE(engine.set_midi_instrument(0, instrument));
  if (input != nullptr) {
    engine.set_midi_input_source(input, 0);
    uint8_t note = 90;
    for (int64_t frame : kLiveDeviceFrame) {
      REQUIRE(input->push_event_at_render_frame(sonare::midi::make_midi1_note_on(0, 1, note, 100),
                                                frame));
      ++note;
    }
  }

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = 0;
  REQUIRE(engine.push_command(play));

  sonare::rt::Command seek{};
  seek.type = sonare::rt::CommandType::kTransportSeekSample;
  seek.sample_time = kSeekDeviceFrame;
  seek.arg.i = kSeekTargetTimeline;
  REQUIRE(engine.push_command(seek));

  std::vector<float> out(static_cast<size_t>(kRunFrames), 0.0f);
  std::vector<float> right(static_cast<size_t>(kBlock), 0.0f);
  std::vector<float> left(static_cast<size_t>(kBlock), 0.0f);
  float* io[] = {left.data(), right.data()};
  for (int block = 0; block < kBlockCount; ++block) {
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    engine.process(io, 2, kBlock);
    std::copy(left.begin(), left.end(), out.begin() + static_cast<size_t>(block) * kBlock);
  }
  if (instrument != nullptr) engine.set_midi_instrument(0, nullptr);
  return out;
}

std::vector<int64_t> nonzero_indices(const std::vector<float>& buffer) {
  std::vector<int64_t> indices;
  for (size_t i = 0; i < buffer.size(); ++i) {
    if (std::abs(buffer[i]) > 1e-6f) indices.push_back(static_cast<int64_t>(i));
  }
  return indices;
}

}  // namespace

TEST_CASE("Instrument event frames share one clock domain across seek and loop",
          "[engine][midi][instrument]") {
  // Oracle pass: identical script, no instrument bound. Every impulse here comes
  // from the audio clip, positioned purely from the transport's timeline sample
  // position, so its index is the DEVICE frame at which that timeline sample was
  // rendered.
  const std::vector<float> clip_only = run_script(nullptr, nullptr);
  const std::vector<int64_t> clip_impulses = nonzero_indices(clip_only);
  INFO("clip-only impulse device frames: " << [&] {
    std::string s;
    for (int64_t i : clip_impulses) s += std::to_string(i) + " ";
    return s;
  }());
  // The script must actually traverse both discontinuities: a pre-seek impulse,
  // a post-seek one, and at least one from the looped-back pass.
  REQUIRE(clip_impulses.size() >= 4);
  REQUIRE(clip_impulses.front() == kPairedTimeline[0]);
  REQUIRE(clip_impulses[1] > kSeekDeviceFrame);

  ClockDomainProbe probe;
  sonare::host::FixedMidiInputSource<8> input;
  const std::vector<float> with_instrument = run_script(&probe, &input);

  REQUIRE_FALSE(probe.placements.empty());

  // The contract: every event delivered between two process() calls must land
  // inside the block that follows, in the one basis set_transport() supplies.
  for (size_t i = 0; i < probe.placements.size(); ++i) {
    const auto& p = probe.placements[i];
    INFO("placement " << i << ": block_first_frame=" << p.block_first_frame
                      << " event.render_frame=" << p.event_frame << " offset=" << p.offset
                      << " expected offset in [0, " << p.block_samples << ")");
    REQUIRE(p.offset >= 0);
    REQUIRE(p.offset < p.block_samples);
  }

  // Sequenced note-ons must sum with the clip impulse they were authored
  // alongside: same output frame, amplitude 2.0 instead of 1.0.
  for (int64_t index : clip_impulses) {
    INFO("clip impulse at device frame " << index << " expected instrument note-on to coincide");
    REQUIRE(with_instrument[static_cast<size_t>(index)] ==
            Catch::Approx(clip_only[static_cast<size_t>(index)] + 1.0f));
  }

  // Live MIDI input is already device-framed; its audio must land on exactly the
  // frame the host stamped, with no clip impulse to hide behind.
  for (int64_t frame : kLiveDeviceFrame) {
    INFO("live input device frame " << frame);
    REQUIRE(clip_only[static_cast<size_t>(frame)] == 0.0f);
    REQUIRE(with_instrument[static_cast<size_t>(frame)] == Catch::Approx(1.0f));
  }

  // Non-vacuity: the run must have reached the paths that mix domains, i.e. at
  // least one note-on placement after the seek and one after the loop wrap.
  size_t note_ons_after_seek = 0;
  for (const auto& p : probe.placements) {
    if (p.note_on && p.block_first_frame >= kSeekDeviceFrame) ++note_ons_after_seek;
  }
  REQUIRE(note_ons_after_seek >= 3);
}
