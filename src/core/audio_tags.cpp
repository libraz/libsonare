#include "core/audio_tags.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/audio_io.h"
#include "core/audio_tags_id3.h"
#include "dr_wav.h"
#include "util/exception.h"

#ifdef SONARE_WITH_FFMPEG
#include "core/audio_io_ffmpeg.h"
#endif

namespace sonare {
namespace {

constexpr std::string_view kReplacement = "\xEF\xBF\xBD";

std::string sanitize_utf8(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const uint8_t first = static_cast<uint8_t>(text[i]);
    if (first == 0) break;
    if (first < 0x80u) {
      out.push_back(static_cast<char>(first));
      ++i;
      continue;
    }

    size_t length = 0;
    uint32_t codepoint = 0;
    uint32_t minimum = 0;
    if ((first & 0xE0u) == 0xC0u) {
      length = 2;
      codepoint = first & 0x1Fu;
      minimum = 0x80u;
    } else if ((first & 0xF0u) == 0xE0u) {
      length = 3;
      codepoint = first & 0x0Fu;
      minimum = 0x800u;
    } else if ((first & 0xF8u) == 0xF0u) {
      length = 4;
      codepoint = first & 0x07u;
      minimum = 0x10000u;
    }

    if (length == 0 || i + length > text.size()) {
      out.append(kReplacement);
      ++i;
      continue;
    }
    bool valid = true;
    for (size_t j = 1; j < length; ++j) {
      const uint8_t next = static_cast<uint8_t>(text[i + j]);
      if ((next & 0xC0u) != 0x80u) {
        valid = false;
        break;
      }
      codepoint = (codepoint << 6) | (next & 0x3Fu);
    }
    if (!valid || codepoint < minimum || codepoint > 0x10FFFFu ||
        (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
      out.append(kReplacement);
      ++i;
      continue;
    }
    out.append(text.data() + i, length);
    i += length;
  }
  return out;
}

std::string decode_cp1252(std::string_view text) {
  constexpr std::array<uint16_t, 32> kWindows1252 = {
      0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
      0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
      0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
  std::string out;
  out.reserve(text.size() * 2);
  for (unsigned char value : text) {
    if (value == 0) break;
    if (value >= 0x80u && value <= 0x9Fu) {
      detail::append_tag_utf8(out, kWindows1252[value - 0x80u]);
    } else {
      detail::append_tag_utf8(out, value);
    }
  }
  return out;
}

std::string clean_text(std::string_view text, bool cp1252 = false) {
  if (!cp1252) return sanitize_utf8(text);
  const std::string valid_utf8 = sanitize_utf8(text);
  size_t source_length = 0;
  while (source_length < text.size() && text[source_length] != '\0') ++source_length;
  if (valid_utf8.size() == source_length) {
    bool same = true;
    for (size_t i = 0; i < source_length; ++i) {
      if (valid_utf8[i] != text[i]) {
        same = false;
        break;
      }
    }
    if (same) return valid_utf8;
  }
  return decode_cp1252(text);
}

const AudioTagNormalization* normalization_for(std::string_view key) noexcept {
  for (const auto& normalization : kAudioTagNormalizations) {
    for (const std::string_view source_key : normalization.source_keys) {
      if (!source_key.empty() && key == source_key) return &normalization;
    }
  }
  return nullptr;
}

bool is_track_value(std::string_view value) {
  if (value == "0/0" || value.empty()) return false;
  const size_t slash = value.find('/');
  if (slash == std::string_view::npos) {
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char c) { return c >= '0' && c <= '9'; });
  }
  if (slash == 0 || slash + 1 == value.size()) return false;
  for (size_t i = 0; i < value.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (i == slash) {
      if (c != '/') return false;
    } else if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

std::string normalize_track_value(std::string value) {
  if (value.find('/') != std::string::npos) return value;
  const size_t first_nonzero = value.find_first_not_of('0');
  if (first_nonzero == std::string::npos) return "0";
  value.erase(0, first_nonzero);
  return value;
}

void append_tag(AudioTags& tags, std::string_view raw_key, std::string_view raw_value,
                bool cp1252 = false) {
  std::string key = clean_text(raw_key);
  std::string value = clean_text(raw_value, cp1252);
  if (key.empty()) return;
  const AudioTagNormalization* normalization = normalization_for(key);
  if (normalization != nullptr && normalization->canonical == "track" && !is_track_value(value)) {
    normalization = nullptr;
  }
  if (normalization != nullptr) {
    key.assign(normalization->canonical);
    if (normalization->canonical == "track") value = normalize_track_value(std::move(value));
  }
  const auto [it, inserted] = tags.emplace(key, value);
  if (!inserted) {
    it->second.append("; ");
    it->second.append(value);
  }
}

std::string combine_track_date(std::string_view year, std::string_view day_month) {
  if (year.size() != 4 || day_month.size() != 4 ||
      !std::all_of(year.begin(), year.end(),
                   [](unsigned char c) { return c >= '0' && c <= '9'; }) ||
      !std::all_of(day_month.begin(), day_month.end(),
                   [](unsigned char c) { return c >= '0' && c <= '9'; })) {
    return {};
  }
  const int day = (day_month[0] - '0') * 10 + day_month[1] - '0';
  const int month = (day_month[2] - '0') * 10 + day_month[3] - '0';
  if (day < 1 || day > 31 || month < 1 || month > 12) return {};
  std::string value(year);
  value.push_back('-');
  value.append(day_month.substr(2, 2));
  value.push_back('-');
  value.append(day_month.substr(0, 2));
  return value;
}

std::vector<std::pair<std::string, std::string>> prepare_id3_v2(
    const std::vector<std::pair<std::string, std::string>>& raw) {
  std::vector<std::pair<std::string, std::string>> prepared;
  prepared.reserve(raw.size());
  bool has_tdrc = false;
  std::string year;
  std::string day_month;
  for (const auto& item : raw) {
    if (item.first == "TDRC") has_tdrc = true;
    if (year.empty() && item.first == "TYER") year = item.second;
    if (day_month.empty() && item.first == "TDAT") day_month = item.second;
  }
  bool added_combined_date = false;
  for (const auto& item : raw) {
    if (!has_tdrc && item.first == "TYER") {
      if (!added_combined_date && item.first == "TYER") {
        const std::string combined = combine_track_date(year, day_month);
        prepared.emplace_back("DATE", combined.empty() ? year : combined);
        added_combined_date = true;
      }
      continue;
    }
    if (!has_tdrc && item.first == "TDAT" && !year.empty()) continue;
    if (has_tdrc && (item.first == "TYER" || item.first == "TDAT")) continue;
    prepared.push_back(item);
  }
  return prepared;
}

uint32_t read_le32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

bool valid_info_list(const uint8_t* data, size_t begin, size_t end) {
  size_t cursor = begin;
  while (cursor < end) {
    if (end - cursor < 8) return false;
    const size_t chunk_size = read_le32(data + cursor + 4);
    cursor += 8;
    if (chunk_size > end - cursor) return false;
    const size_t padded_size = chunk_size + (chunk_size & 1u);
    if (padded_size > end - cursor) return false;
    cursor += padded_size;
  }
  return cursor == end;
}

bool valid_riff_layout(const uint8_t* data, size_t size) {
  if (data == nullptr || size < 12 || std::memcmp(data, "RIFF", 4) != 0 ||
      std::memcmp(data + 8, "WAVE", 4) != 0) {
    return false;
  }
  const size_t riff_size = read_le32(data + 4);
  if (riff_size < 4 || riff_size > size - 8) return false;
  const size_t riff_end = riff_size + 8;
  size_t cursor = 12;
  while (cursor < riff_end) {
    if (riff_end - cursor < 8) return false;
    const size_t chunk_size = read_le32(data + cursor + 4);
    const size_t body = cursor + 8;
    if (chunk_size > riff_end - body) return false;
    const size_t padded_size = chunk_size + (chunk_size & 1u);
    if (padded_size > riff_end - body) return false;
    if (std::memcmp(data + cursor, "LIST", 4) == 0 && chunk_size >= 4 &&
        !valid_info_list(data, body + 4, body + chunk_size)) {
      return false;
    }
    cursor = body + padded_size;
  }
  return cursor == riff_end;
}

struct alignas(std::max_align_t) AllocationHeader {
  size_t size;
};

struct BoundedAllocator {
  size_t limit = 0;
  size_t used = 0;
};

void* bounded_malloc(size_t size, void* user_data) {
  auto* allocator = static_cast<BoundedAllocator*>(user_data);
  if (size > allocator->limit - allocator->used ||
      size > std::numeric_limits<size_t>::max() - sizeof(AllocationHeader)) {
    return nullptr;
  }
  auto* header = static_cast<AllocationHeader*>(std::malloc(sizeof(AllocationHeader) + size));
  if (header == nullptr) return nullptr;
  header->size = size;
  allocator->used += size;
  return header + 1;
}

void* bounded_realloc(void* pointer, size_t size, void* user_data) {
  if (pointer == nullptr) return bounded_malloc(size, user_data);
  auto* allocator = static_cast<BoundedAllocator*>(user_data);
  auto* old_header = static_cast<AllocationHeader*>(pointer) - 1;
  const size_t old_size = old_header->size;
  if (size > allocator->limit - (allocator->used - old_size) ||
      size > std::numeric_limits<size_t>::max() - sizeof(AllocationHeader)) {
    return nullptr;
  }
  auto* header =
      static_cast<AllocationHeader*>(std::realloc(old_header, sizeof(AllocationHeader) + size));
  if (header == nullptr) return nullptr;
  header->size = size;
  allocator->used = allocator->used - old_size + size;
  return header + 1;
}

void bounded_free(void* pointer, void* user_data) {
  if (pointer == nullptr) return;
  auto* allocator = static_cast<BoundedAllocator*>(user_data);
  auto* header = static_cast<AllocationHeader*>(pointer) - 1;
  allocator->used -= header->size;
  std::free(header);
}

struct DrWavGuard {
  drwav wav{};
  bool opened = false;
  ~DrWavGuard() {
    if (opened) drwav_uninit(&wav);
  }
};

std::string_view info_id_for_metadata_type(drwav_metadata_type type) {
  switch (type) {
    case drwav_metadata_type_list_info_title:
      return "INAM";
    case drwav_metadata_type_list_info_artist:
      return "IART";
    case drwav_metadata_type_list_info_album:
      return "IPRD";
    case drwav_metadata_type_list_info_date:
      return "ICRD";
    case drwav_metadata_type_list_info_genre:
      return "IGNR";
    case drwav_metadata_type_list_info_tracknumber:
      return "ITRK";
    case drwav_metadata_type_list_info_comment:
      return "ICMT";
    case drwav_metadata_type_list_info_software:
      return "ISFT";
    case drwav_metadata_type_list_info_copyright:
      return "ICOP";
    case drwav_metadata_type_list_info_location:
      return "IARL";
    case drwav_metadata_type_list_info_organization:
      return "ICMS";
    case drwav_metadata_type_list_info_keywords:
      return "IKEY";
    case drwav_metadata_type_list_info_medium:
      return "IMED";
    case drwav_metadata_type_list_info_description:
      return "ISBJ";
    default:
      return {};
  }
}

AudioTags read_wav_tags(const uint8_t* data, size_t size) {
  AudioTags tags;
  if (!valid_riff_layout(data, size)) return tags;

  // Every metadata record costs one drwav_metadata and stems from a chunk of at least 8 bytes.
  constexpr size_t kMinChunkBytes = 8;
  const size_t record_bound = size / kMinChunkBytes + 1;
  BoundedAllocator allocator;
  allocator.limit = size > std::numeric_limits<size_t>::max() / 4 ||
                            record_bound > (std::numeric_limits<size_t>::max() - size * 4) /
                                               sizeof(drwav_metadata)
                        ? std::numeric_limits<size_t>::max()
                        : size * 4 + record_bound * sizeof(drwav_metadata);
  drwav_allocation_callbacks callbacks{&allocator, bounded_malloc, bounded_realloc, bounded_free};
  DrWavGuard guard;
  // The flags are init flags, not metadata types: every metadata type is read.
  if (!drwav_init_memory_with_metadata(&guard.wav, data, size, 0, &callbacks)) {
    return tags;
  }
  guard.opened = true;
  for (drwav_uint32 i = 0; i < guard.wav.metadataCount; ++i) {
    const drwav_metadata& metadata = guard.wav.pMetadata[i];
    if (metadata.type == drwav_metadata_type_unknown) {
      if (metadata.data.unknown.chunkLocation != drwav_metadata_location_inside_info_list) continue;
      const std::string key(reinterpret_cast<const char*>(metadata.data.unknown.id), 4);
      const std::string value =
          metadata.data.unknown.dataSizeInBytes == 0
              ? std::string{}
              : std::string(reinterpret_cast<const char*>(metadata.data.unknown.pData),
                            static_cast<size_t>(metadata.data.unknown.dataSizeInBytes));
      append_tag(tags, key, value, true);
      continue;
    }
    const std::string_view key = info_id_for_metadata_type(metadata.type);
    if (key.empty()) continue;
    const auto& text = metadata.data.infoText;
    const std::string_view value = text.pString == nullptr
                                       ? std::string_view{}
                                       : std::string_view(text.pString, text.stringLength);
    append_tag(tags, key, value, true);
  }
  return tags;
}

AudioTags normalize_raw_tags(const std::vector<std::pair<std::string, std::string>>& raw,
                             bool cp1252 = false) {
  AudioTags tags;
  for (const auto& [key, value] : raw) append_tag(tags, key, value, cp1252);
  return tags;
}

}  // namespace

std::string_view wav_info_id_for_tag(std::string_view tag) noexcept {
  for (const auto& normalization : kAudioTagNormalizations) {
    if (normalization.canonical == tag) return normalization.wav_info_id;
  }
  return {};
}

std::string_view sniff_optional_audio_format(const uint8_t* data, size_t size) noexcept {
  if (data == nullptr) return {};
  if (size >= 4 && std::memcmp(data, "fLaC", 4) == 0) return "flac";
  if (size >= 4 && std::memcmp(data, "OggS", 4) == 0) return "ogg";
  if (size >= 8 && std::memcmp(data + 4, "ftyp", 4) == 0) return "m4a";
  if (size >= 2 && data[0] == 0xFFu && (data[1] & 0xF6u) == 0xF0u) return "aac";
  return {};
}

AudioTags read_audio_tags(const uint8_t* data, size_t size) {
  SONARE_CHECK_MSG(data != nullptr && size != 0, ErrorCode::InvalidFormat,
                   "Unsupported audio format");
  switch (detect_format(data, size)) {
    case AudioFormat::WAV:
      return read_wav_tags(data, size);
    case AudioFormat::MP3: {
      const detail::Id3RawTags raw = detail::read_id3_raw_tags(data, size);
      AudioTags tags = normalize_raw_tags(prepare_id3_v2(raw.v2));
      const AudioTags fallback = normalize_raw_tags(raw.v1);
      for (const auto& [key, value] : fallback) tags.emplace(key, value);
      return tags;
    }
    case AudioFormat::Unknown:
      break;
  }

  const std::string_view optional_format = sniff_optional_audio_format(data, size);
#ifdef SONARE_WITH_FFMPEG
  if (!optional_format.empty()) {
    return normalize_raw_tags(read_raw_audio_tags_ffmpeg(data, size));
  }
#else
  if (!optional_format.empty()) {
    throw SonareParameterRefusal::predicate_with_reason(
        ErrorCode::NotSupported, "data", "uses a format unavailable for tag reading in this build",
        ErrorReason::NotCompiledIn);
  }
#endif
  throw SonareException(ErrorCode::InvalidFormat, "Unsupported audio format");
}

}  // namespace sonare
