#include "core/audio_tags.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/exception.h"

namespace {

using Bytes = std::vector<uint8_t>;

void append_u32_le(Bytes& bytes, uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
  bytes.push_back(static_cast<uint8_t>(value >> 16));
  bytes.push_back(static_cast<uint8_t>(value >> 24));
}

void append_u32_be(Bytes& bytes, uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value >> 24));
  bytes.push_back(static_cast<uint8_t>(value >> 16));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
  bytes.push_back(static_cast<uint8_t>(value));
}

void append_syncsafe(Bytes& bytes, size_t value) {
  bytes.push_back(static_cast<uint8_t>((value >> 21) & 0x7Fu));
  bytes.push_back(static_cast<uint8_t>((value >> 14) & 0x7Fu));
  bytes.push_back(static_cast<uint8_t>((value >> 7) & 0x7Fu));
  bytes.push_back(static_cast<uint8_t>(value & 0x7Fu));
}

void append_chunk(Bytes& bytes, std::string_view id, const Bytes& payload) {
  REQUIRE(id.size() == 4);
  bytes.insert(bytes.end(), id.begin(), id.end());
  append_u32_le(bytes, static_cast<uint32_t>(payload.size()));
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  if ((payload.size() & 1u) != 0) bytes.push_back(0);
}

Bytes bytes_from(std::string_view text) { return Bytes(text.begin(), text.end()); }

Bytes make_wav(const std::vector<std::pair<std::string, Bytes>>& info) {
  Bytes body{'W', 'A', 'V', 'E'};
  Bytes format;
  format.reserve(16);
  // PCM, one channel.
  format.insert(format.end(), {1, 0, 1, 0});
  append_u32_le(format, 44100);
  append_u32_le(format, 88200);
  format.push_back(2);
  format.push_back(0);
  format.push_back(16);
  format.push_back(0);
  append_chunk(body, "fmt ", format);

  if (!info.empty()) {
    Bytes list{'I', 'N', 'F', 'O'};
    for (const auto& [id, value] : info) append_chunk(list, id, value);
    append_chunk(body, "LIST", list);
  }
  append_chunk(body, "data", Bytes{0, 0});

  Bytes wav{'R', 'I', 'F', 'F'};
  append_u32_le(wav, static_cast<uint32_t>(body.size()));
  wav.insert(wav.end(), body.begin(), body.end());
  return wav;
}

void append_id3_frame(Bytes& body, std::string_view id, const Bytes& payload, uint8_t major) {
  REQUIRE(id.size() == 4);
  body.insert(body.end(), id.begin(), id.end());
  if (major == 3) {
    append_u32_be(body, static_cast<uint32_t>(payload.size()));
  } else {
    append_syncsafe(body, payload.size());
  }
  body.push_back(0);
  body.push_back(0);
  body.insert(body.end(), payload.begin(), payload.end());
}

Bytes text_frame(std::string_view text, uint8_t encoding = 0) {
  Bytes payload{encoding};
  payload.insert(payload.end(), text.begin(), text.end());
  return payload;
}

Bytes make_mp3(const Bytes& body = {}, uint8_t major = 3, const Bytes& v1 = {},
               bool footer = false) {
  Bytes mp3;
  if (!body.empty()) {
    const auto flags = static_cast<uint8_t>(footer ? 0x10 : 0);
    mp3.insert(mp3.end(), {'I', 'D', '3', major, 0, flags});
    append_syncsafe(mp3, body.size());
    mp3.insert(mp3.end(), body.begin(), body.end());
    if (footer) {
      mp3.insert(mp3.end(), {'3', 'D', 'I', major, 0, flags});
      append_syncsafe(mp3, body.size());
    }
  }

  const size_t first_frame = mp3.size();
  mp3.resize(first_frame + 834, 0);
  for (size_t offset : {first_frame, first_frame + 417}) {
    mp3[offset] = 0xFF;
    mp3[offset + 1] = 0xFB;
    mp3[offset + 2] = 0x90;
    mp3[offset + 3] = 0x64;
  }
  mp3.insert(mp3.end(), v1.begin(), v1.end());
  return mp3;
}

Bytes make_id3v1(std::string_view title, std::string_view artist, std::string_view album,
                 std::string_view year, std::string_view comment, uint8_t track, uint8_t genre) {
  Bytes tag(128, 0);
  std::memcpy(tag.data(), "TAG", 3);
  const auto put = [&tag](size_t offset, size_t width, std::string_view text) {
    std::copy_n(text.begin(), std::min(width, text.size()), tag.begin() + offset);
  };
  put(3, 30, title);
  put(33, 30, artist);
  put(63, 30, album);
  put(93, 4, year);
  if (track != 0) {
    put(97, 28, comment);
    tag[125] = 0;
    tag[126] = track;
  } else {
    put(97, 30, comment);
  }
  tag[127] = genre;
  return tag;
}

}  // namespace

TEST_CASE("audio tag normalization and optional format sniffing", "[core][audio_tags]") {
  CHECK(sonare::wav_info_id_for_tag("title") == "INAM");
  CHECK(sonare::wav_info_id_for_tag("artist") == "IART");
  CHECK(sonare::wav_info_id_for_tag("albumartist").empty());
  CHECK(sonare::wav_info_id_for_tag("IENG").empty());

  const std::array<std::pair<Bytes, std::string_view>, 4> formats = {{
      {Bytes{'f', 'L', 'a', 'C'}, "flac"},
      {Bytes{'O', 'g', 'g', 'S'}, "ogg"},
      {Bytes{0, 0, 0, 16, 'f', 't', 'y', 'p'}, "m4a"},
      {Bytes{0xFF, 0xF1}, "aac"},
  }};
  for (const auto& [data, expected] : formats) {
    CHECK(sonare::sniff_optional_audio_format(data.data(), data.size()) == expected);
  }
  CHECK(sonare::sniff_optional_audio_format(nullptr, 12).empty());
  CHECK(sonare::sniff_optional_audio_format(nullptr, 0).empty());
}

TEST_CASE("WAV INFO tags normalize, decode text, and retain unknown IDs", "[core][audio_tags]") {
  const Bytes wav = make_wav({
      {"INAM", Bytes{'C', 'a', 'f', 'e', 0x80, 0, 'x'}},
      {"IART", bytes_from("A")},
      {"IART", bytes_from("B")},
      {"IPRD", {}},
      {"ICRD", bytes_from("2024")},
      {"IGNR", bytes_from("Rock")},
      {"ITRK", bytes_from("03")},
      {"ICMT", bytes_from("note")},
      {"IENG", bytes_from("producer")},
      {"iENG", bytes_from("lower")},
  });
  const sonare::AudioTags tags = sonare::read_audio_tags(wav.data(), wav.size());
  CHECK(tags.at("title") == "Cafe\xE2\x82\xAC");
  CHECK(tags.at("artist") == "A; B");
  CHECK(tags.count("album") == 1);
  CHECK(tags.at("album").empty());
  CHECK(tags.at("date") == "2024");
  CHECK(tags.at("genre") == "Rock");
  CHECK(tags.at("track") == "3");
  CHECK(tags.at("comment") == "note");
  CHECK(tags.at("IENG") == "producer");
  CHECK(tags.at("iENG") == "lower");

  Bytes malformed = wav;
  malformed[4] = 0xFF;
  malformed[5] = 0xFF;
  malformed[6] = 0xFF;
  malformed[7] = 0x7F;
  CHECK(sonare::read_audio_tags(malformed.data(), malformed.size()).empty());
}

TEST_CASE("ID3v2.3 values override ID3v1 and combine date frames", "[core][audio_tags]") {
  Bytes body;
  append_id3_frame(body, "TIT2", text_frame("V2 title"), 3);
  append_id3_frame(body, "TPE1", text_frame("V2 artist"), 3);
  append_id3_frame(body, "TALB", text_frame("V2 album"), 3);
  append_id3_frame(body, "TYER", text_frame("2024"), 3);
  append_id3_frame(body, "TDAT", text_frame("3112"), 3);
  append_id3_frame(body, "TRCK", text_frame("03"), 3);
  Bytes comment{0, 'e', 'n', 'g', 0};
  const Bytes comment_text = bytes_from("V2 comment");
  comment.insert(comment.end(), comment_text.begin(), comment_text.end());
  append_id3_frame(body, "COMM", comment, 3);
  Bytes user_text{3};
  const Bytes description = bytes_from("MOOD");
  user_text.insert(user_text.end(), description.begin(), description.end());
  user_text.push_back(0);
  const Bytes mood = bytes_from("calm");
  user_text.insert(user_text.end(), mood.begin(), mood.end());
  append_id3_frame(body, "TXXX", user_text, 3);

  const Bytes id3v1 = make_id3v1("V1 title", "V1 artist", "V1 album", "1999", "V1 note", 9, 17);
  const Bytes mp3 = make_mp3(body, 3, id3v1);
  const sonare::AudioTags tags = sonare::read_audio_tags(mp3.data(), mp3.size());
  CHECK(tags.at("title") == "V2 title");
  CHECK(tags.at("artist") == "V2 artist");
  CHECK(tags.at("album") == "V2 album");
  CHECK(tags.at("date") == "2024-12-31");
  CHECK(tags.at("track") == "3");
  CHECK(tags.at("comment") == "V2 comment");
  CHECK(tags.at("genre") == "Rock");
  CHECK(tags.at("TXXX:MOOD") == "calm");
}

TEST_CASE("ID3v2.4 UTF-8 and ID3v1 fallback", "[core][audio_tags]") {
  Bytes body;
  append_id3_frame(body, "TIT2", text_frame(std::string_view("A\xC3\0ignored", 10), 3), 4);
  append_id3_frame(body, "TCON", text_frame("Electronic"), 4);
  append_id3_frame(body, "TPUB", text_frame("Independent"), 4);
  const Bytes id3v1 = make_id3v1("fallback", "artist", "album", "2001", "comment", 5, 0);
  const Bytes mp3 = make_mp3(body, 4, id3v1, true);
  const sonare::AudioTags tags = sonare::read_audio_tags(mp3.data(), mp3.size());
  CHECK(tags.at("title") == "A\xEF\xBF\xBD");
  CHECK(tags.at("artist") == "artist");
  CHECK(tags.at("album") == "album");
  CHECK(tags.at("date") == "2001");
  CHECK(tags.at("genre") == "Electronic");
  CHECK(tags.at("track") == "5");
  CHECK(tags.at("comment") == "comment");
  CHECK(tags.at("TPUB") == "Independent");
}

TEST_CASE("malformed ID3 frames are skipped and tagless MP3 stays empty", "[core][audio_tags]") {
  Bytes malformed_body{'T', 'I', 'T', '2'};
  append_u32_be(malformed_body, 100);
  malformed_body.push_back(0);
  malformed_body.push_back(0);
  const Bytes id3v1 = make_id3v1("fallback", "", "", "", "", 0, 0xFF);
  const Bytes mp3 = make_mp3(malformed_body, 3, id3v1);
  CHECK(sonare::read_audio_tags(mp3.data(), mp3.size()).at("title") == "fallback");

  const Bytes tagless = make_mp3();
  CHECK(sonare::read_audio_tags(tagless.data(), tagless.size()).empty());
}

TEST_CASE("audio tag reading distinguishes unknown and unavailable formats", "[core][audio_tags]") {
  const Bytes unknown{'n', 'o', 'p', 'e'};
  try {
    (void)sonare::read_audio_tags(unknown.data(), unknown.size());
    FAIL("Unknown input was accepted");
  } catch (const sonare::SonareException& error) {
    CHECK(error.code() == sonare::ErrorCode::InvalidFormat);
  }
#ifndef SONARE_WITH_FFMPEG
  const Bytes flac{'f', 'L', 'a', 'C'};
  try {
    (void)sonare::read_audio_tags(flac.data(), flac.size());
    FAIL("Unavailable input was accepted");
  } catch (const sonare::SonareParameterRefusal& error) {
    CHECK(error.code() == sonare::ErrorCode::NotSupported);
    CHECK(error.reason() == sonare::ErrorReason::NotCompiledIn);
    CHECK(error.subject() == "data");
  }
#endif
}

TEST_CASE("long WAV INFO values are retained", "[core][audio_tags]") {
  const std::string value(65536, 'x');
  const Bytes wav = make_wav({{"INAM", Bytes(value.begin(), value.end())}});
  const sonare::AudioTags tags = sonare::read_audio_tags(wav.data(), wav.size());
  REQUIRE(tags.count("title") == 1);
  CHECK(tags.at("title").size() == value.size());
  CHECK(tags.at("title") == value);
}
