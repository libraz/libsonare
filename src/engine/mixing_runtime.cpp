#include "engine/mixing_runtime.h"

namespace sonare::engine {

bool MixingRuntime::bind(mixing::ChannelStrip* strip) noexcept {
  // strip_ is a plain raw pointer the audio thread reads in process_at(); it is
  // not published through a deferred-reclaim RtPublisher. bind() must therefore
  // run on the control thread and not concurrently with process() (see
  // RealtimeEngine's thread-safety contract), since the caller destroys the
  // previously bound strip right after rebinding.
  strip_ = strip;
  return strip_ != nullptr;
}

void MixingRuntime::prepare(double sample_rate, int max_block_size) {
  if (strip_) strip_->prepare(sample_rate, max_block_size);
}

void MixingRuntime::process(float* const* channels, int num_channels, int num_samples) {
  process_at(channels, num_channels, num_samples, 0);
}

void MixingRuntime::process_at(float* const* channels, int num_channels, int num_samples,
                               int64_t timeline_sample) noexcept {
  if (!strip_) return;
  strip_->process_at(channels, num_channels, num_samples, timeline_sample);
}

void MixingRuntime::reset() {
  if (strip_) strip_->reset();
}

int MixingRuntime::latency_samples() const noexcept {
  return strip_ ? strip_->latency_samples() : 0;
}

int MixingRuntime::latency_samples_q8() const noexcept {
  return strip_ ? strip_->latency_samples_q8() : 0;
}

bool MixingRuntime::set_parameter(unsigned int param_id, float value) {
  if (!strip_) return false;
  if (!is_supported_parameter(param_id)) return false;
  switch (param_id) {
    case kFaderDb:
      strip_->set_fader_db(value);
      return true;
    case kPan:
      strip_->set_pan(value);
      return true;
    case kWidth:
      strip_->set_width(value);
      return true;
    default:
      return false;
  }
}

bool MixingRuntime::parameter_is_realtime_safe(unsigned int param_id) const noexcept {
  return is_supported_parameter(param_id);
}

}  // namespace sonare::engine
