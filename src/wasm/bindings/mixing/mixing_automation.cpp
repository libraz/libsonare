/// @file mixing_automation.cpp
/// @brief Embind scene-based mixer facade: automation scheduling + meter reads.

#ifdef __EMSCRIPTEN__

#include "mixing_wasm.h"

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

// Schedules sample-accurate insert-parameter automation on the strip at
// strip_index. insert_index addresses the strip's combined insert sequence
// [pre-inserts... post-inserts...]. param_id is processor-specific. sample_pos
// is in absolute samples from the start of processing. curve: 0 = Linear,
// 1 = Exponential.
void MixerWasm::scheduleInsertAutomation(unsigned int strip_index, unsigned int insert_index,
                                         unsigned int param_id, double sample_pos, float value,
                                         int curve) {
  SonareStrip* strip = sonare_mixer_strip_at(mixer_, static_cast<size_t>(strip_index));
  if (strip == nullptr) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState,
                                  "mixer strip index out of range");
  }
  SonareError err = sonare_strip_schedule_insert_automation(
      strip, insert_index, param_id, static_cast<int64_t>(sample_pos), value, curve);
  if (err != SONARE_OK) {
    throw sonare::SonareException(
        sonare::ErrorCode::InvalidState,
        std::string("failed to schedule insert automation: ") + sonare_error_message(err));
  }
}

// Reads a meter snapshot at the given tap point. tap: 0 = pre-fader,
// 1 = post-fader (see SonareMeterTap). Returns the full snapshot.
val MixerWasm::meterTap(unsigned int strip_index, int tap) {
  SonareMixMeterSnapshot snapshot{};
  checkStripError(sonare_strip_meter_tap(stripAt(strip_index), tap, &snapshot),
                  "failed to read meter tap");
  return mixMeterSnapshotToVal(snapshot);
}

// Reads the strip's current (post-fader) meter snapshot. Tap-less, mirroring
// the Node/Python stripMeter contract which calls sonare_strip_meter; the
// tap-selectable variant is meterTap.
val MixerWasm::stripMeter(unsigned int strip_index) {
  SonareMixMeterSnapshot snapshot{};
  checkStripError(sonare_strip_meter(stripAt(strip_index), &snapshot),
                  "failed to read strip meter");
  return mixMeterSnapshotToVal(snapshot);
}

// Schedules sample-accurate fader automation on a strip. sample_pos uses the
// absolute-sample timeline; curve: 0 = Linear, 1 = Exponential.
void MixerWasm::scheduleFaderAutomation(unsigned int strip_index, double sample_pos, float fader_db,
                                        int curve) {
  checkStripError(sonare_strip_schedule_fader_automation(
                      stripAt(strip_index), static_cast<int64_t>(sample_pos), fader_db, curve),
                  "failed to schedule fader automation");
}

void MixerWasm::schedulePanAutomation(unsigned int strip_index, double sample_pos, float pan,
                                      int curve) {
  checkStripError(sonare_strip_schedule_pan_automation(
                      stripAt(strip_index), static_cast<int64_t>(sample_pos), pan, curve),
                  "failed to schedule pan automation");
}

void MixerWasm::scheduleWidthAutomation(unsigned int strip_index, double sample_pos, float width,
                                        int curve) {
  checkStripError(sonare_strip_schedule_width_automation(
                      stripAt(strip_index), static_cast<int64_t>(sample_pos), width, curve),
                  "failed to schedule width automation");
}

// Schedules sample-accurate send-level automation on a strip's send.
void MixerWasm::scheduleSendAutomation(unsigned int strip_index, size_t send_index,
                                       double sample_pos, float db, int curve) {
  checkStripError(
      sonare_strip_schedule_send_automation(stripAt(strip_index), send_index,
                                            static_cast<int64_t>(sample_pos), db, curve),
      "failed to schedule send automation");
}

// Reads up to max_points of the strip's most recent goniometer samples.
// Returns an array of { left, right } points (oldest to newest).
val MixerWasm::readGoniometerLatest(unsigned int strip_index, size_t max_points) {
  SonareStrip* strip = stripAt(strip_index);
  val out = val::array();
  if (max_points == 0) {
    return out;
  }
  std::vector<SonareMixGoniometerPoint> points(max_points);
  const size_t count = sonare_strip_read_goniometer_latest(strip, points.data(), max_points);
  for (size_t index = 0; index < count; ++index) {
    val point = val::object();
    point.set("left", points[index].left);
    point.set("right", points[index].right);
    out.call<void>("push", point);
  }
  return out;
}

void registerMixerAutomationMeters(class_<MixerWasm>& cls) {
  cls.function("scheduleInsertAutomation", &MixerWasm::scheduleInsertAutomation)
      .function("meterTap", &MixerWasm::meterTap)
      .function("stripMeter", &MixerWasm::stripMeter)
      .function("scheduleFaderAutomation", &MixerWasm::scheduleFaderAutomation)
      .function("schedulePanAutomation", &MixerWasm::schedulePanAutomation)
      .function("scheduleWidthAutomation", &MixerWasm::scheduleWidthAutomation)
      .function("scheduleSendAutomation", &MixerWasm::scheduleSendAutomation)
      .function("readGoniometerLatest", &MixerWasm::readGoniometerLatest);
}

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH

#endif  // __EMSCRIPTEN__
