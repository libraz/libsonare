#include "core/audio_io.h"

#include <algorithm>
#include <cstring>

// File-path I/O is unavailable in WebAssembly builds; the core exposes only the
// buffer-based loaders there (see the load_buffer_* functions below).
#ifndef __EMSCRIPTEN__
#include <fstream>
#endif

#include "util/exception.h"

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

#ifndef SONARE_WITH_FFMPEG
#ifndef __EMSCRIPTEN__
/// @brief Extracts a lowercase file extension (including the leading dot) from a path.
/// @return The extension (e.g. ".m4a"), or an empty string if none is found.
/// @note Only used by the unsupported-format messages; with FFmpeg enabled, any
///       decoder error surfaces via @ref load_buffer_ffmpeg / @ref load_ffmpeg
///       so the extension hint is no longer needed.
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

/// @brief RAII guard for MP3 decode buffer.
/// @details Ensures mp3dec_file_info_t.buffer is freed even on exception.
struct Mp3BufferGuard {
  mp3d_sample_t* ptr = nullptr;
  ~Mp3BufferGuard() {
    if (ptr) {
      free(ptr);
    }
  }
};

/// @brief Converts interleaved stereo to mono by averaging channels.
std::vector<float> stereo_to_mono(const float* data, size_t total_samples, int channels) {
  size_t frame_count = total_samples / static_cast<size_t>(channels);
  std::vector<float> mono(frame_count);

  if (channels == 1) {
    std::copy(data, data + frame_count, mono.begin());
  } else if (channels == 2) {
    for (size_t i = 0; i < frame_count; ++i) {
      mono[i] = (data[i * 2] + data[i * 2 + 1]) * 0.5f;
    }
  } else {
    // Average all channels
    for (size_t i = 0; i < frame_count; ++i) {
      float sum = 0.0f;
      for (int ch = 0; ch < channels; ++ch) {
        sum += data[i * channels + ch];
      }
      mono[i] = sum / static_cast<float>(channels);
    }
  }
  return mono;
}

/// @brief Converts interleaved int16 stereo to mono float.
/// @details Handles int16-to-float conversion and channel mixing in a single pass.
std::vector<float> int16_stereo_to_mono(const mp3d_sample_t* data, size_t total_samples,
                                        int channels) {
  size_t frame_count = total_samples / static_cast<size_t>(channels);
  std::vector<float> mono(frame_count);
  constexpr float kInt16Scale = 1.0f / 32768.0f;

  if (channels == 1) {
    for (size_t i = 0; i < frame_count; ++i) {
      mono[i] = static_cast<float>(data[i]) * kInt16Scale;
    }
  } else if (channels == 2) {
    for (size_t i = 0; i < frame_count; ++i) {
      mono[i] = (static_cast<float>(data[i * 2]) + static_cast<float>(data[i * 2 + 1])) *
                kInt16Scale * 0.5f;
    }
  } else {
    for (size_t i = 0; i < frame_count; ++i) {
      float sum = 0.0f;
      for (int ch = 0; ch < channels; ++ch) {
        sum += static_cast<float>(data[i * channels + ch]);
      }
      mono[i] = sum * kInt16Scale / static_cast<float>(channels);
    }
  }
  return mono;
}

#ifndef __EMSCRIPTEN__
/// @brief Reads entire file into memory.
/// @param path Path to the file
/// @param max_size Maximum allowed file size in bytes (0 = no limit)
std::vector<uint8_t> read_file(const std::string& path, size_t max_size = 0) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
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

AudioLoadResult load_buffer_wav(const uint8_t* data, size_t size) {
  drwav wav;
  drwav_bool32 ok = drwav_init_memory(&wav, data, size, nullptr);
  SONARE_CHECK_MSG(ok, ErrorCode::DecodeFailed, "Failed to parse WAV data");

  int sample_rate = static_cast<int>(wav.sampleRate);
  int channels = static_cast<int>(wav.channels);
  SONARE_CHECK_MSG(sample_rate > 0, ErrorCode::DecodeFailed, "Invalid WAV sample rate");
  SONARE_CHECK_MSG(channels >= kMinSupportedChannels, ErrorCode::DecodeFailed,
                   "Invalid WAV channel count");

  size_t total_samples = wav.totalPCMFrameCount * wav.channels;
  std::vector<float> samples(total_samples);

  drwav_uint64 frames_read =
      drwav_read_pcm_frames_f32(&wav, wav.totalPCMFrameCount, samples.data());

  drwav_uninit(&wav);

  SONARE_CHECK_MSG(frames_read > 0, ErrorCode::DecodeFailed, "No audio frames in WAV data");

  // Convert to mono
  std::vector<float> mono = stereo_to_mono(samples.data(), frames_read * channels, channels);
  return {std::move(mono), sample_rate};
}

AudioLoadResult load_buffer_mp3(const uint8_t* data, size_t size) {
  mp3dec_t mp3d;
  mp3dec_file_info_t info;

  mp3dec_init(&mp3d);
  int result = mp3dec_load_buf(&mp3d, data, size, &info, nullptr, nullptr);
  SONARE_CHECK_MSG(result == 0, ErrorCode::DecodeFailed, "Failed to decode MP3 data");

  // Use RAII guard to ensure buffer is freed even on exception
  Mp3BufferGuard buffer_guard;
  buffer_guard.ptr = info.buffer;

  SONARE_CHECK_MSG(info.samples > 0, ErrorCode::DecodeFailed, "No audio samples in MP3 data");

  int sample_rate = info.hz;
  int channels = info.channels;
  SONARE_CHECK_MSG(sample_rate > 0, ErrorCode::DecodeFailed, "Invalid MP3 sample rate");
  SONARE_CHECK_MSG(channels >= kMinSupportedChannels, ErrorCode::DecodeFailed,
                   "Invalid MP3 channel count");
  size_t total_samples = static_cast<size_t>(info.samples);

  // Convert int16 samples to float and mono
  std::vector<float> mono = int16_stereo_to_mono(info.buffer, total_samples, channels);

  // buffer_guard will free info.buffer on destruction
  return {std::move(mono), sample_rate};
}

#ifndef __EMSCRIPTEN__
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

  drwav wav;
  drwav_bool32 ok = drwav_init_file_write(&wav, path.c_str(), &format, nullptr);
  SONARE_CHECK_MSG(ok, ErrorCode::DecodeFailed, "Failed to create WAV file: " + path);

  if (bits_per_sample == 16) {
    // Convert float to int16
    std::vector<int16_t> int_samples(n_samples);
    for (size_t i = 0; i < n_samples; ++i) {
      float clamped = std::max(-1.0f, std::min(1.0f, samples[i]));
      int_samples[i] = static_cast<int16_t>(clamped * 32767.0f);
    }
    drwav_uint64 written = drwav_write_pcm_frames(&wav, n_samples, int_samples.data());
    drwav_uninit(&wav);
    SONARE_CHECK_MSG(written == n_samples, ErrorCode::DecodeFailed, "Failed to write all samples");
  } else {
    // 24-bit: convert float to int32 (upper 24 bits)
    std::vector<int32_t> int_samples(n_samples);
    for (size_t i = 0; i < n_samples; ++i) {
      float clamped = std::max(-1.0f, std::min(1.0f, samples[i]));
      int_samples[i] = static_cast<int32_t>(clamped * 8388607.0f);  // 2^23 - 1
    }
    drwav_uint64 written = drwav_write_pcm_frames(&wav, n_samples, int_samples.data());
    drwav_uninit(&wav);
    SONARE_CHECK_MSG(written == n_samples, ErrorCode::DecodeFailed, "Failed to write all samples");
  }
}

void save_wav(const std::string& path, const std::vector<float>& samples, int sample_rate,
              int bits_per_sample) {
  save_wav(path, samples.data(), samples.size(), sample_rate, bits_per_sample);
}
#endif  // !__EMSCRIPTEN__

}  // namespace sonare
