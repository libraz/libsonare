#include "streaming/stream_resampler.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "CDSPResampler.h"
#include "util/exception.h"

namespace sonare::streaming_detail {

namespace {
/// Block size fed to the resampler per process() iteration. Matches the value
/// used by the one-shot core resampler so the underlying filter is built with
/// the same MaxInLen characteristics.
constexpr int kBlockSize = 1024;
}  // namespace

struct StreamResampler::Impl {
  int src_sr;
  int dst_sr;
  bool passthrough;
  std::unique_ptr<r8b::CDSPResampler24> resampler;
  std::vector<double> input_block;  ///< Reused double-precision input scratch.
  size_t total_input_samples = 0;
  size_t total_output_samples = 0;
};

StreamResampler::StreamResampler(int src_sr, int dst_sr) : impl_(std::make_unique<Impl>()) {
  SONARE_CHECK(src_sr > 0 && dst_sr > 0, ErrorCode::InvalidParameter);
  impl_->src_sr = src_sr;
  impl_->dst_sr = dst_sr;
  impl_->passthrough = (src_sr == dst_sr);
  if (!impl_->passthrough) {
    impl_->resampler = std::make_unique<r8b::CDSPResampler24>(
        static_cast<double>(src_sr), static_cast<double>(dst_sr), kBlockSize);
  }
  impl_->input_block.reserve(kBlockSize);
}

StreamResampler::~StreamResampler() = default;
StreamResampler::StreamResampler(StreamResampler&&) noexcept = default;
StreamResampler& StreamResampler::operator=(StreamResampler&&) noexcept = default;

void StreamResampler::process(const float* samples, size_t n_samples, std::vector<float>& out) {
  if (samples == nullptr || n_samples == 0) {
    return;
  }
  if (n_samples > std::numeric_limits<size_t>::max() - impl_->total_input_samples) {
    throw SonareException(ErrorCode::InvalidParameter, "stream resampler input count overflows");
  }
  impl_->total_input_samples += n_samples;

  if (impl_->passthrough) {
    out.insert(out.end(), samples, samples + n_samples);
    if (n_samples > std::numeric_limits<size_t>::max() - impl_->total_output_samples) {
      throw SonareException(ErrorCode::InvalidParameter, "stream resampler output count overflows");
    }
    impl_->total_output_samples += n_samples;
    return;
  }

  // Feed the persistent resampler in fixed-size blocks. Because the same
  // resampler instance is reused across process() calls, its poly-phase filter
  // history carries over and chunk boundaries join seamlessly (no click, no
  // drift). Output emerges with a constant start-up latency that we do not
  // attempt to flush mid-stream. End-of-stream latency is drained exactly once
  // by finalize().
  size_t offset = 0;
  while (offset < n_samples) {
    const int block_len =
        static_cast<int>(std::min(n_samples - offset, static_cast<size_t>(kBlockSize)));

    impl_->input_block.resize(static_cast<size_t>(block_len));
    for (int i = 0; i < block_len; ++i) {
      impl_->input_block[static_cast<size_t>(i)] = static_cast<double>(samples[offset + i]);
    }

    double* output_ptr = nullptr;
    const int output_len =
        impl_->resampler->process(impl_->input_block.data(), block_len, output_ptr);

    if (output_len > 0 && output_ptr != nullptr) {
      if (static_cast<size_t>(output_len) >
          std::numeric_limits<size_t>::max() - impl_->total_output_samples) {
        throw SonareException(ErrorCode::InvalidParameter,
                              "stream resampler output count overflows");
      }
      const size_t prev = out.size();
      out.resize(prev + static_cast<size_t>(output_len));
      for (int i = 0; i < output_len; ++i) {
        out[prev + static_cast<size_t>(i)] = static_cast<float>(output_ptr[i]);
      }
      impl_->total_output_samples += static_cast<size_t>(output_len);
    }

    offset += static_cast<size_t>(block_len);
  }
}

void StreamResampler::finalize(std::vector<float>& out) {
  if (impl_->passthrough || impl_->total_input_samples == 0) {
    return;
  }

  const long double exact_output = static_cast<long double>(impl_->total_input_samples) *
                                   static_cast<long double>(impl_->dst_sr) /
                                   static_cast<long double>(impl_->src_sr);
  if (!std::isfinite(exact_output) ||
      exact_output > static_cast<long double>(std::numeric_limits<size_t>::max()) - 0.5L) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "stream resampler analytic output length overflows");
  }
  const size_t expected_output = static_cast<size_t>(std::round(exact_output));
  if (impl_->total_output_samples >= expected_output) {
    return;
  }
  if (expected_output > static_cast<size_t>(std::numeric_limits<int>::max())) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "stream resampler final output exceeds supported drain length");
  }
  // r8brain's drain-length helper returns int input samples. Downsampling can
  // require substantially more input than output, so constrain the requested
  // output by the source/destination ratio before calling it; otherwise a very
  // long high-rate stream could overflow inside the helper despite fitting the
  // output-side int check above. kBlockSize leaves room for filter latency.
  const long double max_helper_output =
      static_cast<long double>(std::numeric_limits<int>::max() - kBlockSize) *
      static_cast<long double>(impl_->dst_sr) / static_cast<long double>(impl_->src_sr);
  if (static_cast<long double>(expected_output) > max_helper_output) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "stream resampler final input exceeds supported drain length");
  }
  const int required_input_int =
      impl_->resampler->getInputRequiredForOutput(static_cast<int>(expected_output));
  if (required_input_int < 0) {
    throw SonareException(ErrorCode::InvalidState,
                          "stream resampler returned an invalid drain length");
  }
  const size_t required_input = static_cast<size_t>(required_input_int);
  size_t flush_remaining =
      required_input > impl_->total_input_samples ? required_input - impl_->total_input_samples : 0;

  impl_->input_block.assign(kBlockSize, 0.0);
  while (impl_->total_output_samples < expected_output && flush_remaining > 0) {
    const size_t block_size = std::min(flush_remaining, static_cast<size_t>(kBlockSize));
    double* output_ptr = nullptr;
    const int output_len = impl_->resampler->process(impl_->input_block.data(),
                                                     static_cast<int>(block_size), output_ptr);
    if (output_len > 0 && output_ptr != nullptr) {
      const size_t remaining_output = expected_output - impl_->total_output_samples;
      const size_t append_count = std::min(remaining_output, static_cast<size_t>(output_len));
      const size_t previous_size = out.size();
      out.resize(previous_size + append_count);
      for (size_t i = 0; i < append_count; ++i) {
        out[previous_size + i] = static_cast<float>(output_ptr[i]);
      }
      impl_->total_output_samples += append_count;
    }
    flush_remaining -= block_size;
  }
  if (impl_->total_output_samples != expected_output) {
    throw SonareException(ErrorCode::InvalidState,
                          "stream resampler could not drain to the analytic output length");
  }
}

void StreamResampler::reset() {
  if (impl_->resampler) {
    impl_->resampler->clear();
  }
  impl_->total_input_samples = 0;
  impl_->total_output_samples = 0;
}

}  // namespace sonare::streaming_detail
