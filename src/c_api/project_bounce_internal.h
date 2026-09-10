#pragma once

/// @file project_bounce_internal.h
/// @brief Guards, the C callback instrument adapter and the hosted-instrument
///        record shared by the project-bounce translation units.

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <vector>

#include "c_api/project_internal.h"
#include "util/numeric_validation.h"

#if defined(SONARE_WITH_ARRANGEMENT)

namespace sonare_c_bounce_detail {

inline bool checked_nonnegative_add(int64_t lhs, int64_t rhs, int64_t* out) noexcept {
  return lhs >= 0 && rhs >= 0 && sonare::numeric::checked_add(lhs, rhs, out);
}

inline bool checked_frame_count(int64_t frames, size_t* out) noexcept {
  if (out == nullptr || frames < 0) return false;
  if (static_cast<uintmax_t>(frames) > static_cast<uintmax_t>(std::numeric_limits<size_t>::max())) {
    return false;
  }
  *out = static_cast<size_t>(frames);
  return true;
}

inline bool checked_frame_shape(int64_t frames, size_t channels, size_t* out) noexcept {
  size_t frame_count = 0;
  return checked_frame_count(frames, &frame_count) &&
         sonare::numeric::checked_size_product(frame_count, channels, kMaxBufferSize, out);
}

#if defined(SONARE_WITH_MIXING)
inline bool checked_frame_shape(int64_t frames, size_t channels, size_t copies,
                                size_t* out) noexcept {
  size_t per_copy = 0;
  return checked_frame_shape(frames, channels, &per_copy) &&
         sonare::numeric::checked_size_product(per_copy, copies, kMaxBufferSize, out);
}

inline bool checked_midi_source_stem_shape(size_t track_count, int64_t frames,
                                           size_t* out) noexcept {
  size_t stem_count = 0;
  return sonare::numeric::checked_add(track_count, size_t{1}, &stem_count) &&
         checked_frame_shape(frames, 2, stem_count, out);
}
#endif

// Adapts a host's C callback table to a sonare::midi::MidiInstrument so the
// bounce engine can drive an external instrument: events are forwarded to
// on_event at their sample-accurate render frame and render() sums the audio.
// Only opaque UMP words / planar buffers cross the seam (invariant 6).
//
// Clock domain: the engine stamps events in DEVICE render frames (see "Event
// clock domain" in midi/instrument.h), a counter the C seam gives the host no
// way to observe -- render() reports only a frame count. Events are therefore
// held until process(), where the block's first device frame (from
// set_transport) turns each into an intra-block offset, and re-expressed in the
// only basis the host can keep: frames this instrument has been asked to render.
// The two bases differ whenever the engine renders nothing (a stopped transport
// with no sounding note, e.g. the smoother-priming block below), so forwarding
// the raw device frame would place every later event too late by that amount.
class CallbackInstrument final : public sonare::midi::MidiInstrument {
 public:
  explicit CallbackInstrument(const SonareInstrumentCallbacks& callbacks) : cb_(callbacks) {}

  void prepare(double sample_rate, int max_block_size) override {
    pending_count_ = 0;
    if (cb_.prepare) cb_.prepare(cb_.user_data, sample_rate, max_block_size);
  }
  void set_transport(const sonare::transport::TransportState& state) noexcept override {
    block_first_frame_ = state.render_frame;
  }
  void process(float* const* channels, int num_channels, int num_samples) override {
    flush_pending(num_samples);
    if (cb_.render) cb_.render(cb_.user_data, channels, num_channels, num_samples);
    rendered_frames_ += num_samples;
  }
  // rendered_frames_ is deliberately NOT cleared: the C table has no reset
  // callback, so the host's own frame counter keeps running across the several
  // render passes a stem bounce makes, and this mirror must keep running too.
  void reset() override { pending_count_ = 0; }
  int latency_samples() const noexcept override { return cb_.latency_samples; }
  int tail_samples() const noexcept override { return cb_.tail_samples; }
  void on_event(uint32_t destination_id, const sonare::midi::MidiEvent& event) noexcept override {
    // Only the UMP words cross the seam, so nothing here borrows the event's
    // SysEx view, which is valid for the duration of this call alone.
    if (cb_.on_event == nullptr || pending_count_ >= pending_.size()) return;
    pending_[pending_count_++] = {destination_id, event.render_frame, event.ump};
  }

  /// CONTROL thread, once a render has produced its last block: forwards
  /// whatever the per-block hold still carries.
  ///
  /// The engine releases every note still sounding AFTER its final process()
  /// call (RealtimeEngine::render_offline), so the note-off that closes a
  /// sustained note -- and the channel reset that follows it -- arrive when no
  /// further block will ever flush them. Without this drain a host would see the
  /// note-on and never its release, and an external instrument would be left
  /// with the note hanging past the end of the bounce. They are placed at
  /// rendered_frames_, one past the last rendered frame, which is the host-basis
  /// image of the render frame the engine stamped them with.
  void flush_trailing_events() noexcept {
    if (cb_.on_event == nullptr) {
      pending_count_ = 0;
      return;
    }
    for (size_t i = 0; i < pending_count_; ++i) {
      cb_.on_event(cb_.user_data, pending_[i].destination_id, pending_[i].ump.words,
                   pending_[i].ump.word_count, rendered_frames_);
    }
    pending_count_ = 0;
  }

 private:
  struct PendingEvent {
    uint32_t destination_id = 0;
    int64_t render_frame = 0;
    sonare::midi::Ump ump{};
  };

  void flush_pending(int num_samples) noexcept {
    const int64_t last = num_samples > 0 ? num_samples - 1 : 0;
    for (size_t i = 0; i < pending_count_; ++i) {
      const int64_t offset = pending_[i].render_frame - block_first_frame_;
      const int64_t placed = offset < 0 ? 0 : offset > last ? last : offset;
      cb_.on_event(cb_.user_data, pending_[i].destination_id, pending_[i].ump.words,
                   pending_[i].ump.word_count, rendered_frames_ + placed);
    }
    pending_count_ = 0;
  }

  SonareInstrumentCallbacks cb_;
  // Bounded per-block event hold. One sub-block normally carries a single event
  // (the engine splits at every MIDI frame); the dense case is a hang-note
  // release, which is capped by the sequencer's active-note table.
  std::array<PendingEvent, 512> pending_{};
  size_t pending_count_ = 0;
  int64_t block_first_frame_ = 0;
  int64_t rendered_frames_ = 0;
};

// A destination id paired with a borrowed instrument pointer (the owning storage
// outlives the render in the caller). Used by the shared bounce core so the
// callback and built-in-synth paths share one render implementation.
struct HostedInstrument {
  uint32_t destination_id = 0;
  sonare::midi::MidiInstrument* instrument = nullptr;
  // Non-null only when `instrument` is the C callback seam. That adapter holds a
  // block's events until the block renders, so the end-of-render release needs
  // an explicit drain (see CallbackInstrument::flush_trailing_events). The
  // built-in synth and SF2 paths consume events as they arrive and leave this
  // null. Naming the concrete type here keeps the drain a compile-time fact
  // rather than a downcast at the end of every render.
  CallbackInstrument* callback = nullptr;
};

// Renders the compiled timeline offline through a fresh engine into `channels`
// (num_channels deinterleaved buffers of length render_frames). `keep` selects
// which clips to include by track id (a null/empty function keeps everything),
// so the channel-strip bounce can isolate one track's audio into a dry stem.
// The hosted instruments are reset and re-registered per render so a stem starts
// from a clean voice state; only clips whose track passes `keep` fire events.
inline bool render_timeline(const arr::CompiledTimeline& timeline,
                            const std::function<bool(uint32_t)>& keep,
                            const std::vector<HostedInstrument>& instruments, double sample_rate,
                            int block_size, int num_channels, int64_t render_frames,
                            std::vector<std::vector<float>>* channels, bool include_audio = true,
                            bool include_midi = true) {
  size_t render_frame_count = 0;
  size_t render_floats = 0;
  if (channels == nullptr || num_channels <= 0 ||
      !checked_frame_shape(render_frames, static_cast<size_t>(num_channels), &render_floats) ||
      !checked_frame_count(render_frames, &render_frame_count)) {
    return false;
  }
  arr::CompiledTimeline filtered = timeline;  // copy re-points marker name pointers
  if (!include_audio) filtered.audio_clips.clear();
  if (!include_midi) filtered.midi_clips.clear();
  if (keep) {
    filtered.audio_clips.erase(
        std::remove_if(filtered.audio_clips.begin(), filtered.audio_clips.end(),
                       [&](const sonare::engine::ClipSchedule& c) { return !keep(c.track_id); }),
        filtered.audio_clips.end());
    filtered.midi_clips.erase(
        std::remove_if(filtered.midi_clips.begin(), filtered.midi_clips.end(),
                       [&](const sonare::midi::MidiClipSchedule& c) { return !keep(c.track_id); }),
        filtered.midi_clips.end());
  }

  sonare::engine::RealtimeEngine engine;
  engine.prepare(sample_rate, block_size);
  arr::apply_to_engine(filtered, engine);
  for (const HostedInstrument& hosted : instruments) {
    hosted.instrument->reset();
    engine.set_midi_instrument(hosted.destination_id, hosted.instrument);
  }

  // Prime the parameter smoothers before the audible render so a non-default
  // static fader/pan does not fade in over the first ~5 ms block. Lane fader/pan
  // smoothers only advance while lanes render, so one process() pass with the
  // transport stopped applies automation at the start position and drains queued
  // commands (setting the smoother targets), then settle_parameters() snaps the
  // smoothers to those targets. Without this the bounce's first block ramps in
  // from 0 dB / centre, which live playback never does and which breaks bit-exact
  // determinism. The primed block renders into a throwaway buffer.
  {
    size_t block_count = 0;
    if (!checked_frame_count(block_size, &block_count)) return false;
    std::vector<std::vector<float>> prime(static_cast<size_t>(num_channels),
                                          std::vector<float>(block_count, 0.0f));
    std::vector<float*> prime_ptrs;
    prime_ptrs.reserve(prime.size());
    for (auto& channel : prime) prime_ptrs.push_back(channel.data());
    engine.process(prime_ptrs.data(), num_channels, block_size);
    engine.settle_parameters();
  }

  sonare::rt::Command play{};
  play.type = sonare::rt::CommandType::kTransportPlay;
  play.sample_time = -1;
  engine.push_command(play);

  channels->assign(static_cast<size_t>(num_channels), std::vector<float>(render_frame_count, 0.0f));
  std::vector<float*> ptrs;
  ptrs.reserve(channels->size());
  for (auto& channel : *channels) ptrs.push_back(channel.data());
  engine.render_offline(ptrs.data(), num_channels, render_frames, block_size);
  for (const HostedInstrument& hosted : instruments) {
    engine.set_midi_instrument(hosted.destination_id, nullptr);
  }
  // Drain after unbinding, not before: clearing a destination releases anything
  // still sounding on it through the OUTGOING instrument, so a drain placed
  // first would leave those releases held for the next pass.
  for (const HostedInstrument& hosted : instruments) {
    if (hosted.callback != nullptr) hosted.callback->flush_trailing_events();
  }
  return true;
}

}  // namespace sonare_c_bounce_detail

#endif  // SONARE_WITH_ARRANGEMENT
