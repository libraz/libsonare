#include "core/audio_tags_id3.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sonare::detail {
namespace {

constexpr size_t kHeaderSize = 10;
constexpr size_t kFrameHeaderSize = 10;
constexpr size_t kV1TagSize = 128;

void append_codepoint(std::string& out, uint32_t codepoint) {
  if (codepoint <= 0x7Fu) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FFu) {
    out.push_back(static_cast<char>(0xC0u | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
  } else if (codepoint <= 0xFFFFu) {
    out.push_back(static_cast<char>(0xE0u | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
  } else {
    out.push_back(static_cast<char>(0xF0u | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
  }
}

std::string latin1_to_utf8(const uint8_t* data, size_t size) {
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size && data[i] != 0; ++i) append_codepoint(out, data[i]);
  return out;
}

std::string decode_utf16(const uint8_t* data, size_t size, bool big_endian, bool has_bom) {
  if (has_bom && size >= 2) {
    if (data[0] == 0xFFu && data[1] == 0xFEu) {
      big_endian = false;
      data += 2;
      size -= 2;
    } else if (data[0] == 0xFEu && data[1] == 0xFFu) {
      big_endian = true;
      data += 2;
      size -= 2;
    }
  }

  std::string out;
  out.reserve(size * 2);
  size_t offset = 0;
  bool terminated = false;
  while (offset + 1 < size) {
    const uint16_t first = big_endian
                               ? static_cast<uint16_t>((data[offset] << 8) | data[offset + 1])
                               : static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
    offset += 2;
    if (first == 0) {
      terminated = true;
      break;
    }

    if (first >= 0xD800u && first <= 0xDBFFu) {
      if (offset + 1 >= size) {
        append_codepoint(out, 0xFFFDu);
        break;
      }
      const uint16_t second = big_endian
                                  ? static_cast<uint16_t>((data[offset] << 8) | data[offset + 1])
                                  : static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
      if (second < 0xDC00u || second > 0xDFFFu) {
        append_codepoint(out, 0xFFFDu);
        continue;
      }
      offset += 2;
      const uint32_t codepoint = 0x10000u + ((static_cast<uint32_t>(first) - 0xD800u) << 10) +
                                 (static_cast<uint32_t>(second) - 0xDC00u);
      append_codepoint(out, codepoint);
    } else if (first >= 0xDC00u && first <= 0xDFFFu) {
      append_codepoint(out, 0xFFFDu);
    } else {
      append_codepoint(out, first);
    }
  }
  if (!terminated && offset < size) append_codepoint(out, 0xFFFDu);
  return out;
}

std::string decode_text(uint8_t encoding, const uint8_t* data, size_t size) {
  if (encoding == 0) return latin1_to_utf8(data, size);
  if (encoding == 1) return decode_utf16(data, size, true, true);
  if (encoding == 2) return decode_utf16(data, size, true, false);
  if (encoding == 3) {
    size_t length = 0;
    while (length < size && data[length] != 0) ++length;
    return std::string(reinterpret_cast<const char*>(data), length);
  }
  return {};
}

size_t find_terminator(const uint8_t* data, size_t size, uint8_t encoding) {
  const size_t width = encoding == 1 || encoding == 2 ? 2 : 1;
  for (size_t offset = 0; offset + width <= size; offset += width) {
    if (data[offset] == 0 && (width == 1 || data[offset + 1] == 0)) return offset;
  }
  return size;
}

size_t read_syncsafe(const uint8_t* data, bool& valid) {
  size_t value = 0;
  valid = true;
  for (size_t i = 0; i < 4; ++i) {
    if ((data[i] & 0x80u) != 0) valid = false;
    value = (value << 7) | (data[i] & 0x7Fu);
  }
  return value;
}

uint32_t read_be32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

bool is_text_frame(std::string_view id) { return id.front() == 'T' || id == "COMM"; }

void remove_unsynchronization(std::vector<uint8_t>& bytes) {
  size_t out = 0;
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[out++] = bytes[i];
    if (bytes[i] == 0xFFu && i + 1 < bytes.size() && bytes[i + 1] == 0) ++i;
  }
  bytes.resize(out);
}

void add_text_frame(std::vector<std::pair<std::string, std::string>>& tags, std::string_view id,
                    const uint8_t* data, size_t size) {
  if (size == 0) return;
  const uint8_t encoding = data[0];
  if (encoding > 3) return;
  if (id == "COMM") {
    if (size < 4) return;
    const uint8_t* description = data + 4;
    const size_t description_size = find_terminator(description, size - 4, encoding);
    if (description_size == size - 4) return;
    const size_t separator = encoding == 1 || encoding == 2 ? 2 : 1;
    const size_t value_offset =
        description_size < size - 4 ? 4 + description_size + separator : size;
    tags.emplace_back("COMM", decode_text(encoding, data + value_offset, size - value_offset));
    return;
  }
  if (id == "TXXX") {
    const uint8_t* description = data + 1;
    const size_t description_size = find_terminator(description, size - 1, encoding);
    if (description_size == size - 1) return;
    const size_t separator = encoding == 1 || encoding == 2 ? 2 : 1;
    const std::string key = "TXXX:" + decode_text(encoding, description, description_size);
    const size_t value_offset =
        description_size < size - 1 ? 1 + description_size + separator : size;
    tags.emplace_back(key, decode_text(encoding, data + value_offset, size - value_offset));
    return;
  }
  if (id.front() == 'T')
    tags.emplace_back(std::string(id), decode_text(encoding, data + 1, size - 1));
}

void read_v2(const uint8_t* data, size_t size,
             std::vector<std::pair<std::string, std::string>>& tags) {
  if (size < kHeaderSize || std::memcmp(data, "ID3", 3) != 0) return;
  const uint8_t major = data[3];
  if ((major != 3 && major != 4) || data[4] == 0xFFu) return;
  bool size_valid = false;
  const size_t body_size = read_syncsafe(data + 6, size_valid);
  if (!size_valid || body_size > size - kHeaderSize) return;
  const size_t body_end = kHeaderSize + body_size;
  const bool has_footer = major == 4 && (data[5] & 0x10u) != 0;
  if (has_footer) {
    if (size - body_end < kHeaderSize || std::memcmp(data + body_end, "3DI", 3) != 0) return;
  }

  std::vector<uint8_t> body(data + kHeaderSize, data + body_end);
  const bool tag_unsynchronized = (data[5] & 0x80u) != 0;
  if (major == 3 && tag_unsynchronized) remove_unsynchronization(body);

  size_t offset = 0;
  if ((data[5] & 0x40u) != 0) {
    if (body.size() < 4) return;
    if (major == 3) {
      const size_t ext_size = read_be32(body.data());
      if (ext_size > body.size() - 4) return;
      offset = 4 + ext_size;
    } else {
      bool ext_valid = false;
      const size_t ext_size = read_syncsafe(body.data(), ext_valid);
      if (!ext_valid || ext_size < 4 || ext_size > body.size()) return;
      offset = ext_size;
    }
  }

  while (offset + kFrameHeaderSize <= body.size()) {
    const uint8_t* frame = body.data() + offset;
    if (frame[0] == 0) break;
    const std::string_view id(reinterpret_cast<const char*>(frame), 4);
    if (!std::all_of(id.begin(), id.end(), [](unsigned char c) {
          return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        })) {
      break;
    }

    size_t frame_size = 0;
    if (major == 3) {
      frame_size = read_be32(frame + 4);
    } else {
      bool frame_size_valid = false;
      frame_size = read_syncsafe(frame + 4, frame_size_valid);
      if (!frame_size_valid) break;
    }
    const uint8_t format_flags = frame[9];
    const size_t payload_start = offset + kFrameHeaderSize;
    if (frame_size > body.size() - payload_start) break;
    offset = payload_start + frame_size;

    const bool compressed = major == 3 ? (format_flags & 0x80u) != 0 : (format_flags & 0x08u) != 0;
    const bool encrypted = major == 3 ? (format_flags & 0x40u) != 0 : (format_flags & 0x04u) != 0;
    if (compressed || encrypted || !is_text_frame(id)) continue;

    std::vector<uint8_t> payload(body.begin() + static_cast<std::ptrdiff_t>(payload_start),
                                 body.begin() + static_cast<std::ptrdiff_t>(offset));
    size_t prefix = 0;
    if (major == 3 && (format_flags & 0x20u) != 0) prefix = 1;
    if (major == 4 && (format_flags & 0x40u) != 0) prefix = 1;
    if (major == 4 && (format_flags & 0x01u) != 0) prefix += 4;
    if (prefix > payload.size()) continue;
    payload.erase(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(prefix));
    if (major == 4 && (tag_unsynchronized || (format_flags & 0x02u) != 0)) {
      remove_unsynchronization(payload);
    }
    add_text_frame(tags, id, payload.data(), payload.size());
  }
}

std::string read_v1_field(const uint8_t* data, size_t size) {
  while (size > 0 && (data[size - 1] == 0 || data[size - 1] == ' ')) --size;
  return latin1_to_utf8(data, size);
}

void read_v1(const uint8_t* data, size_t size,
             std::vector<std::pair<std::string, std::string>>& tags) {
  if (size < kV1TagSize || std::memcmp(data + size - kV1TagSize, "TAG", 3) != 0) return;
  const uint8_t* tag = data + size - kV1TagSize;
  const std::array<std::pair<std::string_view, std::pair<size_t, size_t>>, 4> fields = {{
      {"TITLE", {3, 30}},
      {"ARTIST", {33, 30}},
      {"ALBUM", {63, 30}},
      {"DATE", {93, 4}},
  }};
  for (const auto& field : fields) {
    const std::string value = read_v1_field(tag + field.second.first, field.second.second);
    if (!value.empty()) tags.emplace_back(field.first, value);
  }

  const bool has_track = tag[125] == 0 && tag[126] != 0;
  const size_t comment_size = has_track ? 28 : 30;
  const std::string comment = read_v1_field(tag + 97, comment_size);
  if (!comment.empty()) tags.emplace_back("COMMENT", comment);
  if (has_track) tags.emplace_back("TRACKNUMBER", std::to_string(tag[126]));

  constexpr std::array<std::string_view, 80> kGenres = {"Blues",
                                                        "Classic Rock",
                                                        "Country",
                                                        "Dance",
                                                        "Disco",
                                                        "Funk",
                                                        "Grunge",
                                                        "Hip-Hop",
                                                        "Jazz",
                                                        "Metal",
                                                        "New Age",
                                                        "Oldies",
                                                        "Other",
                                                        "Pop",
                                                        "R&B",
                                                        "Rap",
                                                        "Reggae",
                                                        "Rock",
                                                        "Techno",
                                                        "Industrial",
                                                        "Alternative",
                                                        "Ska",
                                                        "Death Metal",
                                                        "Pranks",
                                                        "Soundtrack",
                                                        "Euro-Techno",
                                                        "Ambient",
                                                        "Trip-Hop",
                                                        "Vocal",
                                                        "Jazz+Funk",
                                                        "Fusion",
                                                        "Trance",
                                                        "Classical",
                                                        "Instrumental",
                                                        "Acid",
                                                        "House",
                                                        "Game",
                                                        "Sound Clip",
                                                        "Gospel",
                                                        "Noise",
                                                        "Alternative Rock",
                                                        "Bass",
                                                        "Soul",
                                                        "Punk",
                                                        "Space",
                                                        "Meditative",
                                                        "Instrumental Pop",
                                                        "Instrumental Rock",
                                                        "Ethnic",
                                                        "Gothic",
                                                        "Darkwave",
                                                        "Techno-Industrial",
                                                        "Electronic",
                                                        "Pop-Folk",
                                                        "Eurodance",
                                                        "Dream",
                                                        "Southern Rock",
                                                        "Comedy",
                                                        "Cult",
                                                        "Gangsta",
                                                        "Top 40",
                                                        "Christian Rap",
                                                        "Pop/Funk",
                                                        "Jungle",
                                                        "Native American",
                                                        "Cabaret",
                                                        "New Wave",
                                                        "Psychedelic",
                                                        "Rave",
                                                        "Showtunes",
                                                        "Trailer",
                                                        "Lo-Fi",
                                                        "Tribal",
                                                        "Acid Punk",
                                                        "Acid Jazz",
                                                        "Polka",
                                                        "Retro",
                                                        "Musical",
                                                        "Rock & Roll",
                                                        "Hard Rock"};
  if (tag[127] < kGenres.size()) tags.emplace_back("GENRE", kGenres[tag[127]]);
}

}  // namespace

Id3RawTags read_id3_raw_tags(const uint8_t* data, size_t size) {
  Id3RawTags tags;
  if (data == nullptr || size == 0) return tags;
  read_v2(data, size, tags.v2);
  read_v1(data, size, tags.v1);
  return tags;
}

}  // namespace sonare::detail
