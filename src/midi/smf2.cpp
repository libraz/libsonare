#include "midi/smf2.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "util/constants.h"

namespace sonare::midi {
namespace {

// 8-byte ASCII file header.
constexpr char kFileHeader[8] = {'S', 'M', 'F', '2', 'C', 'L', 'I', 'P'};

// UMP message type nibbles (word[0] bits 28..31).
constexpr uint32_t kMtUtility = 0x0u;
constexpr uint32_t kMtMidi1 = 0x2u;
constexpr uint32_t kMtData64 = 0x3u;   // SysEx7.
constexpr uint32_t kMtData128 = 0x5u;  // SysEx8 / Mixed Data Set.
constexpr uint32_t kMtMidi2 = 0x4u;
constexpr uint32_t kMtFlexData = 0xDu;
constexpr uint32_t kMtStream = 0xFu;

// Utility message status nibbles (word[0] bits 20..23).
constexpr uint8_t kUtilityDctpq = 0x3u;            // Delta Clockstamp Ticks Per Quarter.
constexpr uint8_t kUtilityDeltaClockstamp = 0x4u;  // Delta Clockstamp (DCS).

// UMP Stream status (word[0] bits 16..25; only the low byte differs here).
constexpr uint8_t kStreamStartOfClip = 0x20u;
constexpr uint8_t kStreamEndOfClip = 0x21u;

// Flex Data status banks (word[0] byte 2) and status codes (byte 3).
constexpr uint8_t kFlexBankSetupPerformance = 0x00u;
constexpr uint8_t kFlexBankMetadataText = 0x01u;
constexpr uint8_t kFlexStatusSetTempo = 0x00u;
constexpr uint8_t kFlexStatusSetTimeSignature = 0x01u;
constexpr uint8_t kFlexStatusSongName = 0x02u;  // Track / song name (metadata text).

// SysEx7 (MT=0x3) packet status (high nibble of word[0] byte 1).
constexpr uint8_t kSysex7Complete = 0x0u;
constexpr uint8_t kSysex7Start = 0x1u;
constexpr uint8_t kSysex7Continue = 0x2u;
constexpr uint8_t kSysex7End = 0x3u;

// Tempo is encoded as 10-nanosecond units per quarter note: BPM = 60s / QN, and
// QN(ns) = tempo * 10, so BPM = 60e9 / (tempo*10) = 6e9 / tempo. This is a Flex
// Data domain constant, not a universal numeric constant.
constexpr double kTenNanosPerQuarterToBpm = 6.0e9;
constexpr double kDefaultBpm = sonare::constants::kDefaultBpm;

// Number of 32-bit words a UMP message occupies, keyed by message type nibble.
int words_for_message_type(uint32_t mt) noexcept {
  switch (mt) {
    case 0x0u:
    case 0x1u:
    case 0x2u:
    case 0x6u:
    case 0x7u:
      return 1;
    case 0x3u:
    case 0x4u:
    case 0x8u:
    case 0x9u:
    case 0xAu:
      return 2;
    case 0xBu:
    case 0xCu:
      return 3;
    default:  // 0x5, 0xD, 0xE, 0xF.
      return 4;
  }
}

// ---------------------------------------------------------------------------
// Big-endian 32-bit word reader with bounds checking. Never reads out of bounds.
// ---------------------------------------------------------------------------
class WordReader {
 public:
  WordReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  size_t remaining_words() const noexcept { return (size_ - pos_) / 4u; }
  bool overflow() const noexcept { return overflow_; }

  // Reads one big-endian 32-bit word, or sets overflow and returns 0.
  uint32_t word() noexcept {
    if (size_ - pos_ < 4u) {
      overflow_ = true;
      return 0;
    }
    const uint32_t b0 = data_[pos_];
    const uint32_t b1 = data_[pos_ + 1];
    const uint32_t b2 = data_[pos_ + 2];
    const uint32_t b3 = data_[pos_ + 3];
    pos_ += 4u;
    return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
  }

  // Advances `count` already-consumed bytes for the header check.
  void advance(size_t count) noexcept { pos_ = std::min(size_, pos_ + count); }
  size_t pos() const noexcept { return pos_; }

 private:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t pos_ = 0;
  bool overflow_ = false;
};

double tick_to_ppq(uint64_t tick, uint16_t dctpq) noexcept {
  if (dctpq == 0) return 0.0;
  return static_cast<double>(tick) / static_cast<double>(dctpq);
}

// Appends the up-to-12 text bytes carried in words[1..3] of a Flex Data packet
// to `out`, stopping at the first NUL (text is NUL-padded within a packet).
void append_flex_text(const std::array<uint32_t, 4>& words, std::string* out) {
  for (int w = 1; w < 4; ++w) {
    for (int b = 3; b >= 0; --b) {
      const uint8_t ch = static_cast<uint8_t>((words[static_cast<size_t>(w)] >> (b * 8)) & 0xFFu);
      if (ch == 0u) return;
      out->push_back(static_cast<char>(ch));
    }
  }
}

// Extracts the SysEx7 data bytes (0..6) carried by a 2-word MT=0x3 packet.
void append_sysex7_bytes(uint32_t word0, uint32_t word1, std::vector<uint8_t>* out) {
  const uint8_t num_bytes = static_cast<uint8_t>((word0 >> 16) & 0x0Fu);
  std::array<uint8_t, 6> bytes = {
      static_cast<uint8_t>((word0 >> 8) & 0xFFu),  static_cast<uint8_t>(word0 & 0xFFu),
      static_cast<uint8_t>((word1 >> 24) & 0xFFu), static_cast<uint8_t>((word1 >> 16) & 0xFFu),
      static_cast<uint8_t>((word1 >> 8) & 0xFFu),  static_cast<uint8_t>(word1 & 0xFFu)};
  for (uint8_t i = 0; i < num_bytes && i < 6; ++i) out->push_back(bytes[i]);
}

// Extracts the SysEx8 data bytes carried by a 4-word MT=0x5 packet. SysEx8 holds
// full 8-bit data (no 7-bit restriction). word0 byte1 low nibble counts the
// valid bytes INCLUDING the Stream ID (word0 byte2); the actual payload follows
// the Stream ID, starting at word0 byte3 and continuing through words 1..3.
void append_sysex8_bytes(const std::array<uint32_t, 4>& words, std::vector<uint8_t>* out) {
  const uint8_t num_bytes = static_cast<uint8_t>((words[0] >> 16) & 0x0Fu);
  if (num_bytes <= 1u) return;  // Only a Stream ID (or nothing) — no payload.
  const uint8_t data_bytes = static_cast<uint8_t>(num_bytes - 1u);
  std::array<uint8_t, 13> bytes = {static_cast<uint8_t>(words[0] & 0xFFu),
                                   static_cast<uint8_t>((words[1] >> 24) & 0xFFu),
                                   static_cast<uint8_t>((words[1] >> 16) & 0xFFu),
                                   static_cast<uint8_t>((words[1] >> 8) & 0xFFu),
                                   static_cast<uint8_t>(words[1] & 0xFFu),
                                   static_cast<uint8_t>((words[2] >> 24) & 0xFFu),
                                   static_cast<uint8_t>((words[2] >> 16) & 0xFFu),
                                   static_cast<uint8_t>((words[2] >> 8) & 0xFFu),
                                   static_cast<uint8_t>(words[2] & 0xFFu),
                                   static_cast<uint8_t>((words[3] >> 24) & 0xFFu),
                                   static_cast<uint8_t>((words[3] >> 16) & 0xFFu),
                                   static_cast<uint8_t>((words[3] >> 8) & 0xFFu),
                                   static_cast<uint8_t>(words[3] & 0xFFu)};
  for (uint8_t i = 0; i < data_bytes && i < bytes.size(); ++i) out->push_back(bytes[i]);
}

// ---------------------------------------------------------------------------
// Export writer helpers.
// ---------------------------------------------------------------------------
void put_word(std::vector<uint8_t>* out, uint32_t w) {
  out->push_back(static_cast<uint8_t>((w >> 24) & 0xFFu));
  out->push_back(static_cast<uint8_t>((w >> 16) & 0xFFu));
  out->push_back(static_cast<uint8_t>((w >> 8) & 0xFFu));
  out->push_back(static_cast<uint8_t>(w & 0xFFu));
}

uint32_t dctpq_word(uint16_t dctpq) noexcept {
  return (kMtUtility << 28) | (static_cast<uint32_t>(kUtilityDctpq) << 20) |
         static_cast<uint32_t>(dctpq);
}

uint32_t delta_clockstamp_word(uint32_t ticks) noexcept {
  return (kMtUtility << 28) | (static_cast<uint32_t>(kUtilityDeltaClockstamp) << 20) |
         (ticks & 0xFFFFFu);
}

// UMP Stream messages are 128-bit (four 32-bit words). Only word[0] carries the
// status here; the remaining three words are zero.
void put_stream(std::vector<uint8_t>* out, uint8_t status) {
  put_word(out, (kMtStream << 28) | (static_cast<uint32_t>(status) << 16));
  put_word(out, 0);
  put_word(out, 0);
  put_word(out, 0);
}

// Flex Data word[0] for a group-addressed complete packet.
uint32_t flex_word0(uint8_t bank, uint8_t status) noexcept {
  // byte1 = (format<<6)|(addr<<4)|channel, format=complete(0), addr=group(1).
  constexpr uint8_t kByte1GroupComplete = 0x10u;
  return (kMtFlexData << 28) | (static_cast<uint32_t>(kByte1GroupComplete) << 16) |
         (static_cast<uint32_t>(bank) << 8) | static_cast<uint32_t>(status);
}

// A single Delta Clockstamp carries a 20-bit delta. To express a delta larger
// than 0xFFFFF, the MIDI Clip File spec chains multiple DCS messages, which the
// importer accumulates additively (running_tick += word & 0xFFFFF). Emit one
// max-valued (0xFFFFF) DCS per full 20-bit span, then the remainder, so deltas
// of any size survive the round trip.
void put_dcs(std::vector<uint8_t>* out, uint64_t* last_tick, uint64_t tick) {
  uint64_t delta = tick >= *last_tick ? tick - *last_tick : 0u;
  while (delta > 0xFFFFFu) {
    put_word(out, delta_clockstamp_word(0xFFFFFu));
    delta -= 0xFFFFFu;
  }
  put_word(out, delta_clockstamp_word(static_cast<uint32_t>(delta)));
  *last_tick = tick;
}

uint64_t ppq_to_tick(double ppq, uint16_t dctpq) noexcept {
  const double t = ppq * static_cast<double>(dctpq);
  if (!std::isfinite(t) || t <= 0.0) return 0u;
  // The absolute tick is not bounded by the 20-bit DCS field; deltas between
  // events are chained across multiple DCS words by put_dcs. Clamp only to the
  // representable 64-bit range to avoid UB on pathological input.
  const double clamped = std::min(t, static_cast<double>(0xFFFFFFFFFFFFFull));
  return static_cast<uint64_t>(std::llround(clamped));
}

}  // namespace

// ===========================================================================
// Import
// ===========================================================================

Smf2ImportResult import_clip_file(const uint8_t* data, size_t size) {
  Smf2ImportResult result;

  if (data == nullptr || size == 0) {
    result.status = Smf2Status::kInvalidArgument;
    result.diagnostic = "null or empty MIDI Clip File buffer";
    return result;
  }
  if (size < sizeof(kFileHeader) || std::memcmp(data, kFileHeader, sizeof(kFileHeader)) != 0) {
    result.status = Smf2Status::kBadHeader;
    result.diagnostic = "missing SMF2CLIP file header";
    return result;
  }
  if ((size - sizeof(kFileHeader)) % 4u != 0u) {
    result.status = Smf2Status::kTruncated;
    result.diagnostic = "buffer ended mid-word";
    return result;
  }

  WordReader reader(data, size);
  reader.advance(sizeof(kFileHeader));

  uint16_t dctpq = 0;
  bool saw_dctpq = false;
  uint64_t running_tick = 0;
  bool saw_end_of_clip = false;

  MidiClip clip;
  std::string name;
  bool has_events = false;
  double last_event_ppq = 0.0;
  double end_clip_ppq = 0.0;

  std::vector<uint8_t> pending_sysex;
  double pending_sysex_ppq = 0.0;
  uint8_t pending_sysex_group = 0;
  bool pending_sysex_active = false;

  auto store_pending_sysex = [&](uint8_t group) {
    const SysExHandle handle = result.sysex_store.add(pending_sysex);
    pending_sysex.clear();
    pending_sysex_active = false;
    if (handle == 0) {
      ++result.skipped_events;
      return;
    }
    MidiClipEvent ev;
    ev.ppq = pending_sysex_ppq;
    ev.ump = make_sysex_handle(group, handle);
    clip.add_event(ev);
    has_events = true;
    last_event_ppq = std::max(last_event_ppq, pending_sysex_ppq);
  };

  while (reader.remaining_words() > 0 && !saw_end_of_clip) {
    const size_t word0_pos = reader.pos();
    const uint32_t word0 = reader.word();
    if (reader.overflow()) break;
    const uint32_t mt = (word0 >> 28) & 0x0Fu;
    const int nwords = words_for_message_type(mt);

    // Ensure the whole message is present before interpreting it.
    if (reader.remaining_words() + 1u < static_cast<size_t>(nwords)) {
      result.status = Smf2Status::kTruncated;
      result.diagnostic = "buffer ended mid-message";
      break;
    }
    std::array<uint32_t, 4> words = {word0, 0, 0, 0};
    for (int i = 1; i < nwords; ++i) words[static_cast<size_t>(i)] = reader.word();
    if (reader.overflow()) {
      result.status = Smf2Status::kTruncated;
      result.diagnostic = "buffer ended mid-message";
      break;
    }
    (void)word0_pos;

    const double ppq = tick_to_ppq(running_tick, dctpq);

    if (mt == kMtUtility) {
      const uint8_t status = static_cast<uint8_t>((word0 >> 20) & 0x0Fu);
      if (status == kUtilityDctpq) {
        dctpq = static_cast<uint16_t>(word0 & 0xFFFFu);
        saw_dctpq = (dctpq != 0);
      } else if (status == kUtilityDeltaClockstamp) {
        running_tick += static_cast<uint64_t>(word0 & 0xFFFFFu);
      } else {
        ++result.skipped_events;  // JR clock / timestamp / NOOP — not timed data.
      }
      continue;
    }

    if (mt == kMtStream) {
      const uint8_t status = static_cast<uint8_t>((word0 >> 16) & 0xFFu);
      if (status == kStreamEndOfClip) {
        saw_end_of_clip = true;
        end_clip_ppq = ppq;
      } else if (status != kStreamStartOfClip) {
        ++result.skipped_events;
      }
      continue;
    }

    if (mt == kMtFlexData) {
      const uint8_t bank = static_cast<uint8_t>((word0 >> 8) & 0xFFu);
      const uint8_t status = static_cast<uint8_t>(word0 & 0xFFu);
      if (bank == kFlexBankSetupPerformance && status == kFlexStatusSetTempo) {
        const uint32_t tempo10ns = words[1];
        transport::TempoSegment seg;
        seg.start_ppq = ppq;
        seg.bpm =
            tempo10ns > 0 ? kTenNanosPerQuarterToBpm / static_cast<double>(tempo10ns) : kDefaultBpm;
        result.tempo_segments.push_back(seg);
      } else if (bank == kFlexBankSetupPerformance && status == kFlexStatusSetTimeSignature) {
        const uint8_t numerator = static_cast<uint8_t>((words[1] >> 24) & 0xFFu);
        const uint8_t denominator = static_cast<uint8_t>((words[1] >> 16) & 0xFFu);
        const uint8_t n32 = static_cast<uint8_t>((words[1] >> 8) & 0xFFu);
        if (numerator > 0 && denominator > 0) {
          transport::TimeSignatureSegment seg;
          seg.start_ppq = ppq;
          seg.time_sig.numerator = static_cast<int>(numerator);
          seg.time_sig.denominator = static_cast<int>(denominator);
          if (n32 > 0) seg.thirty_seconds_per_quarter = n32;
          result.time_signatures.push_back(seg);
        } else {
          ++result.skipped_events;
        }
      } else if (bank == kFlexBankMetadataText && status == kFlexStatusSongName) {
        append_flex_text(words, &name);
      } else {
        // Key signature, copyright, lyrics, and other Flex Data forms have no
        // home in the normalized model.
        ++result.skipped_events;
      }
      continue;
    }

    if (mt == kMtMidi1 || mt == kMtMidi2) {
      if (!saw_dctpq) {
        result.status = Smf2Status::kMissingDctpq;
        result.diagnostic = "channel-voice event before DCTPQ";
        break;
      }
      Ump ump;
      ump.words[0] = words[0];
      ump.words[1] = words[1];
      ump.word_count = static_cast<uint8_t>(mt == kMtMidi1 ? 1 : 2);
      ump.group = static_cast<uint8_t>((word0 >> 24) & 0x0Fu);
      MidiClipEvent ev;
      ev.ppq = ppq;
      ev.ump = ump;
      clip.add_event(ev);
      has_events = true;
      last_event_ppq = ppq;
      continue;
    }

    if (mt == kMtData64) {
      const uint8_t packet_status = static_cast<uint8_t>((word0 >> 20) & 0x0Fu);
      const uint8_t group = static_cast<uint8_t>((word0 >> 24) & 0x0Fu);
      if (packet_status == kSysex7Complete) {
        pending_sysex.clear();
        pending_sysex_ppq = ppq;
        append_sysex7_bytes(words[0], words[1], &pending_sysex);
        store_pending_sysex(group);
        continue;
      }
      if (packet_status == kSysex7Start) {
        if (pending_sysex_active) ++result.skipped_events;
        pending_sysex.clear();
        pending_sysex_ppq = ppq;
        pending_sysex_group = group;
        pending_sysex_active = true;
        append_sysex7_bytes(words[0], words[1], &pending_sysex);
        continue;
      }
      if (packet_status == kSysex7Continue) {
        if (!pending_sysex_active) {
          ++result.skipped_events;
          continue;
        }
        append_sysex7_bytes(words[0], words[1], &pending_sysex);
        continue;
      }
      if (packet_status == kSysex7End) {
        if (!pending_sysex_active) {
          ++result.skipped_events;
          continue;
        }
        append_sysex7_bytes(words[0], words[1], &pending_sysex);
        store_pending_sysex(pending_sysex_group);
        continue;
      }
      ++result.skipped_events;
      continue;
    }

    if (mt == kMtData128) {
      // SysEx8 shares the SysEx7 status-nibble convention (complete/start/
      // continue/end) in word0 bits 20..23.
      const uint8_t packet_status = static_cast<uint8_t>((word0 >> 20) & 0x0Fu);
      const uint8_t group = static_cast<uint8_t>((word0 >> 24) & 0x0Fu);
      if (packet_status == kSysex7Complete) {
        pending_sysex.clear();
        pending_sysex_ppq = ppq;
        append_sysex8_bytes(words, &pending_sysex);
        store_pending_sysex(group);
        continue;
      }
      if (packet_status == kSysex7Start) {
        if (pending_sysex_active) ++result.skipped_events;
        pending_sysex.clear();
        pending_sysex_ppq = ppq;
        pending_sysex_group = group;
        pending_sysex_active = true;
        append_sysex8_bytes(words, &pending_sysex);
        continue;
      }
      if (packet_status == kSysex7Continue) {
        if (!pending_sysex_active) {
          ++result.skipped_events;
          continue;
        }
        append_sysex8_bytes(words, &pending_sysex);
        continue;
      }
      if (packet_status == kSysex7End) {
        if (!pending_sysex_active) {
          ++result.skipped_events;
          continue;
        }
        append_sysex8_bytes(words, &pending_sysex);
        store_pending_sysex(pending_sysex_group);
        continue;
      }
      ++result.skipped_events;
      continue;
    }

    // System and reserved message types are not represented.
    ++result.skipped_events;
  }

  if (pending_sysex_active) {
    ++result.skipped_events;
  }

  if (result.status != Smf2Status::kOk && result.status != Smf2Status::kMissingDctpq) {
    return result;
  }
  if (!saw_dctpq && has_events) {
    result.status = Smf2Status::kMissingDctpq;
    if (result.diagnostic.empty()) result.diagnostic = "no DCTPQ message";
    return result;
  }

  result.ticks_per_quarter = dctpq;
  if (result.tempo_segments.empty()) result.tempo_segments.push_back({0.0, kDefaultBpm, 0.0});
  if (result.time_signatures.empty()) result.time_signatures.push_back({0.0, {4, 4}});

  if (has_events) {
    clip.sort_stable();
    result.clips.push_back(std::move(clip));
    result.clip_names.push_back(name);
    const double length = saw_end_of_clip ? std::max(end_clip_ppq, last_event_ppq) : last_event_ppq;
    result.clip_lengths_ppq.push_back(length);
  }
  return result;
}

// ===========================================================================
// Export
// ===========================================================================

namespace {

// A single sequence-data item positioned at an integer tick.
struct SeqItem {
  uint64_t tick = 0;
  int order = 0;  // Tie-break: tempo(0) / time-sig(1) before notes(2) at same tick.
  std::array<uint32_t, 4> words{};
  int word_count = 0;
};

bool emit_sysex7(std::vector<SeqItem>* items, uint64_t tick, const std::vector<uint8_t>& payload,
                 uint8_t group) {
  // Strip a leading 0xF0 / trailing 0xF7 if present; UMP SysEx7 carries the
  // payload without the MIDI 1.0 framing bytes.
  size_t begin = 0;
  size_t end = payload.size();
  if (begin < end && payload[begin] == 0xF0u) ++begin;
  if (end > begin && payload[end - 1] == 0xF7u) --end;
  const size_t total = end - begin;
  if (total == 0) return false;
  size_t offset = 0;
  bool first = true;
  do {
    const size_t chunk = std::min<size_t>(6u, total - offset);
    uint8_t status;
    if (total <= 6u) {
      status = kSysex7Complete;
    } else if (first) {
      status = kSysex7Start;
    } else if (offset + chunk >= total) {
      status = kSysex7End;
    } else {
      status = kSysex7Continue;
    }
    std::array<uint8_t, 6> bytes{};
    for (size_t i = 0; i < chunk; ++i) bytes[i] = payload[begin + offset + i];
    SeqItem item;
    item.tick = tick;
    item.order = 2;
    item.words[0] = (kMtData64 << 28) | (static_cast<uint32_t>(group & 0x0Fu) << 24) |
                    (static_cast<uint32_t>(status) << 20) | (static_cast<uint32_t>(chunk) << 16) |
                    (static_cast<uint32_t>(bytes[0]) << 8) | static_cast<uint32_t>(bytes[1]);
    item.words[1] = (static_cast<uint32_t>(bytes[2]) << 24) |
                    (static_cast<uint32_t>(bytes[3]) << 16) |
                    (static_cast<uint32_t>(bytes[4]) << 8) | static_cast<uint32_t>(bytes[5]);
    item.word_count = 2;
    items->push_back(item);
    offset += chunk;
    first = false;
  } while (offset < total);
  return true;
}

bool needs_sysex8(const std::vector<uint8_t>& payload) {
  size_t begin = 0;
  size_t end = payload.size();
  if (begin < end && payload[begin] == 0xF0u) ++begin;
  if (end > begin && payload[end - 1] == 0xF7u) --end;
  for (size_t i = begin; i < end; ++i) {
    if (payload[i] > 0x7Fu) return true;
  }
  return false;
}

bool emit_sysex8(std::vector<SeqItem>* items, uint64_t tick, const std::vector<uint8_t>& payload,
                 uint8_t group) {
  // Match the SysEx7 exporter: tolerate MIDI 1.0 framing in the side store, but
  // UMP SysEx8 packets carry only the payload bytes after the Stream ID.
  size_t begin = 0;
  size_t end = payload.size();
  if (begin < end && payload[begin] == 0xF0u) ++begin;
  if (end > begin && payload[end - 1] == 0xF7u) --end;
  const size_t total = end - begin;
  if (total == 0) return false;
  size_t offset = 0;
  bool first = true;
  do {
    const size_t chunk = std::min<size_t>(13u, total - offset);
    uint8_t status;
    if (total <= 13u) {
      status = kSysex7Complete;
    } else if (first) {
      status = kSysex7Start;
    } else if (offset + chunk >= total) {
      status = kSysex7End;
    } else {
      status = kSysex7Continue;
    }
    std::array<uint8_t, 13> bytes{};
    for (size_t i = 0; i < chunk; ++i) bytes[i] = payload[begin + offset + i];
    SeqItem item;
    item.tick = tick;
    item.order = 2;
    const uint8_t num_bytes = static_cast<uint8_t>(chunk + 1u);  // Stream ID + payload.
    item.words[0] = (kMtData128 << 28) | (static_cast<uint32_t>(group & 0x0Fu) << 24) |
                    (static_cast<uint32_t>(status) << 20) |
                    (static_cast<uint32_t>(num_bytes) << 16) | (0u << 8) |
                    static_cast<uint32_t>(bytes[0]);
    item.words[1] = (static_cast<uint32_t>(bytes[1]) << 24) |
                    (static_cast<uint32_t>(bytes[2]) << 16) |
                    (static_cast<uint32_t>(bytes[3]) << 8) | static_cast<uint32_t>(bytes[4]);
    item.words[2] = (static_cast<uint32_t>(bytes[5]) << 24) |
                    (static_cast<uint32_t>(bytes[6]) << 16) |
                    (static_cast<uint32_t>(bytes[7]) << 8) | static_cast<uint32_t>(bytes[8]);
    item.words[3] = (static_cast<uint32_t>(bytes[9]) << 24) |
                    (static_cast<uint32_t>(bytes[10]) << 16) |
                    (static_cast<uint32_t>(bytes[11]) << 8) | static_cast<uint32_t>(bytes[12]);
    item.word_count = 4;
    items->push_back(item);
    offset += chunk;
    first = false;
  } while (offset < total);
  return true;
}

}  // namespace

Smf2ExportResult export_clip_file(
    const MidiClip& clip, const std::vector<transport::TempoSegment>& tempo_segments,
    const std::vector<transport::TimeSignatureSegment>& time_signatures,
    const Smf2ExportOptions& options) {
  Smf2ExportResult result;
  const uint16_t dctpq = options.ticks_per_quarter == 0 ? 480u : options.ticks_per_quarter;

  std::vector<uint8_t>& out = result.bytes;
  out.insert(out.end(), kFileHeader, kFileHeader + sizeof(kFileHeader));

  // Configuration header: DCS(0) + DCTPQ.
  put_word(&out, delta_clockstamp_word(0));
  put_word(&out, dctpq_word(dctpq));

  // First tempo / time-signature + optional name live in the config header,
  // each with a prepended DCS(0).
  if (!tempo_segments.empty()) {
    const double bpm = tempo_segments.front().bpm > 0.0 ? tempo_segments.front().bpm : kDefaultBpm;
    const uint32_t tempo10ns = static_cast<uint32_t>(std::llround(kTenNanosPerQuarterToBpm / bpm));
    put_word(&out, delta_clockstamp_word(0));
    put_word(&out, flex_word0(kFlexBankSetupPerformance, kFlexStatusSetTempo));
    put_word(&out, tempo10ns);
    put_word(&out, 0);
    put_word(&out, 0);
  }
  if (!time_signatures.empty()) {
    const auto& seg = time_signatures.front();
    const uint8_t num = static_cast<uint8_t>(std::clamp(seg.time_sig.numerator, 1, 255));
    const uint8_t den = static_cast<uint8_t>(std::clamp(seg.time_sig.denominator, 1, 255));
    const uint8_t n32 = static_cast<uint8_t>(std::clamp(
        seg.thirty_seconds_per_quarter > 0 ? seg.thirty_seconds_per_quarter : 8, 1, 255));
    put_word(&out, delta_clockstamp_word(0));
    put_word(&out, flex_word0(kFlexBankSetupPerformance, kFlexStatusSetTimeSignature));
    put_word(&out, (static_cast<uint32_t>(num) << 24) | (static_cast<uint32_t>(den) << 16) |
                       (static_cast<uint32_t>(n32) << 8));
    put_word(&out, 0);
    put_word(&out, 0);
  }
  if (!options.name.empty()) {
    std::array<uint8_t, 12> text{};
    const size_t n = std::min<size_t>(options.name.size(), text.size());
    for (size_t i = 0; i < n; ++i) text[i] = static_cast<uint8_t>(options.name[i]);
    if (options.name.size() > text.size()) ++result.skipped_events;
    put_word(&out, delta_clockstamp_word(0));
    put_word(&out, flex_word0(kFlexBankMetadataText, kFlexStatusSongName));
    for (int w = 0; w < 3; ++w) {
      put_word(&out, (static_cast<uint32_t>(text[static_cast<size_t>(w) * 4 + 0]) << 24) |
                         (static_cast<uint32_t>(text[static_cast<size_t>(w) * 4 + 1]) << 16) |
                         (static_cast<uint32_t>(text[static_cast<size_t>(w) * 4 + 2]) << 8) |
                         static_cast<uint32_t>(text[static_cast<size_t>(w) * 4 + 3]));
    }
  }

  // Start of clip.
  put_word(&out, delta_clockstamp_word(0));
  put_stream(&out, kStreamStartOfClip);

  // Build the merged sequence-data item list: tempo / time-sig changes after the
  // first, plus all clip events.
  std::vector<SeqItem> items;
  for (size_t i = 1; i < tempo_segments.size(); ++i) {
    const double bpm = tempo_segments[i].bpm > 0.0 ? tempo_segments[i].bpm : kDefaultBpm;
    SeqItem item;
    item.tick = ppq_to_tick(tempo_segments[i].start_ppq, dctpq);
    item.order = 0;
    item.words[0] = flex_word0(kFlexBankSetupPerformance, kFlexStatusSetTempo);
    item.words[1] = static_cast<uint32_t>(std::llround(kTenNanosPerQuarterToBpm / bpm));
    item.word_count = 4;
    items.push_back(item);
  }
  for (size_t i = 1; i < time_signatures.size(); ++i) {
    const auto& seg = time_signatures[i];
    const uint8_t num = static_cast<uint8_t>(std::clamp(seg.time_sig.numerator, 1, 255));
    const uint8_t den = static_cast<uint8_t>(std::clamp(seg.time_sig.denominator, 1, 255));
    const uint8_t n32 = static_cast<uint8_t>(std::clamp(
        seg.thirty_seconds_per_quarter > 0 ? seg.thirty_seconds_per_quarter : 8, 1, 255));
    SeqItem item;
    item.tick = ppq_to_tick(seg.start_ppq, dctpq);
    item.order = 1;
    item.words[0] = flex_word0(kFlexBankSetupPerformance, kFlexStatusSetTimeSignature);
    item.words[1] = (static_cast<uint32_t>(num) << 24) | (static_cast<uint32_t>(den) << 16) |
                    (static_cast<uint32_t>(n32) << 8);
    item.word_count = 4;
    items.push_back(item);
  }
  for (const MidiClipEvent& ev : clip.events()) {
    const uint64_t tick = ppq_to_tick(ev.ppq, dctpq);
    if (ev.ump.sysex_handle != 0) {
      const std::vector<uint8_t>* payload = options.sysex_store != nullptr
                                                ? options.sysex_store->lookup(ev.ump.sysex_handle)
                                                : nullptr;
      if (payload == nullptr) {
        ++result.skipped_events;
        continue;
      }
      const bool emitted = needs_sysex8(*payload)
                               ? emit_sysex8(&items, tick, *payload, ev.ump.group)
                               : emit_sysex7(&items, tick, *payload, ev.ump.group);
      if (!emitted) {
        ++result.skipped_events;
      }
      continue;
    }
    SeqItem item;
    item.tick = tick;
    item.order = 2;
    const uint32_t mt = (ev.ump.words[0] >> 28) & 0x0Fu;
    item.word_count = ev.ump.word_count > 0 ? ev.ump.word_count : words_for_message_type(mt);
    for (int w = 0; w < item.word_count && w < 4; ++w)
      item.words[static_cast<size_t>(w)] = ev.ump.words[w];
    items.push_back(item);
  }

  std::stable_sort(items.begin(), items.end(), [](const SeqItem& a, const SeqItem& b) {
    if (a.tick != b.tick) return a.tick < b.tick;
    return a.order < b.order;
  });

  uint64_t last_tick = 0;
  uint64_t max_tick = 0;
  for (const SeqItem& item : items) {
    put_dcs(&out, &last_tick, item.tick);
    for (int w = 0; w < item.word_count; ++w) put_word(&out, item.words[static_cast<size_t>(w)]);
    max_tick = std::max(max_tick, item.tick);
  }

  // End of clip at the final event tick.
  put_dcs(&out, &last_tick, max_tick);
  put_stream(&out, kStreamEndOfClip);

  return result;
}

}  // namespace sonare::midi
