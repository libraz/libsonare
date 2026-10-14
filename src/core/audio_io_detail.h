#pragma once

// Private file/CLI bridge.  This header intentionally lives below src/ rather
// than include/ and is not part of the installed API.  A command that needs
// the source layout can ask the decoder for it while the legacy mono fold is
// still produced by the same codec pass.

#include <cstddef>
#include <exception>
#include <string>
#include <vector>

#include "core/audio_io.h"

namespace sonare {

#if defined(__GNUC__) || defined(__clang__)
#define SONARE_AUDIO_IO_DETAIL_HIDDEN __attribute__((visibility("hidden")))
#else
#define SONARE_AUDIO_IO_DETAIL_HIDDEN
#endif

enum class RetainChannels {
  None,
  StereoIfExact,
  All,
};

struct DecodedInputData {
  std::vector<float> mono;
  std::vector<float> interleaved;
  int sample_rate = 0;
  // Metadata's source count remains separate from the decoded retained count:
  // FFmpeg's old advisory probe is allowed to fail while the decoder still
  // exposes a usable channel layout to a channel-preserving handler.
  int source_channels = 0;
  int retained_channels = 0;
  // These are advisory/retained-only failures.  The mono decode remains the
  // authoritative input and the caller decides where the old second-load
  // point should surface either exception.
  std::exception_ptr metadata_error;
  std::exception_ptr retained_error;
};

SONARE_AUDIO_IO_DETAIL_HIDDEN DecodedInputData
load_audio_input_once(const std::string& path, RetainChannels retention,
                      const AudioLoadOptions& options = kDefaultLoadOptions);

#ifdef SONARE_WITH_FFMPEG
SONARE_AUDIO_IO_DETAIL_HIDDEN DecodedInputData load_buffer_ffmpeg_input(const uint8_t* data,
                                                                        size_t size,
                                                                        RetainChannels retention);
#endif

/// Apply the same decoded-file validation used by Audio::from_file, then move
/// the validated mono samples into an Audio handle without decoding again.
class Audio;
SONARE_AUDIO_IO_DETAIL_HIDDEN Audio audio_from_decoded_mono(std::vector<float> samples,
                                                            int sample_rate);

}  // namespace sonare

#undef SONARE_AUDIO_IO_DETAIL_HIDDEN
