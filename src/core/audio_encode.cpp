#include "core/audio_encode.h"

#include <algorithm>
#include <cmath>

#include "core/audio.h"
#include "util/constants.h"
#include "util/exception.h"
#include "util/numeric_validation.h"
#include "util/parameter_checks.h"

#ifdef SONARE_WITH_FFMPEG
#include "core/audio_io_ffmpeg.h"
#endif

namespace sonare {

bool audio_encode_format_available(std::string_view format) noexcept {
  if (format == "wav") return true;
#ifdef SONARE_WITH_FFMPEG
  return ffmpeg_audio_encode_available(format);
#else
  return false;
#endif
}

[[noreturn]] void detail::refuse_unavailable_audio_encoder() {
  throw SonareParameterRefusal::predicate_with_reason(ErrorCode::NotSupported, "format",
                                                      "is unavailable for encoding in this build",
                                                      ErrorReason::NotCompiledIn);
}

std::vector<uint8_t> encode_audio(const float* samples, std::size_t frames, int num_channels,
                                  int sample_rate, const AudioEncodeOptions& options) {
  namespace check = util::check;
  constexpr int kMaxWavChannels = 255;
  const auto& format = options.format;
  const bool wav = format == "wav";
  const bool flac = format == "flac";
  if (!wav && std::find(kOptionalAudioEncodeFormats.begin(), kOptionalAudioEncodeFormats.end(),
                        format) == kOptionalAudioEncodeFormats.end()) {
    check::refuse("format", "must be wav, flac, mp3, ogg, opus, or m4a");
  }

  if (frames == 0) check::refuse("frames", "must be positive");
  if (num_channels <= 0) check::refuse("num_channels", "must be positive");
  if (wav && num_channels > kMaxWavChannels) {
    check::refuse("num_channels", "must be at most 255 for wav");
  }
  check::integer_within(sample_rate, kAudioSampleRateDomain, "sample_rate");
  if ((format == "mp3" || format == "opus") && sample_rate > constants::kDefaultDawSampleRate) {
    check::refuse("sample_rate", "must be at most 48000 for this format");
  }
  if (samples == nullptr) check::refuse("samples", "must be a non-empty buffer");

  const int bit_depth = options.bit_depth.value_or(16);
  if (options.bit_depth.has_value() &&
      ((!wav && !flac) || (bit_depth != 16 && bit_depth != 24 && !(wav && bit_depth == 32)))) {
    check::refuse("bit_depth", "is not supported by this format");
  }
  if (options.quality.has_value()) {
    if (wav || flac) check::refuse("quality", "is not supported by this format");
    check::in_range(*options.quality, 0.0, 1.0, "quality");
  }
  std::size_t count = 0;
  if (!numeric::checked_size_product(frames, static_cast<std::size_t>(num_channels),
                                     kMaxAudioBufferSize, &count)) {
    check::refuse_relation({"frames", "num_channels"}, "{0} times {1} exceeds the sample limit");
  }
  if (!std::all_of(samples, samples + count, numeric::finite<float>)) {
    check::refuse("samples", "must hold only finite values");
  }

  if (wav) {
    return detail::encode_wav_planar(samples, frames, num_channels, sample_rate, bit_depth,
                                     options.tags);
  }
#ifdef SONARE_WITH_FFMPEG
  if (ffmpeg_audio_encode_available(format)) {
    const float default_quality = format == "mp3"    ? 0.6f
                                  : format == "ogg"  ? 0.5f
                                  : format == "opus" ? 0.3f
                                                     : 0.4f;
    return encode_audio_ffmpeg(samples, frames, num_channels, sample_rate, format, bit_depth,
                               options.quality.value_or(default_quality), options.tags);
  }
#endif
  detail::refuse_unavailable_audio_encoder();
}

}  // namespace sonare
