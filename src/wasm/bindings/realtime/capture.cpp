/// @file realtime_engine_capture.cpp
/// @brief Embind realtime-engine facade: capture & recording.

#ifdef __EMSCRIPTEN__

#include "realtime_engine_wasm.h"

namespace {

sonare::engine::CaptureSource captureSourceFromName(const std::string& source) {
  if (source == "output") return sonare::engine::CaptureSource::kOutput;
  if (source == "input") return sonare::engine::CaptureSource::kInput;
  throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                "capture source must be 'output' or 'input'");
}

const char* captureSourceName(sonare::engine::CaptureSource source) {
  return source == sonare::engine::CaptureSource::kInput ? "input" : "output";
}

}  // namespace

void RealtimeEngineWasm::setCaptureBuffer(int num_channels, int capacity_frames) {
  if (num_channels <= 0 || capacity_frames <= 0) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "capture buffer dimensions must be positive");
  }
  capture_storage_.assign(static_cast<size_t>(num_channels),
                          std::vector<float>(static_cast<size_t>(capacity_frames), 0.0f));
  capture_ptrs_.clear();
  capture_ptrs_.reserve(capture_storage_.size());
  for (auto& channel : capture_storage_) {
    capture_ptrs_.push_back(channel.data());
  }
  engine_.set_capture_segment(
      {capture_ptrs_.data(), num_channels, static_cast<int64_t>(capacity_frames)});
}

void RealtimeEngineWasm::armCapture(bool armed) { engine_.set_capture_armed(armed); }
void RealtimeEngineWasm::setCapturePunch(int64_t start_sample, int64_t end_sample, bool enabled) {
  engine_.set_capture_punch(start_sample, end_sample, enabled);
}
void RealtimeEngineWasm::setCaptureSource(std::string source) {
  engine_.set_capture_source(captureSourceFromName(source));
}
void RealtimeEngineWasm::setRecordOffsetSamples(int64_t offset_samples) {
  engine_.set_record_offset_samples(offset_samples);
}
void RealtimeEngineWasm::setInputMonitor(bool enabled, float gain) {
  if (!std::isfinite(gain)) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                  "input monitor gain must be finite");
  }
  engine_.set_input_monitor(enabled, gain);
}
void RealtimeEngineWasm::resetCapture() { engine_.reset_capture(); }

val RealtimeEngineWasm::captureStatus() const {
  val out = val::object();
  out.set("capturedFrames", static_cast<double>(engine_.captured_frames()));
  out.set("overflowCount", engine_.capture_overflow_count());
  out.set("armed", engine_.capture_armed());
  out.set("punchEnabled", engine_.capture_punch_enabled());
  out.set("source", captureSourceName(engine_.capture_source()));
  out.set("recordOffsetSamples", static_cast<double>(engine_.record_offset_samples()));
  return out;
}

val RealtimeEngineWasm::capturedAudio() const {
  const int64_t frames = engine_.captured_frames();
  val out = val::array();
  for (size_t ch = 0; ch < capture_storage_.size(); ++ch) {
    const size_t count =
        static_cast<size_t>(std::min<int64_t>(frames, capture_storage_[ch].size()));
    std::vector<float> channel(capture_storage_[ch].begin(), capture_storage_[ch].begin() + count);
    out.set(static_cast<int>(ch), vectorToFloat32Array(channel));
  }
  return out;
}

void registerRealtimeEngineCapture(class_<RealtimeEngineWasm>& cls) {
  cls.function("setCaptureBuffer", &RealtimeEngineWasm::setCaptureBuffer)
      .function("armCapture", &RealtimeEngineWasm::armCapture)
      .function("setCapturePunch", &RealtimeEngineWasm::setCapturePunch)
      .function("setCaptureSource", &RealtimeEngineWasm::setCaptureSource)
      .function("setRecordOffsetSamples", &RealtimeEngineWasm::setRecordOffsetSamples)
      .function("setInputMonitor", &RealtimeEngineWasm::setInputMonitor)
      .function("resetCapture", &RealtimeEngineWasm::resetCapture)
      .function("captureStatus", &RealtimeEngineWasm::captureStatus)
      .function("capturedAudio", &RealtimeEngineWasm::capturedAudio);
}

#endif  // __EMSCRIPTEN__
