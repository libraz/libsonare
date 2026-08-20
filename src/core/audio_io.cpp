#include "core/audio_io.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// File-path I/O is unavailable in WebAssembly builds; the core exposes only the
// buffer-based loaders there (see the load_buffer_* functions below).
#ifndef __EMSCRIPTEN__
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#endif

#include "mixing/downmix.h"
#include "util/exception.h"
#include "util/numeric_validation.h"
#include "util/resource_limits.h"

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

/// @brief Folds one interleaved decode chunk down to mono and appends it to @p mono.
/// @details Layouts the speaker model covers go through @ref sonare::mixing::downmix,
///          so the built-in decoders apply the same ITU-R BS.775 rule the rest of
///          the library uses: the center and surround feeds enter the front pair
///          at -3 dB and the LFE plane is dropped. Channel counts outside the
///          model (quad, LCR, and anything above 7.1) carry no speaker assignment
///          the matrix can reason about, so they keep the unweighted mean rather
///          than losing their unmapped planes.
/// @param interleaved Frame-interleaved chunk of @p n_frames * @p channels samples.
/// @param n_frames    Frames in the chunk.
/// @param channels    Source channel count (>= @ref kMinSupportedChannels).
/// @param planar      Caller-owned de-interleave scratch, reused across chunks.
/// @param mono        Destination; the folded frames are appended to it.
void append_mono_fold(const float* interleaved, size_t n_frames, int channels,
                      std::vector<float>& planar, std::vector<float>& mono) {
  const size_t base = mono.size();
  mono.resize(base + n_frames);
  float* out = mono.data() + base;

  if (!has_modelled_layout(channels)) {
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
  std::array<const float*, kMaxLayoutPlanes> planes{};
  for (int channel = 0; channel < channels; ++channel) {
    float* plane = planar.data() + static_cast<size_t>(channel) * n_frames;
    for (size_t frame = 0; frame < n_frames; ++frame) {
      plane[frame] =
          interleaved[frame * static_cast<size_t>(channels) + static_cast<size_t>(channel)];
    }
    planes[static_cast<size_t>(channel)] = plane;
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
  std::ifstream file(utf8_to_wide_path(path), std::ios::binary | std::ios::ate);
#else
  std::ifstream file(path, std::ios::binary | std::ios::ate);
#endif
  SONARE_CHECK_MSG(file.is_open(), ErrorCode::FileNotFound, "Cannot open file: " + path);

  auto size = file.tellg();

  // Check file size before allocating memory
  if (max_size > 0) {
    SONARE_CHECK_MSG(static_cast<size_t>(size) <= max_size, ErrorCode::InvalidParameter,
                     "File too large: " + std::to_string(static_cast<size_t>(size)) +
                         " bytes (max: " + std::to_string(max_size) + " bytes)");
  }

  file.seekg(0, std::ios::beg);

  std::vector<uint8_t> buffer(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(buffer.data()), size);
  SONARE_CHECK_MSG(file.good(), ErrorCode::DecodeFailed, "Failed to read file: " + path);

  return buffer;
}
#endif  // !__EMSCRIPTEN__

}  // namespace

AudioFormat detect_format(const uint8_t* data, size_t size) {
  if (size < 12) {
    return AudioFormat::Unknown;
  }

  // WAV: "RIFF....WAVE"
  if (data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' && data[8] == 'W' &&
      data[9] == 'A' && data[10] == 'V' && data[11] == 'E') {
    return AudioFormat::WAV;
  }

  // MP3: Frame sync (0xFF 0xFB/0xFA/0xF3/0xF2/0xE3/0xE2) or ID3 tag
  if ((data[0] == 0xFF && (data[1] & 0xE0) == 0xE0) ||
      (data[0] == 'I' && data[1] == 'D' && data[2] == '3')) {
    return AudioFormat::MP3;
  }

  return AudioFormat::Unknown;
}

namespace {

InterleavedAudioLoadResult load_buffer_wav_interleaved(const uint8_t* data, size_t size) {
  drwav wav;
  const drwav_bool32 ok = drwav_init_memory(&wav, data, size, nullptr);
  SONARE_CHECK_MSG(ok, ErrorCode::DecodeFailed, "Failed to parse WAV data");

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
  std::vector<float> interleaved(total_samples);
  constexpr size_t kDecodeChunkFrames = 16 * 1024;
  drwav_uint64 total_frames_read = 0;
  while (total_frames_read < wav.totalPCMFrameCount) {
    const drwav_uint64 request =
        std::min<drwav_uint64>(kDecodeChunkFrames, wav.totalPCMFrameCount - total_frames_read);
    const size_t offset = static_cast<size_t>(total_frames_read) * static_cast<size_t>(channels);
    const drwav_uint64 frames_read =
        drwav_read_pcm_frames_f32(&wav, request, interleaved.data() + offset);
    if (frames_read == 0) break;
    total_frames_read += frames_read;
  }
  drwav_uninit(&wav);

  SONARE_CHECK_MSG(total_frames_read > 0, ErrorCode::DecodeFailed, "No audio frames in WAV data");
  interleaved.resize(static_cast<size_t>(total_frames_read) * static_cast<size_t>(channels));
  return {std::move(interleaved), sample_rate, channels};
}

InterleavedAudioLoadResult load_buffer_mp3_interleaved(const uint8_t* data, size_t size) {
  Mp3DecoderGuard decoder_guard;
  const int result = mp3dec_ex_open_buf(&decoder_guard.decoder, data, size, 0);
  SONARE_CHECK_MSG(result == 0, ErrorCode::DecodeFailed, "Failed to decode MP3 data");
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
  std::array<mp3d_sample_t, kMp3DecodeChunkSamples> chunk{};
  std::vector<float> interleaved;
  interleaved.reserve(static_cast<size_t>(declared_samples));
  while (true) {
    const size_t read_samples = mp3dec_ex_read(&decoder_guard.decoder, chunk.data(), chunk.size());
    if (read_samples == 0) break;
    size_t next_total = 0;
    SONARE_CHECK_MSG(numeric::checked_add(interleaved.size(), read_samples, &next_total) &&
                         next_total <= resource::kMaxOfflineAudioSamples,
                     ErrorCode::DecodeFailed,
                     "MP3 decoded more samples than the offline decode limit");
    SONARE_CHECK_MSG(read_samples % static_cast<size_t>(channels) == 0, ErrorCode::DecodeFailed,
                     "MP3 decoder returned an incomplete audio frame");
    interleaved.reserve(next_total);
    for (size_t i = 0; i < read_samples; ++i) {
      interleaved.push_back(static_cast<float>(chunk[i]) / 32768.0f);
    }
  }
  SONARE_CHECK_MSG(!interleaved.empty(), ErrorCode::DecodeFailed, "No audio samples in MP3 data");
  return {std::move(interleaved), sample_rate, channels};
}

[[maybe_unused]] InterleavedAudioLoadResult load_buffer_interleaved(const uint8_t* data,
                                                                    size_t size) {
  SONARE_CHECK_MSG(size <= resource::kMaxAudioFileBytes, ErrorCode::InvalidParameter,
                   "Audio buffer too large: " + std::to_string(size) +
                       " bytes (max: " + std::to_string(resource::kMaxAudioFileBytes) + ")");
  switch (detect_format(data, size)) {
    case AudioFormat::WAV:
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
  drwav wav;
  drwav_bool32 ok = drwav_init_memory(&wav, data, size, nullptr);
  SONARE_CHECK_MSG(ok, ErrorCode::DecodeFailed, "Failed to parse WAV data");

  int sample_rate = static_cast<int>(wav.sampleRate);
  int channels = static_cast<int>(wav.channels);
  SONARE_CHECK_MSG(sample_rate > 0, ErrorCode::DecodeFailed, "Invalid WAV sample rate");
  SONARE_CHECK_MSG(channels >= kMinSupportedChannels, ErrorCode::DecodeFailed,
                   "Invalid WAV channel count");

  // A crafted WAV (e.g. an MS-ADPCM `fact` chunk) can declare a huge PCM frame
  // count before any real sample data is present. Reject over the offline decode
  // limit — with an overflow-safe multiply — before the multi-GB zero-init.
  size_t total_samples = 0;
  SONARE_CHECK_MSG(numeric::checked_size_product(static_cast<size_t>(wav.totalPCMFrameCount),
                                                 static_cast<size_t>(wav.channels),
                                                 resource::kMaxOfflineAudioSamples, &total_samples),
                   ErrorCode::DecodeFailed,
                   "WAV declares more samples than the offline decode limit");
  constexpr size_t kDecodeChunkFrames = 16 * 1024;
  const size_t frame_capacity = static_cast<size_t>(wav.totalPCMFrameCount);
  std::vector<float> mono;
  mono.reserve(frame_capacity);
  size_t scratch_samples = 0;
  SONARE_CHECK_MSG(numeric::checked_size_product(
                       std::min(kDecodeChunkFrames, frame_capacity), static_cast<size_t>(channels),
                       resource::kMaxOfflineAudioSamples, &scratch_samples),
                   ErrorCode::DecodeFailed, "WAV channel layout exceeds the decode limit");
  std::vector<float> scratch(scratch_samples);
  std::vector<float> planar;
  drwav_uint64 total_frames_read = 0;
  while (total_frames_read < wav.totalPCMFrameCount) {
    const drwav_uint64 request =
        std::min<drwav_uint64>(kDecodeChunkFrames, wav.totalPCMFrameCount - total_frames_read);
    const drwav_uint64 frames_read = drwav_read_pcm_frames_f32(&wav, request, scratch.data());
    if (frames_read == 0) break;
    append_mono_fold(scratch.data(), static_cast<size_t>(frames_read), channels, planar, mono);
    total_frames_read += frames_read;
  }
  drwav_uninit(&wav);

  SONARE_CHECK_MSG(total_frames_read > 0, ErrorCode::DecodeFailed, "No audio frames in WAV data");
  return {std::move(mono), sample_rate};
}

AudioLoadResult load_buffer_mp3(const uint8_t* data, size_t size) {
  // mp3dec_load_buf decodes the entire stream into one malloc before exposing
  // its sample count, so a low-bitrate, long-duration upload can OOM before we
  // get a chance to enforce kMaxOfflineAudioSamples. The streaming API scans
  // metadata/indexes first, then lets us enforce the bound for every decoded
  // chunk before appending PCM to the result.
  Mp3DecoderGuard decoder_guard;
  const int result = mp3dec_ex_open_buf(&decoder_guard.decoder, data, size, 0);
  SONARE_CHECK_MSG(result == 0, ErrorCode::DecodeFailed, "Failed to decode MP3 data");
  decoder_guard.opened = true;

  SONARE_CHECK_MSG(decoder_guard.decoder.samples > 0, ErrorCode::DecodeFailed,
                   "No audio samples in MP3 data");

  int sample_rate = decoder_guard.decoder.info.hz;
  int channels = decoder_guard.decoder.info.channels;
  SONARE_CHECK_MSG(sample_rate > 0, ErrorCode::DecodeFailed, "Invalid MP3 sample rate");
  SONARE_CHECK_MSG(channels >= kMinSupportedChannels, ErrorCode::DecodeFailed,
                   "Invalid MP3 channel count");

  const uint64_t declared_samples = decoder_guard.decoder.samples;
  SONARE_CHECK_MSG(declared_samples <= resource::kMaxOfflineAudioSamples, ErrorCode::DecodeFailed,
                   "MP3 declares more samples than the offline decode limit");

  constexpr size_t kMp3DecodeChunkSamples = 16 * 1024;
  std::array<mp3d_sample_t, kMp3DecodeChunkSamples> chunk{};
  std::vector<float> mono;
  mono.reserve(static_cast<size_t>(declared_samples / static_cast<uint64_t>(channels)));
  std::vector<float> normalized;
  std::vector<float> planar;
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

    const size_t frame_count = read_samples / static_cast<size_t>(channels);
    normalized.resize(read_samples);
    for (size_t i = 0; i < read_samples; ++i) {
      normalized[i] = static_cast<float>(chunk[i]) / 32768.0f;
    }
    append_mono_fold(normalized.data(), frame_count, channels, planar, mono);
  }
  SONARE_CHECK_MSG(!mono.empty(), ErrorCode::DecodeFailed, "No audio samples in MP3 data");

  return {std::move(mono), sample_rate};
}

#ifndef __EMSCRIPTEN__
InterleavedAudioLoadResult load_audio_interleaved(const std::string& path,
                                                  const AudioLoadOptions& options) {
  const std::vector<uint8_t> data = read_file(path, options.max_file_size);
  return load_buffer_interleaved(data.data(), data.size());
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

AudioLoadResult load_buffer(const uint8_t* data, size_t size) {
  // The file-path loaders cap their input at kMaxAudioFileBytes before decoding
  // (read_file). The buffer/memory path is the entry most exposed to untrusted
  // input, so apply the same ceiling here to reject an oversized blob before any
  // decoder can start expanding it. Match read_file's error class.
  SONARE_CHECK_MSG(size <= resource::kMaxAudioFileBytes, ErrorCode::InvalidParameter,
                   "Audio buffer too large: " + std::to_string(size) +
                       " bytes (max: " + std::to_string(resource::kMaxAudioFileBytes) + " bytes)");
  AudioFormat format = detect_format(data, size);

  switch (format) {
    case AudioFormat::WAV:
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
  return load_buffer(data.data(), data.size());
}

int audio_channel_count(const std::string& path, const AudioLoadOptions& options) {
  const std::vector<uint8_t> data = read_file(path, options.max_file_size);
  switch (detect_format(data.data(), data.size())) {
    case AudioFormat::WAV: {
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

/// Quantizes a normalized float sample to signed 16-bit PCM using
/// round-half-away-from-zero (@c std::lroundf), the nearest-neighbor rounding
/// libsndfile and other reference encoders use, instead of C++'s default
/// toward-zero truncation. The input is clamped to [-1, 1] and the rounded
/// result to the int16 range so a +1.0 peak cannot overflow.
int16_t float_to_pcm16(float sample) {
  const float clamped = std::max(-1.0f, std::min(1.0f, sample));
  const long v = std::lroundf(clamped * 32767.0f);
  return static_cast<int16_t>(std::max<long>(-32768, std::min<long>(32767, v)));
}

/// Quantizes a normalized float sample to a signed 24-bit PCM value (held in an
/// int32) using the same round-to-nearest behavior as @ref float_to_pcm16.
int32_t float_to_pcm24(float sample) {
  const float clamped = std::max(-1.0f, std::min(1.0f, sample));
  const long v = std::lroundf(clamped * 8388607.0f);  // 2^23 - 1
  return static_cast<int32_t>(std::max<long>(-8388608, std::min<long>(8388607, v)));
}

#ifndef __EMSCRIPTEN__
// Per-writer-unique sibling temp-file path for an atomic write: content is
// written here first and only renamed over the destination once complete, so an
// interrupted or failed write can never truncate an existing file. The process
// id plus a monotonic counter keep concurrent writers to the same destination on
// distinct temp files; a fixed name would let two writers open and interleave
// into the same temp before either renames, leaving a corrupt result. The same
// scheme is duplicated in tools/cli/sonare_cli_project.cpp (the CLI reaches this
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

// fsyncs the completed temp file (best-effort durability) then atomically renames
// it over the destination.
void finalize_atomic(const std::string& tmp, const std::string& path) {
#ifndef _WIN32
  const int fd = ::open(tmp.c_str(), O_RDONLY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
#endif
  SONARE_CHECK_MSG(std::rename(tmp.c_str(), path.c_str()) == 0, ErrorCode::DecodeFailed,
                   "Failed to finalize file: " + path);
}
#endif  // !__EMSCRIPTEN__

}  // namespace

void save_wav(const std::string& path, const float* samples, size_t n_samples, int sample_rate,
              int bits_per_sample) {
  SONARE_CHECK_MSG(samples != nullptr, ErrorCode::InvalidParameter, "Samples pointer is null");
  SONARE_CHECK_MSG(n_samples > 0, ErrorCode::InvalidParameter, "No samples to save");
  SONARE_CHECK_MSG(sample_rate > 0, ErrorCode::InvalidParameter, "Invalid sample rate");
  SONARE_CHECK_MSG(bits_per_sample == 16 || bits_per_sample == 24, ErrorCode::InvalidParameter,
                   "bits_per_sample must be 16 or 24");

  drwav_data_format format;
  format.container = drwav_container_riff;
  format.format = DR_WAVE_FORMAT_PCM;
  format.channels = 1;
  format.sampleRate = static_cast<drwav_uint32>(sample_rate);
  format.bitsPerSample = static_cast<drwav_uint32>(bits_per_sample);

  const std::string tmp = atomic_tmp_path(path);
  TmpFileGuard guard{tmp};
  drwav wav;
  drwav_bool32 ok = drwav_init_file_write(&wav, tmp.c_str(), &format, nullptr);
  SONARE_CHECK_MSG(ok, ErrorCode::EncodeFailed, "Failed to create WAV file: " + path);

  if (bits_per_sample == 16) {
    // Convert float to int16
    std::vector<int16_t> int_samples(n_samples);
    for (size_t i = 0; i < n_samples; ++i) {
      int_samples[i] = float_to_pcm16(samples[i]);
    }
    drwav_uint64 written = drwav_write_pcm_frames(&wav, n_samples, int_samples.data());
    drwav_uninit(&wav);
    SONARE_CHECK_MSG(written == n_samples, ErrorCode::EncodeFailed, "Failed to write all samples");
  } else {
    // 24-bit: pack as tightly packed 3-byte little-endian samples. dr_wav's
    // writer is a raw byte copy (bytesToWrite = frames * channels * 24 / 8) with
    // no width/stride conversion, so a 4-byte int32 buffer would be read
    // misaligned. Build the exact 3-byte-per-sample byte stream it expects.
    std::vector<uint8_t> bytes(n_samples * 3);
    for (size_t i = 0; i < n_samples; ++i) {
      int32_t v = float_to_pcm24(samples[i]);
      bytes[i * 3 + 0] = static_cast<uint8_t>(v & 0xFF);
      bytes[i * 3 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
      bytes[i * 3 + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    }
    drwav_uint64 written = drwav_write_pcm_frames(&wav, n_samples, bytes.data());
    drwav_uninit(&wav);
    SONARE_CHECK_MSG(written == n_samples, ErrorCode::EncodeFailed, "Failed to write all samples");
  }
  finalize_atomic(tmp, path);
  guard.committed = true;
}

void save_wav(const std::string& path, const std::vector<float>& samples, int sample_rate,
              int bits_per_sample) {
  save_wav(path, samples.data(), samples.size(), sample_rate, bits_per_sample);
}

namespace {

/// Packs normalized float samples into the on-disk PCM byte stream: 16-bit
/// little-endian int16 or 24-bit tightly-packed 3-byte little-endian, clamped to
/// [-1, 1].
std::vector<uint8_t> pack_pcm_bytes(const float* samples, size_t n_samples, int bits_per_sample) {
  std::vector<uint8_t> bytes(n_samples * static_cast<size_t>(bits_per_sample / 8));
  if (bits_per_sample == 16) {
    for (size_t i = 0; i < n_samples; ++i) {
      auto v = float_to_pcm16(samples[i]);
      bytes[i * 2 + 0] = static_cast<uint8_t>(v & 0xFF);
      bytes[i * 2 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    }
  } else {  // 24-bit
    for (size_t i = 0; i < n_samples; ++i) {
      auto v = float_to_pcm24(samples[i]);
      bytes[i * 3 + 0] = static_cast<uint8_t>(v & 0xFF);
      bytes[i * 3 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
      bytes[i * 3 + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    }
  }
  return bytes;
}

void write_le16(std::ostream& os, uint16_t value) {
  const uint8_t b[2] = {static_cast<uint8_t>(value & 0xFF),
                        static_cast<uint8_t>((value >> 8) & 0xFF)};
  os.write(reinterpret_cast<const char*>(b), 2);
}

void write_le32(std::ostream& os, uint32_t value) {
  const uint8_t b[4] = {
      static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF),
      static_cast<uint8_t>((value >> 16) & 0xFF), static_cast<uint8_t>((value >> 24) & 0xFF)};
  os.write(reinterpret_cast<const char*>(b), 4);
}

/// Hand-writes a WAVE_FORMAT_EXTENSIBLE PCM file. The pinned dr_wav (v0.14.4)
/// has no channelMask field and rejects EXTENSIBLE on write, so surround beds
/// must be serialized directly.
void write_wav_extensible(const std::string& path, const float* interleaved, size_t n_frames,
                          int channel_count, uint32_t channel_mask, int sample_rate,
                          int bits_per_sample) {
  const int bytes_per_sample = bits_per_sample / 8;
  const size_t n_samples = n_frames * static_cast<size_t>(channel_count);
  const std::vector<uint8_t> pcm = pack_pcm_bytes(interleaved, n_samples, bits_per_sample);

  const auto data_size = static_cast<uint32_t>(pcm.size());
  const auto block_align = static_cast<uint16_t>(channel_count * bytes_per_sample);
  const auto byte_rate = static_cast<uint32_t>(sample_rate) * block_align;
  const uint32_t fmt_size = 40;  // 16 base + 2 (cbSize) + 22 (extension)
  const uint32_t riff_size = 4 + (8 + fmt_size) + (8 + data_size);

  // KSDATAFORMAT_SUBTYPE_PCM = {00000001-0000-0010-8000-00AA00389B71}, stored
  // as little-endian Data1/2/3 followed by the 8 Data4 bytes verbatim.
  static const uint8_t kPcmSubFormat[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                            0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

  const std::string tmp = atomic_tmp_path(path);
  TmpFileGuard guard{tmp};
  std::ofstream os(tmp, std::ios::binary);
  SONARE_CHECK_MSG(os.is_open(), ErrorCode::EncodeFailed, "Failed to create WAV file: " + path);

  os.write("RIFF", 4);
  write_le32(os, riff_size);
  os.write("WAVE", 4);

  os.write("fmt ", 4);
  write_le32(os, fmt_size);
  write_le16(os, 0xFFFE);  // WAVE_FORMAT_EXTENSIBLE
  write_le16(os, static_cast<uint16_t>(channel_count));
  write_le32(os, static_cast<uint32_t>(sample_rate));
  write_le32(os, byte_rate);
  write_le16(os, block_align);
  write_le16(os, static_cast<uint16_t>(bits_per_sample));
  write_le16(os, 22);                                      // cbSize
  write_le16(os, static_cast<uint16_t>(bits_per_sample));  // wValidBitsPerSample
  write_le32(os, channel_mask);
  os.write(reinterpret_cast<const char*>(kPcmSubFormat), 16);

  os.write("data", 4);
  write_le32(os, data_size);
  os.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size()));
  os.flush();
  SONARE_CHECK_MSG(os.good(), ErrorCode::EncodeFailed, "Failed to write WAV file: " + path);
  os.close();
  finalize_atomic(tmp, path);
  guard.committed = true;
}

}  // namespace

void save_wav_multichannel(const std::string& path, const float* interleaved, size_t n_frames,
                           int channel_count, ChannelLayout layout, int sample_rate,
                           int bits_per_sample) {
  SONARE_CHECK_MSG(interleaved != nullptr, ErrorCode::InvalidParameter, "Samples pointer is null");
  SONARE_CHECK_MSG(n_frames > 0, ErrorCode::InvalidParameter, "No frames to save");
  SONARE_CHECK_MSG(sample_rate > 0, ErrorCode::InvalidParameter, "Invalid sample rate");
  SONARE_CHECK_MSG(bits_per_sample == 16 || bits_per_sample == 24, ErrorCode::InvalidParameter,
                   "bits_per_sample must be 16 or 24");
  SONARE_CHECK_MSG(channel_count == sonare::channel_count(layout), ErrorCode::InvalidParameter,
                   "channel_count does not match the layout");

  // RIFF stores both the data chunk and the enclosing file size in uint32.
  // Reject before packing/allocating: otherwise a >4 GiB surround render writes
  // a truncated header that many readers interpret as a corrupt short file.
  const size_t bytes_per_frame =
      static_cast<size_t>(channel_count) * static_cast<size_t>(bits_per_sample / 8);
  constexpr size_t kExtensibleHeaderBytes = 68;
  const size_t max_data_bytes =
      static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - (kExtensibleHeaderBytes - 8);
  SONARE_CHECK_MSG(bytes_per_frame > 0 && n_frames <= max_data_bytes / bytes_per_frame,
                   ErrorCode::InvalidParameter,
                   "WAV data exceeds RIFF 32-bit size limit; use a chunked container");

  if (channel_count <= 2) {
    // Plain WAVE_FORMAT_PCM for mono/stereo (maximum compatibility, and
    // bit-identical to the existing mono/stereo writer).
    drwav_data_format format;
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_PCM;
    format.channels = static_cast<drwav_uint32>(channel_count);
    format.sampleRate = static_cast<drwav_uint32>(sample_rate);
    format.bitsPerSample = static_cast<drwav_uint32>(bits_per_sample);

    const std::string tmp = atomic_tmp_path(path);
    TmpFileGuard guard{tmp};
    drwav wav;
    drwav_bool32 ok = drwav_init_file_write(&wav, tmp.c_str(), &format, nullptr);
    SONARE_CHECK_MSG(ok, ErrorCode::EncodeFailed, "Failed to create WAV file: " + path);

    const std::vector<uint8_t> pcm =
        pack_pcm_bytes(interleaved, n_frames * static_cast<size_t>(channel_count), bits_per_sample);
    drwav_uint64 written = drwav_write_pcm_frames(&wav, n_frames, pcm.data());
    drwav_uninit(&wav);
    SONARE_CHECK_MSG(written == n_frames, ErrorCode::EncodeFailed, "Failed to write all frames");
    finalize_atomic(tmp, path);
    guard.committed = true;
    return;
  }

  write_wav_extensible(path, interleaved, n_frames, channel_count, wave_channel_mask(layout),
                       sample_rate, bits_per_sample);
}
#endif  // !__EMSCRIPTEN__

}  // namespace sonare
