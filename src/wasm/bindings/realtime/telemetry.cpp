/// @file realtime_engine_telemetry.cpp
/// @brief Embind realtime-engine facade: telemetry & metering drains.

#ifdef __EMSCRIPTEN__

#include "realtime_engine_wasm.h"

val RealtimeEngineWasm::drainTelemetry(int max_records) {
  val out = val::array();
  if (max_records <= 0) return out;
  sonare::engine::Telemetry telemetry{};
  int count = 0;
  while (count < max_records && engine_.pop_telemetry(telemetry)) {
    val item = val::object();
    item.set("type", static_cast<int>(telemetry.type));
    item.set("error", static_cast<int>(telemetry.error));
    item.set("renderFrame", static_cast<double>(telemetry.render_frame));
    item.set("timelineSample", static_cast<double>(telemetry.timeline_sample));
    item.set("audibleTimelineSample", static_cast<double>(telemetry.audible_timeline_sample));
    item.set("graphLatencySamplesQ8", telemetry.graph_latency_samples_q8);
    item.set("value", telemetry.value);
    out.set(count++, item);
  }
  return out;
}

val RealtimeEngineWasm::drainMeterTelemetry(int max_records) {
  val out = val::array();
  if (max_records <= 0) return out;
#if defined(SONARE_WITH_MIXING)
  sonare::engine::MeterTelemetryRecord meter{};
  int count = 0;
  while (count < max_records && engine_.pop_meter_telemetry(meter)) {
    val item = val::object();
    item.set("targetId", meter.target_id);
    item.set("renderFrame", static_cast<double>(meter.render_frame));
    item.set("seq", static_cast<double>(meter.seq));
    item.set("peakDbL", meter.peak_db[0]);
    item.set("peakDbR", meter.peak_db[1]);
    item.set("rmsDbL", meter.rms_db[0]);
    item.set("rmsDbR", meter.rms_db[1]);
    item.set("truePeakDbL", meter.true_peak_db[0]);
    item.set("truePeakDbR", meter.true_peak_db[1]);
    item.set("maxTruePeakDb", meter.max_true_peak_db);
    item.set("correlation", meter.correlation);
    item.set("monoCompatWidth", meter.mono_compat_width);
    item.set("momentaryLufs", meter.momentary_lufs);
    item.set("shortTermLufs", meter.short_term_lufs);
    item.set("integratedLufs", meter.integrated_lufs);
    item.set("gainReductionDb", meter.gain_reduction_db);
    item.set("droppedRecords", meter.dropped_records);
    out.set(count++, item);
  }
#else
  (void)max_records;
#endif
  return out;
}

// Per-plane meter drain for surround targets. peakDb/rmsDb/truePeakDb are JS
// arrays of channelCount planes (canonical WAVE order); drainMeterTelemetry
// stays the stereo fast path. Shares one queue with it — call only one.
val RealtimeEngineWasm::drainMeterTelemetryWide(int max_records) {
  val out = val::array();
  if (max_records <= 0) return out;
#if defined(SONARE_WITH_MIXING)
  sonare::engine::MeterTelemetryRecord meter{};
  int count = 0;
  while (count < max_records && engine_.pop_meter_telemetry(meter)) {
    val item = val::object();
    item.set("targetId", meter.target_id);
    item.set("renderFrame", static_cast<double>(meter.render_frame));
    item.set("seq", static_cast<double>(meter.seq));
    int planes = meter.channel_count;
    if (planes < 0) planes = 0;
    if (planes > sonare::mixing::kMaxMeterChannels) planes = sonare::mixing::kMaxMeterChannels;
    item.set("channelCount", planes);
    val peak = val::array();
    val rms = val::array();
    val true_peak = val::array();
    for (int ch = 0; ch < planes; ++ch) {
      peak.set(ch, meter.peak_db[static_cast<size_t>(ch)]);
      rms.set(ch, meter.rms_db[static_cast<size_t>(ch)]);
      true_peak.set(ch, meter.true_peak_db[static_cast<size_t>(ch)]);
    }
    item.set("peakDb", peak);
    item.set("rmsDb", rms);
    item.set("truePeakDb", true_peak);
    item.set("maxTruePeakDb", meter.max_true_peak_db);
    item.set("correlation", meter.correlation);
    item.set("monoCompatWidth", meter.mono_compat_width);
    item.set("momentaryLufs", meter.momentary_lufs);
    item.set("shortTermLufs", meter.short_term_lufs);
    item.set("integratedLufs", meter.integrated_lufs);
    item.set("gainReductionDb", meter.gain_reduction_db);
    item.set("droppedRecords", meter.dropped_records);
    out.set(count++, item);
  }
#else
  (void)max_records;
#endif
  return out;
}

unsigned int RealtimeEngineWasm::configureScopeTelemetry(int interval_frames,
                                                         unsigned int band_count) {
#if defined(SONARE_WITH_MIXING)
  return engine_.configure_scope_telemetry(interval_frames, band_count);
#else
  (void)interval_frames;
  (void)band_count;
  return 0;
#endif
}

val RealtimeEngineWasm::drainScopeTelemetry(int max_records) {
  val out = val::array();
  if (max_records <= 0) return out;
#if defined(SONARE_WITH_MIXING)
  sonare::engine::ScopeTelemetryRecord rec{};
  int count = 0;
  while (count < max_records && engine_.pop_scope_telemetry(rec)) {
    val item = val::object();
    item.set("targetId", rec.target_id);
    item.set("renderFrame", static_cast<double>(rec.render_frame));
    item.set("seq", static_cast<double>(rec.seq));
    item.set("droppedRecords", rec.dropped_records);
    val bands = val::array();
    for (uint32_t b = 0; b < rec.band_count && b < rec.bands.size(); ++b) {
      bands.set(b, rec.bands[b]);
    }
    item.set("bands", bands);
    val points = val::array();
    for (uint32_t p = 0; p < rec.point_count && p < rec.points.size(); ++p) {
      val point = val::object();
      point.set("left", rec.points[p].left);
      point.set("right", rec.points[p].right);
      points.set(p, point);
    }
    item.set("points", points);
    out.set(count++, item);
  }
#else
  (void)max_records;
#endif
  return out;
}

void registerRealtimeEngineTelemetry(class_<RealtimeEngineWasm>& cls) {
  cls.function("drainTelemetry", &RealtimeEngineWasm::drainTelemetry)
      .function("drainMeterTelemetry", &RealtimeEngineWasm::drainMeterTelemetry)
      .function("drainMeterTelemetryWide", &RealtimeEngineWasm::drainMeterTelemetryWide)
      .function("configureScopeTelemetry", &RealtimeEngineWasm::configureScopeTelemetry)
      .function("drainScopeTelemetry", &RealtimeEngineWasm::drainScopeTelemetry);
}

#endif  // __EMSCRIPTEN__
