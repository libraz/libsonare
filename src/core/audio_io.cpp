#include "core/audio_io.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "core/audio_io_detail.h"

// File-path I/O is unavailable in WebAssembly builds; the core exposes only the
// buffer-based loaders there (see the load_buffer_* functions below).
#ifndef __EMSCRIPTEN__
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#endif

#include "mixing/downmix.h"
#include "util/exception.h"
#include "util/numeric_validation.h"
#include "util/resource_limits.h"
#include "util/splitmix64.h"

#ifdef SONARE_WITH_FFMPEG
#include "core/audio_io_ffmpeg.h"
#endif

// dr_wav implementation
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

// minimp3 implementation
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "minimp3_ex.h"

namespace sonare {

namespace {

constexpr int kMinSupportedChannels = 1;

/// @brief Widest plane count any ChannelLayout carries.
constexpr int kMaxLayoutPlanes = channel_count(ChannelLayout::SevenPointOne);

/// @brief True when a channel count maps onto a modelled speaker layout (1/2/6/8).
/// @details layout_from_channel_count() falls back to stereo for every other
///          count, so round-tripping the count through it is the membership test.
constexpr bool has_modelled_layout(int channels) noexcept {
  return channels == channel_count(layout_from_channel_count(channels));
}

/// @brief How a stream folds to mono, decided once from its channel count and speaker mask.
struct MonoFold {
  /// No modelled layout fits the planes, so the fold is their unweighted mean.
  bool mean = false;
  /// Source plane carrying each canonical plane of the modelled layout.
  std::array<int, kMaxLayoutPlanes> source_plane{};
};

/// @brief Plane carrying @p speaker under WAVE speaker mask @p mask, or -1 when absent.
int wave_mask_plane(uint32_t mask, WaveSpeaker speaker) noexcept {
  if (speaker == WaveSpeaker::None) return -1;
  const uint32_t bit = 1u << static_cast<int>(speaker);
  if ((mask & bit) == 0) return -1;
  int plane = 0;
  for (uint32_t below = mask & (bit - 1u); below != 0; below &= below - 1u) ++plane;
  return plane;
}

/// @brief Chooses the mono fold for @p channels planes carrying WAVE speaker mask @p mask.
/// @details Layouts the speaker model covers go through @ref sonare::mixing::downmix,
///          so the built-in decoders apply the same ITU-R BS.775 rule the rest of
///          the library uses: the center and surround feeds enter the front pair
///          at -3 dB and the LFE plane is dropped. A mask naming one position per
///          plane places the planes by role; a mask missing a role the layout needs
///          (6.0 has no LFE) leaves the planes unmodelled. A zero or inconsistent mask
///          assigns no positions, so the channel count picks the canonical layout.
///          Unmodelled planes (quad, LCR, 6.0, anything above 7.1) keep the unweighted
///          mean rather than losing a plane.
MonoFold mono_fold_for(int channels, uint32_t mask) {
  MonoFold fold;
  if (!has_modelled_layout(channels)) {
    fold.mean = true;
    return fold;
  }
  for (int plane = 0; plane < channels; ++plane)
    fold.source_plane[static_cast<size_t>(plane)] = plane;
  int positions = 0;
  for (uint32_t bits = mask; bits != 0; bits &= bits - 1u) ++positions;
  if (positions != channels) return fold;

  const SpeakerRole* roles = speaker_roles(layout_from_channel_count(channels));
  for (int plane = 0; plane < channels; ++plane) {
    const RoleSpeakers candidates = role_speakers(roles[plane]);
    int source = wave_mask_plane(mask, candidates.primary);
    if (source < 0) source = wave_mask_plane(mask, candidates.alternate);
    if (source < 0) {
      fold.mean = true;
      return fold;
    }
    fold.source_plane[static_cast<size_t>(plane)] = source;
  }
  return fold;
}

/// @brief Folds one interleaved decode chunk down to mono and appends it to @p mono.
/// @param interleaved Frame-interleaved chunk of @p n_frames * @p channels samples.
/// @param n_frames    Frames in the chunk.
/// @param channels    Source channel count (>= @ref kMinSupportedChannels).
/// @param fold        The stream's fold from @ref mono_fold_for.
/// @param planar      Caller-owned de-interleave scratch, reused across chunks.
/// @param mono        Destination; the folded frames are appended to it.
void append_mono_fold(const float* interleaved, size_t n_frames, int channels, const MonoFold& fold,
                      std::vector<float>& planar, std::vector<float>& mono) {
  const size_t base = mono.size();
  mono.resize(base + n_frames);
  float* out = mono.data() + base;

  if (fold.mean) {
    for (size_t frame = 0; frame < n_frames; ++frame) {
      double sum = 0.0;
      const size_t frame_base = frame * static_cast<size_t>(channels);
      for (int channel = 0; channel < channels; ++channel) {
        sum += interleaved[frame_base + static_cast<size_t>(channel)];
      }
      out[frame] = static_cast<float>(sum / static_cast<double>(channels));
    }
    return;
  }

  // downmix() reads planar input, so split the chunk into planes first.
  planar.resize(n_frames * static_cast<size_t>(channels));
  for (int channel = 0; channel < channels; ++channel) {
    float* plane = planar.data() + static_cast<size_t>(channel) * n_frames;
    for (size_t frame = 0; frame < n_frames; ++frame) {
      plane[frame] =
          interleaved[frame * static_cast<size_t>(channels) + static_cast<size_t>(channel)];
    }
  }
  std::array<const float*, kMaxLayoutPlanes> planes{};
  for (int plane = 0; plane < channels; ++plane) {
    const size_t source = static_cast<size_t>(fold.source_plane[static_cast<size_t>(plane)]);
    planes[static_cast<size_t>(plane)] = planar.data() + source * n_frames;
  }
  float* mono_plane[1] = {out};
  mixing::downmix(layout_from_channel_count(channels), ChannelLayout::Mono, planes.data(),
                  mono_plane, n_frames);
}

#ifndef SONARE_WITH_FFMPEG
#ifndef __EMSCRIPTEN__
/// @brief Extracts a lowercase file extension (including the leading dot) from a path.
/// @return The extension (e.g. ".m4a"), or an empty string if none is found.
/// @note Only used by the unsupported-format messages; with FFmpeg enabled, any
///       decoder error surfaces via @ref load_buffer_ffmpeg so the extension
///       hint is no longer needed.
std::string extract_extension(const std::string& path) {
  // Find the last '.' after the last path separator so directory dots are ignored.
  size_t sep = path.find_last_of("/\\");
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (sep != std::string::npos && dot < sep)) {
    return "";
  }
  std::string ext = path.substr(dot);
  for (char& c : ext) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return ext;
}
#endif  // !__EMSCRIPTEN__

/// @brief Builds an actionable "unsupported format" error message for buffer input.
std::string unsupported_buffer_message() {
  return "Unsupported audio format. Supported codecs: WAV, MP3. "
         "For M4A/AAC/FLAC/OGG, rebuild libsonare with -DSONARE_WITH_FFMPEG=ON, "
         "convert via 'ffmpeg -i input.<ext> output.wav', "
         "or pass float samples to Audio.from_buffer().";
}

#ifndef __EMSCRIPTEN__
/// @brief Builds an actionable "unsupported format" error message for file input.
/// @param path The path being loaded (used to extract and display the extension).
std::string unsupported_file_message(const std::string& path) {
  std::string ext = extract_extension(path);
  std::string ext_label = ext.empty() ? "(no extension)" : "'" + ext + "'";
  return "Unsupported audio format: " + ext_label +
         ". Supported: WAV, MP3. Rebuild with -DSONARE_WITH_FFMPEG=ON for "
         "M4A/AAC/FLAC/OGG, or convert via: ffmpeg -i \"" +
         path + "\" output.wav";
}
#endif  // !__EMSCRIPTEN__
#endif  // !SONARE_WITH_FFMPEG

/// @brief RAII guard for minimp3's streaming decoder state.
struct Mp3DecoderGuard {
  mp3dec_ex_t decoder{};
  bool opened = false;
  ~Mp3DecoderGuard() {
    if (opened) mp3dec_ex_close(&decoder);
  }
};

/// Owns an initialized `drwav` so the decoders can allocate inside their decode
/// loop. Every allocation there can throw, and dr_wav's own metadata block is
/// freed by drwav_uninit(), not by unwinding.
struct WavDecoderGuard {
  drwav wav{};
  bool opened = false;
  ~WavDecoderGuard() {
    if (opened) drwav_uninit(&wav);
  }
};

/// Upper bound on the interleaved samples a WAV's data chunk can actually
/// produce, derived from bytes that are really present.
///
/// A declared PCM frame count is a header field with nothing behind it: an
/// MS-ADPCM `fact` chunk can claim 500M frames over a kilobyte of real data
/// (dr_wav honours `fact` for that format tag), and the data chunk's own size
/// field is equally unbacked, so both are capped against the buffer the caller
/// actually handed us. Deliberately an over-estimate — 8 bits per byte over the
/// bits one sample occupies ignores per-block ADPCM headers — because its job
/// is to bound a reservation, not to predict the decode.
size_t decodable_sample_ceiling(const drwav& wav, size_t buffer_size) {
  if (wav.bitsPerSample == 0 || wav.channels == 0) return 0;
  const uint64_t available_bytes =
      std::min<uint64_t>(wav.dataChunkDataSize, static_cast<uint64_t>(buffer_size));
  const uint64_t bits = static_cast<uint64_t>(wav.bitsPerSample);
  const uint64_t samples = (available_bytes * 8u + bits - 1u) / bits;
  return static_cast<size_t>(
      std::min<uint64_t>(samples, static_cast<uint64_t>(resource::kMaxOfflineAudioSamples)));
}

#ifndef __EMSCRIPTEN__
#ifdef _WIN32
std::wstring utf8_to_wide_path(const std::string& path) {
  if (path.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                                        static_cast<int>(path.size()), nullptr, 0);
  SONARE_CHECK_MSG(count > 0, ErrorCode::InvalidParameter, "Path is not valid UTF-8: " + path);
  std::wstring wide(static_cast<size_t>(count), L'\0');
  const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                                          static_cast<int>(path.size()), wide.data(), count);
  SONARE_CHECK_MSG(written == count, ErrorCode::InvalidParameter,
                   "Failed to decode UTF-8 path: " + path);
  return wide;
}
#endif

/// @brief Reads entire file into memory.
/// @param path Path to the file
/// @param max_size Maximum allowed file size in bytes (0 = no limit)
std::vector<uint8_t> read_file(const std::string& path, size_t max_size = 0) {
#ifdef _WIN32
  std::ifstream file(utf8_to_wide_path(path), std::ios::binary);
#else
  std::ifstream file(path, std::ios::binary);
#endif
  SONARE_CHECK_MSG(file.is_open(), ErrorCode::FileNotFound, "Cannot open file: " + path);

  // The seek/tell probe only sizes the reserve: a non-regular input (a FIFO, a
  // character device) fails the seek and has no size to report at all, so a
  // negative result means unknown rather than enormous. Clearing on both sides
  // of the rewind keeps such an input readable from where it was opened.
  file.seekg(0, std::ios::end);
  const std::streamoff probed = file.tellg();
  if (max_size > 0 && probed > 0) {
    SONARE_CHECK_MSG(static_cast<uint64_t>(probed) <= static_cast<uint64_t>(max_size),
                     ErrorCode::InvalidParameter,
                     "File too large: " + std::to_string(probed) +
                         " bytes (max: " + std::to_string(max_size) + " bytes)");
  }
  file.clear();
  file.seekg(0, std::ios::beg);
  file.clear();

  // The probe is a snapshot the read cannot rely on, so the ceiling is enforced
  // on the bytes as they arrive and nothing past it is ever buffered.
  constexpr size_t kReadChunkBytes = 64u * 1024u;
  std::vector<uint8_t> buffer;
  if (probed > 0) buffer.reserve(static_cast<size_t>(probed));
  std::vector<char> chunk(kReadChunkBytes);
  while (file) {
    file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const auto read_bytes = static_cast<size_t>(file.gcount());
    if (read_bytes == 0) break;
    // Subtraction rather than addition: the sum of two size_t operands can wrap
    // where the remaining budget cannot.
    SONARE_CHECK_MSG(max_size == 0 || read_bytes <= max_size - buffer.size(),
                     ErrorCode::InvalidParameter,
                     "File too large: exceeds " + std::to_string(max_size) + " bytes");
    buffer.insert(buffer.end(), chunk.data(), chunk.data() + read_bytes);
  }
  SONARE_CHECK_MSG(!file.bad(), ErrorCode::DecodeFailed, "Failed to read file: " + path);

  return buffer;
}
#endif  // !__EMSCRIPTEN__

}  // namespace

namespace {

/// Bytes past any ID3v2 tags searched for the first MPEG frame when junk precedes it.
constexpr size_t kMpegSyncSearchBytes = 64u * 1024u;

/// Frames a header found past junk must chain into, as minimp3's own resync demands.
constexpr int kMpegJunkChainFrames = 2;

/// Bytes of an MPEG audio frame header.
constexpr size_t kMpegHeaderBytes = HDR_SIZE;

/// Most samples per channel one input byte decodes to outside free format: a
/// 24-byte MPEG-2 Layer III frame (8 kbps, 24 kHz) carries 576.
constexpr uint64_t kMaxMpegSamplesPerInputByte = 24;

/// Whether the bytes of @p tag (without its terminator) sit at data[offset].
/// Every container signature this file sniffs is matched here.
template <size_t N>
bool has_tag_at(const uint8_t* data, size_t size, size_t offset, const char (&tag)[N]) {
  constexpr size_t kTagBytes = N - 1;
  return offset <= size && size - offset >= kTagBytes &&
         std::memcmp(data + offset, tag, kTagBytes) == 0;
}

/// Whether @p data starts with an MPEG audio frame header minimp3 accepts. Raw
/// ADTS AAC shares the sync word but carries layer 0, so it does not pass.
bool is_mpeg_audio_frame_header(const uint8_t* data, size_t size) {
  return size >= kMpegHeaderBytes && hdr_valid(data) != 0;
}

/// Whether the frame at @p data is followed by kMpegJunkChainFrames frames of the
/// same stream; the buffer ending after at least one of them also counts.
bool mpeg_frame_chains(const uint8_t* data, size_t size) {
  size_t pos = 0;
  for (int matched = 0; matched < kMpegJunkChainFrames; ++matched) {
    // Free format leaves the length unknown, so it cannot be chained.
    const int frame_bytes = hdr_frame_bytes(data + pos, 0);
    if (frame_bytes <= 0) return false;
    pos += static_cast<size_t>(frame_bytes + hdr_padding(data + pos));
    if (pos > size || size - pos < kMpegHeaderBytes) return matched > 0;
    if (!hdr_compare(data, data + pos)) return false;
  }
  return true;
}

/// Whether the buffer carries an MPEG audio stream the built-in decoder resyncs
/// onto: ID3v2 tags and zero padding are skipped, a header at the stream start
/// is taken alone, and one further in must chain so a stray sync word in junk
/// or a foreign stream does not count. A buffer ending inside the tags stays
/// MP3 so minimp3 reports it.
bool contains_mpeg_audio_stream(const uint8_t* data, size_t size) {
  constexpr size_t kId3HeaderBytes = 10;
  size_t pos = 0;
  bool tagged = false;
  while (size - pos >= kId3HeaderBytes && has_tag_at(data, size, pos, "ID3")) {
    // Syncsafe 28-bit body size, plus a 10-byte footer when flag 0x10 is set.
    const size_t body = (static_cast<size_t>(data[pos + 6] & 0x7F) << 21) |
                        (static_cast<size_t>(data[pos + 7] & 0x7F) << 14) |
                        (static_cast<size_t>(data[pos + 8] & 0x7F) << 7) |
                        static_cast<size_t>(data[pos + 9] & 0x7F);
    const size_t footer = (data[pos + 5] & 0x10) != 0 ? kId3HeaderBytes : 0;
    if (body + footer > size - pos - kId3HeaderBytes) return true;
    pos += kId3HeaderBytes + body + footer;
    tagged = true;
  }
  while (pos < size && data[pos] == 0) ++pos;
  if (tagged && size - pos < kMpegHeaderBytes) return true;
  if (is_mpeg_audio_frame_header(data + pos, size - pos)) return true;

  const size_t end = size - pos > kMpegSyncSearchBytes ? pos + kMpegSyncSearchBytes : size;
  for (size_t at = pos + 1; at + kMpegHeaderBytes <= end; ++at) {
    if (is_mpeg_audio_frame_header(data + at, size - at) &&
        mpeg_frame_chains(data + at, size - at)) {
      return true;
    }
  }
  return false;
}

}  // namespace

AudioFormat detect_format(const uint8_t* data, size_t size) {
  if (size < 12) {
    return AudioFormat::Unknown;
  }

  // WAV: "RIFF....WAVE" at offset 0, where dr_wav expects it.
  if (has_tag_at(data, size, 0, "RIFF") && has_tag_at(data, size, 8, "WAVE")) {
    return AudioFormat::WAV;
  }

  // MP3: a decodable MPEG audio stream, wherever it starts. A sync word or tag
  // alone is not a codec claim: ADTS AAC and tagged FLAC/AAC carry them too and
  // belong to FFmpeg, as a RIFF carrying a foreign codec does.
  return contains_mpeg_audio_stream(data, size) ? AudioFormat::MP3 : AudioFormat::Unknown;
}

namespace {

#ifdef SONARE_WITH_FFMPEG
/// Whether the built-in WAV decoder can handle the codec a RIFF/WAVE declares.
///
/// detect_format() answers a container question — "is this RIFF/WAVE?" — and a
/// RIFF container can carry codecs dr_wav does not implement, MPEG Layer-3
/// (0x0055) being the one that occurs in the wild. Dispatching on the container
/// alone made the built-in sniffer's claim final: the file was handed to dr_wav,
/// dr_wav refused it, and the FFmpeg fallback that could have decoded it was
/// never reached because it only sat on the Unknown branch. So the codec tag,
/// not the container, decides which decoder gets the buffer.
///
/// Returns true when the fmt chunk is absent or unreadable, leaving dr_wav to
/// report its own error for a malformed file rather than routing it elsewhere.
bool built_in_wav_decoder_supports(const uint8_t* data, size_t size) {
  constexpr size_t kRiffHeaderBytes = 12;  // "RIFF" + size + "WAVE"
  constexpr size_t kChunkHeaderBytes = 8;  // id + size
  const auto read_u16 = [data](size_t offset) {
    return static_cast<uint16_t>(static_cast<uint16_t>(data[offset]) |
                                 static_cast<uint16_t>(data[offset + 1] << 8));
  };
  const auto read_u32 = [data](size_t offset) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(data[offset + i]) << (8 * i);
    return value;
  };

  size_t offset = kRiffHeaderBytes;
  while (offset + kChunkHeaderBytes <= size) {
    const uint32_t chunk_size = read_u32(offset + 4);
    const size_t body = offset + kChunkHeaderBytes;
    if (has_tag_at(data, size, offset, "fmt ")) {
      if (chunk_size < 2 || body + 2 > size) return true;
      uint16_t format_tag = read_u16(body);
      if (format_tag == DR_WAVE_FORMAT_EXTENSIBLE) {
        // The real codec lives in the SubFormat GUID's first two bytes.
        if (chunk_size < 26 || body + 26 > size) return true;
        format_tag = read_u16(body + 24);
      }
      switch (format_tag) {
        case DR_WAVE_FORMAT_PCM:
        case DR_WAVE_FORMAT_ADPCM:
        case DR_WAVE_FORMAT_IEEE_FLOAT:
        case DR_WAVE_FORMAT_ALAW:
        case DR_WAVE_FORMAT_MULAW:
        case DR_WAVE_FORMAT_DVI_ADPCM:
          return true;
        default:
          return false;
      }
    }
    // Odd bodies carry a pad byte; an empty chunk still advances by its header.
    const uint64_t advance = uint64_t{kChunkHeaderBytes} + chunk_size + (chunk_size & 1u);
    if (advance > size - offset) return true;
    offset += static_cast<size_t>(advance);
  }
  return true;
}
#endif  // SONARE_WITH_FFMPEG

template <typename Append>
void append_optional_retained(Append&& append, bool defer_errors, bool& enabled,
                              std::exception_ptr& retained_error,
                              std::vector<float>& retained_samples) {
  if (!enabled) return;
  if (!defer_errors) {
    append();
    return;
  }
  try {
    append();
  } catch (...) {
    retained_error = std::current_exception();
    retained_samples.clear();
    enabled = false;
  }
}

/// Returns capacity beyond twice the size of @p samples to the allocator.
void release_excess_capacity(std::vector<float>& samples) {
  if (samples.capacity() / 2 > samples.size()) samples.shrink_to_fit();
}

bool builtin_retains_channels(RetainChannels retention, int channels) {
  return retention == RetainChannels::All ||
         (retention == RetainChannels::StereoIfExact && channels == 2);
}

DecodedInputData decode_builtin_wav(const uint8_t* data, size_t size, RetainChannels retention,
                                    bool include_mono, bool defer_retained_errors) {
  WavDecoderGuard guard;
  guard.opened = drwav_init_memory(&guard.wav, data, size, nullptr) != 0;
  SONARE_CHECK_MSG(guard.opened, ErrorCode::DecodeFailed, "Failed to parse WAV data");
  drwav& wav = guard.wav;

  const int sample_rate = static_cast<int>(wav.sampleRate);
  const int channels = static_cast<int>(wav.channels);
  SONARE_CHECK_MSG(sample_rate > 0, ErrorCode::DecodeFailed, "Invalid WAV sample rate");
  SONARE_CHECK_MSG(channels >= kMinSupportedChannels, ErrorCode::DecodeFailed,
                   "Invalid WAV channel count");

  size_t total_samples = 0;
  SONARE_CHECK_MSG(numeric::checked_size_product(static_cast<size_t>(wav.totalPCMFrameCount),
                                                 static_cast<size_t>(wav.channels),
                                                 resource::kMaxOfflineAudioSamples, &total_samples),
                   ErrorCode::DecodeFailed,
                   "WAV declares more samples than the offline decode limit");
  constexpr size_t kDecodeChunkFrames = 16 * 1024;
  const size_t frame_capacity = static_cast<size_t>(wav.totalPCMFrameCount);
  const bool keep_channels = builtin_retains_channels(retention, channels);

  DecodedInputData result;
  result.sample_rate = sample_rate;
  result.source_channels = channels;
  result.retained_channels = keep_channels ? channels : 0;
  if (include_mono) {
    // Passing the offline limit is not the same as being backed by real data:
    // the limit still admits a declared 500M frames, which is a 2 GB
    // reservation for whatever bytes the file actually carries.
    result.mono.reserve(std::min(
        frame_capacity, decodable_sample_ceiling(wav, size) / static_cast<size_t>(channels)));
  }

  bool retain_enabled = keep_channels;
  if (retain_enabled) {
    append_optional_retained(
        [&] {
          result.interleaved.reserve(std::min(total_samples, decodable_sample_ceiling(wav, size)));
        },
        defer_retained_errors, retain_enabled, result.retained_error, result.interleaved);
  }

  size_t scratch_samples = 0;
  SONARE_CHECK_MSG(numeric::checked_size_product(
                       std::min(kDecodeChunkFrames, frame_capacity), static_cast<size_t>(channels),
                       resource::kMaxOfflineAudioSamples, &scratch_samples),
                   ErrorCode::DecodeFailed, "WAV channel layout exceeds the decode limit");
  std::vector<float> scratch(scratch_samples);
  std::vector<float> planar;
  const MonoFold fold = mono_fold_for(channels, wav.fmt.channelMask);
  drwav_uint64 total_frames_read = 0;
  while (total_frames_read < wav.totalPCMFrameCount) {
    const drwav_uint64 request =
        std::min<drwav_uint64>(kDecodeChunkFrames, wav.totalPCMFrameCount - total_frames_read);
    const drwav_uint64 frames_read = drwav_read_pcm_frames_f32(&wav, request, scratch.data());
    if (frames_read == 0) break;
    const size_t frame_count = static_cast<size_t>(frames_read);
    const size_t read_samples = frame_count * static_cast<size_t>(channels);
    if (retain_enabled) {
      append_optional_retained(
          [&] {
            size_t next_size = 0;
            SONARE_CHECK_MSG(
                numeric::checked_add(result.interleaved.size(), read_samples, &next_size) &&
                    next_size <= resource::kMaxOfflineAudioSamples,
                ErrorCode::DecodeFailed, "WAV decoded more samples than the offline decode limit");
            result.interleaved.insert(result.interleaved.end(), scratch.begin(),
                                      scratch.begin() + static_cast<std::ptrdiff_t>(read_samples));
          },
          defer_retained_errors, retain_enabled, result.retained_error, result.interleaved);
    }
    if (include_mono) {
      append_mono_fold(scratch.data(), frame_count, channels, fold, planar, result.mono);
    }
    total_frames_read += frames_read;
  }

  SONARE_CHECK_MSG(total_frames_read > 0, ErrorCode::DecodeFailed, "No audio frames in WAV data");
  if (!keep_channels) result.retained_channels = 0;
  return result;
}

DecodedInputData decode_builtin_mp3(const uint8_t* data, size_t size, RetainChannels retention,
                                    bool include_mono, bool defer_retained_errors) {
  // mp3dec_load_buf decodes the entire stream into one malloc before exposing
  // its sample count, so a low-bitrate, long-duration upload can OOM before we
  // get a chance to enforce kMaxOfflineAudioSamples. The streaming API scans
  // metadata/indexes first, then lets us enforce the bound for every decoded
  // chunk before appending PCM to the result.
  Mp3DecoderGuard decoder_guard;
  const int open_result = mp3dec_ex_open_buf(&decoder_guard.decoder, data, size, 0);
  SONARE_CHECK_MSG(open_result == 0, ErrorCode::DecodeFailed, "Failed to decode MP3 data");
  decoder_guard.opened = true;

  SONARE_CHECK_MSG(decoder_guard.decoder.samples > 0, ErrorCode::DecodeFailed,
                   "No audio samples in MP3 data");
  const int sample_rate = decoder_guard.decoder.info.hz;
  const int channels = decoder_guard.decoder.info.channels;
  SONARE_CHECK_MSG(sample_rate > 0, ErrorCode::DecodeFailed, "Invalid MP3 sample rate");
  SONARE_CHECK_MSG(channels >= kMinSupportedChannels, ErrorCode::DecodeFailed,
                   "Invalid MP3 channel count");
  const uint64_t declared_samples = decoder_guard.decoder.samples;
  SONARE_CHECK_MSG(declared_samples <= resource::kMaxOfflineAudioSamples, ErrorCode::DecodeFailed,
                   "MP3 declares more samples than the offline decode limit");

  constexpr size_t kMp3DecodeChunkSamples = 16 * 1024;
  const bool keep_channels = builtin_retains_channels(retention, channels);
  DecodedInputData result;
  result.sample_rate = sample_rate;
  result.source_channels = channels;
  result.retained_channels = keep_channels ? channels : 0;
  // A Xing/Info frame count is a header claim with nothing behind it, so the
  // reservation is also capped by the frames the input bytes can hold.
  const uint64_t reserve_frames =
      std::min(declared_samples / static_cast<uint64_t>(channels),
               static_cast<uint64_t>(size) * kMaxMpegSamplesPerInputByte);
  if (include_mono) {
    result.mono.reserve(static_cast<size_t>(reserve_frames));
  }
  bool retain_enabled = keep_channels;
  if (retain_enabled) {
    append_optional_retained(
        [&] {
          result.interleaved.reserve(
              static_cast<size_t>(reserve_frames * static_cast<uint64_t>(channels)));
        },
        defer_retained_errors, retain_enabled, result.retained_error, result.interleaved);
  }

  std::array<mp3d_sample_t, kMp3DecodeChunkSamples> chunk{};
  std::vector<float> normalized;
  std::vector<float> planar;
  // MP3 carries no speaker positions, so its channel count decides the fold.
  const MonoFold mp3_fold = mono_fold_for(channels, 0);
  size_t decoded_samples = 0;
  while (true) {
    const size_t read_samples = mp3dec_ex_read(&decoder_guard.decoder, chunk.data(), chunk.size());
    if (read_samples == 0) break;
    size_t next_total = 0;
    SONARE_CHECK_MSG(numeric::checked_add(decoded_samples, read_samples, &next_total) &&
                         next_total <= resource::kMaxOfflineAudioSamples,
                     ErrorCode::DecodeFailed,
                     "MP3 decoded more samples than the offline decode limit");
    SONARE_CHECK_MSG(read_samples % static_cast<size_t>(channels) == 0, ErrorCode::DecodeFailed,
                     "MP3 decoder returned an incomplete audio frame");
    decoded_samples = next_total;

    normalized.resize(read_samples);
    for (size_t i = 0; i < read_samples; ++i) {
      normalized[i] = static_cast<float>(chunk[i]) / 32768.0f;
    }
    if (retain_enabled) {
      append_optional_retained(
          [&] {
            result.interleaved.insert(result.interleaved.end(), normalized.begin(),
                                      normalized.end());
          },
          defer_retained_errors, retain_enabled, result.retained_error, result.interleaved);
    }
    if (include_mono) {
      append_mono_fold(normalized.data(), read_samples / static_cast<size_t>(channels), channels,
                       mp3_fold, planar, result.mono);
    }
  }
  SONARE_CHECK_MSG(!include_mono || !result.mono.empty(), ErrorCode::DecodeFailed,
                   "No audio samples in MP3 data");
  SONARE_CHECK_MSG(include_mono || !result.interleaved.empty(), ErrorCode::DecodeFailed,
                   "No audio samples in MP3 data");
  // Only an overstated frame count leaves the reservation well above the decoded size.
  release_excess_capacity(result.mono);
  if (retain_enabled) {
    append_optional_retained([&] { release_excess_capacity(result.interleaved); },
                             defer_retained_errors, retain_enabled, result.retained_error,
                             result.interleaved);
  }
  if (!keep_channels) result.retained_channels = 0;
  return result;
}

DecodedInputData decode_builtin_input(const uint8_t* data, size_t size, AudioFormat format,
                                      RetainChannels retention, bool include_mono,
                                      bool defer_retained_errors) {
  if (format == AudioFormat::WAV) {
    return decode_builtin_wav(data, size, retention, include_mono, defer_retained_errors);
  }
  SONARE_CHECK_MSG(format == AudioFormat::MP3, ErrorCode::InvalidFormat,
                   "Built-in input loader received an unsupported format");
  return decode_builtin_mp3(data, size, retention, include_mono, defer_retained_errors);
}

InterleavedAudioLoadResult load_buffer_wav_interleaved(const uint8_t* data, size_t size) {
  auto result =
      decode_builtin_input(data, size, AudioFormat::WAV, RetainChannels::All, false, false);
  return {std::move(result.interleaved), result.sample_rate, result.retained_channels};
}

InterleavedAudioLoadResult load_buffer_mp3_interleaved(const uint8_t* data, size_t size) {
  auto result =
      decode_builtin_input(data, size, AudioFormat::MP3, RetainChannels::All, false, false);
  return {std::move(result.interleaved), result.sample_rate, result.retained_channels};
}

// @p max_bytes is the ceiling the caller is under, with 0 meaning no limit, so
// the path loader hands its own AudioLoadOptions::max_file_size down rather than
// meeting a second fixed one it cannot raise.
InterleavedAudioLoadResult load_buffer_interleaved_bounded(const uint8_t* data, size_t size,
                                                           size_t max_bytes) {
  SONARE_CHECK_MSG(max_bytes == 0 || size <= max_bytes, ErrorCode::InvalidParameter,
                   "Audio buffer too large: " + std::to_string(size) +
                       " bytes (max: " + std::to_string(max_bytes) + ")");
  switch (detect_format(data, size)) {
    case AudioFormat::WAV:
#ifdef SONARE_WITH_FFMPEG
      if (!built_in_wav_decoder_supports(data, size)) {
        return load_buffer_ffmpeg_interleaved(data, size);
      }
#endif
      return load_buffer_wav_interleaved(data, size);
    case AudioFormat::MP3:
      return load_buffer_mp3_interleaved(data, size);
    default:
#ifdef SONARE_WITH_FFMPEG
      return load_buffer_ffmpeg_interleaved(data, size);
#else
      SONARE_CHECK_MSG(false, ErrorCode::InvalidFormat,
                       "Interleaved audio loading supports WAV and MP3 only");
#endif
  }
}

}  // namespace

AudioLoadResult load_buffer_wav(const uint8_t* data, size_t size) {
  auto result =
      decode_builtin_input(data, size, AudioFormat::WAV, RetainChannels::None, true, false);
  return {std::move(result.mono), result.sample_rate};
}

AudioLoadResult load_buffer_mp3(const uint8_t* data, size_t size) {
  auto result =
      decode_builtin_input(data, size, AudioFormat::MP3, RetainChannels::None, true, false);
  return {std::move(result.mono), result.sample_rate};
}

InterleavedAudioLoadResult load_buffer_interleaved(const uint8_t* data, size_t size) {
  return load_buffer_interleaved_bounded(data, size, resource::kMaxAudioFileBytes);
}

#ifndef __EMSCRIPTEN__
InterleavedAudioLoadResult load_audio_interleaved(const std::string& path,
                                                  const AudioLoadOptions& options) {
  const std::vector<uint8_t> data = read_file(path, options.max_file_size);
  return load_buffer_interleaved_bounded(data.data(), data.size(), options.max_file_size);
}

DecodedInputData load_audio_input_once(const std::string& path, RetainChannels retention,
                                       const AudioLoadOptions& options) {
  const std::vector<uint8_t> data = read_file(path, options.max_file_size);
  const AudioFormat format = detect_format(data.data(), data.size());
  switch (format) {
    case AudioFormat::WAV:
#ifdef SONARE_WITH_FFMPEG
      if (!built_in_wav_decoder_supports(data.data(), data.size())) {
        return load_buffer_ffmpeg_input(data.data(), data.size(), retention);
      }
#endif
      break;
    case AudioFormat::MP3:
      break;
    case AudioFormat::Unknown:
#ifdef SONARE_WITH_FFMPEG
      return load_buffer_ffmpeg_input(data.data(), data.size(), retention);
#else
      SONARE_CHECK_MSG(false, ErrorCode::InvalidFormat, unsupported_file_message(path));
#endif
  }
  // Built-in WAV/MP3 decode reuses the bytes read above for every representation.
  if (format == AudioFormat::WAV || format == AudioFormat::MP3) {
    // Retained-only failures are deferred so the mono result still reaches the handler.
    return decode_builtin_input(data.data(), data.size(), format, retention, true, true);
  }
#ifdef SONARE_WITH_FFMPEG
  SONARE_CHECK_MSG(false, ErrorCode::InvalidFormat, "Unsupported audio format");
#else
  SONARE_CHECK_MSG(false, ErrorCode::InvalidFormat, unsupported_file_message(path));
#endif
}

AudioLoadResult load_wav(const std::string& path) {
  std::vector<uint8_t> data = read_file(path, kDefaultLoadOptions.max_file_size);
  return load_buffer_wav(data.data(), data.size());
}

AudioLoadResult load_mp3(const std::string& path) {
  std::vector<uint8_t> data = read_file(path, kDefaultLoadOptions.max_file_size);
  return load_buffer_mp3(data.data(), data.size());
}
#endif  // !__EMSCRIPTEN__

namespace {

// Shared body of load_buffer(). @p max_bytes is the ceiling the caller is under,
// with 0 meaning no limit, so a path loader can hand its own
// AudioLoadOptions::max_file_size down instead of hitting a second, fixed one it
// cannot raise.
AudioLoadResult load_buffer_bounded(const uint8_t* data, size_t size, size_t max_bytes) {
  SONARE_CHECK_MSG(max_bytes == 0 || size <= max_bytes, ErrorCode::InvalidParameter,
                   "Audio buffer too large: " + std::to_string(size) +
                       " bytes (max: " + std::to_string(max_bytes) + " bytes)");
  AudioFormat format = detect_format(data, size);

  switch (format) {
    case AudioFormat::WAV:
#ifdef SONARE_WITH_FFMPEG
      // A RIFF container is not a codec claim: hand a WAV carrying something
      // dr_wav cannot decode to FFmpeg instead of failing on it.
      if (!built_in_wav_decoder_supports(data, size)) {
        return load_buffer_ffmpeg(data, size);
      }
#endif
      return load_buffer_wav(data, size);
    case AudioFormat::MP3:
      return load_buffer_mp3(data, size);
    default:
#ifdef SONARE_WITH_FFMPEG
      return load_buffer_ffmpeg(data, size);
#else
      SONARE_CHECK_MSG(false, ErrorCode::InvalidFormat, unsupported_buffer_message());
#endif
  }
}

}  // namespace

AudioLoadResult load_buffer(const uint8_t* data, size_t size) {
  // The buffer/memory path takes no options and is the entry most exposed to
  // untrusted input, so it keeps the library ceiling: reject an oversized blob
  // before any decoder can start expanding it. A caller that needs a different
  // ceiling goes through the path loaders, which carry AudioLoadOptions.
  return load_buffer_bounded(data, size, resource::kMaxAudioFileBytes);
}

#ifndef __EMSCRIPTEN__
AudioLoadResult load_audio(const std::string& path, const AudioLoadOptions& options) {
  std::vector<uint8_t> data = read_file(path, options.max_file_size);
  AudioFormat format = detect_format(data.data(), data.size());
  if (format == AudioFormat::Unknown) {
#ifdef SONARE_WITH_FFMPEG
    // Defer to FFmpeg, which handles a far wider set of containers and codecs
    // (M4A/AAC/FLAC/OGG/Opus/WMA/...) than the built-in WAV/MP3 sniffers.
    return load_buffer_ffmpeg(data.data(), data.size());
#else
    SONARE_CHECK_MSG(false, ErrorCode::InvalidFormat, unsupported_file_message(path));
#endif
  }
  // read_file already applied options.max_file_size, which is the only ceiling
  // this path has: passing it down instead of the library default is what makes
  // max_file_size = 0 mean "no limit" all the way through, as it is documented
  // to, rather than failing at the fixed buffer ceiling after the whole file has
  // been read into memory.
  return load_buffer_bounded(data.data(), data.size(), options.max_file_size);
}

int audio_channel_count(const std::string& path, const AudioLoadOptions& options) {
  const std::vector<uint8_t> data = read_file(path, options.max_file_size);
  switch (detect_format(data.data(), data.size())) {
    case AudioFormat::WAV: {
#ifdef SONARE_WITH_FFMPEG
      // Same dispatch rule as load_buffer(): a codec dr_wav cannot open must not
      // report a different channel count here than the loader will decode.
      if (!built_in_wav_decoder_supports(data.data(), data.size())) {
        return probe_channels_ffmpeg(data.data(), data.size());
      }
#endif
      drwav wav;
      SONARE_CHECK_MSG(drwav_init_memory(&wav, data.data(), data.size(), nullptr),
                       ErrorCode::DecodeFailed, "Failed to parse WAV data");
      const int channels = static_cast<int>(wav.channels);
      drwav_uninit(&wav);
      return channels;
    }
    case AudioFormat::MP3: {
      Mp3DecoderGuard decoder_guard;
      SONARE_CHECK_MSG(mp3dec_ex_open_buf(&decoder_guard.decoder, data.data(), data.size(), 0) == 0,
                       ErrorCode::DecodeFailed, "Failed to decode MP3 data");
      decoder_guard.opened = true;
      return decoder_guard.decoder.info.channels;
    }
    case AudioFormat::Unknown:
#ifdef SONARE_WITH_FFMPEG
      return probe_channels_ffmpeg(data.data(), data.size());
#else
      return 0;
#endif
  }
  return 0;
}

namespace {

/// Seed of channel 0's PCM-writer dither stream; channel c's seed is this XOR
/// c * kSplitMix64Gamma. Mirrored by the Python CLI writer (_cli_common.py).
constexpr uint64_t kPcmDitherSeed = 0x5043'4D44'4954'4852ull;

/// TPDF dither, in LSBs, for one sample: the difference of two 24-bit uniforms
/// drawn at (2 * frame, 2 * frame + 1) of the channel's SplitMix64 stream. Both
/// uniforms and their difference are exact in float, so every writer that
/// follows this recipe produces the same codes.
float pcm_tpdf_lsb(uint64_t channel_seed, uint64_t frame) noexcept {
  constexpr float kUnit = 1.0f / 16777216.0f;
  const float a = static_cast<float>(splitmix64_at(channel_seed, 2 * frame) >> 40) * kUnit;
  const float b = static_cast<float>(splitmix64_at(channel_seed, 2 * frame + 1) >> 40) * kUnit;
  return a - b;
}

/// Quantizes a normalized float sample onto a signed PCM grid of @p scale codes
/// per unit, with @p dither_lsb added before the nearest-code rounding
/// (round-half-away-from-zero, @c std::lroundf, as libsndfile and other
/// reference encoders round). The scale is a power of two, so the scaled sample
/// is exact and the only rounding is the final one. The input is clamped to
/// [-1, 1] and the code to [min_code, max_code], so +1.0 lands on the positive
/// ceiling.
///
/// An exact zero is digital silence and stays code 0, undithered, so a muted
/// passage is not filled with hiss. A non-finite sample has no PCM image, so it
/// is written as digital silence and counted in @p non_finite. The clamp cannot stand in for that
/// check: a comparison against a non-finite value is false, so std::min returns its other argument
/// and the sample would reach the file as positive full scale, indistinguishable from a peak the
/// encoder meant to produce.
long quantize_pcm(float sample, float scale, long min_code, long max_code, float dither_lsb,
                  size_t& non_finite) {
  if (!std::isfinite(sample)) {
    ++non_finite;
    return 0;
  }
  if (sample == 0.0f) return 0;
  const float clamped = std::max(-1.0f, std::min(1.0f, sample));
  const long v = std::lroundf(clamped * scale + dither_lsb);
  return std::max(min_code, std::min(max_code, v));
}

/// True when every finite sample already sits on the PCM grid of @p scale, i.e.
/// the stream was decoded from, or finalized to, this word length. Such a
/// stream is written back code for code; dither is for requantization only.
bool on_pcm_grid(const float* samples, size_t n_samples, float scale) noexcept {
  for (size_t i = 0; i < n_samples; ++i) {
    if (!std::isfinite(samples[i])) continue;
    const float scaled = std::max(-1.0f, std::min(1.0f, samples[i])) * scale;
    if (scaled != std::floor(scaled)) return false;
  }
  return true;
}

#ifndef __EMSCRIPTEN__
// Per-writer-unique sibling temp-file path for an atomic write: content is
// written here first and only renamed over the destination once complete, so an
// interrupted or failed write can never truncate an existing file. The process
// id plus a monotonic counter keep concurrent writers to the same destination on
// distinct temp files; a fixed name would let two writers open and interleave
// into the same temp before either renames, leaving a corrupt result. The same
// scheme is duplicated in tools/cli/sonare_cli_common.cpp (the CLI reaches this
// only through the C ABI and cannot share this internal helper).
std::string atomic_tmp_path(const std::string& path) {
  static std::atomic<uint64_t> counter{0};
#ifdef _WIN32
  const unsigned long pid = ::GetCurrentProcessId();
#else
  const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
  const uint64_t seq = counter.fetch_add(1, std::memory_order_relaxed);
  return path + ".sonare-tmp." + std::to_string(pid) + "." + std::to_string(seq);
}

// Removes the temp file unless the write committed, so a failed write leaves the
// destination untouched and no stray temp file behind.
struct TmpFileGuard {
  std::string tmp;
  bool committed = false;
  ~TmpFileGuard() {
    if (!committed) std::remove(tmp.c_str());
  }
};

// RIFF stores both the data chunk and the enclosing file size in uint32. Every
// WAV writer applies this before it packs or allocates anything: past the bound
// the header is truncated to a value that reads as a corrupt short file, and
// there is no error at write time to notice it by. The bound is the EXTENSIBLE
// header's, the larger of the two the writers emit, so the mono/stereo path
// enforces the same effective length whichever entry point produced it -- the
// two are documented as bit-identical, and a limit that differed between them
// would make that true of the bytes and false of the acceptable inputs.
void check_riff_size_fits(size_t n_frames, int channel_count, int bits_per_sample) {
  const size_t bytes_per_frame =
      static_cast<size_t>(channel_count) * static_cast<size_t>(bits_per_sample / 8);
  constexpr size_t kExtensibleHeaderBytes = 68;
  const size_t max_data_bytes =
      static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - (kExtensibleHeaderBytes - 8);
  SONARE_CHECK_MSG(bytes_per_frame > 0 && n_frames <= max_data_bytes / bytes_per_frame,
                   ErrorCode::InvalidParameter,
                   "WAV data exceeds RIFF 32-bit size limit; use a chunked container");
}
#endif  // !__EMSCRIPTEN__

}  // namespace

namespace {

/// Packs interleaved normalized float samples into the on-disk PCM byte stream:
/// 16-bit little-endian int16 or 24-bit tightly-packed 3-byte little-endian.
/// A stream not already on the target grid is TPDF-dithered per channel before
/// rounding, so a decaying tail fades into noise at the last bits instead of
/// turning into truncation distortion and then hard silence. Non-finite samples
/// are counted into @p non_finite by the quantizer.
std::vector<uint8_t> pack_pcm_bytes(const float* samples, size_t n_samples, int channel_count,
                                    int bits_per_sample, size_t& non_finite) {
  const bool sixteen = bits_per_sample == 16;
  const float scale = sixteen ? 32768.0f : 8388608.0f;
  const long min_code = sixteen ? -32768 : -8388608;
  const long max_code = sixteen ? 32767 : 8388607;
  const bool dither = !on_pcm_grid(samples, n_samples, scale);
  const auto channels = static_cast<size_t>(channel_count);
  const size_t width = static_cast<size_t>(bits_per_sample / 8);
  std::vector<uint8_t> bytes(n_samples * width);
  for (size_t i = 0; i < n_samples; ++i) {
    const uint64_t channel_seed = kPcmDitherSeed ^ (uint64_t{i % channels} * kSplitMix64Gamma);
    const float d = dither ? pcm_tpdf_lsb(channel_seed, i / channels) : 0.0f;
    const long v = quantize_pcm(samples[i], scale, min_code, max_code, d, non_finite);
    for (size_t b = 0; b < width; ++b) {
      bytes[i * width + b] =
          static_cast<uint8_t>((static_cast<unsigned long>(v) >> (8 * b)) & 0xFF);
    }
  }
  return bytes;
}

void append_le16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void append_le32(std::vector<uint8_t>& out, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
  }
}

void append_tag(std::vector<uint8_t>& out, const char (&tag)[5]) {
  out.insert(out.end(), tag, tag + 4);
}

struct FileCloser {
  void operator()(std::FILE* file) const { std::fclose(file); }
};

/// Writes a complete WAV image to a sibling temp file and renames it over @p path
/// only after the write, flush, fsync and close have all succeeded, so a failure
/// at any stage leaves an existing destination untouched. The image is fully
/// built before the file is opened, so nothing between open and close allocates.
void write_wav_image_atomic(const std::string& path, const std::vector<uint8_t>& image) {
  const std::string tmp = atomic_tmp_path(path);
  TmpFileGuard guard{tmp};
  std::unique_ptr<std::FILE, FileCloser> file(std::fopen(tmp.c_str(), "wb"));
  SONARE_CHECK_MSG(file != nullptr, ErrorCode::EncodeFailed, "Failed to create WAV file: " + path);
  bool ok = std::fwrite(image.data(), 1, image.size(), file.get()) == image.size() &&
            std::fflush(file.get()) == 0;
#ifndef _WIN32
  ok = ok && ::fsync(fileno(file.get())) == 0;
#endif
  // fclose can report a deferred write error of its own, so it is checked too.
  ok = std::fclose(file.release()) == 0 && ok;
  SONARE_CHECK_MSG(ok, ErrorCode::EncodeFailed, "Failed to write WAV file: " + path);
  // The rename is the last stage of writing the output, so it carries the same
  // error class as every earlier one. A destination that resolves to a
  // directory fails here and nowhere else, and reporting it as a decode failure
  // told the caller their input could not be read.
  SONARE_CHECK_MSG(std::rename(tmp.c_str(), path.c_str()) == 0, ErrorCode::EncodeFailed,
                   "Failed to finalize file: " + path);
  guard.committed = true;
}

/// Builds a plain WAVE_FORMAT_PCM file image (mono/stereo): the canonical 44-byte
/// header, the samples, and the RIFF pad byte after an odd-sized data chunk.
std::vector<uint8_t> encode_pcm_wav(const float* interleaved, size_t n_frames, int channel_count,
                                    int sample_rate, int bits_per_sample,
                                    size_t* non_finite_samples) {
  size_t non_finite = 0;
  const std::vector<uint8_t> pcm =
      pack_pcm_bytes(interleaved, n_frames * static_cast<size_t>(channel_count), channel_count,
                     bits_per_sample, non_finite);
  if (non_finite_samples != nullptr) *non_finite_samples = non_finite;

  const auto data_size = static_cast<uint32_t>(pcm.size());
  const uint32_t pad = data_size % 2;
  const auto block_align = static_cast<uint16_t>(channel_count * (bits_per_sample / 8));
  const uint32_t fmt_size = 16;

  std::vector<uint8_t> image;
  image.reserve(44 + pcm.size() + pad);
  append_tag(image, "RIFF");
  append_le32(image, 4 + (8 + fmt_size) + (8 + data_size + pad));
  append_tag(image, "WAVE");
  append_tag(image, "fmt ");
  append_le32(image, fmt_size);
  append_le16(image, 1);  // WAVE_FORMAT_PCM
  append_le16(image, static_cast<uint16_t>(channel_count));
  append_le32(image, static_cast<uint32_t>(sample_rate));
  append_le32(image, static_cast<uint32_t>(sample_rate) * block_align);
  append_le16(image, block_align);
  append_le16(image, static_cast<uint16_t>(bits_per_sample));
  append_tag(image, "data");
  append_le32(image, data_size);
  image.insert(image.end(), pcm.begin(), pcm.end());
  if (pad != 0) image.push_back(0);
  return image;
}

/// Builds a WAVE_FORMAT_EXTENSIBLE PCM file image. The pinned dr_wav (v0.14.4)
/// has no channelMask field and rejects EXTENSIBLE on write, so surround beds
/// must be serialized directly.
std::vector<uint8_t> encode_extensible_wav(const float* interleaved, size_t n_frames,
                                           int channel_count, uint32_t channel_mask,
                                           int sample_rate, int bits_per_sample,
                                           size_t* non_finite_samples) {
  const int bytes_per_sample = bits_per_sample / 8;
  const size_t n_samples = n_frames * static_cast<size_t>(channel_count);
  size_t non_finite = 0;
  const std::vector<uint8_t> pcm =
      pack_pcm_bytes(interleaved, n_samples, channel_count, bits_per_sample, non_finite);
  if (non_finite_samples != nullptr) *non_finite_samples = non_finite;

  const auto data_size = static_cast<uint32_t>(pcm.size());
  const auto block_align = static_cast<uint16_t>(channel_count * bytes_per_sample);
  const auto byte_rate = static_cast<uint32_t>(sample_rate) * block_align;
  const uint32_t fmt_size = 40;  // 16 base + 2 (cbSize) + 22 (extension)
  const uint32_t riff_size = 4 + (8 + fmt_size) + (8 + data_size);

  // KSDATAFORMAT_SUBTYPE_PCM = {00000001-0000-0010-8000-00AA00389B71}, stored
  // as little-endian Data1/2/3 followed by the 8 Data4 bytes verbatim.
  static const uint8_t kPcmSubFormat[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                            0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

  std::vector<uint8_t> image;
  image.reserve(68 + pcm.size());
  append_tag(image, "RIFF");
  append_le32(image, riff_size);
  append_tag(image, "WAVE");

  append_tag(image, "fmt ");
  append_le32(image, fmt_size);
  append_le16(image, 0xFFFE);  // WAVE_FORMAT_EXTENSIBLE
  append_le16(image, static_cast<uint16_t>(channel_count));
  append_le32(image, static_cast<uint32_t>(sample_rate));
  append_le32(image, byte_rate);
  append_le16(image, block_align);
  append_le16(image, static_cast<uint16_t>(bits_per_sample));
  append_le16(image, 22);                                      // cbSize
  append_le16(image, static_cast<uint16_t>(bits_per_sample));  // wValidBitsPerSample
  append_le32(image, channel_mask);
  image.insert(image.end(), kPcmSubFormat, kPcmSubFormat + 16);

  append_tag(image, "data");
  append_le32(image, data_size);
  image.insert(image.end(), pcm.begin(), pcm.end());
  return image;
}

}  // namespace

void save_wav(const std::string& path, const float* samples, size_t n_samples, int sample_rate,
              int bits_per_sample, size_t* non_finite_samples) {
  SONARE_CHECK_MSG(samples != nullptr, ErrorCode::InvalidParameter, "Samples pointer is null");
  SONARE_CHECK_MSG(n_samples > 0, ErrorCode::InvalidParameter, "No samples to save");
  SONARE_CHECK_MSG(sample_rate > 0, ErrorCode::InvalidParameter, "Invalid sample rate");
  SONARE_CHECK_MSG(bits_per_sample == 16 || bits_per_sample == 24, ErrorCode::InvalidParameter,
                   "bits_per_sample must be 16 or 24");
  check_riff_size_fits(n_samples, 1, bits_per_sample);
  write_wav_image_atomic(path, encode_pcm_wav(samples, n_samples, 1, sample_rate, bits_per_sample,
                                              non_finite_samples));
}

void save_wav(const std::string& path, const std::vector<float>& samples, int sample_rate,
              int bits_per_sample, size_t* non_finite_samples) {
  save_wav(path, samples.data(), samples.size(), sample_rate, bits_per_sample, non_finite_samples);
}

void save_wav_multichannel(const std::string& path, const float* interleaved, size_t n_frames,
                           int channel_count, ChannelLayout layout, int sample_rate,
                           int bits_per_sample, size_t* non_finite_samples) {
  SONARE_CHECK_MSG(interleaved != nullptr, ErrorCode::InvalidParameter, "Samples pointer is null");
  SONARE_CHECK_MSG(n_frames > 0, ErrorCode::InvalidParameter, "No frames to save");
  SONARE_CHECK_MSG(sample_rate > 0, ErrorCode::InvalidParameter, "Invalid sample rate");
  SONARE_CHECK_MSG(bits_per_sample == 16 || bits_per_sample == 24, ErrorCode::InvalidParameter,
                   "bits_per_sample must be 16 or 24");
  SONARE_CHECK_MSG(channel_count == sonare::channel_count(layout), ErrorCode::InvalidParameter,
                   "channel_count does not match the layout");

  check_riff_size_fits(n_frames, channel_count, bits_per_sample);

  // Plain WAVE_FORMAT_PCM for mono/stereo (maximum compatibility, and
  // bit-identical to the mono writer); EXTENSIBLE with a channel mask above that.
  write_wav_image_atomic(
      path, channel_count <= 2 ? encode_pcm_wav(interleaved, n_frames, channel_count, sample_rate,
                                                bits_per_sample, non_finite_samples)
                               : encode_extensible_wav(interleaved, n_frames, channel_count,
                                                       wave_channel_mask(layout), sample_rate,
                                                       bits_per_sample, non_finite_samples));
}
#endif  // !__EMSCRIPTEN__

}  // namespace sonare
