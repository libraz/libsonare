/// @file mixing_automation.cpp
/// @brief Embind scene-based mixer facade: automation scheduling + meter reads.

#ifdef __EMSCRIPTEN__

#include <algorithm>

#include "c_api/mixing_input_domains.h"
#include "mixing/channel_strip.h"
#include "mixing_wasm.h"

#if defined(SONARE_WITH_MIXING) && defined(SONARE_WITH_GRAPH)

// Schedules sample-accurate insert-parameter automation on the strip at
// strip_index. insert_index addresses the strip's combined insert sequence
// [pre-inserts... post-inserts...]. param_id is processor-specific. sample_pos
// is in absolute samples from the start of processing. curve: 0 = Linear,
// 1 = Exponential, 2 = Hold, 3 = SCurve.
void MixerWasm::scheduleInsertAutomation(const val& strip_index_val, const val& insert_index_val,
                                         const val& param_id_val, const val& sample_pos_val,
                                         const val& value_val, const val& curve_val) {
  const int64_t sample_pos = checkedInt64FromVal(sample_pos_val, "samplePos");
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const unsigned int insert_index = checkedUintFromVal(insert_index_val, "insertIndex");
  const unsigned int param_id = domainNumberFromVal<unsigned int>(
      param_id_val, "paramId", sonare_c_detail::kStripInsertAutomationParamIdDomain);
  const float value = checkedFloatFromVal(value_val, "value");
  const int curve =
      domainNumberFromVal<int>(curve_val, "curve", sonare_c_detail::kStripAutomationCurveDomain);
  SonareStrip* strip = sonare_mixer_strip_at(mixer_, static_cast<size_t>(strip_index));
  if (strip == nullptr) {
    throwLastCError();
  }
  SonareError err = sonare_strip_schedule_insert_automation(strip, insert_index, param_id,
                                                            sample_pos, value, curve);
  if (err != SONARE_OK) {
    throw sonare::SonareException(sonare::ErrorCode::InvalidState, cErrorMessage(err));
  }
}

// Reads a strip's meter snapshot at the given tap point. tap: 0 = pre-fader,
// 1 = post-fader (see SonareMeterTap). Returns the full snapshot.
val MixerWasm::stripMeter(const val& strip_index_val, const val& tap_val) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const int tap = checkedIntFromVal(tap_val, "tap");
  SonareMixMeterSnapshot snapshot{};
  checkStripError(sonare_strip_meter(stripAt(strip_index), tap, &snapshot));
  return mixMeterSnapshotToVal(snapshot);
}

val MixerWasm::busMeter(std::string bus_id) {
  SonareMixMeterSnapshot snapshot{};
  checkStripError(sonare_mixer_bus_meter(mixer_, wasmCString(bus_id, "busId"), &snapshot));
  return mixMeterSnapshotToVal(snapshot);
}

// Number of blocks in which the strip discarded recursive state because a
// non-finite value had reached it. See sonare_strip_non_finite_discard_count.
double MixerWasm::stripNonFiniteDiscardCount(const val& strip_index_val) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  size_t count = 0;
  checkStripError(sonare_strip_non_finite_discard_count(stripAt(strip_index), &count));
  return static_cast<double>(count);
}

// Number of blocks in which a bus discarded recursive state because a
// non-finite value had reached it. See
// sonare_mixer_bus_non_finite_discard_count.
double MixerWasm::busNonFiniteDiscardCount(std::string bus_id) {
  size_t count = 0;
  checkStripError(
      sonare_mixer_bus_non_finite_discard_count(mixer_, wasmCString(bus_id, "busId"), &count));
  return static_cast<double>(count);
}

// Schedules sample-accurate fader automation on a strip. sample_pos uses the
// absolute-sample timeline; curve: 0 = Linear, 1 = Exponential, 2 = Hold,
// 3 = SCurve.
void MixerWasm::scheduleFaderAutomation(const val& strip_index_val, const val& sample_pos_val,
                                        const val& fader_db_val, const val& curve_val) {
  const int64_t sample_pos = checkedInt64FromVal(sample_pos_val, "samplePos");
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const float fader_db = checkedFloatFromVal(fader_db_val, "faderDb");
  const int curve =
      domainNumberFromVal<int>(curve_val, "curve", sonare_c_detail::kStripAutomationCurveDomain);
  checkStripError(
      sonare_strip_schedule_fader_automation(stripAt(strip_index), sample_pos, fader_db, curve));
}

void MixerWasm::schedulePanAutomation(const val& strip_index_val, const val& sample_pos_val,
                                      const val& pan_val, const val& curve_val) {
  const int64_t sample_pos = checkedInt64FromVal(sample_pos_val, "samplePos");
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const float pan = checkedFloatFromVal(pan_val, "pan");
  const int curve =
      domainNumberFromVal<int>(curve_val, "curve", sonare_c_detail::kStripAutomationCurveDomain);
  checkStripValueError(
      sonare_strip_schedule_pan_automation(stripAt(strip_index), sample_pos, pan, curve));
}

void MixerWasm::scheduleWidthAutomation(const val& strip_index_val, const val& sample_pos_val,
                                        const val& width_val, const val& curve_val) {
  const int64_t sample_pos = checkedInt64FromVal(sample_pos_val, "samplePos");
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const float width = checkedFloatFromVal(width_val, "width");
  const int curve =
      domainNumberFromVal<int>(curve_val, "curve", sonare_c_detail::kStripAutomationCurveDomain);
  checkStripValueError(
      sonare_strip_schedule_width_automation(stripAt(strip_index), sample_pos, width, curve));
}

// Schedules sample-accurate send-level automation on a strip's send.
void MixerWasm::scheduleSendAutomation(const val& strip_index_val, const val& send_index_val,
                                       const val& sample_pos_val, const val& db_val,
                                       const val& curve_val) {
  const int64_t sample_pos = checkedInt64FromVal(sample_pos_val, "samplePos");
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const std::size_t send_index =
      static_cast<std::size_t>(checkedUintFromVal(send_index_val, "sendIndex"));
  const float db = checkedFloatFromVal(db_val, "db");
  const int curve =
      domainNumberFromVal<int>(curve_val, "curve", sonare_c_detail::kStripAutomationCurveDomain);
  checkStripError(sonare_strip_schedule_send_automation(stripAt(strip_index), send_index,
                                                        sample_pos, db, curve));
}

// Reads up to max_points of the strip's most recent goniometer samples.
// Returns an array of { left, right } points (oldest to newest).
//
// max_points is a REQUEST, not an allocation size. It arrives as a double
// rather than a size_t because embind converts a JS number to size_t with a
// plain cast: a `-1` would already have wrapped to SIZE_MAX and a NaN would
// already be undefined behaviour by the time a size_t parameter is in hand.
// Validated first, then the working buffer is bounded by the strip's ring
// instead of by the caller's number -- one read can never return more than the
// ring holds, so the cap drops no point. Without both steps a large or negative
// count reached `std::vector(n)` directly, and in WASM that is not an
// exception a caller can see: it is an out-of-memory abort of the whole module.
val MixerWasm::readGoniometerLatest(const val& strip_index_val, double max_points) {
  const unsigned int strip_index = checkedUintFromVal(strip_index_val, "stripIndex");
  const size_t requested = wasmCountArg(max_points, "maxPoints");
  SonareStrip* strip = stripAt(strip_index);
  val out = val::array();
  const size_t capped = std::min(requested, sonare::mixing::ChannelStrip::kGoniometerCapacity);
  if (capped == 0) {
    return out;
  }
  std::vector<SonareMixGoniometerPoint> points(capped);
  const size_t count = sonare_strip_read_goniometer_latest(strip, points.data(), capped);
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
      .function("stripMeter", &MixerWasm::stripMeter)
      .function("busMeter", &MixerWasm::busMeter)
      .function("stripNonFiniteDiscardCount", &MixerWasm::stripNonFiniteDiscardCount)
      .function("busNonFiniteDiscardCount", &MixerWasm::busNonFiniteDiscardCount)
      .function("scheduleFaderAutomation", &MixerWasm::scheduleFaderAutomation)
      .function("schedulePanAutomation", &MixerWasm::schedulePanAutomation)
      .function("scheduleWidthAutomation", &MixerWasm::scheduleWidthAutomation)
      .function("scheduleSendAutomation", &MixerWasm::scheduleSendAutomation)
      .function("readGoniometerLatest", &MixerWasm::readGoniometerLatest);
}

#endif  // SONARE_WITH_MIXING && SONARE_WITH_GRAPH

#endif  // __EMSCRIPTEN__
