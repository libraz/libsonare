#include "core/audio_io_ffmpeg.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <ratio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/audio_encode.h"
#include "core/audio_io_detail.h"
#include "mixing/downmix.h"
#include "util/exception.h"
#include "util/numeric_validation.h"
#include "util/resource_limits.h"

// FFmpeg headers expose a classic C API. Their public macros and inline helpers
// trip several of the project's strict warnings (notably -Wzero-as-null-pointer
// constant inside av_err2str and friends), so disable the offenders only while
// we include the FFmpeg headers. The pragmas are scoped to this translation
// unit; the rest of the project keeps full warnings-as-errors semantics.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
#pragma clang diagnostic ignored "-Wreserved-id-macro"
#pragma clang diagnostic ignored "-Wc++98-compat-pedantic"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace sonare {

namespace {

/// @brief Translates an FFmpeg error code to a readable string.
std::string ff_err(int errnum) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(errnum, buf, sizeof(buf));
  return std::string(buf);
}

// ---------------------------------------------------------------------------
// RAII wrappers for FFmpeg resources. All deleters are no-ops on nullptr so
// the unique_ptrs can be reset/replaced freely during error handling.
// ---------------------------------------------------------------------------

struct AVFormatContextDeleter {
  void operator()(AVFormatContext* ctx) const {
    if (ctx != nullptr) {
      avformat_close_input(&ctx);
    }
  }
};
using AVFormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;

struct AVOutputContextDeleter {
  void operator()(AVFormatContext* ctx) const {
    if (ctx != nullptr) {
      ctx->pb = nullptr;
      avformat_free_context(ctx);
    }
  }
};
using AVOutputContextPtr = std::unique_ptr<AVFormatContext, AVOutputContextDeleter>;

struct AVCodecContextDeleter {
  void operator()(AVCodecContext* ctx) const {
    if (ctx != nullptr) {
      avcodec_free_context(&ctx);
    }
  }
};
using AVCodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;

struct AVPacketDeleter {
  void operator()(AVPacket* pkt) const {
    if (pkt != nullptr) {
      av_packet_free(&pkt);
    }
  }
};
using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;

struct AVFrameDeleter {
  void operator()(AVFrame* frame) const {
    if (frame != nullptr) {
      av_frame_free(&frame);
    }
  }
};
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

struct SwrContextDeleter {
  void operator()(SwrContext* swr) const {
    if (swr != nullptr) {
      swr_free(&swr);
    }
  }
};
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;

struct AVAudioFifoDeleter {
  void operator()(AVAudioFifo* fifo) const {
    if (fifo != nullptr) av_audio_fifo_free(fifo);
  }
};
using AVAudioFifoPtr = std::unique_ptr<AVAudioFifo, AVAudioFifoDeleter>;

struct ChannelLayoutGuard {
  AVChannelLayout layout{};
  ~ChannelLayoutGuard() { av_channel_layout_uninit(&layout); }
};

struct AVIOContextDeleter {
  void operator()(AVIOContext* io) const {
    if (io != nullptr) {
      // io->buffer can be reallocated by libavformat, so always read it back.
      av_freep(&io->buffer);
      avio_context_free(&io);
    }
  }
};
using AVIOContextPtr = std::unique_ptr<AVIOContext, AVIOContextDeleter>;

// ---------------------------------------------------------------------------
// Memory-backed AVIO callbacks.
// ---------------------------------------------------------------------------

/// @brief State used by the custom AVIO callbacks for in-memory decoding.
struct MemoryIOState {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t pos = 0;
};

int memory_read(void* opaque, uint8_t* buf, int buf_size) {
  auto* state = static_cast<MemoryIOState*>(opaque);
  if (state->pos >= state->size) {
    return AVERROR_EOF;
  }
  size_t remaining = state->size - state->pos;
  size_t to_copy = std::min(static_cast<size_t>(buf_size), remaining);
  std::memcpy(buf, state->data + state->pos, to_copy);
  state->pos += to_copy;
  return static_cast<int>(to_copy);
}

int64_t memory_seek(void* opaque, int64_t offset, int whence) {
  auto* state = static_cast<MemoryIOState*>(opaque);
  if ((whence & AVSEEK_SIZE) != 0) {
    return static_cast<int64_t>(state->size);
  }
  int64_t base = 0;
  switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET:
      base = 0;
      break;
    case SEEK_CUR:
      base = static_cast<int64_t>(state->pos);
      break;
    case SEEK_END:
      base = static_cast<int64_t>(state->size);
      break;
    default:
      return -1;
  }
  int64_t target = base + offset;
  if (target < 0 || target > static_cast<int64_t>(state->size)) {
    return -1;
  }
  state->pos = static_cast<size_t>(target);
  return target;
}

// ---------------------------------------------------------------------------
// Mono downmix matrix. swresample's own default folds the front pair at -3 dB
// each without normalizing, which is sqrt(2) hotter than the ITU-R BS.775 rule
// mixing/downmix.h declares and the built-in WAV/MP3 decoders apply. Overriding
// it keeps one downmix rule per source layout across every decoder path.
// ---------------------------------------------------------------------------

/// @brief Coefficients of the library's mono fold, in canonical plane order.
/// @details Obtained by running one unit impulse per source plane through
///          sonare::mixing::downmix(). The fold is linear, so its impulse
///          response is exactly its coefficient row; deriving the row this way
///          rather than restating the constants means this path cannot drift
///          from the rule in mixing/downmix.h.
std::vector<double> mono_downmix_row(ChannelLayout layout) {
  const size_t planes = static_cast<size_t>(channel_count(layout));
  std::vector<float> impulse(planes, 0.0f);
  std::vector<const float*> in(planes);
  for (size_t plane = 0; plane < planes; ++plane) in[plane] = &impulse[plane];

  std::vector<double> row(planes, 0.0);
  for (size_t plane = 0; plane < planes; ++plane) {
    std::fill(impulse.begin(), impulse.end(), 0.0f);
    impulse[plane] = 1.0f;
    float folded = 0.0f;
    float* out[1] = {&folded};
    mixing::downmix(layout, ChannelLayout::Mono, in.data(), out, 1);
    row[plane] = static_cast<double>(folded);
  }
  return row;
}

static_assert(AV_CHAN_FRONT_LEFT == 0 && AV_CHAN_LOW_FREQUENCY == 3 && AV_CHAN_BACK_CENTER == 8 &&
                  AV_CHAN_SIDE_RIGHT == 10,
              "FFmpeg channel ids must follow the WAVE speaker-mask bit numbering");

AVChannel av_channel(WaveSpeaker speaker) {
  return speaker == WaveSpeaker::None ? AV_CHAN_NONE
                                      : static_cast<AVChannel>(static_cast<int>(speaker));
}

/// @brief Plane index of a canonical speaker role within @p layout, or -1.
int layout_index_for_role(const AVChannelLayout& layout, SpeakerRole role) {
  const RoleSpeakers candidates = role_speakers(role);
  int index = av_channel_layout_index_from_channel(&layout, av_channel(candidates.primary));
  if (index < 0 && candidates.alternate != WaveSpeaker::None) {
    index = av_channel_layout_index_from_channel(&layout, av_channel(candidates.alternate));
  }
  return index;
}

/// @brief Installs the library's mono fold as @p swr's rematrix matrix.
/// @details Layouts outside the speaker model, and any layout that does not
///          carry every role the model expects, fall back to the unweighted mean
///          of all planes -- the same fallback core/audio_io.cpp applies, so the
///          two decoder paths agree there too.
/// @param swr Allocated resampler that has not been initialized yet.
/// @param in_layout Source channel layout.
/// @return 0 on success, or a negative FFmpeg error code.
int set_mono_downmix_matrix(SwrContext* swr, const AVChannelLayout& in_layout) {
  const int channels = in_layout.nb_channels;
  // A single-plane source reaches mono untouched, with no matrix to install.
  if (channels <= 1) return 0;

  std::vector<double> matrix(static_cast<size_t>(channels), 1.0 / static_cast<double>(channels));
  const ChannelLayout layout = layout_from_channel_count(channels);
  if (channels == channel_count(layout)) {
    const std::vector<double> row = mono_downmix_row(layout);
    const SpeakerRole* roles = speaker_roles(layout);
    std::vector<double> mapped(static_cast<size_t>(channels), 0.0);
    bool complete = true;
    for (int plane = 0; plane < channels; ++plane) {
      const int index = layout_index_for_role(in_layout, roles[plane]);
      if (index < 0 || index >= channels) {
        complete = false;
        break;
      }
      mapped[static_cast<size_t>(index)] = row[static_cast<size_t>(plane)];
    }
    if (complete) matrix = std::move(mapped);
  }
  return swr_set_matrix(swr, matrix.data(), channels);
}

// ---------------------------------------------------------------------------
// Core decode loop shared between file- and memory-based entry points.
// ---------------------------------------------------------------------------

/// @brief Whether a send/receive error belongs to one packet rather than to the decode.
/// @details Decoders report damaged data with codec-private codes as well as
///          AVERROR_INVALIDDATA, so every failure except allocation is per-packet;
///          a stream in which nothing decodes still fails after the loop.
bool is_skippable_decode_error(int err) {
  return err < 0 && err != AVERROR(EAGAIN) && err != AVERROR_EOF && err != AVERROR(ENOMEM);
}

/// @brief Decodes the first audio stream of @p format_ctx into float32.
/// @param retention Select whether source-order channels are retained alongside
///        the mandatory mono fold.
/// @return The validated mono output plus optional retained source channels.
DecodedInputData decode_first_audio_stream(AVFormatContext* format_ctx, RetainChannels retention,
                                           bool include_mono) {
  int ret = avformat_find_stream_info(format_ctx, nullptr);
  SONARE_CHECK_MSG(ret >= 0, ErrorCode::DecodeFailed,
                   "FFmpeg: avformat_find_stream_info failed: " + ff_err(ret));

  int stream_index = -1;
  for (unsigned i = 0; i < format_ctx->nb_streams; ++i) {
    if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      stream_index = static_cast<int>(i);
      break;
    }
  }
  SONARE_CHECK_MSG(stream_index >= 0, ErrorCode::DecodeFailed,
                   "FFmpeg: no audio stream found in input");

  AVStream* stream = format_ctx->streams[stream_index];
  AVCodecParameters* codecpar = stream->codecpar;

  // Metadata, not the first frame, is the advisory channel count; a missing layout is deferred.
  int metadata_channels = 0;
  std::exception_ptr metadata_error;
  try {
    SONARE_CHECK_MSG(codecpar->ch_layout.nb_channels > 0, ErrorCode::DecodeFailed,
                     "FFmpeg: audio stream has no channel layout");
    metadata_channels = codecpar->ch_layout.nb_channels;
  } catch (...) {
    metadata_error = std::current_exception();
  }

  const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
  SONARE_CHECK_MSG(codec != nullptr, ErrorCode::DecodeFailed,
                   "FFmpeg: no decoder found for codec id " +
                       std::to_string(static_cast<int>(codecpar->codec_id)));

  AVCodecContextPtr codec_ctx(avcodec_alloc_context3(codec));
  SONARE_CHECK_MSG(codec_ctx != nullptr, ErrorCode::OutOfMemory,
                   "FFmpeg: avcodec_alloc_context3 returned null");

  ret = avcodec_parameters_to_context(codec_ctx.get(), codecpar);
  SONARE_CHECK_MSG(ret >= 0, ErrorCode::DecodeFailed,
                   "FFmpeg: avcodec_parameters_to_context failed: " + ff_err(ret));

  ret = avcodec_open2(codec_ctx.get(), codec, nullptr);
  SONARE_CHECK_MSG(ret >= 0, ErrorCode::DecodeFailed,
                   "FFmpeg: avcodec_open2 failed: " + ff_err(ret));

  // The stream-level parameters may be provisional (notably implicit HE-AAC).
  // Adopt the first decoded frame's actual rate and configure swresample from
  // frame parameters. The legacy mono path may rebuild it if a
  // concatenated/renegotiated stream changes format later; interleaved output
  // rejects channel-layout changes because changing output_channels after
  // appending samples would make the result ambiguous.  The private combined
  // path keeps this mono resampler authoritative and runs a second resampler
  // over the same decoded frames only when the caller asks to retain channels;
  // the public interleaved wrapper passes include_mono=false and remains a
  // channel-only decode.
  int sample_rate = 0;
  const AVChannelLayout mono_layout = AV_CHANNEL_LAYOUT_MONO;
  SwrContextPtr mono_swr;
  SwrContextPtr retained_swr;
  ChannelLayoutGuard active_in_layout;
  ChannelLayoutGuard active_retained_layout;
  int active_input_rate = 0;
  AVSampleFormat active_input_format = AV_SAMPLE_FMT_NONE;
  int retained_channels = 0;
  bool retain_enabled = retention == RetainChannels::All ||
                        (retention == RetainChannels::StereoIfExact && metadata_channels == 2);
  std::exception_ptr retained_error;

  AVPacketPtr packet(av_packet_alloc());
  SONARE_CHECK_MSG(packet != nullptr, ErrorCode::OutOfMemory,
                   "FFmpeg: av_packet_alloc returned null");
  AVFramePtr frame(av_frame_alloc());
  SONARE_CHECK_MSG(frame != nullptr, ErrorCode::OutOfMemory,
                   "FFmpeg: av_frame_alloc returned null");

  std::vector<float> mono_samples;
  std::vector<float> retained_samples;
  // A corrupt packet or frame is skipped; the decode fails only when no frame decodes at all.
  size_t corrupt_skipped = 0;
  size_t decoded_frames = 0;

  auto append_swr_flush = [&](SwrContext* context, int output_channels,
                              std::vector<float>& output) {
    if (context == nullptr || sample_rate <= 0 || active_input_rate <= 0) return;
    while (true) {
      const int64_t delay = swr_get_delay(context, active_input_rate);
      const int max_out =
          static_cast<int>(av_rescale_rnd(delay, sample_rate, active_input_rate, AV_ROUND_UP));
      if (max_out <= 0) return;
      const size_t prev_size = output.size();
      size_t output_capacity = 0;
      SONARE_CHECK_MSG(prev_size <= resource::kMaxOfflineAudioSamples && output_channels > 0,
                       ErrorCode::DecodeFailed, "FFmpeg flush output exceeds the offline limit");
      SONARE_CHECK_MSG(numeric::checked_size_product(
                           static_cast<size_t>(max_out), static_cast<size_t>(output_channels),
                           resource::kMaxOfflineAudioSamples - prev_size, &output_capacity) &&
                           numeric::checked_add(prev_size, output_capacity, &output_capacity),
                       ErrorCode::DecodeFailed, "FFmpeg flush exceeds the offline decode limit");
      const size_t output_end = output_capacity;
      output.resize(output_end);
      uint8_t* out_ptr = reinterpret_cast<uint8_t*>(output.data() + prev_size);
      const int converted = swr_convert(context, &out_ptr, max_out, nullptr, 0);
      if (converted <= 0) {
        output.resize(prev_size);
        return;
      }
      size_t converted_samples = 0;
      SONARE_CHECK_MSG(numeric::checked_size_product(
                           static_cast<size_t>(converted), static_cast<size_t>(output_channels),
                           resource::kMaxOfflineAudioSamples, &converted_samples) &&
                           converted_samples <= output_end - prev_size,
                       ErrorCode::DecodeFailed, "FFmpeg flush produced too many samples");
      output.resize(prev_size + converted_samples);
    }
  };

  auto configure_swr_for_frame = [&]() {
    const int input_rate = frame->sample_rate > 0 ? frame->sample_rate : codec_ctx->sample_rate;
    SONARE_CHECK_MSG(input_rate > 0, ErrorCode::DecodeFailed,
                     "FFmpeg: decoded frame has no valid sample rate");
    const auto input_format = static_cast<AVSampleFormat>(frame->format);
    SONARE_CHECK_MSG(input_format != AV_SAMPLE_FMT_NONE, ErrorCode::DecodeFailed,
                     "FFmpeg: decoded frame has no valid sample format");

    ChannelLayoutGuard resolved_layout_guard;
    AVChannelLayout& resolved_layout = resolved_layout_guard.layout;
    const AVChannelLayout* advertised = &frame->ch_layout;
    if (advertised->nb_channels <= 0 || advertised->order == AV_CHANNEL_ORDER_UNSPEC) {
      advertised = &codec_ctx->ch_layout;
    }
    if (advertised->nb_channels > 0 && advertised->order != AV_CHANNEL_ORDER_UNSPEC) {
      ret = av_channel_layout_copy(&resolved_layout, advertised);
      SONARE_CHECK_MSG(ret >= 0, ErrorCode::DecodeFailed,
                       "FFmpeg: av_channel_layout_copy failed: " + ff_err(ret));
    } else {
      const int channels =
          frame->ch_layout.nb_channels > 0
              ? frame->ch_layout.nb_channels
              : (codec_ctx->ch_layout.nb_channels > 0 ? codec_ctx->ch_layout.nb_channels : 1);
      av_channel_layout_default(&resolved_layout, channels);
    }

    if (sample_rate == 0) sample_rate = input_rate;
    const int frame_channels = resolved_layout.nb_channels;
    SONARE_CHECK_MSG(frame_channels > 0, ErrorCode::DecodeFailed,
                     "FFmpeg: decoded frame has no valid channel count");
    const bool input_unchanged =
        active_input_rate > 0 && input_rate == active_input_rate &&
        input_format == active_input_format && active_in_layout.layout.nb_channels > 0 &&
        av_channel_layout_compare(&resolved_layout, &active_in_layout.layout) == 0;
    if (!input_unchanged && include_mono) {
      append_swr_flush(mono_swr.get(), 1, mono_samples);
      mono_swr.reset();
    }
    if (!input_unchanged) {
      av_channel_layout_uninit(&active_in_layout.layout);
      ret = av_channel_layout_copy(&active_in_layout.layout, &resolved_layout);
      SONARE_CHECK_MSG(ret >= 0, ErrorCode::DecodeFailed,
                       "FFmpeg: failed to retain decoded frame channel layout: " + ff_err(ret));

      if (include_mono) {
        SwrContext* mono_raw = nullptr;
        ret = swr_alloc_set_opts2(&mono_raw, &mono_layout, AV_SAMPLE_FMT_FLT, sample_rate,
                                  &active_in_layout.layout, input_format, input_rate, 0, nullptr);
        mono_swr.reset(mono_raw);
        SONARE_CHECK_MSG(ret >= 0 && mono_swr != nullptr, ErrorCode::DecodeFailed,
                         "FFmpeg: swr_alloc_set_opts2 failed: " + ff_err(ret));
        // Rebuilt with the context, so a midstream layout change re-derives the
        // matrix for the new source layout rather than carrying the old one over.
        ret = set_mono_downmix_matrix(mono_swr.get(), active_in_layout.layout);
        SONARE_CHECK_MSG(ret >= 0, ErrorCode::DecodeFailed,
                         "FFmpeg: swr_set_matrix failed: " + ff_err(ret));
        ret = swr_init(mono_swr.get());
        SONARE_CHECK_MSG(ret >= 0, ErrorCode::DecodeFailed,
                         "FFmpeg: swr_init failed: " + ff_err(ret));
      }
    }

    if (retention != RetainChannels::None && retain_enabled && !retained_error) {
      try {
        const bool retained_layout_unchanged =
            retained_swr != nullptr &&
            av_channel_layout_compare(&resolved_layout, &active_retained_layout.layout) == 0;
        if (retained_swr != nullptr && !retained_layout_unchanged) {
          throw SonareException(
              ErrorCode::DecodeFailed,
              "FFmpeg: interleaved decode does not support a channel layout change midstream");
        }
        // A rate/format renegotiation (e.g. AAC SBR) flushes and rebuilds; only a layout change is
        // refused.
        if (retained_swr != nullptr && !input_unchanged) {
          append_swr_flush(retained_swr.get(), retained_channels, retained_samples);
          retained_swr.reset();
        }
        if (retained_swr == nullptr) {
          if (!retained_layout_unchanged) {
            av_channel_layout_uninit(&active_retained_layout.layout);
            ret = av_channel_layout_copy(&active_retained_layout.layout, &resolved_layout);
            SONARE_CHECK_MSG(ret >= 0, ErrorCode::DecodeFailed,
                             "FFmpeg: failed to retain output channel layout: " + ff_err(ret));
          }
          retained_channels = frame_channels;
          SwrContext* retained_raw = nullptr;
          ret = swr_alloc_set_opts2(&retained_raw, &active_retained_layout.layout,
                                    AV_SAMPLE_FMT_FLT, sample_rate, &active_in_layout.layout,
                                    input_format, input_rate, 0, nullptr);
          retained_swr.reset(retained_raw);
          SONARE_CHECK_MSG(ret >= 0 && retained_swr != nullptr, ErrorCode::DecodeFailed,
                           "FFmpeg: swr_alloc_set_opts2 failed: " + ff_err(ret));
          ret = swr_init(retained_swr.get());
          SONARE_CHECK_MSG(ret >= 0, ErrorCode::DecodeFailed,
                           "FFmpeg: swr_init failed: " + ff_err(ret));
        }
      } catch (...) {
        if (!include_mono) throw;
        retained_error = std::current_exception();
        retained_swr.reset();
        retained_samples.clear();
        retain_enabled = false;
      }
    }
    av_channel_layout_uninit(&resolved_layout);
    if (!input_unchanged) {
      active_input_rate = input_rate;
      active_input_format = input_format;
    }
  };

  auto drain_decoder = [&]() {
    while (true) {
      int recv_ret = avcodec_receive_frame(codec_ctx.get(), frame.get());
      if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) {
        return recv_ret;
      }
      if (is_skippable_decode_error(recv_ret)) {
        ++corrupt_skipped;
        continue;
      }
      if (recv_ret < 0) {
        SONARE_CHECK_MSG(false, ErrorCode::DecodeFailed,
                         "FFmpeg: avcodec_receive_frame failed: " + ff_err(recv_ret));
      }

      ++decoded_frames;
      configure_swr_for_frame();
      const auto append_frame = [&](SwrContext* context, int output_channels,
                                    std::vector<float>& output, bool optional) {
        try {
          // Estimate the worst-case number of output samples for this input frame.
          const int64_t delay = swr_get_delay(context, active_input_rate);
          int max_out = static_cast<int>(av_rescale_rnd(delay + frame->nb_samples, sample_rate,
                                                        active_input_rate, AV_ROUND_UP));
          if (max_out <= 0) max_out = frame->nb_samples;

          const size_t prev_size = output.size();
          size_t output_capacity = 0;
          SONARE_CHECK_MSG(prev_size <= resource::kMaxOfflineAudioSamples && output_channels > 0,
                           ErrorCode::DecodeFailed,
                           "FFmpeg output exceeds the offline decode limit");
          SONARE_CHECK_MSG(numeric::checked_size_product(
                               static_cast<size_t>(max_out), static_cast<size_t>(output_channels),
                               resource::kMaxOfflineAudioSamples - prev_size, &output_capacity) &&
                               numeric::checked_add(prev_size, output_capacity, &output_capacity),
                           ErrorCode::DecodeFailed,
                           "FFmpeg decoded frame exceeds the offline decode limit");
          const size_t output_end = output_capacity;
          output.resize(output_end);
          uint8_t* out_ptr = reinterpret_cast<uint8_t*>(output.data() + prev_size);
          const int converted =
              swr_convert(context, &out_ptr, max_out,
                          const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
          if (converted < 0) {
            output.resize(prev_size);
            SONARE_CHECK_MSG(false, ErrorCode::DecodeFailed,
                             "FFmpeg: swr_convert failed: " + ff_err(converted));
          }
          size_t converted_samples = 0;
          SONARE_CHECK_MSG(numeric::checked_size_product(
                               static_cast<size_t>(converted), static_cast<size_t>(output_channels),
                               resource::kMaxOfflineAudioSamples, &converted_samples) &&
                               converted_samples <= output_end - prev_size,
                           ErrorCode::DecodeFailed,
                           "FFmpeg produced too many samples for the output buffer");
          output.resize(prev_size + converted_samples);
          // A short, highly-compressed stream can decode to an unbounded sample pool.
          // Cap total accumulation at the offline decode limit so a crafted file
          // cannot drive an out-of-memory condition.
          SONARE_CHECK_MSG(output.size() <= resource::kMaxOfflineAudioSamples,
                           ErrorCode::DecodeFailed,
                           "FFmpeg decoded more samples than the offline decode limit");
        } catch (...) {
          if (!optional) throw;
          retained_error = std::current_exception();
          retained_swr.reset();
          retained_samples.clear();
          retain_enabled = false;
        }
      };
      if (include_mono) append_frame(mono_swr.get(), 1, mono_samples, false);
      if (retained_swr != nullptr && retain_enabled && !retained_error) {
        append_frame(retained_swr.get(), retained_channels, retained_samples, include_mono);
      }
      av_frame_unref(frame.get());
    }
  };

  while (true) {
    ret = av_read_frame(format_ctx, packet.get());
    if (ret == AVERROR_EOF) {
      break;
    }
    if (ret < 0) {
      SONARE_CHECK_MSG(false, ErrorCode::DecodeFailed,
                       "FFmpeg: av_read_frame failed: " + ff_err(ret));
    }

    if (packet->stream_index == stream_index) {
      int send_ret = avcodec_send_packet(codec_ctx.get(), packet.get());
      // A full decoder is drained and the same packet resent rather than dropped.
      if (send_ret == AVERROR(EAGAIN)) {
        drain_decoder();
        send_ret = avcodec_send_packet(codec_ctx.get(), packet.get());
      }
      if (is_skippable_decode_error(send_ret)) {
        ++corrupt_skipped;
      } else if (send_ret < 0) {
        av_packet_unref(packet.get());
        SONARE_CHECK_MSG(false, ErrorCode::DecodeFailed,
                         "FFmpeg: avcodec_send_packet failed: " + ff_err(send_ret));
      }
      drain_decoder();
    }
    av_packet_unref(packet.get());
  }

  // Flush the decoder.
  ret = avcodec_send_packet(codec_ctx.get(), nullptr);
  SONARE_CHECK_MSG(ret >= 0 || ret == AVERROR_EOF, ErrorCode::DecodeFailed,
                   "FFmpeg: flushing decoder failed: " + ff_err(ret));
  drain_decoder();

  SONARE_CHECK_MSG(decoded_frames > 0, ErrorCode::DecodeFailed,
                   "FFmpeg: no decodable audio frame (" + std::to_string(corrupt_skipped) +
                       " corrupt packets skipped)");

  if (include_mono) append_swr_flush(mono_swr.get(), 1, mono_samples);
  if (retained_swr != nullptr && retain_enabled && !retained_error) {
    try {
      append_swr_flush(retained_swr.get(), retained_channels, retained_samples);
    } catch (...) {
      if (!include_mono) throw;
      retained_error = std::current_exception();
      retained_samples.clear();
      retained_swr.reset();
      retain_enabled = false;
    }
  }

  if (include_mono) {
    SONARE_CHECK_MSG(!mono_samples.empty(), ErrorCode::DecodeFailed,
                     "FFmpeg: decoded zero audio samples");
  }

  DecodedInputData result;
  result.mono = std::move(mono_samples);
  result.sample_rate = sample_rate;
  result.source_channels = metadata_channels;
  result.retained_channels = retained_channels;
  if (retained_swr != nullptr && retain_enabled && !retained_error) {
    result.interleaved = std::move(retained_samples);
  }
  result.metadata_error = metadata_error;
  result.retained_error = retained_error;
  return result;
}

}  // namespace

namespace {

/// @brief An opened in-memory FFmpeg input; members release in reverse order, so the format
///        context closes before the AVIO context and the read state it points at.
struct MemoryInput {
  std::unique_ptr<MemoryIOState> io_state;
  AVIOContextPtr avio;
  AVFormatContextPtr format_ctx;
};

struct AVDictionaryDeleter {
  void operator()(AVDictionary* dict) const { av_dict_free(&dict); }
};
using AVDictionaryPtr = std::unique_ptr<AVDictionary, AVDictionaryDeleter>;

/// @brief Demuxers that read their streams from their own bytes: audio formats and the A/V
///        containers that carry audio. Playlist, concat, image-sequence and manifest demuxers
///        open further inputs named by the data, so none is listed.
constexpr const char* kAudioDemuxerAllowlist =
    "aac,ac3,adx,aiff,amr,amrnb,amrwb,ape,asf,ast,au,avi,caf,dsf,dts,dtshd,eac3,flac,flv,gsm,iff,"
    "ircam,loas,matroska,mlp,mov,mp3,mpc,mpc8,mpeg,mpegts,nistsphere,nut,ogg,qoa,rm,shn,sox,tak,"
    "truehd,tta,voc,w64,wav,wv,xwma";

/// @brief io_open callback that refuses every secondary input a demuxer asks for.
int refuse_secondary_input(AVFormatContext* /*s*/, AVIOContext** /*pb*/, const char* /*url*/,
                           int /*flags*/, AVDictionary** /*options*/) {
  return AVERROR(EPERM);
}

/// @brief Opens @p data as an AVFormat input over custom memory AVIO.
/// @details The only input libavformat may read is the caller's buffer: the demuxer must be on
///          @ref kAudioDemuxerAllowlist, no protocol is whitelisted, and io_open refuses.
/// @throws SonareException on an empty buffer, an allocation failure, or an unreadable container.
MemoryInput open_memory_input(const uint8_t* data, size_t size,
                              ErrorCode invalid_input_code = ErrorCode::DecodeFailed) {
  SONARE_CHECK_MSG(data != nullptr && size > 0, invalid_input_code, "FFmpeg: empty input buffer");

  constexpr int kIOBufferSize = 32 * 1024;
  MemoryInput input;
  input.io_state = std::make_unique<MemoryIOState>();
  input.io_state->data = data;
  input.io_state->size = size;
  input.io_state->pos = 0;

  uint8_t* io_buffer = static_cast<uint8_t*>(av_malloc(kIOBufferSize));
  SONARE_CHECK_MSG(io_buffer != nullptr, ErrorCode::OutOfMemory,
                   "FFmpeg: av_malloc for AVIO buffer failed");

  input.avio.reset(avio_alloc_context(io_buffer, kIOBufferSize, /*write_flag=*/0,
                                      input.io_state.get(), &memory_read, /*write=*/nullptr,
                                      &memory_seek));
  if (input.avio == nullptr) {
    av_free(io_buffer);
    SONARE_CHECK_MSG(false, ErrorCode::OutOfMemory, "FFmpeg: avio_alloc_context returned null");
  }

  AVFormatContext* fmt_raw = avformat_alloc_context();
  SONARE_CHECK_MSG(fmt_raw != nullptr, ErrorCode::OutOfMemory,
                   "FFmpeg: avformat_alloc_context returned null");
  fmt_raw->pb = input.avio.get();
  fmt_raw->flags |= AVFMT_FLAG_CUSTOM_IO;
  fmt_raw->io_open = &refuse_secondary_input;

  input.format_ctx.reset(fmt_raw);

  AVDictionary* options_raw = nullptr;
  int ret = av_dict_set(&options_raw, "format_whitelist", kAudioDemuxerAllowlist, 0);
  if (ret >= 0) ret = av_dict_set(&options_raw, "protocol_whitelist", "", 0);
  AVDictionaryPtr options(options_raw);
  SONARE_CHECK_MSG(ret >= 0, ErrorCode::OutOfMemory, "FFmpeg: av_dict_set failed: " + ff_err(ret));

  options_raw = options.release();
  ret = avformat_open_input(&fmt_raw, /*url=*/nullptr, /*fmt=*/nullptr, &options_raw);
  options.reset(options_raw);
  if (ret < 0) {
    // On failure libavformat already freed the context; release ours to avoid
    // a double-free in the destructor.
    (void)input.format_ctx.release();
    SONARE_CHECK_MSG(false, invalid_input_code,
                     "FFmpeg: avformat_open_input failed (unreadable input, or a container "
                     "outside the audio demuxer allowlist): " +
                         ff_err(ret));
  }
  // After success fmt_raw is unchanged but is now owned by format_ctx.
  return input;
}

DecodedInputData load_buffer_ffmpeg_impl(const uint8_t* data, size_t size, RetainChannels retention,
                                         bool include_mono) {
  const MemoryInput input = open_memory_input(data, size);
  return decode_first_audio_stream(input.format_ctx.get(), retention, include_mono);
}

}  // namespace

AudioLoadResult load_buffer_ffmpeg(const uint8_t* data, size_t size) {
  auto result = load_buffer_ffmpeg_impl(data, size, RetainChannels::None, true);
  return {std::move(result.mono), result.sample_rate};
}

InterleavedAudioLoadResult load_buffer_ffmpeg_interleaved(const uint8_t* data, size_t size) {
  auto result = load_buffer_ffmpeg_impl(data, size, RetainChannels::All, false);
  if (result.retained_error != nullptr) std::rethrow_exception(result.retained_error);
  SONARE_CHECK_MSG(!result.interleaved.empty(), ErrorCode::DecodeFailed,
                   "FFmpeg: retained channel decode is unavailable");
  const int channels =
      result.retained_channels > 0 ? result.retained_channels : result.source_channels;
  return {std::move(result.interleaved), result.sample_rate, channels};
}

DecodedInputData load_buffer_ffmpeg_input(const uint8_t* data, size_t size,
                                          RetainChannels retention) {
  return load_buffer_ffmpeg_impl(data, size, retention, true);
}

int probe_channels_ffmpeg(const uint8_t* data, size_t size) {
  const MemoryInput input = open_memory_input(data, size);
  AVFormatContext* format_ctx = input.format_ctx.get();
  const int ret = avformat_find_stream_info(format_ctx, nullptr);
  SONARE_CHECK_MSG(ret >= 0, ErrorCode::DecodeFailed,
                   "FFmpeg: avformat_find_stream_info failed: " + ff_err(ret));
  for (unsigned i = 0; i < format_ctx->nb_streams; ++i) {
    const AVCodecParameters* params = format_ctx->streams[i]->codecpar;
    if (params->codec_type == AVMEDIA_TYPE_AUDIO) {
      SONARE_CHECK_MSG(params->ch_layout.nb_channels > 0, ErrorCode::DecodeFailed,
                       "FFmpeg: audio stream has no channel layout");
      return params->ch_layout.nb_channels;
    }
  }
  SONARE_CHECK_MSG(false, ErrorCode::DecodeFailed, "FFmpeg: no audio stream found in input");
}

namespace {

struct FFmpegEncodeFormat {
  std::string_view token;
  const char* encoder_name;
  const char* muxer_name;
  int max_channels;
};

constexpr std::array<FFmpegEncodeFormat, 5> kFFmpegEncodeFormats = {{
    {"flac", "flac", "flac", 8},
    {"mp3", "libmp3lame", "mp3", 2},
    {"ogg", "libvorbis", "ogg", 0},
    {"opus", "libopus", "ogg", 8},
    {"m4a", "aac", "mp4", 8},
}};

const FFmpegEncodeFormat* find_ffmpeg_encode_format(std::string_view token) {
  for (const auto& format : kFFmpegEncodeFormats) {
    if (format.token == token) return &format;
  }
  return nullptr;
}

struct EncoderConfigList {
  const void* values = nullptr;
  int count = 0;
};

enum class EncoderConfigKind { SampleRate, SampleFormat, ChannelLayout };

EncoderConfigList encoder_config(const AVCodec* codec, EncoderConfigKind config) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
  AVCodecConfig ffmpeg_config = AV_CODEC_CONFIG_SAMPLE_RATE;
  switch (config) {
    case EncoderConfigKind::SampleRate:
      ffmpeg_config = AV_CODEC_CONFIG_SAMPLE_RATE;
      break;
    case EncoderConfigKind::SampleFormat:
      ffmpeg_config = AV_CODEC_CONFIG_SAMPLE_FORMAT;
      break;
    case EncoderConfigKind::ChannelLayout:
      ffmpeg_config = AV_CODEC_CONFIG_CHANNEL_LAYOUT;
      break;
  }
  EncoderConfigList result;
  if (avcodec_get_supported_config(nullptr, codec, ffmpeg_config, 0, &result.values,
                                   &result.count) < 0) {
    return {};
  }
  return result;
#else
  switch (config) {
    case EncoderConfigKind::SampleRate: {
      const int* values = codec->supported_samplerates;
      if (values == nullptr) return {};
      int count = 0;
      while (values[count] != 0) ++count;
      return {values, count};
    }
    case EncoderConfigKind::SampleFormat: {
      const AVSampleFormat* values = codec->sample_fmts;
      if (values == nullptr) return {};
      int count = 0;
      while (values[count] != AV_SAMPLE_FMT_NONE) ++count;
      return {values, count};
    }
    case EncoderConfigKind::ChannelLayout: {
      const AVChannelLayout* values = codec->ch_layouts;
      if (values == nullptr) return {};
      int count = 0;
      while (values[count].nb_channels != 0) ++count;
      return {values, count};
    }
    default:
      return {};
  }
#endif
}

bool encoder_supports_sample_rate(const AVCodec* codec, int sample_rate) {
  const EncoderConfigList list = encoder_config(codec, EncoderConfigKind::SampleRate);
  if (list.values == nullptr) return true;
  const auto* rates = static_cast<const int*>(list.values);
  return std::find(rates, rates + list.count, sample_rate) != rates + list.count;
}

bool encoder_supports_layout(const AVCodec* codec, const AVChannelLayout& layout) {
  const EncoderConfigList list = encoder_config(codec, EncoderConfigKind::ChannelLayout);
  if (list.values == nullptr) return true;
  const auto* layouts = static_cast<const AVChannelLayout*>(list.values);
  for (int i = 0; i < list.count; ++i) {
    if (av_channel_layout_compare(&layouts[i], &layout) == 0) return true;
  }
  return false;
}

AVSampleFormat encoder_sample_format(const AVCodec* codec, std::string_view format, int bit_depth) {
  const EncoderConfigList list = encoder_config(codec, EncoderConfigKind::SampleFormat);
  if (list.values == nullptr || list.count == 0) return AV_SAMPLE_FMT_FLTP;
  const auto* formats = static_cast<const AVSampleFormat*>(list.values);
  if (format == "flac") {
    const AVSampleFormat preferred16[] = {AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_S32,
                                          AV_SAMPLE_FMT_S32P};
    const AVSampleFormat preferred24[] = {AV_SAMPLE_FMT_S32, AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S16,
                                          AV_SAMPLE_FMT_S16P};
    const AVSampleFormat* preferred = bit_depth == 24 ? preferred24 : preferred16;
    for (size_t i = 0; i < 4; ++i) {
      if (std::find(formats, formats + list.count, preferred[i]) != formats + list.count) {
        return preferred[i];
      }
    }
  }
  return formats[0];
}

class DynamicOutputBuffer {
 public:
  DynamicOutputBuffer() = default;
  DynamicOutputBuffer(const DynamicOutputBuffer&) = delete;
  DynamicOutputBuffer& operator=(const DynamicOutputBuffer&) = delete;

  ~DynamicOutputBuffer() { discard(); }

  int open() { return avio_open_dyn_buf(&context_); }

  AVIOContext* get() const { return context_; }

  int close(uint8_t** data) {
    AVIOContext* context = std::exchange(context_, nullptr);
    return avio_close_dyn_buf(context, data);
  }

 private:
  void discard() {
    if (context_ == nullptr) return;
    uint8_t* data = nullptr;
    (void)avio_close_dyn_buf(std::exchange(context_, nullptr), &data);
    av_free(data);
  }

  AVIOContext* context_ = nullptr;
};

struct AVFreeDeleter {
  void operator()(uint8_t* data) const { av_free(data); }
};

void append_dictionary_tags(std::vector<std::pair<std::string, std::string>>& tags,
                            const AVDictionary* dictionary) {
  const AVDictionaryEntry* entry = nullptr;
  while ((entry = av_dict_get(dictionary, "", entry, AV_DICT_IGNORE_SUFFIX)) != nullptr) {
    const std::pair<std::string, std::string> value(entry->key, entry->value);
    if (std::find(tags.begin(), tags.end(), value) == tags.end()) tags.push_back(value);
  }
}

void write_tags(AVDictionary** destination, const std::map<std::string, std::string>& tags) {
  for (const auto& [key, value] : tags) {
    const int ret = av_dict_set(destination, key.c_str(), value.c_str(), 0);
    SONARE_CHECK_MSG(ret >= 0, ErrorCode::EncodeFailed,
                     "FFmpeg: could not set output tag " + key + ": " + ff_err(ret));
  }
}

AVFramePtr make_audio_frame(AVSampleFormat sample_format, const AVChannelLayout& layout,
                            int sample_rate, int capacity) {
  AVFramePtr frame(av_frame_alloc());
  SONARE_CHECK_MSG(frame != nullptr, ErrorCode::OutOfMemory,
                   "FFmpeg: av_frame_alloc returned null");
  frame->format = sample_format;
  frame->sample_rate = sample_rate;
  frame->nb_samples = capacity;
  const int copy_result = av_channel_layout_copy(&frame->ch_layout, &layout);
  SONARE_CHECK_MSG(copy_result >= 0, ErrorCode::EncodeFailed,
                   "FFmpeg: av_channel_layout_copy failed: " + ff_err(copy_result));
  const int buffer_result = av_frame_get_buffer(frame.get(), 0);
  SONARE_CHECK_MSG(buffer_result >= 0, ErrorCode::OutOfMemory,
                   "FFmpeg: av_frame_get_buffer failed: " + ff_err(buffer_result));
  return frame;
}

void check_output_io(AVFormatContext* context) {
  SONARE_CHECK_MSG(
      context->pb == nullptr || context->pb->error >= 0, ErrorCode::EncodeFailed,
      "FFmpeg: output AVIO failed: " + ff_err(context->pb == nullptr ? 0 : context->pb->error));
}

}  // namespace

std::vector<std::pair<std::string, std::string>> read_raw_audio_tags_ffmpeg(const uint8_t* data,
                                                                            size_t size) {
  const MemoryInput input = open_memory_input(data, size, ErrorCode::InvalidFormat);
  AVFormatContext* format_context = input.format_ctx.get();
  AVStream* first_audio_stream = nullptr;
  for (unsigned int i = 0; i < format_context->nb_streams; ++i) {
    AVStream* stream = format_context->streams[i];
    if (stream != nullptr && stream->codecpar != nullptr &&
        stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      first_audio_stream = stream;
      break;
    }
  }
  SONARE_CHECK_MSG(first_audio_stream != nullptr, ErrorCode::InvalidFormat,
                   "FFmpeg: input has no audio stream");

  std::vector<std::pair<std::string, std::string>> tags;
  append_dictionary_tags(tags, format_context->metadata);
  append_dictionary_tags(tags, first_audio_stream->metadata);
  return tags;
}

bool ffmpeg_audio_encode_available(std::string_view format) noexcept {
  const FFmpegEncodeFormat* spec = find_ffmpeg_encode_format(format);
  if (spec == nullptr) return false;
  return avcodec_find_encoder_by_name(spec->encoder_name) != nullptr &&
         av_guess_format(spec->muxer_name, nullptr, nullptr) != nullptr;
}

std::vector<uint8_t> encode_audio_ffmpeg(const float* planar, size_t frames, int num_channels,
                                         int sample_rate, std::string_view format, int bit_depth,
                                         float quality,
                                         const std::map<std::string, std::string>& tags) {
  const FFmpegEncodeFormat* spec = find_ffmpeg_encode_format(format);
  SONARE_CHECK_MSG(spec != nullptr, ErrorCode::InvalidParameter,
                   "format must be flac, mp3, ogg, opus, or m4a");
  const AVCodec* codec = avcodec_find_encoder_by_name(spec->encoder_name);
  const AVOutputFormat* muxer = av_guess_format(spec->muxer_name, nullptr, nullptr);
  if (codec == nullptr || muxer == nullptr) detail::refuse_unavailable_audio_encoder();

  if (spec->max_channels > 0 && num_channels > spec->max_channels) {
    throw SonareParameterRefusal::predicate(
        ErrorCode::InvalidParameter, "num_channels",
        "must be at most " + std::to_string(spec->max_channels) + " for this format");
  }
  ChannelLayoutGuard input_layout;
  av_channel_layout_default(&input_layout.layout, num_channels);
  if (input_layout.layout.nb_channels != num_channels ||
      !encoder_supports_layout(codec, input_layout.layout)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, "num_channels",
                                            "is not supported by the encoder for this format");
  }
  if (!encoder_supports_sample_rate(codec, sample_rate)) {
    throw SonareParameterRefusal::predicate(ErrorCode::InvalidParameter, "sample_rate",
                                            "is not supported by the encoder for this format");
  }

  const AVSampleFormat sample_format = encoder_sample_format(codec, format, bit_depth);
  AVCodecContextPtr codec_context(avcodec_alloc_context3(codec));
  SONARE_CHECK_MSG(codec_context != nullptr, ErrorCode::OutOfMemory,
                   "FFmpeg: avcodec_alloc_context3 returned null");
  codec_context->sample_rate = sample_rate;
  codec_context->sample_fmt = sample_format;
  codec_context->time_base = AVRational{1, sample_rate};
  const int layout_result = av_channel_layout_copy(&codec_context->ch_layout, &input_layout.layout);
  SONARE_CHECK_MSG(layout_result >= 0, ErrorCode::EncodeFailed,
                   "FFmpeg: av_channel_layout_copy failed: " + ff_err(layout_result));

  if (format == "flac") {
    codec_context->bits_per_raw_sample = bit_depth;
  } else if (format == "mp3") {
    const int q = static_cast<int>(std::lround(9.0 * (1.0 - static_cast<double>(quality))));
    codec_context->flags |= AV_CODEC_FLAG_QSCALE;
    codec_context->global_quality = q * FF_QP2LAMBDA;
  } else if (format == "ogg") {
    const double q = -1.0 + 11.0 * static_cast<double>(quality);
    codec_context->flags |= AV_CODEC_FLAG_QSCALE;
    codec_context->global_quality = static_cast<int>(std::lround(q * FF_QP2LAMBDA));
  } else {
    const double kilobits_per_second = format == "opus"
                                           ? 32.0 + 288.0 * static_cast<double>(quality)
                                           : 64.0 + 256.0 * static_cast<double>(quality);
    codec_context->bit_rate = static_cast<int64_t>(
        std::llround(kilobits_per_second * static_cast<double>(std::kilo::num)));
  }
  if ((muxer->flags & AVFMT_GLOBALHEADER) != 0) codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  int ret = avcodec_open2(codec_context.get(), codec, nullptr);
  SONARE_CHECK_MSG(ret >= 0, ErrorCode::EncodeFailed,
                   "FFmpeg: avcodec_open2 failed: " + ff_err(ret));

  AVFormatContext* output_raw = nullptr;
  ret = avformat_alloc_output_context2(&output_raw, muxer, nullptr, nullptr);
  AVOutputContextPtr output_context(output_raw);
  SONARE_CHECK_MSG(ret >= 0 && output_context != nullptr, ErrorCode::EncodeFailed,
                   "FFmpeg: avformat_alloc_output_context2 failed: " + ff_err(ret));

  AVStream* stream = avformat_new_stream(output_context.get(), nullptr);
  SONARE_CHECK_MSG(stream != nullptr, ErrorCode::OutOfMemory,
                   "FFmpeg: avformat_new_stream returned null");
  stream->time_base = codec_context->time_base;
  ret = avcodec_parameters_from_context(stream->codecpar, codec_context.get());
  SONARE_CHECK_MSG(ret >= 0, ErrorCode::EncodeFailed,
                   "FFmpeg: avcodec_parameters_from_context failed: " + ff_err(ret));
  write_tags(&output_context->metadata, tags);
  write_tags(&stream->metadata, tags);

  DynamicOutputBuffer output_buffer;
  ret = output_buffer.open();
  SONARE_CHECK_MSG(ret >= 0, ErrorCode::OutOfMemory,
                   "FFmpeg: avio_open_dyn_buf failed: " + ff_err(ret));
  output_context->pb = output_buffer.get();
  output_context->flags |= AVFMT_FLAG_CUSTOM_IO;
  SONARE_CHECK_MSG((output_context->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0,
                   ErrorCode::EncodeFailed, "FFmpeg: dynamic output AVIO is not seekable");

  AVDictionary* muxer_options_raw = nullptr;
  if (format == "m4a") {
    ret = av_dict_set(&muxer_options_raw, "movflags", "use_metadata_tags", 0);
  }
  AVDictionaryPtr muxer_options(muxer_options_raw);
  SONARE_CHECK_MSG(ret >= 0, ErrorCode::OutOfMemory, "FFmpeg: av_dict_set failed: " + ff_err(ret));
  muxer_options_raw = muxer_options.release();
  ret = avformat_write_header(output_context.get(), &muxer_options_raw);
  muxer_options.reset(muxer_options_raw);
  SONARE_CHECK_MSG(ret >= 0, ErrorCode::EncodeFailed,
                   "FFmpeg: avformat_write_header failed: " + ff_err(ret));
  check_output_io(output_context.get());

  AVPacketPtr packet(av_packet_alloc());
  SONARE_CHECK_MSG(packet != nullptr, ErrorCode::OutOfMemory,
                   "FFmpeg: av_packet_alloc returned null");
  auto drain_encoder = [&]() {
    while (true) {
      const int receive_result = avcodec_receive_packet(codec_context.get(), packet.get());
      if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) return;
      SONARE_CHECK_MSG(receive_result >= 0, ErrorCode::EncodeFailed,
                       "FFmpeg: avcodec_receive_packet failed: " + ff_err(receive_result));
      av_packet_rescale_ts(packet.get(), codec_context->time_base, stream->time_base);
      packet->stream_index = stream->index;
      const int write_result = av_interleaved_write_frame(output_context.get(), packet.get());
      av_packet_unref(packet.get());
      SONARE_CHECK_MSG(write_result >= 0, ErrorCode::EncodeFailed,
                       "FFmpeg: av_interleaved_write_frame failed: " + ff_err(write_result));
      check_output_io(output_context.get());
    }
  };
  auto send_frame = [&](AVFrame* frame) {
    int send_result = avcodec_send_frame(codec_context.get(), frame);
    if (send_result == AVERROR(EAGAIN)) {
      drain_encoder();
      send_result = avcodec_send_frame(codec_context.get(), frame);
    }
    SONARE_CHECK_MSG(send_result >= 0, ErrorCode::EncodeFailed,
                     "FFmpeg: avcodec_send_frame failed: " + ff_err(send_result));
    drain_encoder();
  };

  const int frame_size = codec_context->frame_size;
  const int chunk_size = frame_size > 0 ? frame_size : 4096;
  const int initial_capacity = std::max(chunk_size, 1);
  AVAudioFifoPtr fifo(av_audio_fifo_alloc(sample_format, num_channels, initial_capacity));
  SONARE_CHECK_MSG(fifo != nullptr, ErrorCode::OutOfMemory,
                   "FFmpeg: av_audio_fifo_alloc returned null");
  SwrContext* resampler_raw = nullptr;
  ret = swr_alloc_set_opts2(&resampler_raw, &codec_context->ch_layout, sample_format, sample_rate,
                            &input_layout.layout, AV_SAMPLE_FMT_FLTP, sample_rate, 0, nullptr);
  SwrContextPtr resampler(resampler_raw);
  SONARE_CHECK_MSG(ret >= 0 && resampler != nullptr, ErrorCode::EncodeFailed,
                   "FFmpeg: swr_alloc_set_opts2 failed: " + ff_err(ret));
  ret = swr_init(resampler.get());
  SONARE_CHECK_MSG(ret >= 0, ErrorCode::EncodeFailed, "FFmpeg: swr_init failed: " + ff_err(ret));

  const int conversion_capacity = swr_get_out_samples(resampler.get(), chunk_size);
  SONARE_CHECK_MSG(conversion_capacity > 0, ErrorCode::EncodeFailed,
                   "FFmpeg: swr_get_out_samples returned an invalid capacity");
  AVFramePtr conversion_frame =
      make_audio_frame(sample_format, codec_context->ch_layout, sample_rate, conversion_capacity);
  auto append_fifo = [&](AVFrame* frame, int samples) {
    if (samples <= 0) return;
    const int current_size = av_audio_fifo_size(fifo.get());
    SONARE_CHECK_MSG(samples <= std::numeric_limits<int>::max() - current_size,
                     ErrorCode::EncodeFailed, "FFmpeg: audio FIFO size overflow");
    ret = av_audio_fifo_realloc(fifo.get(), current_size + samples);
    SONARE_CHECK_MSG(ret >= 0, ErrorCode::OutOfMemory,
                     "FFmpeg: av_audio_fifo_realloc failed: " + ff_err(ret));
    ret = av_audio_fifo_write(fifo.get(), reinterpret_cast<void**>(frame->extended_data), samples);
    SONARE_CHECK_MSG(ret == samples, ErrorCode::EncodeFailed, "FFmpeg: short write to audio FIFO");
  };

  std::vector<const uint8_t*> input_planes(static_cast<size_t>(num_channels));
  size_t input_offset = 0;
  while (input_offset < frames) {
    const int input_count =
        static_cast<int>(std::min(frames - input_offset, static_cast<size_t>(chunk_size)));
    for (int channel = 0; channel < num_channels; ++channel) {
      const size_t plane_offset = static_cast<size_t>(channel) * frames + input_offset;
      input_planes[static_cast<size_t>(channel)] =
          reinterpret_cast<const uint8_t*>(planar + plane_offset);
    }
    ret = av_frame_make_writable(conversion_frame.get());
    SONARE_CHECK_MSG(ret >= 0, ErrorCode::EncodeFailed,
                     "FFmpeg: av_frame_make_writable failed: " + ff_err(ret));
    const int converted = swr_convert(resampler.get(), conversion_frame->extended_data,
                                      conversion_capacity, input_planes.data(), input_count);
    SONARE_CHECK_MSG(converted >= 0, ErrorCode::EncodeFailed,
                     "FFmpeg: swr_convert failed: " + ff_err(converted));
    append_fifo(conversion_frame.get(), converted);
    input_offset += static_cast<size_t>(input_count);
  }

  while (true) {
    const int flush_capacity = swr_get_out_samples(resampler.get(), 0);
    SONARE_CHECK_MSG(flush_capacity >= 0, ErrorCode::EncodeFailed,
                     "FFmpeg: swr_get_out_samples failed during flush");
    if (flush_capacity == 0) break;
    if (flush_capacity > conversion_frame->nb_samples) {
      conversion_frame =
          make_audio_frame(sample_format, codec_context->ch_layout, sample_rate, flush_capacity);
    }
    ret = av_frame_make_writable(conversion_frame.get());
    SONARE_CHECK_MSG(ret >= 0, ErrorCode::EncodeFailed,
                     "FFmpeg: av_frame_make_writable failed: " + ff_err(ret));
    const int converted = swr_convert(resampler.get(), conversion_frame->extended_data,
                                      conversion_frame->nb_samples, nullptr, 0);
    SONARE_CHECK_MSG(converted >= 0, ErrorCode::EncodeFailed,
                     "FFmpeg: swr_convert flush failed: " + ff_err(converted));
    if (converted == 0) break;
    append_fifo(conversion_frame.get(), converted);
  }

  AVFramePtr encode_frame = make_audio_frame(sample_format, codec_context->ch_layout, sample_rate,
                                             std::max(frame_size, chunk_size));
  int64_t next_pts = 0;
  auto encode_fifo_samples = [&](int samples_to_encode, int samples_to_read) {
    ret = av_frame_make_writable(encode_frame.get());
    SONARE_CHECK_MSG(ret >= 0, ErrorCode::EncodeFailed,
                     "FFmpeg: av_frame_make_writable failed: " + ff_err(ret));
    encode_frame->nb_samples = samples_to_encode;
    encode_frame->pts = next_pts;
    av_samples_set_silence(encode_frame->extended_data, 0, samples_to_encode, num_channels,
                           sample_format);
    if (samples_to_read > 0) {
      ret = av_audio_fifo_read(fifo.get(), reinterpret_cast<void**>(encode_frame->extended_data),
                               samples_to_read);
      SONARE_CHECK_MSG(ret == samples_to_read, ErrorCode::EncodeFailed,
                       "FFmpeg: short read from audio FIFO");
    }
    next_pts += samples_to_encode;
    send_frame(encode_frame.get());
  };

  const int output_frame_size = frame_size > 0 ? frame_size : chunk_size;
  while (av_audio_fifo_size(fifo.get()) >= output_frame_size) {
    encode_fifo_samples(output_frame_size, output_frame_size);
  }
  const int remaining = av_audio_fifo_size(fifo.get());
  if (remaining > 0) {
    const bool can_encode_short = frame_size <= 0 ||
                                  (codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) != 0 ||
                                  (codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) != 0;
    encode_fifo_samples(can_encode_short ? remaining : output_frame_size, remaining);
  }

  ret = avcodec_send_frame(codec_context.get(), nullptr);
  if (ret == AVERROR(EAGAIN)) {
    drain_encoder();
    ret = avcodec_send_frame(codec_context.get(), nullptr);
  }
  SONARE_CHECK_MSG(ret >= 0 || ret == AVERROR_EOF, ErrorCode::EncodeFailed,
                   "FFmpeg: flushing encoder failed: " + ff_err(ret));
  drain_encoder();

  ret = av_write_trailer(output_context.get());
  SONARE_CHECK_MSG(ret >= 0, ErrorCode::EncodeFailed,
                   "FFmpeg: av_write_trailer failed: " + ff_err(ret));
  check_output_io(output_context.get());
  avio_flush(output_context->pb);
  check_output_io(output_context.get());
  output_context->pb = nullptr;

  uint8_t* encoded_data = nullptr;
  const int encoded_size = output_buffer.close(&encoded_data);
  std::unique_ptr<uint8_t, AVFreeDeleter> encoded_buffer(encoded_data);
  SONARE_CHECK_MSG(encoded_size > 0 && encoded_buffer != nullptr, ErrorCode::EncodeFailed,
                   "FFmpeg: dynamic output buffer is empty");
  return std::vector<uint8_t>(encoded_buffer.get(), encoded_buffer.get() + encoded_size);
}

}  // namespace sonare
