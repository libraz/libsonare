/// @file realtime_engine_capture.cpp
/// @brief Embind realtime-engine facade: capture & recording.

#ifdef __EMSCRIPTEN__

#include <cmath>

#include "c_api/engine_config_domains.h"
#include "realtime_engine_wasm.h"
#include "util/parameter_checks.h"

namespace {

sonare::engine::CaptureSource captureSourceFromVal(const val& source) {
  if (source.typeOf().as<std::string>() == "string") {
    const std::string name = source.as<std::string>();
    if (name == "output") return sonare::engine::CaptureSource::kOutput;
    if (name == "input") return sonare::engine::CaptureSource::kInput;
  } else if (source.typeOf().as<std::string>() == "number") {
    const double ordinal = source.as<double>();
    // Nothing narrows here: the equality comparisons below already exclude every
    // non-finite and fractional value this guard names, so it cannot fire.
    if (std::isfinite(ordinal) && std::floor(ordinal) == ordinal) {
      if (ordinal == 0) return sonare::engine::CaptureSource::kOutput;
      if (ordinal == 1) return sonare::engine::CaptureSource::kInput;
    }
  }
  throw WasmRangeError("capture source must be 'output' or 'input' (or ordinal 0 or 1)");
}

const char* captureSourceName(sonare::engine::CaptureSource source) {
  return source == sonare::engine::CaptureSource::kInput ? "input" : "output";
}

// An enabled empty window would read as "no punch" and record everything.
void checkPunchWindow(int64_t start, int64_t end, bool enabled, const char* start_name,
                      const char* end_name) {
  sonare::util::check::integer_within(start, sonare_c_detail::kEngineCapturePunchStartDomain,
                                      start_name);
  if (end < start) {
    sonare::util::check::refuse_relation({end_name, start_name}, "{0} must not be before {1}");
  }
  if (enabled && end == start) {
    sonare::util::check::refuse_relation({end_name, start_name}, "{0} must not equal {1}");
  }
}

}  // namespace

void RealtimeEngineWasm::setCaptureBuffer(const val& num_channels_val,
                                          const val& capacity_frames_val) {
  const int num_channels = checkedIntFromVal(num_channels_val, "numChannels");
  const int capacity_frames = checkedIntFromVal(capacity_frames_val, "capacityFrames");
  sonare::util::check::integer_within(num_channels, sonare_c_detail::kEngineCaptureChannelsDomain,
                                      "numChannels");
  sonare::util::check::integer_within(
      capacity_frames, sonare_c_detail::kEngineCaptureCapacityFramesDomain, "capacityFrames");
  allocatePlanes(capture_storage_, capture_ptrs_, num_channels, capacity_frames);
  engine_.set_capture_segment(
      {capture_ptrs_.data(), num_channels, static_cast<int64_t>(capacity_frames)});
}

void RealtimeEngineWasm::armCapture(bool armed) { engine_.set_capture_armed(armed); }
void RealtimeEngineWasm::setCapturePunch(int64_t start_sample, int64_t end_sample, bool enabled) {
  checkPunchWindow(start_sample, end_sample, enabled, "start_sample", "end_sample");
  engine_.set_capture_punch(start_sample, end_sample, enabled);
}
void RealtimeEngineWasm::setCaptureSource(val source) {
  engine_.set_capture_source(captureSourceFromVal(source));
}
void RealtimeEngineWasm::setRecordOffsetSamples(int64_t offset_samples) {
  engine_.set_record_offset_samples(offset_samples);
}
// The C ABI's subjects, so a refusal reads as sonare_engine_set_capture_timing's does.
void RealtimeEngineWasm::setCaptureTiming(int64_t record_offset_samples, int64_t punch_start_sample,
                                          int64_t punch_end_sample, bool punch_enabled) {
  checkPunchWindow(punch_start_sample, punch_end_sample, punch_enabled, "punch_start_sample",
                   "punch_end_sample");
  engine_.set_capture_timing(record_offset_samples, punch_start_sample, punch_end_sample,
                             punch_enabled);
}
void RealtimeEngineWasm::setInputMonitor(bool enabled, const val& gain_val) {
  const float gain = checkedFloatFromVal(gain_val, "gain");
  engine_.set_input_monitor(enabled, gain);
}
val RealtimeEngineWasm::inputMonitor() const {
  const auto monitor = engine_.input_monitor();
  val out = val::object();
  out.set("enabled", monitor.enabled);
  out.set("gain", monitor.gain);
  return out;
}
void RealtimeEngineWasm::resetCapture() { engine_.reset_capture(); }

val RealtimeEngineWasm::captureStatus() const {
  val out = val::object();
  out.set("capturedFrames", static_cast<double>(engine_.captured_frames()));
  out.set("overflowCount", static_cast<double>(engine_.capture_overflow_count()));
  out.set("droppedFrames", static_cast<double>(engine_.capture_dropped_frames()));
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
        frames > 0 ? static_cast<size_t>(std::min<int64_t>(frames, capture_storage_[ch].size()))
                   : 0;
    // The result owns its JS ArrayBuffer, so transferring it cannot detach the capture storage.
    out.set(static_cast<int>(ch), vectorToFloat32Array(capture_storage_[ch].data(), count));
  }
  return out;
}

void registerRealtimeEngineCapture(class_<RealtimeEngineWasm>& cls) {
  cls.function("setCaptureBuffer", &RealtimeEngineWasm::setCaptureBuffer)
      .function("armCapture", &RealtimeEngineWasm::armCapture)
      .function("setCapturePunch", &RealtimeEngineWasm::setCapturePunch)
      .function("setCaptureSource", &RealtimeEngineWasm::setCaptureSource)
      .function("setRecordOffsetSamples", &RealtimeEngineWasm::setRecordOffsetSamples)
      .function("setCaptureTiming", &RealtimeEngineWasm::setCaptureTiming)
      .function("setInputMonitor", &RealtimeEngineWasm::setInputMonitor)
      .function("inputMonitor", &RealtimeEngineWasm::inputMonitor)
      .function("resetCapture", &RealtimeEngineWasm::resetCapture)
      .function("captureStatus", &RealtimeEngineWasm::captureStatus)
      .function("capturedAudio", &RealtimeEngineWasm::capturedAudio);
}

#endif  // __EMSCRIPTEN__
