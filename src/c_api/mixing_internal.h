#pragma once

#include <sonare/sonare_c.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "graph/connection.h"
#include "graph/graph.h"
#include "mastering/api/insert_factory.h"
#include "mixing/api/presets.h"
#include "mixing/api/scene.h"
#include "mixing/channel_strip.h"
#include "mixing/fx_bus.h"
#include "mixing/send.h"
#include "rt/processor_base.h"
#include "sonare_c_internal.h"

struct SonareMixer;

struct SonareStrip {
  std::string id;
  sonare::mixing::api::Strip scene_strip;
  sonare::mixing::ChannelStrip strip;
  SonareMixer* owner = nullptr;
};

struct SonareMixer {
  int sample_rate = 48000;
  int max_block_size = 0;
  std::vector<std::unique_ptr<SonareStrip>> strips;
  std::vector<sonare::mixing::api::Bus> buses;
  std::vector<sonare::mixing::api::VcaGroup> vca_groups;
  std::vector<sonare::mixing::api::Connection> connections;
  std::string master_id;
  sonare::graph::Graph graph;
  bool compiled_dirty = true;
  int latency_samples = 0;
  int tail_samples = 0;
  std::vector<float> scratch_left;
  std::vector<float> scratch_right;
};

namespace sonare_c_mixing_detail {

static_assert(static_cast<int>(sonare::mixing::AutomationCurveType::Linear) == 0,
              "sonare::mixing::AutomationCurveType::Linear must be ordinal 0 to keep "
              "sonare_strip_schedule_*_automation curve ABI stable");
static_assert(static_cast<int>(sonare::mixing::AutomationCurveType::Exponential) == 1,
              "sonare::mixing::AutomationCurveType::Exponential must be ordinal 1");
static_assert(static_cast<int>(sonare::mixing::AutomationCurveType::Hold) == 2,
              "sonare::mixing::AutomationCurveType::Hold must be ordinal 2");
static_assert(static_cast<int>(sonare::mixing::AutomationCurveType::SCurve) == 3,
              "sonare::mixing::AutomationCurveType::SCurve must be ordinal 3");

inline bool parse_automation_curve(int curve, sonare::mixing::AutomationCurveType* out) noexcept {
  if (!out) {
    return false;
  }
  if (curve < 0 || curve > 3) {
    return false;
  }
  *out = static_cast<sonare::mixing::AutomationCurveType>(curve);
  return true;
}

inline sonare::mixing::PanMode to_pan_mode(int mode) {
  switch (mode) {
    case SONARE_PAN_MODE_BALANCE:
      return sonare::mixing::PanMode::Balance;
    case SONARE_PAN_MODE_STEREO_PAN:
      return sonare::mixing::PanMode::StereoPan;
    case SONARE_PAN_MODE_DUAL_PAN:
      return sonare::mixing::PanMode::DualPan;
    default:
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown mixing pan mode");
  }
}

inline sonare::mixing::PanLaw to_pan_law(int law) {
  switch (law) {
    case 0:
      return sonare::mixing::PanLaw::Const3dB;
    case 1:
      return sonare::mixing::PanLaw::Const4p5dB;
    case 2:
      return sonare::mixing::PanLaw::Const6dB;
    case 3:
      return sonare::mixing::PanLaw::Linear0dB;
    default:
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter, "unknown mixing pan law");
  }
}

inline int from_pan_law(sonare::mixing::PanLaw law) {
  switch (law) {
    case sonare::mixing::PanLaw::Const3dB:
      return 0;
    case sonare::mixing::PanLaw::Const4p5dB:
      return 1;
    case sonare::mixing::PanLaw::Const6dB:
      return 2;
    case sonare::mixing::PanLaw::Linear0dB:
      return 3;
  }
  return 0;
}

inline int from_pan_mode(sonare::mixing::PanMode mode) {
  switch (mode) {
    case sonare::mixing::PanMode::Balance:
      return SONARE_PAN_MODE_BALANCE;
    case sonare::mixing::PanMode::StereoPan:
      return SONARE_PAN_MODE_STEREO_PAN;
    case sonare::mixing::PanMode::DualPan:
      return SONARE_PAN_MODE_DUAL_PAN;
  }
  return SONARE_PAN_MODE_BALANCE;
}

inline sonare::mixing::SendTiming to_send_timing(int timing) {
  switch (timing) {
    case SONARE_SEND_TIMING_PRE_FADER:
      return sonare::mixing::SendTiming::PreFader;
    case SONARE_SEND_TIMING_POST_FADER:
      return sonare::mixing::SendTiming::PostFader;
    default:
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "unknown mixing send timing");
  }
}

inline sonare::mixing::SendTiming to_send_timing(sonare::mixing::api::SendTiming timing) {
  return timing == sonare::mixing::api::SendTiming::PreFader
             ? sonare::mixing::SendTiming::PreFader
             : sonare::mixing::SendTiming::PostFader;
}

inline sonare::mixing::api::SendTiming to_api_send_timing(int timing) {
  switch (timing) {
    case SONARE_SEND_TIMING_PRE_FADER:
      return sonare::mixing::api::SendTiming::PreFader;
    case SONARE_SEND_TIMING_POST_FADER:
      return sonare::mixing::api::SendTiming::PostFader;
    default:
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "unknown mixing send timing");
  }
}

inline void copy_meter_snapshot(const sonare::mixing::MeterSnapshot& snapshot,
                                SonareMixMeterSnapshot* out) {
  out->peak_db_l = snapshot.peak_db[0];
  out->peak_db_r = snapshot.peak_db[1];
  out->rms_db_l = snapshot.rms_db[0];
  out->rms_db_r = snapshot.rms_db[1];
  out->correlation = snapshot.correlation;
  out->mono_compat_width = snapshot.mono_compat_width;
  out->mono_compat_peak = snapshot.mono_compat_peak;
  out->mono_compat_side_rms = snapshot.mono_compat_side_rms;
  out->likely_mono_compatible = snapshot.likely_mono_compatible ? 1 : 0;
  out->momentary_lufs = snapshot.momentary_lufs;
  out->short_term_lufs = snapshot.short_term_lufs;
  out->integrated_lufs = snapshot.integrated_lufs;
  out->gain_reduction_db = snapshot.gain_reduction_db;
  out->true_peak_db_l = snapshot.true_peak_db[0];
  out->true_peak_db_r = snapshot.true_peak_db[1];
  out->max_true_peak_db = snapshot.max_true_peak_db;
  out->seq = snapshot.seq;
}

void apply_solo_mutes(SonareMixer* mixer);
void build_and_compile(SonareMixer* mixer);

}  // namespace sonare_c_mixing_detail
