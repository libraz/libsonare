#include "streaming/stream_resampler.h"

#include <algorithm>

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

  if (impl_->passthrough) {
    out.insert(out.end(), samples, samples + n_samples);
    return;
  }

  // Feed the persistent resampler in fixed-size blocks. Because the same
  // resampler instance is reused across process() calls, its poly-phase filter
  // history carries over and chunk boundaries join seamlessly (no click, no
  // drift). Output emerges with a constant start-up latency that we do not
  // attempt to flush away — flushing with zeros is exactly what would corrupt a
  // continuous stream.
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
      const size_t prev = out.size();
      out.resize(prev + static_cast<size_t>(output_len));
      for (int i = 0; i < output_len; ++i) {
        out[prev + static_cast<size_t>(i)] = static_cast<float>(output_ptr[i]);
      }
    }

    offset += static_cast<size_t>(block_len);
  }
}

void StreamResampler::reset() {
  if (impl_->resampler) {
    impl_->resampler->clear();
  }
}

}  // namespace sonare::streaming_detail
