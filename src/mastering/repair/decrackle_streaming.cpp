#include "mastering/repair/decrackle_streaming.h"

#include <algorithm>

#include "mastering/common/prepare_args.h"
#include "mastering/dynamics/channel_limits.h"
#include "util/exception.h"
#include "util/validated.h"

namespace sonare::mastering::repair {
namespace {

DecrackleConfig streaming_config(const DecrackleConfig& config) {
  const auto validated = Validated<DecrackleConfig>::make(config);
  if (validated->mode != DecrackleMode::Median) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "decrackle mode must be median when decrackling a stream: the wavelet "
                          "mode shrinks a transform of the whole signal");
  }
  return validated.get();
}

}  // namespace

StreamingDecrackle::StreamingDecrackle(const DecrackleConfig& config)
    : config_(streaming_config(config)) {}

void StreamingDecrackle::prepare(double sample_rate, int max_block_size) {
  prepare(sample_rate, max_block_size, static_cast<int>(dynamics::kRealtimePreparedChannels));
}

void StreamingDecrackle::prepare(double sample_rate, int max_block_size, int max_channels) {
  validate_prepare_args(sample_rate, max_block_size, max_channels, "StreamingDecrackle");
  max_channels_ = max_channels;
  before_.assign(static_cast<std::size_t>(max_channels_), 0.0f);
  held_.assign(static_cast<std::size_t>(max_channels_), 0.0f);
  prepared_ = true;
  reset();
}

void StreamingDecrackle::process(float* const* channels, int num_channels, int num_samples) {
  ensure_prepared(prepared_, "StreamingDecrackle");
  if (!validate_process_buffers(channels, num_channels, num_samples)) return;
  if (num_channels > max_channels_) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "num_channels exceeds prepared StreamingDecrackle capacity");
  }
  for (int ch = 0; ch < num_channels; ++ch) {
    float* data = channels[ch];
    float before = before_[static_cast<std::size_t>(ch)];
    float held = held_[static_cast<std::size_t>(ch)];
    std::size_t seen = samples_seen_;
    for (int i = 0; i < num_samples; ++i) {
      const float next = data[i];
      // The first sample has no left neighbour and passes through, as offline.
      float out = 0.0f;
      if (seen == 1) {
        out = held;
      } else if (seen >= 2) {
        float median = 0.0f;
        out =
            detail::crackle_median(before, held, next, config_.threshold, &median) ? median : held;
      }
      data[i] = out;
      before = held;
      held = next;
      ++seen;
    }
    before_[static_cast<std::size_t>(ch)] = before;
    held_[static_cast<std::size_t>(ch)] = held;
  }
  samples_seen_ += static_cast<std::size_t>(num_samples);
}

void StreamingDecrackle::reset() {
  std::fill(before_.begin(), before_.end(), 0.0f);
  std::fill(held_.begin(), held_.end(), 0.0f);
  samples_seen_ = 0;
}

}  // namespace sonare::mastering::repair
