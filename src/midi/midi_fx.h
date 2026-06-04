#pragma once

/// @file midi_fx.h
/// @brief RT-safe MIDI effects: transform an input MidiEvent list into a
///        fixed-capacity output buffer without any heap allocation on the audio
///        path.
///
/// Threading / RT contract
/// -----------------------
///  - CONTROL thread: construct a @ref MidiFxChain, configure its stages via the
///    setters (transpose / quantize / velocity curve / arpeggiator / chord /
///    humanize), then call prepare() once. Configuration MAY allocate (it does
///    not, today, but is allowed to).
///  - AUDIO thread: process() reads a span of input @ref MidiEvent and writes the
///    transformed events into a caller-owned fixed-capacity @ref MidiFxBuffer.
///    process() performs ZERO heap allocation, takes NO lock and does NO I/O. If
///    a transform would exceed the output capacity, the excess events are DROPPED
///    and an atomic overflow-telemetry counter is bumped; the buffer is NEVER
///    grown.
///
/// Determinism
/// -----------
/// Humanize and the arpeggiator gate derive every "random" decision from an
/// explicit uint32_t seed via an inline SplitMix64 PRNG (see midi_fx.cpp). The
/// same seed and the same input always produce the same output; no Date / now /
/// std::rand is ever consulted.
///
/// Layering: depends ONLY on midi/ump, midi/midi_event and the standard library.
/// It does NOT depend on arrangement/ or engine/.

#include <array>
#include <cstddef>
#include <cstdint>

#include "midi/midi_event.h"
#include "midi/ump.h"
#include "rt/overflow_counter.h"

namespace sonare::midi {

/// Shared fixed PPQ-to-frame scale used when MIDI FX are applied outside a real
/// tempo map (C ABI project bake / realtime JSON config / WASM facade). High
/// enough to preserve sub-tick PPQ edits while keeping int64 headroom.
inline constexpr int64_t kMidiFxPpqScale = 960000;

/// Fixed-capacity output buffer for MIDI FX. Lives on the caller's stack (or a
/// pre-allocated member); process() only ever writes into `events[0..size)`.
///
/// Capacity rationale: the worst-case fan-out stage is the arpeggiator/chord
/// expansion. A single input note-on can expand to a chord of up to
/// kMaxChordNotes (8) notes, and the arpeggiator can additionally emit up to
/// kMaxArpSteps (16) gated note-on/off pairs per source note. A dense control
/// block carries on the order of a few dozen input events. 512 output slots give
/// comfortable headroom for that fan-out within one audio block while staying
/// small enough (each MidiEvent is <= 40 bytes, so < 20 KiB) to live inline on
/// the audio thread; overflow beyond it is surfaced via telemetry, never growth.
struct MidiFxBuffer {
  static constexpr size_t kCapacity = 512;
  std::array<MidiEvent, kCapacity> events{};
  size_t size = 0;

  void clear() noexcept { size = 0; }
  /// Appends `event` if there is room. Returns false (and does nothing) on
  /// overflow so the caller can bump telemetry. Never allocates.
  bool push(const MidiEvent& event) noexcept {
    if (size >= kCapacity) return false;
    events[size++] = event;
    return true;
  }
};

/// Per-stage configuration. All POD so the chain stays trivially copyable.

/// Semitone transpose applied to note-on / note-off / poly-pressure events.
struct TransposeConfig {
  bool enabled = false;
  /// Signed semitone shift. Resulting note numbers are clamped to [0, 127];
  /// events that would land outside that range after clamping are kept at the
  /// clamped boundary (no event is dropped by transpose alone).
  int semitones = 0;
};

/// Snap event render frames to a grid. The grid is expressed in render frames so
/// the control thread can bake a PPQ grid (via TempoMap) into a frame interval
/// before handing the chain to the audio thread.
struct QuantizeConfig {
  static constexpr size_t kMaxGrooveSteps = 16;
  bool enabled = false;
  /// Grid spacing in render frames (must be > 0 to take effect).
  int64_t grid_frames = 0;
  /// Quantize strength in [0, 1]; 1 == full snap to nearest grid line, 0 == no
  /// movement. Intermediate values move the event a fraction of the way.
  float strength = 1.0f;
  /// Swing amount in [0, 1]. 0 uses a straight grid. Values > 0 delay every odd
  /// grid line by up to half a grid step, so full swing maps line 1 to 1.5x
  /// grid_frames while even lines remain fixed.
  float swing = 0.0f;
  /// Optional repeating groove-template offsets, expressed as fractions of
  /// `grid_frames` per grid line. For example, {0, 0.1, -0.05, 0.0} delays line
  /// 1 by 10% of the grid and pulls line 2 5% early. 0 disables the template.
  size_t groove_steps = 0;
  std::array<float, kMaxGrooveSteps> groove_offsets{};
};

/// Velocity remap for note-on events. The 7-bit input velocity is scaled and
/// offset, then optionally gamma-shaped, and clamped to [1, 127] (a note-on
/// keeps a velocity of at least 1 so it never silently becomes a note-off).
struct VelocityCurveConfig {
  bool enabled = false;
  /// Linear gain applied to velocity.
  float scale = 1.0f;
  /// Additive offset (post-scale), in velocity units.
  float offset = 0.0f;
  /// Gamma exponent applied to the normalised velocity (1.0 == linear). Values
  /// > 1 push soft notes softer; values < 1 lift them.
  float gamma = 1.0f;
};

/// Generate a chord from each single input note-on (and matching note-off). Up
/// to kMaxChordNotes intervals (in semitones, relative to the source note) are
/// emitted; interval 0 is the original note. Note-offs mirror the note-ons.
struct ChordConfig {
  static constexpr size_t kMaxChordNotes = 8;
  bool enabled = false;
  size_t count = 0;
  std::array<int, kMaxChordNotes> intervals{};
};

/// Arpeggiate each input note: instead of one sustained note, emit a sequence of
/// short gated note-on/off pairs walking the configured intervals. Timing is in
/// render frames. Deterministic; the optional humanize-style gate jitter is
/// folded into Humanize, not here.
struct ArpeggiatorConfig {
  static constexpr size_t kMaxArpSteps = 16;
  bool enabled = false;
  /// Number of steps to emit per input note-on.
  size_t steps = 0;
  /// Semitone interval for each step (relative to the source note).
  std::array<int, kMaxArpSteps> intervals{};
  /// Frames between successive step onsets.
  int64_t step_frames = 0;
  /// Gate length in frames (note-off fires step_frames later, capped to the step
  /// duration). Must be > 0 and <= step_frames to take effect.
  int64_t gate_frames = 0;
};

/// Deterministic timing / velocity jitter. Randomness comes ONLY from `seed`.
struct HumanizeConfig {
  bool enabled = false;
  /// Explicit PRNG seed. Same seed + same input -> identical output.
  uint32_t seed = 0;
  /// Maximum absolute timing jitter in render frames (uniform in
  /// [-timing_frames, +timing_frames]).
  int64_t timing_frames = 0;
  /// Maximum absolute velocity jitter in velocity units (uniform in
  /// [-velocity_amount, +velocity_amount]); applied to note-ons only.
  int velocity_amount = 0;
};

/// An RT-safe MIDI FX chain. Stages are applied in a fixed, documented order:
///   1. transpose, 2. velocity curve, 3. chord, 4. arpeggiator, 5. quantize,
///   6. humanize.
/// (Pitch shaping first, then fan-out, then time shaping last so quantize/
/// humanize act on the final event set.)
class MidiFxChain {
 public:
  void prepare() noexcept { overflow_count_.reset(); }
  void reset() noexcept { overflow_count_.reset(); }

  void set_transpose(const TransposeConfig& c) noexcept { transpose_ = c; }
  void set_quantize(const QuantizeConfig& c) noexcept { quantize_ = c; }
  void set_velocity_curve(const VelocityCurveConfig& c) noexcept { velocity_ = c; }
  void set_chord(const ChordConfig& c) noexcept { chord_ = c; }
  void set_arpeggiator(const ArpeggiatorConfig& c) noexcept { arpeggiator_ = c; }
  void set_humanize(const HumanizeConfig& c) noexcept { humanize_ = c; }

  const TransposeConfig& transpose() const noexcept { return transpose_; }
  const QuantizeConfig& quantize() const noexcept { return quantize_; }
  const VelocityCurveConfig& velocity_curve() const noexcept { return velocity_; }
  const ChordConfig& chord() const noexcept { return chord_; }
  const ArpeggiatorConfig& arpeggiator() const noexcept { return arpeggiator_; }
  const HumanizeConfig& humanize() const noexcept { return humanize_; }

  /// AUDIO thread: transform `in[0..count)` into `out`. `out` is cleared first.
  /// RT-safe: no allocation, no lock, no I/O. Overflowing events are dropped and
  /// the overflow counter is bumped.
  void process(const MidiEvent* in, size_t count, MidiFxBuffer* out) noexcept;

  /// Telemetry: number of output events dropped because the buffer was full.
  uint32_t overflow_count() const noexcept { return overflow_count_.load(); }

 private:
  // Stage helpers. Each reads `out` in place? No: stages that fan out write into
  // `out`; pure transforms mutate the single event before it is pushed.
  void push_or_overflow(const MidiEvent& ev, MidiFxBuffer* out) noexcept;

  TransposeConfig transpose_{};
  QuantizeConfig quantize_{};
  VelocityCurveConfig velocity_{};
  ChordConfig chord_{};
  ArpeggiatorConfig arpeggiator_{};
  HumanizeConfig humanize_{};

  rt::OverflowCounter overflow_count_{};
};

// ---------------------------------------------------------------------------
// Small MIDI-1.0 channel-voice helpers shared with the implementation/tests.
// ---------------------------------------------------------------------------

/// 7-bit velocity for a MIDI 1.0 note-on / note-off (word[0] bits 0..6). For
/// non-MIDI-1.0 messages the result is unspecified; callers gate on the message
/// type / status first.
inline uint8_t midi1_velocity7(const Ump& ump) noexcept {
  return static_cast<uint8_t>(ump.words[0] & 0x7Fu);
}

}  // namespace sonare::midi
