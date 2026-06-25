#include "core/audio_io_ffmpeg.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "util/exception.h"

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
// Core decode loop shared between file- and memory-based entry points.
// ---------------------------------------------------------------------------

/// @brief Decodes the first audio stream of @p format_ctx into mono float32.
/// @return Tuple of (samples, sample_rate).
AudioLoadResult decode_first_audio_stream(AVFormatContext* format_ctx) {
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

  int sample_rate = codec_ctx->sample_rate;
  SONARE_CHECK_MSG(sample_rate > 0, ErrorCode::DecodeFailed,
                   "FFmpeg: invalid sample rate from decoder");

  // Configure swresample: any input layout/format -> mono float32 planar.
  AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_MONO;
  AVChannelLayout in_layout;
  // av_channel_layout_copy copies the layout descriptor and any heap state.
  ret = av_channel_layout_copy(&in_layout, &codec_ctx->ch_layout);
  SONARE_CHECK_MSG(ret >= 0, ErrorCode::DecodeFailed,
                   "FFmpeg: av_channel_layout_copy failed: " + ff_err(ret));
  // If the decoder did not advertise a layout, derive a default from the
  // channel count so swr_alloc_set_opts2 has something usable.
  if (in_layout.nb_channels == 0 || in_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_default(
        &in_layout, codec_ctx->ch_layout.nb_channels > 0 ? codec_ctx->ch_layout.nb_channels : 1);
  }

  SwrContext* swr_raw = nullptr;
  ret = swr_alloc_set_opts2(&swr_raw, &out_layout, AV_SAMPLE_FMT_FLT, sample_rate, &in_layout,
                            codec_ctx->sample_fmt, sample_rate, 0, nullptr);
  av_channel_layout_uninit(&in_layout);
  SwrContextPtr swr(swr_raw);
  SONARE_CHECK_MSG(ret >= 0 && swr != nullptr, ErrorCode::DecodeFailed,
                   "FFmpeg: swr_alloc_set_opts2 failed: " + ff_err(ret));

  ret = swr_init(swr.get());
  SONARE_CHECK_MSG(ret >= 0, ErrorCode::DecodeFailed, "FFmpeg: swr_init failed: " + ff_err(ret));

  AVPacketPtr packet(av_packet_alloc());
  SONARE_CHECK_MSG(packet != nullptr, ErrorCode::OutOfMemory,
                   "FFmpeg: av_packet_alloc returned null");
  AVFramePtr frame(av_frame_alloc());
  SONARE_CHECK_MSG(frame != nullptr, ErrorCode::OutOfMemory,
                   "FFmpeg: av_frame_alloc returned null");

  std::vector<float> samples;

  auto drain_decoder = [&]() {
    while (true) {
      int recv_ret = avcodec_receive_frame(codec_ctx.get(), frame.get());
      if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) {
        return recv_ret;
      }
      if (recv_ret < 0) {
        SONARE_CHECK_MSG(false, ErrorCode::DecodeFailed,
                         "FFmpeg: avcodec_receive_frame failed: " + ff_err(recv_ret));
      }

      // Estimate the worst-case number of output samples for this input frame.
      int64_t delay = swr_get_delay(swr.get(), sample_rate);
      int max_out = static_cast<int>(
          av_rescale_rnd(delay + frame->nb_samples, sample_rate, sample_rate, AV_ROUND_UP));
      if (max_out <= 0) {
        max_out = frame->nb_samples;
      }

      size_t prev_size = samples.size();
      samples.resize(prev_size + static_cast<size_t>(max_out));
      uint8_t* out_ptr = reinterpret_cast<uint8_t*>(samples.data() + prev_size);

      int converted =
          swr_convert(swr.get(), &out_ptr, max_out,
                      const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
      if (converted < 0) {
        samples.resize(prev_size);
        SONARE_CHECK_MSG(false, ErrorCode::DecodeFailed,
                         "FFmpeg: swr_convert failed: " + ff_err(converted));
      }
      samples.resize(prev_size + static_cast<size_t>(converted));
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
      if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
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

  // Flush swresample with a null input to retrieve any remaining samples.
  while (true) {
    int max_out = static_cast<int>(swr_get_delay(swr.get(), sample_rate));
    if (max_out <= 0) {
      break;
    }
    size_t prev_size = samples.size();
    samples.resize(prev_size + static_cast<size_t>(max_out));
    uint8_t* out_ptr = reinterpret_cast<uint8_t*>(samples.data() + prev_size);
    int converted = swr_convert(swr.get(), &out_ptr, max_out, nullptr, 0);
    if (converted <= 0) {
      samples.resize(prev_size);
      break;
    }
    samples.resize(prev_size + static_cast<size_t>(converted));
  }

  SONARE_CHECK_MSG(!samples.empty(), ErrorCode::DecodeFailed, "FFmpeg: decoded zero audio samples");

  return {std::move(samples), sample_rate};
}

}  // namespace

AudioLoadResult load_buffer_ffmpeg(const uint8_t* data, size_t size) {
  SONARE_CHECK_MSG(data != nullptr && size > 0, ErrorCode::InvalidParameter,
                   "FFmpeg: empty input buffer");

  constexpr int kIOBufferSize = 32 * 1024;
  auto io_state = std::make_unique<MemoryIOState>();
  io_state->data = data;
  io_state->size = size;
  io_state->pos = 0;

  uint8_t* io_buffer = static_cast<uint8_t*>(av_malloc(kIOBufferSize));
  SONARE_CHECK_MSG(io_buffer != nullptr, ErrorCode::OutOfMemory,
                   "FFmpeg: av_malloc for AVIO buffer failed");

  AVIOContextPtr avio(avio_alloc_context(io_buffer, kIOBufferSize, /*write_flag=*/0, io_state.get(),
                                         &memory_read, /*write=*/nullptr, &memory_seek));
  if (avio == nullptr) {
    av_free(io_buffer);
    SONARE_CHECK_MSG(false, ErrorCode::OutOfMemory, "FFmpeg: avio_alloc_context returned null");
  }

  AVFormatContext* fmt_raw = avformat_alloc_context();
  SONARE_CHECK_MSG(fmt_raw != nullptr, ErrorCode::OutOfMemory,
                   "FFmpeg: avformat_alloc_context returned null");
  fmt_raw->pb = avio.get();
  fmt_raw->flags |= AVFMT_FLAG_CUSTOM_IO;

  AVFormatContextPtr format_ctx(fmt_raw);

  int ret = avformat_open_input(&fmt_raw, /*url=*/nullptr, /*fmt=*/nullptr, /*options=*/nullptr);
  // avformat_open_input nulls fmt_raw on success and sets it to the same
  // pointer on failure-then-cleanup, so re-seat the unique_ptr in both cases.
  if (ret < 0) {
    // On failure libavformat already freed the context; release ours to avoid
    // a double-free in the destructor.
    (void)format_ctx.release();
    SONARE_CHECK_MSG(false, ErrorCode::DecodeFailed,
                     "FFmpeg: avformat_open_input failed: " + ff_err(ret));
  }
  // After success fmt_raw is unchanged but is now owned by format_ctx.

  return decode_first_audio_stream(format_ctx.get());
}

}  // namespace sonare
