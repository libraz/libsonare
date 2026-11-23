#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace sonare {

using AudioTags = std::map<std::string, std::string>;

struct AudioTagNormalization {
  std::string_view canonical;
  std::array<std::string_view, 6> source_keys;
  std::string_view wav_info_id;
};

inline constexpr std::array<AudioTagNormalization, 10> kAudioTagNormalizations = {{
    {"title", {"title", "TIT2", "TITLE", "INAM", "©nam", ""}, "INAM"},
    {"artist", {"artist", "TPE1", "ARTIST", "IART", "©ART", ""}, "IART"},
    {"album", {"album", "TALB", "ALBUM", "IPRD", "©alb", ""}, "IPRD"},
    {"albumartist", {"albumartist", "TPE2", "ALBUMARTIST", "aART", "album_artist", ""}, ""},
    {"date", {"date", "TDRC", "TYER", "DATE", "ICRD", "©day"}, "ICRD"},
    {"genre", {"genre", "TCON", "GENRE", "IGNR", "©gen", ""}, "IGNR"},
    {"track", {"track", "TRCK", "TRACKNUMBER", "ITRK", "trkn", ""}, "ITRK"},
    {"disc", {"disc", "TPOS", "DISCNUMBER", "disk", "", ""}, ""},
    {"comment", {"comment", "COMM", "COMMENT", "ICMT", "©cmt", ""}, "ICMT"},
    {"composer", {"composer", "TCOM", "COMPOSER", "©wrt", "", ""}, ""},
}};

AudioTags read_audio_tags(const uint8_t* data, size_t size);
std::string_view wav_info_id_for_tag(std::string_view tag) noexcept;
std::string_view sniff_optional_audio_format(const uint8_t* data, size_t size) noexcept;

}  // namespace sonare
