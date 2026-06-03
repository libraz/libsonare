#include "midi/midi_fx.h"

#include <cmath>
#include <cstdint>

namespace sonare::midi {
namespace {

// ---------------------------------------------------------------------------
// Deterministic PRNG: SplitMix64.
//
// We use SplitMix64 (a public-domain mixing function by Sebastiano Vigna) as the
// per-event deterministic generator. It is tiny, alloc-free, branch-free and
// produces well-distributed 64-bit outputs from a 64-bit state, which makes it
// ideal for RT-safe humanize jitter: the same seed always yields the same
// sequence, and no std::rand / clock is involved.
class SplitMix64 {
 public:
  explicit SplitMix64(uint64_t seed) noexcept : state_(seed) {}

  uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ull;
    uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  // Uniform integer in [-amount, +amount]. amount must be >= 0.
  int64_t symmetric(int64_t amount) noexcept {
    if (amount <= 0) return 0;
    const uint64_t span = static_cast<uint64_t>(amount) * 2ull + 1ull;
    const int64_t v = static_cast<int64_t>(next() % span);
    return v - amount;
  }

 private:
  uint64_t state_;
};

// Mix the chain seed with an event ordinal so each event in the block gets an
// independent (but deterministic) jitter draw.
uint64_t seed_for(uint32_t base_seed, size_t ordinal) noexcept {
  return (static_cast<uint64_t>(base_seed) << 32) ^ (0xD1B54A32D192ED03ull * (ordinal + 1));
}

int clamp_note(int note) noexcept {
  if (note < 0) return 0;
  if (note > 127) return 127;
  return note;
}

int clamp_velocity_on(int velocity) noexcept {
  // A note-on keeps at least velocity 1 so it never degrades into a note-off.
  if (velocity < 1) return 1;
  if (velocity > 127) return 127;
  return velocity;
}

bool is_midi1_channel_voice(const Ump& ump) noexcept {
  return ump.message_type() == UmpMessageType::kMidi1ChannelVoice;
}

// Builds a MIDI-1.0 note-on/off Ump preserving the source group/channel.
Ump make_note(const Ump& src, bool note_on, uint8_t note, uint8_t velocity7) noexcept {
  if (note_on) {
    return make_midi1_note_on(src.group, src.channel(), note, velocity7);
  }
  return make_midi1_note_off(src.group, src.channel(), note, velocity7);
}

}  // namespace

void MidiFxChain::push_or_overflow(const MidiEvent& ev, MidiFxBuffer* out) noexcept {
  if (!out->push(ev)) {
    overflow_count_.fetch_add(1, std::memory_order_relaxed);
  }
}

void MidiFxChain::process(const MidiEvent* in, size_t count, MidiFxBuffer* out) noexcept {
  out->clear();
  if (in == nullptr || out == nullptr) return;

  for (size_t i = 0; i < count; ++i) {
    MidiEvent ev = in[i];
    const bool channel_voice = is_midi1_channel_voice(ev.ump);
    const bool note_on = channel_voice && ev.ump.is_note_on();
    const bool note_off = channel_voice && ev.ump.is_note_off();

    // ---- 1. Transpose (note-on/off note number) ----
    if (transpose_.enabled && (note_on || note_off)) {
      const int shifted = clamp_note(static_cast<int>(ev.ump.note_number()) + transpose_.semitones);
      ev.ump = make_note(ev.ump, note_on, static_cast<uint8_t>(shifted), midi1_velocity7(ev.ump));
    }

    // ---- 2. Velocity curve (note-on only) ----
    if (velocity_.enabled && note_on) {
      const float v = static_cast<float>(midi1_velocity7(ev.ump));
      float shaped = v;
      if (velocity_.gamma != 1.0f && velocity_.gamma > 0.0f) {
        const float normalized = v / 127.0f;
        shaped = std::pow(normalized, velocity_.gamma) * 127.0f;
      }
      shaped = shaped * velocity_.scale + velocity_.offset;
      const int mapped = clamp_velocity_on(static_cast<int>(std::lround(shaped)));
      ev.ump = make_note(ev.ump, true, ev.ump.note_number(), static_cast<uint8_t>(mapped));
    }

    // ---- 3. Chord expansion (note-on/off) ----
    // ---- 4. Arpeggiator expansion (note-on only) ----
    // These fan-out stages produce the events that go into `out`. We compute the
    // expanded note set, then (optionally) arpeggiate, then apply time shaping
    // (quantize/humanize) per emitted event.

    // Gather the base notes to emit (chord or just the single note).
    std::array<uint8_t, ChordConfig::kMaxChordNotes> notes{};
    size_t note_count = 0;
    const uint8_t base_note = ev.ump.note_number();
    if (chord_.enabled && (note_on || note_off) && chord_.count > 0) {
      for (size_t c = 0; c < chord_.count && c < ChordConfig::kMaxChordNotes; ++c) {
        notes[note_count++] =
            static_cast<uint8_t>(clamp_note(static_cast<int>(base_note) + chord_.intervals[c]));
      }
    } else {
      notes[note_count++] = base_note;
    }

    auto shape_and_push = [&](MidiEvent shaped, size_t ordinal) noexcept {
      // ---- 5. Quantize (render frame) ----
      if (quantize_.enabled && quantize_.grid_frames > 0) {
        const int64_t grid = quantize_.grid_frames;
        const int64_t f = shaped.render_frame;
        // Nearest grid line (round-half-up for positive offsets).
        const int64_t snapped = ((f + (f >= 0 ? grid / 2 : -(grid / 2))) / grid) * grid;
        const float strength = quantize_.strength < 0.0f
                                   ? 0.0f
                                   : (quantize_.strength > 1.0f ? 1.0f : quantize_.strength);
        const int64_t delta = snapped - f;
        shaped.render_frame = f + static_cast<int64_t>(std::llround(static_cast<double>(delta) *
                                                                    static_cast<double>(strength)));
      }
      // ---- 6. Humanize (timing + velocity), deterministic from seed ----
      if (humanize_.enabled) {
        SplitMix64 rng(seed_for(humanize_.seed, ordinal));
        if (humanize_.timing_frames > 0) {
          shaped.render_frame += rng.symmetric(humanize_.timing_frames);
          if (shaped.render_frame < 0) shaped.render_frame = 0;
        }
        if (humanize_.velocity_amount > 0 && is_midi1_channel_voice(shaped.ump) &&
            shaped.ump.is_note_on()) {
          const int jitter = static_cast<int>(rng.symmetric(humanize_.velocity_amount));
          const int v = clamp_velocity_on(static_cast<int>(midi1_velocity7(shaped.ump)) + jitter);
          shaped.ump =
              make_note(shaped.ump, true, shaped.ump.note_number(), static_cast<uint8_t>(v));
        }
      }
      push_or_overflow(shaped, out);
    };

    if (note_on && arpeggiator_.enabled && arpeggiator_.steps > 0 && arpeggiator_.step_frames > 0) {
      // Arpeggiate: walk the steps, emitting a gated note-on/off pair per step,
      // for each chord note. The source note-off is consumed (replaced by the
      // arpeggiated gates).
      const int64_t gate =
          arpeggiator_.gate_frames > 0 && arpeggiator_.gate_frames <= arpeggiator_.step_frames
              ? arpeggiator_.gate_frames
              : arpeggiator_.step_frames;
      size_t ordinal = i;
      for (size_t n = 0; n < note_count; ++n) {
        for (size_t s = 0; s < arpeggiator_.steps && s < ArpeggiatorConfig::kMaxArpSteps; ++s) {
          const int arp_note = clamp_note(static_cast<int>(notes[n]) + arpeggiator_.intervals[s]);
          const int64_t onset =
              ev.render_frame + static_cast<int64_t>(s) * arpeggiator_.step_frames;

          MidiEvent on_ev;
          on_ev.render_frame = onset;
          on_ev.ump =
              make_note(ev.ump, true, static_cast<uint8_t>(arp_note), midi1_velocity7(ev.ump));
          shape_and_push(on_ev, ordinal++);

          MidiEvent off_ev;
          off_ev.render_frame = onset + gate;
          off_ev.ump = make_note(ev.ump, false, static_cast<uint8_t>(arp_note), 0);
          shape_and_push(off_ev, ordinal++);
        }
      }
    } else if (note_off && arpeggiator_.enabled && arpeggiator_.steps > 0 &&
               arpeggiator_.step_frames > 0) {
      // The matching source note-off was replaced by the arpeggiated gates;
      // drop it.
    } else if (note_on || note_off) {
      // Non-arpeggiated note path: emit each chord/single note as one event.
      for (size_t n = 0; n < note_count; ++n) {
        MidiEvent note_ev = ev;
        note_ev.ump = make_note(ev.ump, note_on, notes[n], midi1_velocity7(ev.ump));
        shape_and_push(note_ev, i);
      }
    } else {
      // Non-note channel-voice / non-channel-voice events pass through with only
      // time shaping applied.
      shape_and_push(ev, i);
    }
  }
}

}  // namespace sonare::midi
