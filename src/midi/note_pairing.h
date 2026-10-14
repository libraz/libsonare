#pragma once

/// @file note_pairing.h
/// @brief Internal, group-aware LIFO pairing for timed MIDI note events.
///
/// This header is intentionally private to the MIDI implementation. It is not
/// installed and keeps the pairing policy shared by consumers that read MIDI
/// event streams without changing the public note model.

#include <cstdint>
#include <functional>
#include <iterator>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "midi/ump.h"

namespace sonare::midi::detail {

#if defined(__GNUC__) || defined(__clang__)
#define SONARE_MIDI_NOTE_PAIRING_HIDDEN __attribute__((visibility("hidden")))
#else
#define SONARE_MIDI_NOTE_PAIRING_HIDDEN
#endif

/// A decoded event supplied to the pairing helper by its caller.
struct SONARE_MIDI_NOTE_PAIRING_HIDDEN DecodedNoteEvent {
  double ppq = 0.0;
  Ump ump{};
};

/// A completed note pair. The original note-on packet is retained byte-for-byte
/// so callers can apply their own protocol-specific velocity policy.
struct SONARE_MIDI_NOTE_PAIRING_HIDDEN NotePair {
  double start_ppq = 0.0;
  double end_ppq = 0.0;
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t note = 0;
  Ump note_on{};
};

namespace note_pairing_detail {

struct SONARE_MIDI_NOTE_PAIRING_HIDDEN PendingNote {
  double start_ppq = 0.0;
  uint8_t group = 0;
  uint8_t channel = 0;
  uint8_t note = 0;
  Ump note_on{};
};

static_assert(std::is_nothrow_move_constructible_v<PendingNote>);

/// The common case has one pending note for a key. Keep that first note in the
/// map value itself; only a retrigger needs the overflow vector. The first note
/// remains live while overflow entries are pushed and popped, so the vector can
/// empty and be reused without moving the inline entry.
struct SONARE_MIDI_NOTE_PAIRING_HIDDEN PendingStack {
  SONARE_MIDI_NOTE_PAIRING_HIDDEN PendingStack() noexcept = default;

  SONARE_MIDI_NOTE_PAIRING_HIDDEN explicit PendingStack(PendingNote&& first) noexcept
      : first_(std::move(first)), has_first_(true) {}

  SONARE_MIDI_NOTE_PAIRING_HIDDEN PendingStack(PendingStack&&) noexcept = default;
  SONARE_MIDI_NOTE_PAIRING_HIDDEN PendingStack& operator=(PendingStack&&) noexcept = default;
  SONARE_MIDI_NOTE_PAIRING_HIDDEN PendingStack(const PendingStack&) = default;
  SONARE_MIDI_NOTE_PAIRING_HIDDEN PendingStack& operator=(const PendingStack&) = default;

  SONARE_MIDI_NOTE_PAIRING_HIDDEN bool empty() const noexcept { return !has_first_; }

  SONARE_MIDI_NOTE_PAIRING_HIDDEN void push_overflow(PendingNote&& note) {
    overflow_.push_back(std::move(note));
  }

  SONARE_MIDI_NOTE_PAIRING_HIDDEN PendingNote pop() noexcept {
    if (!overflow_.empty()) {
      PendingNote note = std::move(overflow_.back());
      overflow_.pop_back();
      return note;
    }
    has_first_ = false;
    return std::move(first_);
  }

 private:
  PendingNote first_{};
  std::vector<PendingNote> overflow_;
  bool has_first_ = false;
};

static_assert(std::is_nothrow_move_constructible_v<PendingStack>);

/// Pack the complete legal UMP note identity into its 15-bit domain.
///
/// The group is read from word 0 rather than from Ump::group: word 0 is the
/// packet's wire authority, while the side-band field can be stale in decoded
/// or copied event records.
SONARE_MIDI_NOTE_PAIRING_HIDDEN inline uint8_t group_from_word0(uint32_t word0) noexcept {
  const uint8_t type = static_cast<uint8_t>((word0 >> 28) & 0x0Fu);
  // Utility and UMP Stream packets use these bits for endpoint-wide structural
  // fields rather than a group. Keep this local so a debug build of this
  // private header does not export the public inline UMP group helpers.
  if (type == 0x0u || type == 0xFu) return 0;
  return static_cast<uint8_t>((word0 >> 24) & 0x0Fu);
}

SONARE_MIDI_NOTE_PAIRING_HIDDEN inline uint16_t key_for(const Ump& ump) noexcept {
  const uint16_t group = group_from_word0(ump.words[0]);
  const uint16_t channel = ump.channel();
  const uint16_t note = ump.note_number();
  return static_cast<uint16_t>((group << 11) | (channel << 7) | note);
}

SONARE_MIDI_NOTE_PAIRING_HIDDEN inline uint8_t group_for(const Ump& ump) noexcept {
  return group_from_word0(ump.words[0]);
}

struct SONARE_MIDI_NOTE_PAIRING_HIDDEN AlwaysContinue {
  SONARE_MIDI_NOTE_PAIRING_HIDDEN bool operator()() const noexcept { return true; }
};

}  // namespace note_pairing_detail

/// Pair an iterator range of decoded MIDI events.
///
/// @p decode receives each range element and returns a DecodedNoteEvent (or a
/// compatible value exposing `ppq` and `ump`). @p on_pair is called immediately
/// in valid note-off stream order. @p should_continue is checked before each
/// event; a false result stops the scan and returns false. All pending state is
/// local to this call, so an interrupted scan cannot leak open notes into a
/// later call. Hash-table iteration is never used to emit output.
template <typename InputIt, typename Decode, typename OnPair, typename Continue>
SONARE_MIDI_NOTE_PAIRING_HIDDEN bool pair_note_events(InputIt first, InputIt last, Decode decode,
                                                      OnPair on_pair, Continue should_continue) {
  using Pending = note_pairing_detail::PendingNote;
  using PendingStack = note_pairing_detail::PendingStack;
  std::unordered_map<uint16_t, PendingStack> pending;

  for (; first != last; ++first) {
    if (!static_cast<bool>(std::invoke(should_continue))) return false;
    const auto decoded = std::invoke(decode, *first);
    const Ump& ump = decoded.ump;
    const uint16_t key = note_pairing_detail::key_for(ump);

    // Keep this order: MIDI 1 Note On velocity 0 is a note-off, while MIDI 2
    // Note On velocity 0 remains a real note-on.
    if (ump.is_note_off()) {
      const auto it = pending.find(key);
      if (it == pending.end()) continue;

      Pending note = it->second.pop();
      if (it->second.empty()) pending.erase(it);

      // The stack entry has been consumed even when malformed timing drops
      // the pair. This prevents a later off from closing the same note again.
      // Use the positive predicate rather than only `end <= start`: NaN must
      // not pass through as a completed pair.
      if (!(decoded.ppq > note.start_ppq)) continue;
      std::invoke(on_pair, NotePair{note.start_ppq, decoded.ppq, note.group, note.channel,
                                    note.note, std::move(note.note_on)});
      continue;
    }
    if (!ump.is_note_on()) continue;

    Pending note{decoded.ppq, note_pairing_detail::group_for(ump), ump.channel(), ump.note_number(),
                 ump};
    auto [it, inserted] = pending.try_emplace(key, std::move(note));
    if (!inserted) it->second.push_overflow(std::move(note));
  }
  return true;
}

/// Iterator-range overload with no interruption hook.
template <typename InputIt, typename Decode, typename OnPair>
SONARE_MIDI_NOTE_PAIRING_HIDDEN bool pair_note_events(InputIt first, InputIt last, Decode decode,
                                                      OnPair on_pair) {
  return pair_note_events(first, last, std::move(decode), std::move(on_pair),
                          note_pairing_detail::AlwaysContinue{});
}

/// Range overload used by the MIDI consumers.
template <typename Range, typename Decode, typename OnPair, typename Continue>
SONARE_MIDI_NOTE_PAIRING_HIDDEN bool pair_notes(const Range& range, Decode decode, OnPair on_pair,
                                                Continue should_continue) {
  using std::begin;
  using std::end;
  return pair_note_events(begin(range), end(range), std::move(decode), std::move(on_pair),
                          std::move(should_continue));
}

/// Range overload with no interruption hook.
template <typename Range, typename Decode, typename OnPair>
SONARE_MIDI_NOTE_PAIRING_HIDDEN bool pair_notes(const Range& range, Decode decode, OnPair on_pair) {
  using std::begin;
  using std::end;
  return pair_note_events(begin(range), end(range), std::move(decode), std::move(on_pair));
}

}  // namespace sonare::midi::detail

#undef SONARE_MIDI_NOTE_PAIRING_HIDDEN
