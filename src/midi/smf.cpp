#include "midi/smf.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "midi/ump.h"

namespace sonare::midi {
namespace {

// SMF chunk type tags.
constexpr uint8_t kMThd[4] = {'M', 'T', 'h', 'd'};
constexpr uint8_t kMTrk[4] = {'M', 'T', 'r', 'k'};

// SMF event marker bytes.
constexpr uint8_t kMetaPrefix = 0xFFu;
constexpr uint8_t kSysExStart = 0xF0u;
constexpr uint8_t kSysExEscape = 0xF7u;

// Recognized meta event type bytes (the byte following 0xFF).
constexpr uint8_t kMetaTrackName = 0x03u;
constexpr uint8_t kMetaMarker = 0x06u;
constexpr uint8_t kMetaEndOfTrack = 0x2Fu;
constexpr uint8_t kMetaSetTempo = 0x51u;
constexpr uint8_t kMetaTimeSignature = 0x58u;

// Microseconds per minute — the SMF set-tempo meta encodes microseconds per
// quarter note, so BPM = (us-per-minute) / (us-per-quarter). This is an SMF
// domain constant, not a universal numeric constant.
constexpr double kMicrosPerMinute = 60000000.0;
constexpr double kDefaultBpm = 120.0;

// ---------------------------------------------------------------------------
// Byte-buffer reader with bounds checking. All reads validate remaining length
// and set `overflow` (never read out of bounds). Control-thread only.
// ---------------------------------------------------------------------------
class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  size_t remaining() const noexcept { return pos_ <= size_ ? size_ - pos_ : 0; }
  size_t pos() const noexcept { return pos_; }
  bool overflow() const noexcept { return overflow_; }

  uint8_t u8() noexcept {
    if (remaining() < 1) {
      overflow_ = true;
      return 0;
    }
    return data_[pos_++];
  }

  uint16_t u16() noexcept {
    const uint8_t hi = u8();
    const uint8_t lo = u8();
    return static_cast<uint16_t>((static_cast<uint16_t>(hi) << 8) | lo);
  }

  uint32_t u32() noexcept {
    const uint32_t b0 = u8();
    const uint32_t b1 = u8();
    const uint32_t b2 = u8();
    const uint32_t b3 = u8();
    return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
  }

  /// Reads a variable-length quantity (7 bits/byte, MSB = continuation). At most
  /// 4 bytes per the SMF spec; a 5th continuation byte is treated as overflow.
  uint32_t vlq() noexcept {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const uint8_t byte = u8();
      if (overflow_) return 0;
      value = (value << 7) | (byte & 0x7Fu);
      if ((byte & 0x80u) == 0) return value;
    }
    // A fifth byte with the continuation bit still set is malformed.
    overflow_ = true;
    return value;
  }

  /// Returns a pointer to `count` bytes and advances, or nullptr on overflow.
  const uint8_t* take(size_t count) noexcept {
    if (remaining() < count) {
      overflow_ = true;
      return nullptr;
    }
    const uint8_t* p = data_ + pos_;
    pos_ += count;
    return p;
  }

  bool match_tag(const uint8_t (&tag)[4]) noexcept {
    const uint8_t* p = take(4);
    if (p == nullptr) return false;
    return p[0] == tag[0] && p[1] == tag[1] && p[2] == tag[2] && p[3] == tag[3];
  }

 private:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t pos_ = 0;
  bool overflow_ = false;
};

// ---------------------------------------------------------------------------
// Byte-buffer writer helpers (export).
// ---------------------------------------------------------------------------
void put_u8(std::vector<uint8_t>* out, uint8_t v) { out->push_back(v); }

void put_u16(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
  out->push_back(static_cast<uint8_t>(v & 0xFFu));
}

void put_u32(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
  out->push_back(static_cast<uint8_t>(v & 0xFFu));
}

void put_tag(std::vector<uint8_t>* out, const uint8_t (&tag)[4]) {
  out->insert(out->end(), tag, tag + 4);
}

void put_vlq(std::vector<uint8_t>* out, uint32_t value) {
  // Encode 7 bits/byte, big-endian, continuation bit on all but the last.
  std::array<uint8_t, 5> buf{};
  int n = 0;
  buf[n++] = static_cast<uint8_t>(value & 0x7Fu);
  while ((value >>= 7) != 0) {
    buf[n++] = static_cast<uint8_t>((value & 0x7Fu) | 0x80u);
  }
  for (int i = n - 1; i >= 0; --i) out->push_back(buf[i]);
}

// Converts SMF ticks to PPQ (quarter-note units) matching MidiClipEvent::ppq.
double ticks_to_ppq(uint32_t ticks, uint16_t ppqn) noexcept {
  if (ppqn == 0) return 0.0;
  return static_cast<double>(ticks) / static_cast<double>(ppqn);
}

// Converts PPQ (quarter notes) to SMF ticks (rounded to nearest).
int64_t ppq_to_ticks(double ppq, uint16_t ppqn) noexcept {
  const double t = ppq * static_cast<double>(ppqn);
  if (!std::isfinite(t) || t <= 0.0) return 0;
  return static_cast<int64_t>(std::llround(t));
}

// Decodes how many data bytes a MIDI 1.0 channel-voice status carries.
int channel_voice_data_count(uint8_t status) noexcept {
  switch (status & 0xF0u) {
    case 0xC0u:  // Program change.
    case 0xD0u:  // Channel pressure.
      return 1;
    case 0x80u:  // Note off.
    case 0x90u:  // Note on.
    case 0xA0u:  // Poly pressure.
    case 0xB0u:  // Control change.
    case 0xE0u:  // Pitch bend.
      return 2;
    default:
      return -1;
  }
}

// Imports a single MTrk chunk body of `length` bytes starting at the reader's
// current position. Appends parsed channel-voice events to `clip` (PPQ-timed),
// captures track name / markers / tempo / time-sig into the out params, and
// advances the reader to the chunk end. Returns false on malformed track data.
struct TrackParseState {
  MidiClip clip;
  std::string name;
  bool has_midi_events = false;
};

bool parse_track(Reader* reader, size_t length, uint16_t ppqn, TrackParseState* track,
                 std::vector<transport::TempoSegment>* tempos,
                 std::vector<transport::TimeSignatureSegment>* time_sigs,
                 std::vector<SmfMarker>* markers, SysExStore* sysex_store, uint32_t* skipped) {
  const size_t end_pos = reader->pos() + length;
  if (length > reader->remaining()) return false;

  uint32_t tick = 0;
  uint8_t running_status = 0;
  bool saw_end_of_track = false;

  while (reader->pos() < end_pos && !reader->overflow()) {
    const uint32_t delta = reader->vlq();
    if (reader->overflow()) return false;
    tick += delta;
    const double ppq = ticks_to_ppq(tick, ppqn);

    uint8_t status = reader->u8();
    if (reader->overflow()) return false;

    if (status == kMetaPrefix) {
      const uint8_t meta_type = reader->u8();
      const uint32_t meta_len = reader->vlq();
      if (reader->overflow()) return false;
      const uint8_t* payload = reader->take(meta_len);
      if (payload == nullptr) return false;

      switch (meta_type) {
        case kMetaSetTempo: {
          if (meta_len == 3) {
            const uint32_t us_per_quarter = (static_cast<uint32_t>(payload[0]) << 16) |
                                            (static_cast<uint32_t>(payload[1]) << 8) |
                                            static_cast<uint32_t>(payload[2]);
            const double bpm = us_per_quarter > 0
                                   ? kMicrosPerMinute / static_cast<double>(us_per_quarter)
                                   : kDefaultBpm;
            transport::TempoSegment seg;
            seg.start_ppq = ppq;
            seg.bpm = bpm;
            tempos->push_back(seg);
          } else {
            ++(*skipped);
          }
          break;
        }
        case kMetaTimeSignature: {
          if (meta_len >= 2) {
            transport::TimeSignatureSegment seg;
            seg.start_ppq = ppq;
            seg.time_sig.numerator = static_cast<int>(payload[0]);
            // SMF stores the denominator as a power of two (2 => 2^2 = 4).
            seg.time_sig.denominator = 1 << payload[1];
            time_sigs->push_back(seg);
          } else {
            ++(*skipped);
          }
          break;
        }
        case kMetaTrackName: {
          track->name.assign(reinterpret_cast<const char*>(payload), meta_len);
          break;
        }
        case kMetaMarker: {
          SmfMarker marker;
          marker.ppq = ppq;
          marker.text.assign(reinterpret_cast<const char*>(payload), meta_len);
          markers->push_back(marker);
          break;
        }
        case kMetaEndOfTrack:
          saw_end_of_track = true;
          break;
        default:
          ++(*skipped);
          break;
      }
      if (saw_end_of_track) break;
      continue;
    }

    if (status == kSysExStart || status == kSysExEscape) {
      const uint32_t sysex_len = reader->vlq();
      if (reader->overflow()) return false;
      const uint8_t* payload = reader->take(sysex_len);
      if (payload == nullptr) return false;

      const SysExHandle handle =
          sysex_store != nullptr ? sysex_store->add(payload, sysex_len) : SysExHandle{0};
      if (handle == 0) {
        ++(*skipped);
        continue;
      }
      MidiClipEvent ev;
      ev.ppq = ppq;
      ev.ump = make_sysex_handle(/*group=*/0, handle);
      track->clip.add_event(ev);
      track->has_midi_events = true;
      continue;
    }

    // Channel-voice (possibly running status).
    uint8_t first_data = 0;
    bool have_first_data = false;
    if (status & 0x80u) {
      running_status = status;
    } else {
      // Running status: the byte we read was actually the first data byte; the
      // real status is the previously-seen running-status byte.
      if ((running_status & 0x80u) == 0) return false;  // No status to run with.
      first_data = status;
      have_first_data = true;
      status = running_status;
    }

    const int data_count = channel_voice_data_count(status);
    if (data_count < 0) {
      // System common / unknown — cannot safely resync; stop the track.
      ++(*skipped);
      return false;
    }

    // Assemble the up-to-3 raw MIDI bytes and parse via the ump.h adapter.
    std::array<uint8_t, 3> raw{};
    size_t raw_len = 0;
    raw[raw_len++] = status;
    int remaining_data = data_count;
    if (have_first_data) {
      raw[raw_len++] = first_data;
      --remaining_data;
    }
    for (int i = 0; i < remaining_data; ++i) {
      raw[raw_len++] = reader->u8();
    }
    if (reader->overflow()) return false;

    Ump ump;
    uint8_t rs = status;
    const size_t consumed = midi1_bytes_to_ump(raw.data(), raw_len, /*group=*/0, &rs, &ump);
    if (consumed == 0) {
      ++(*skipped);
      continue;
    }
    MidiClipEvent ev;
    ev.ppq = ppq;
    ev.ump = ump;
    track->clip.add_event(ev);
    track->has_midi_events = true;
  }

  if (reader->overflow()) return false;
  // Resynchronize to the declared chunk end (skips any trailing bytes after an
  // explicit end-of-track meta, or unconsumed padding).
  if (reader->pos() < end_pos) {
    if (reader->take(end_pos - reader->pos()) == nullptr) return false;
  }
  return true;
}

}  // namespace

SmfImportResult import_smf(const uint8_t* data, size_t size) {
  SmfImportResult result;
  if (data == nullptr || size == 0) {
    result.status = SmfStatus::kInvalidArgument;
    result.diagnostic = "empty input";
    return result;
  }

  Reader reader(data, size);
  if (!reader.match_tag(kMThd)) {
    result.status = SmfStatus::kBadHeader;
    result.diagnostic = "missing MThd header chunk";
    return result;
  }
  const uint32_t header_len = reader.u32();
  if (reader.overflow() || header_len < 6) {
    result.status = SmfStatus::kBadHeader;
    result.diagnostic = "truncated or short MThd chunk";
    return result;
  }
  const uint16_t format = reader.u16();
  const uint16_t num_tracks = reader.u16();
  const uint16_t division = reader.u16();
  if (reader.overflow()) {
    result.status = SmfStatus::kTruncated;
    result.diagnostic = "truncated MThd fields";
    return result;
  }
  // Skip any extra header bytes beyond the standard 6.
  if (header_len > 6) {
    if (reader.take(header_len - 6) == nullptr) {
      result.status = SmfStatus::kTruncated;
      result.diagnostic = "truncated MThd extension";
      return result;
    }
  }

  if (format > 1) {
    result.status = SmfStatus::kUnsupportedFormat;
    result.diagnostic = "SMF format 2 is not supported";
    return result;
  }
  if (division & 0x8000u) {
    // SMPTE timing (negative frames/second) is not supported.
    result.status = SmfStatus::kUnsupportedFormat;
    result.diagnostic = "SMPTE division is not supported";
    return result;
  }
  if (division == 0) {
    result.status = SmfStatus::kBadHeader;
    result.diagnostic = "zero ticks-per-quarter division";
    return result;
  }

  result.format = format;
  result.ticks_per_quarter = division;

  for (uint16_t t = 0; t < num_tracks; ++t) {
    if (reader.remaining() == 0) {
      // Fewer track chunks than the header claimed: stop gracefully.
      break;
    }
    if (!reader.match_tag(kMTrk)) {
      result.status = SmfStatus::kBadTrack;
      result.diagnostic = "missing MTrk chunk header";
      return result;
    }
    const uint32_t track_len = reader.u32();
    if (reader.overflow()) {
      result.status = SmfStatus::kTruncated;
      result.diagnostic = "truncated MTrk length";
      return result;
    }

    TrackParseState track;
    if (!parse_track(&reader, track_len, division, &track, &result.tempo_segments,
                     &result.time_signatures, &result.markers, &result.sysex_store,
                     &result.skipped_events)) {
      result.status = SmfStatus::kTruncated;
      result.diagnostic = "malformed or truncated track data";
      return result;
    }

    if (track.has_midi_events) {
      track.clip.sort_stable();
      result.clips.push_back(std::move(track.clip));
      result.clip_names.push_back(std::move(track.name));
    }
  }

  // Provide sane defaults so the consumer can hand the segment vectors straight
  // to TempoMap::set_segments without an empty-vector crash.
  if (result.tempo_segments.empty()) {
    transport::TempoSegment seg;
    seg.start_ppq = 0.0;
    seg.bpm = kDefaultBpm;
    result.tempo_segments.push_back(seg);
  }
  if (result.time_signatures.empty()) {
    transport::TimeSignatureSegment seg;
    seg.start_ppq = 0.0;
    result.time_signatures.push_back(seg);
  }
  std::stable_sort(result.tempo_segments.begin(), result.tempo_segments.end(),
                   [](const transport::TempoSegment& a, const transport::TempoSegment& b) {
                     return a.start_ppq < b.start_ppq;
                   });
  std::stable_sort(
      result.time_signatures.begin(), result.time_signatures.end(),
      [](const transport::TimeSignatureSegment& a, const transport::TimeSignatureSegment& b) {
        return a.start_ppq < b.start_ppq;
      });

  result.status = SmfStatus::kOk;
  return result;
}

namespace {

// Appends a complete MTrk chunk (with a 4-byte length prefix) built from
// `body`.
void append_track_chunk(std::vector<uint8_t>* out, const std::vector<uint8_t>& body) {
  put_tag(out, kMTrk);
  put_u32(out, static_cast<uint32_t>(body.size()));
  out->insert(out->end(), body.begin(), body.end());
}

// Encodes a meta event (0xFF type len payload) into `body` with a delta time.
void put_meta(std::vector<uint8_t>* body, uint32_t delta, uint8_t type, const uint8_t* payload,
              size_t len) {
  put_vlq(body, delta);
  put_u8(body, kMetaPrefix);
  put_u8(body, type);
  put_vlq(body, static_cast<uint32_t>(len));
  if (len > 0 && payload != nullptr) {
    body->insert(body->end(), payload, payload + len);
  }
}

void put_sysex(std::vector<uint8_t>* body, uint32_t delta, const std::vector<uint8_t>& payload) {
  put_vlq(body, delta);
  put_u8(body, kSysExStart);
  put_vlq(body, static_cast<uint32_t>(payload.size()));
  body->insert(body->end(), payload.begin(), payload.end());
}

}  // namespace

SmfExportResult export_smf(const std::vector<MidiClip>& clips,
                           const std::vector<transport::TempoSegment>& tempo_segments,
                           const std::vector<transport::TimeSignatureSegment>& time_signatures,
                           const std::vector<std::string>& clip_names,
                           const SmfExportOptions& options) {
  SmfExportResult result;
  const uint16_t ppqn = options.ticks_per_quarter != 0 ? options.ticks_per_quarter : 480;

  // Header: format 1, num_tracks = 1 meta track + one per clip.
  const uint16_t num_tracks = static_cast<uint16_t>(1 + clips.size());
  put_tag(&result.bytes, kMThd);
  put_u32(&result.bytes, 6);
  put_u16(&result.bytes, 1);  // Format 1.
  put_u16(&result.bytes, num_tracks);
  put_u16(&result.bytes, ppqn);

  // -------- Track 0: tempo + time-signature meta. --------
  {
    // Merge tempo + time-sig events sorted by tick so delta times are correct.
    struct MetaItem {
      int64_t tick;
      int kind;  // 0 = tempo, 1 = time-sig.
      double bpm;
      int numerator;
      int denominator;
    };
    std::vector<MetaItem> items;
    for (const auto& seg : tempo_segments) {
      items.push_back(
          {ppq_to_ticks(seg.start_ppq, ppqn), 0, seg.bpm > 0.0 ? seg.bpm : kDefaultBpm, 0, 0});
    }
    for (const auto& seg : time_signatures) {
      items.push_back({ppq_to_ticks(seg.start_ppq, ppqn), 1, 0.0, seg.time_sig.numerator,
                       seg.time_sig.denominator});
    }
    std::stable_sort(items.begin(), items.end(),
                     [](const MetaItem& a, const MetaItem& b) { return a.tick < b.tick; });

    std::vector<uint8_t> body;
    int64_t prev_tick = 0;
    for (const auto& item : items) {
      const uint32_t delta = static_cast<uint32_t>(std::max<int64_t>(0, item.tick - prev_tick));
      prev_tick = item.tick;
      if (item.kind == 0) {
        const double us = kMicrosPerMinute / item.bpm;
        const uint32_t us_per_quarter = static_cast<uint32_t>(std::llround(us));
        const uint8_t payload[3] = {static_cast<uint8_t>((us_per_quarter >> 16) & 0xFFu),
                                    static_cast<uint8_t>((us_per_quarter >> 8) & 0xFFu),
                                    static_cast<uint8_t>(us_per_quarter & 0xFFu)};
        put_meta(&body, delta, kMetaSetTempo, payload, 3);
      } else {
        // Encode denominator as a power of two.
        uint8_t dd = 0;
        int den = item.denominator > 0 ? item.denominator : 4;
        while ((1 << dd) < den && dd < 7) ++dd;
        const uint8_t payload[4] = {static_cast<uint8_t>(item.numerator), dd, 24, 8};
        put_meta(&body, delta, kMetaTimeSignature, payload, 4);
      }
    }
    // End-of-track.
    put_meta(&body, 0, kMetaEndOfTrack, nullptr, 0);
    append_track_chunk(&result.bytes, body);
  }

  // -------- One MTrk per clip. --------
  for (size_t ci = 0; ci < clips.size(); ++ci) {
    MidiClip clip = clips[ci];
    clip.sort_stable();
    std::vector<uint8_t> body;

    // Optional track name meta at tick 0.
    if (ci < clip_names.size() && !clip_names[ci].empty()) {
      put_meta(&body, 0, kMetaTrackName, reinterpret_cast<const uint8_t*>(clip_names[ci].data()),
               clip_names[ci].size());
    }

    int64_t prev_tick = 0;
    for (const MidiClipEvent& ev : clip.events()) {
      const int64_t tick = ppq_to_ticks(ev.ppq, ppqn);

      if (ev.ump.sysex_handle != 0 || ev.ump.message_type() == UmpMessageType::kData64 ||
          ev.ump.message_type() == UmpMessageType::kData128) {
        const std::vector<uint8_t>* payload = options.sysex_store != nullptr
                                                  ? options.sysex_store->lookup(ev.ump.sysex_handle)
                                                  : nullptr;
        if (payload == nullptr) {
          continue;
        }
        const uint32_t delta = static_cast<uint32_t>(std::max<int64_t>(0, tick - prev_tick));
        prev_tick = tick;
        put_sysex(&body, delta, *payload);
        continue;
      }

      // Down-convert MIDI 2.0 events to MIDI 1.0 before serialization. MIDI 1.0
      // events pass through unchanged; per-note controllers yield word_count 0.
      Ump ump = ev.ump;
      if (ump.message_type() == UmpMessageType::kMidi2ChannelVoice) {
        ump = midi2_to_midi1(ump);
      }
      if (ump.message_type() != UmpMessageType::kMidi1ChannelVoice || ump.word_count == 0) {
        continue;  // Unresolved SysEx / dropped 2.0-only messages are not emitted.
      }
      std::array<uint8_t, 3> raw{};
      const size_t n = ump_to_midi1_bytes(ump, raw.data(), raw.size());
      if (n == 0) continue;

      const uint32_t delta = static_cast<uint32_t>(std::max<int64_t>(0, tick - prev_tick));
      prev_tick = tick;
      put_vlq(&body, delta);
      // Always write an explicit status byte (no running-status compression) so
      // the output is unambiguous and simple to re-import.
      body.insert(body.end(), raw.begin(), raw.begin() + n);
    }
    put_meta(&body, 0, kMetaEndOfTrack, nullptr, 0);
    append_track_chunk(&result.bytes, body);
  }

  result.status = SmfStatus::kOk;
  return result;
}

}  // namespace sonare::midi
