#include <algorithm>

#include "c_api/mixing_internal.h"
#include "mixing/alignment_delay.h"

using namespace sonare_c_mixing_detail;

SonareMixer* sonare_mixer_create(int sample_rate, int max_block_size) {
  SONARE_C_API_ENTRY;
  if (sample_rate <= 0 || max_block_size <= 0) {
    sonare_c_detail::set_last_error("mixer: sample_rate and max_block_size must be positive");
    return nullptr;
  }
  try {
    auto* mixer = new SonareMixer;
    mixer->sample_rate = sample_rate;
    mixer->max_block_size = max_block_size;
    return mixer;
    SONARE_C_CATCH_RETURN(nullptr)
  }

  SonareStrip* sonare_mixer_add_strip(SonareMixer * mixer, const char* id) {
    SONARE_C_API_ENTRY;
    if (!mixer || !id) {
      sonare_c_detail::set_last_error("mixer: handle and strip id are required");
      return nullptr;
    }
    try {
      const std::string strip_id = id;
      for (const auto& existing : mixer->strips) {
        if (existing->id == strip_id) {
          sonare_c_detail::set_last_error("mixer: duplicate strip id");
          return nullptr;
        }
      }
      auto strip = std::make_unique<SonareStrip>();
      strip->id = strip_id;
      strip->scene_strip.id = strip->id;
      strip->owner = mixer;
      strip->strip.prepare(static_cast<double>(mixer->sample_rate), mixer->max_block_size);
      for (const auto& group : mixer->vca_groups) {
        if (std::find(group.members.begin(), group.members.end(), strip->id) !=
            group.members.end()) {
          strip->strip.add_vca_group_offset_db(group.gain_db);
        }
      }
      SonareStrip* raw = strip.get();
      mixer->strips.push_back(std::move(strip));
      mixer->compiled_dirty = true;
      return raw;
      SONARE_C_CATCH_RETURN(nullptr)
    }

    SonareError sonare_strip_set_fader_db(SonareStrip * strip, float db) {
      SONARE_C_API_ENTRY;
      if (!strip || !finite(db)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      SONARE_C_TRY
      strip->strip.set_fader_db(db);
      strip->scene_strip.fader_db = db;
      return SONARE_OK;
      SONARE_C_CATCH
    }

    SonareError sonare_strip_set_input_trim_db(SonareStrip * strip, float db) {
      SONARE_C_API_ENTRY;
      if (!strip || !finite(db)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      SONARE_C_TRY
      strip->strip.set_input_trim_db(db);
      strip->scene_strip.input_trim_db = db;
      return SONARE_OK;
      SONARE_C_CATCH
    }

    SonareError sonare_strip_set_pan(SonareStrip * strip, float pan, int pan_mode) {
      SONARE_C_API_ENTRY;
      if (!strip || !finite(pan)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      SONARE_C_TRY
      // pan_mode < 0 (SONARE_PAN_MODE_KEEP) means "keep the strip's current pan
      // mode" — only the pan position moves. This avoids resetting a StereoPan /
      // DualPan strip back to Balance when a caller merely nudges the pan position.
      // Otherwise to_pan_mode throws SONARE_ERROR_INVALID_PARAMETER for unknown
      // modes, so by the time we mirror it into the scene strip it is validated.
      if (pan_mode < 0) {
        // Mirror the strip's live mode into the scene cache so save/reload stays
        // faithful (it is unchanged by this call but may have been set elsewhere).
        strip->scene_strip.pan_mode = from_pan_mode(strip->strip.pan_mode());
      } else {
        strip->strip.set_pan_mode(to_pan_mode(pan_mode));
        strip->scene_strip.pan_mode = pan_mode;
      }
      strip->strip.set_pan(pan);
      // Clamp the cached scene pan to the processor's valid range so save/reload is
      // faithful, matching set_dual_pan (which already clamps) and the live panner.
      strip->scene_strip.pan = std::clamp(pan, -1.0f, 1.0f);
      return SONARE_OK;
      SONARE_C_CATCH
    }

    SonareError sonare_strip_set_dual_pan(SonareStrip * strip, float left_pan, float right_pan) {
      SONARE_C_API_ENTRY;
      if (!strip || !finite(left_pan) || !finite(right_pan)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      SONARE_C_TRY
      strip->strip.set_pan_mode(sonare::mixing::PanMode::DualPan);
      strip->strip.set_dual_pan(left_pan, right_pan);
      // Cache the nominal pan from the clamped L/R so the scene round-trips the same
      // value sonare_mixer_to_scene_json will export for a DualPan strip.
      strip->scene_strip.pan =
          0.5f * (std::clamp(left_pan, -1.0f, 1.0f) + std::clamp(right_pan, -1.0f, 1.0f));
      strip->scene_strip.pan_mode = SONARE_PAN_MODE_DUAL_PAN;
      strip->scene_strip.dual_pan_left = std::clamp(left_pan, -1.0f, 1.0f);
      strip->scene_strip.dual_pan_right = std::clamp(right_pan, -1.0f, 1.0f);
      return SONARE_OK;
      SONARE_C_CATCH
    }

    SonareError sonare_strip_set_surround_pan(SonareStrip * strip, const SonareSurroundPan* pan) {
      SONARE_C_API_ENTRY;
      if (!strip || !pan || !finite(pan->azimuth) || !finite(pan->elevation) ||
          !finite(pan->divergence) || !finite(pan->lfe) || !finite(pan->distance)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      SONARE_C_TRY
      // Stored on the scene strip; inert until the surround DSP path consumes it.
      strip->scene_strip.surround_pan.azimuth = std::clamp(pan->azimuth, -180.0f, 180.0f);
      strip->scene_strip.surround_pan.elevation = pan->elevation;
      strip->scene_strip.surround_pan.divergence = std::clamp(pan->divergence, 0.0f, 1.0f);
      strip->scene_strip.surround_pan.lfe = std::clamp(pan->lfe, 0.0f, 1.0f);
      // distance is a positive scale with a core default of 1.0; SonareSurroundPan
      // has no C field initializers, so a zero-initialized struct (a C host that
      // only sets azimuth) arrives with distance == 0. Treat distance <= 0 as the
      // "keep default" sentinel so such hosts match the Node/Python facades (which
      // inject 1.0) instead of persisting a meaningless distance:0 into the scene.
      strip->scene_strip.surround_pan.distance = pan->distance > 0.0f ? pan->distance : 1.0f;
      sonare::mixing::SurroundPanParams live;
      live.azimuth = strip->scene_strip.surround_pan.azimuth;
      live.elevation = strip->scene_strip.surround_pan.elevation;
      live.divergence = strip->scene_strip.surround_pan.divergence;
      live.lfe = strip->scene_strip.surround_pan.lfe;
      live.distance = strip->scene_strip.surround_pan.distance;
      strip->strip.set_surround_pan_params(live);
      return SONARE_OK;
      SONARE_C_CATCH
    }

    SonareError sonare_strip_set_width(SonareStrip * strip, float width) {
      SONARE_C_API_ENTRY;
      if (!strip || !finite(width)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      SONARE_C_TRY
      strip->strip.set_width(width);
      strip->scene_strip.width = width;
      return SONARE_OK;
      SONARE_C_CATCH
    }

    SonareError sonare_strip_set_muted(SonareStrip * strip, int muted) {
      SONARE_C_API_ENTRY;
      if (!strip) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      SONARE_C_TRY
      strip->strip.set_muted(muted != 0);
      strip->scene_strip.muted = muted != 0;
      return SONARE_OK;
      SONARE_C_CATCH
    }

    SonareError sonare_strip_set_soloed(SonareStrip * strip, int soloed) {
      SONARE_C_API_ENTRY;
      if (!strip) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      SONARE_C_TRY
      strip->strip.set_soloed(soloed != 0);
      strip->scene_strip.soloed = soloed != 0;
      // Solo state is consumed live via implied_mute in process(); recompute the
      // implied mutes across the mixer so the change takes effect on the next
      // process without a full graph recompile.
      if (strip->owner != nullptr) {
        apply_solo_mutes(strip->owner);
      }
      return SONARE_OK;
      SONARE_C_CATCH
    }

    SonareError sonare_strip_set_solo_safe(SonareStrip * strip, int solo_safe) {
      SONARE_C_API_ENTRY;
      if (!strip) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      SONARE_C_TRY
      strip->strip.set_solo_safe(solo_safe != 0);
      strip->scene_strip.solo_safe = solo_safe != 0;
      // Solo-safe strips are excluded from implied mutes; recompute live.
      if (strip->owner != nullptr) {
        apply_solo_mutes(strip->owner);
      }
      return SONARE_OK;
      SONARE_C_CATCH
    }

    SonareError sonare_strip_set_polarity_invert(SonareStrip * strip, int invert_left,
                                                 int invert_right) {
      SONARE_C_API_ENTRY;
      if (!strip) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      SONARE_C_TRY
      strip->strip.set_polarity_invert(invert_left != 0, invert_right != 0);
      strip->scene_strip.polarity_invert_left = invert_left != 0;
      strip->scene_strip.polarity_invert_right = invert_right != 0;
      return SONARE_OK;
      SONARE_C_CATCH
    }

    SonareError sonare_strip_set_pan_law(SonareStrip * strip, int pan_law) {
      SONARE_C_API_ENTRY;
      if (!strip) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      SONARE_C_TRY
      strip->strip.set_pan_law(to_pan_law(pan_law));  // validates pan_law (throws on unknown)
      strip->scene_strip.pan_law = pan_law;
      return SONARE_OK;
      SONARE_C_CATCH
    }

    SonareError sonare_strip_set_channel_delay_samples(SonareStrip * strip, int delay_samples) {
      SONARE_C_API_ENTRY;
      if (!strip || delay_samples < 0 ||
          delay_samples > sonare::mixing::kMaxAlignmentDelaySamples) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      SONARE_C_TRY
      strip->strip.set_channel_delay_samples(delay_samples);
      strip->scene_strip.channel_delay_samples = delay_samples;
      // Channel delay changes the strip's reported latency; mark the graph dirty so
      // latency compensation re-runs at the next compile.
      if (strip->owner != nullptr) {
        strip->owner->compiled_dirty = true;
      }
      return SONARE_OK;
      SONARE_C_CATCH
    }

    SonareError sonare_strip_set_vca_offset_db(SonareStrip * strip, float offset_db) {
      SONARE_C_API_ENTRY;
      if (!strip || !finite(offset_db)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      SONARE_C_TRY
      strip->strip.set_vca_offset_db(offset_db);
      strip->scene_strip.vca_offset_db = offset_db;
      return SONARE_OK;
      SONARE_C_CATCH
    }

    SonareError sonare_strip_add_send(SonareStrip * strip, const char* id,
                                      const char* destination_bus_id, float send_db, int timing,
                                      size_t* index_out) {
      SONARE_C_API_ENTRY;
      if (!strip || !destination_bus_id || !finite(send_db)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      SONARE_C_TRY
      sonare::mixing::SendConfig config;
      config.send_db = send_db;
      config.timing = to_send_timing(timing);
      const size_t index = strip->strip.add_send(config);

      sonare::mixing::api::Send send;
      send.id = id != nullptr ? id : "";
      send.destination_bus_id = destination_bus_id;
      send.send_db = send_db;
      send.timing = to_api_send_timing(timing);
      strip->scene_strip.sends.push_back(std::move(send));
      if (strip->owner != nullptr) {
        strip->owner->compiled_dirty = true;  // send count changes node port layout
      }

      if (index_out != nullptr) {
        *index_out = index;
      }
      return SONARE_OK;
      SONARE_C_CATCH
    }

    SonareError sonare_strip_set_send_db(SonareStrip * strip, size_t index, float send_db) {
      SONARE_C_API_ENTRY;
      if (!strip || !finite(send_db)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      // Reject an out-of-range send index rather than silently returning OK after a
      // no-op (the underlying ChannelStrip ignores unknown indices). Validate
      // against the live ChannelStrip's send count, the single source of truth
      // shared with sonare_strip_schedule_send_automation; add_send/remove_send keep
      // the live strip and the scene_strip mirror index-parallel.
      if (index >= strip->strip.num_sends()) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      SONARE_C_TRY
      strip->strip.set_send_db(index, send_db);
      strip->scene_strip.sends[index].send_db = send_db;
      return SONARE_OK;
      SONARE_C_CATCH
    }

    SonareError sonare_strip_remove_send(SonareStrip * strip, unsigned int index) {
      SONARE_C_API_ENTRY;
      if (!strip) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      // Validate against the live ChannelStrip's send count, the single source of
      // truth (see sonare_strip_set_send_db / sonare_strip_schedule_send_automation).
      if (static_cast<size_t>(index) >= strip->strip.num_sends()) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      SONARE_C_TRY
      const size_t send_index = static_cast<size_t>(index);
      // Drop the send from BOTH the live strip and the scene_strip mirror so the two
      // counters stay consistent. Erasing shifts higher sends down by one
      // index on both sides, keeping them index-parallel.
      strip->strip.remove_send(send_index);
      if (send_index < strip->scene_strip.sends.size()) {
        strip->scene_strip.sends.erase(strip->scene_strip.sends.begin() +
                                       static_cast<std::ptrdiff_t>(send_index));
      }
      // Removing a send changes the strip node's port layout; recompile the routing
      // graph before the next process (matches sonare_strip_add_send).
      if (strip->owner != nullptr) {
        strip->owner->compiled_dirty = true;
      }
      return SONARE_OK;
      SONARE_C_CATCH
    }

    SonareError sonare_strip_meter(const SonareStrip* strip, SonareMixMeterSnapshot* out) {
      SONARE_C_API_ENTRY;
      if (!strip || !out) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      SONARE_C_TRY
      copy_meter_snapshot(strip->strip.meter_snapshot(), out);
      return SONARE_OK;
      SONARE_C_CATCH
    }

    SonareError sonare_strip_meter_tap(const SonareStrip* strip, int tap,
                                       SonareMixMeterSnapshot* out) {
      SONARE_C_API_ENTRY;
      if (!strip || !out) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      sonare::mixing::TapPoint tap_point;
      switch (tap) {
        case SONARE_METER_TAP_PRE_FADER:
          tap_point = sonare::mixing::TapPoint::PreFader;
          break;
        case SONARE_METER_TAP_POST_FADER:
          tap_point = sonare::mixing::TapPoint::PostFader;
          break;
        default:
          return SONARE_ERROR_INVALID_PARAMETER;
      }
      SONARE_C_TRY
      copy_meter_snapshot(strip->strip.meter_snapshot(tap_point), out);
      return SONARE_OK;
      SONARE_C_CATCH
    }

    size_t sonare_strip_read_goniometer_latest(const SonareStrip* strip,
                                               SonareMixGoniometerPoint* out, size_t max_points) {
      if (!strip || !out || max_points == 0) {
        return 0;
      }
      // SonareMixGoniometerPoint and sonare::mixing::GoniometerPoint are both
      // standard-layout {float left; float right;} pairs, so we can write the
      // result directly into the caller's buffer instead of allocating a temporary
      // vector on every (potentially per-frame, UI-rate) call. The static_asserts
      // pin the layout so this stays valid if either struct changes.
      using NativePoint = sonare::mixing::GoniometerPoint;
      static_assert(sizeof(SonareMixGoniometerPoint) == sizeof(NativePoint),
                    "GoniometerPoint size must match the C ABI struct");
      static_assert(offsetof(SonareMixGoniometerPoint, left) == offsetof(NativePoint, left),
                    "GoniometerPoint 'left' offset must match the C ABI struct");
      static_assert(offsetof(SonareMixGoniometerPoint, right) == offsetof(NativePoint, right),
                    "GoniometerPoint 'right' offset must match the C ABI struct");
      // read_goniometer_latest is noexcept, so no try/catch is required here.
      return strip->strip.read_goniometer_latest(reinterpret_cast<NativePoint*>(out), max_points);
    }
    SonareError sonare_strip_schedule_insert_automation(
        SonareStrip * strip, unsigned int insert_index, unsigned int param_id, int64_t sample_pos,
        float value, int curve) {
      SONARE_C_API_ENTRY;
      if (!strip || !finite(value)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      sonare::mixing::AutomationCurveType curve_enum;
      if (!parse_automation_curve(curve, &curve_enum)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      // Bound insert_index against the combined [pre... post...] insert count so
      // callers get an explicit bad-argument classification.
      const size_t insert_count = strip->strip.num_pre_inserts() + strip->strip.num_post_inserts();
      if (insert_index >= insert_count) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      // Reject obviously corrupt ids before descriptor lookup.
      constexpr unsigned int kMaxReasonableParamId = 65535u;
      if (param_id > kMaxReasonableParamId) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      switch (strip->strip.schedule_insert_automation_result(insert_index, param_id, sample_pos,
                                                             value, curve_enum)) {
        case sonare::mixing::InsertAutomationScheduleResult::Success:
          return SONARE_OK;
        case sonare::mixing::InsertAutomationScheduleResult::InvalidParameter:
          return SONARE_ERROR_INVALID_PARAMETER;
        case sonare::mixing::InsertAutomationScheduleResult::NotSupported:
          return SONARE_ERROR_NOT_SUPPORTED;
        case sonare::mixing::InsertAutomationScheduleResult::OutOfMemory:
          return SONARE_ERROR_OUT_OF_MEMORY;
      }
      return SONARE_ERROR_UNKNOWN;
    }

    SonareError sonare_strip_schedule_fader_automation(SonareStrip * strip, int64_t sample_pos,
                                                       float fader_db, int curve) {
      SONARE_C_API_ENTRY;
      if (!strip || !finite(fader_db)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      sonare::mixing::AutomationCurveType curve_enum;
      if (!parse_automation_curve(curve, &curve_enum)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      const auto result =
          strip->strip.schedule_fader_automation_result(sample_pos, fader_db, curve_enum);
      if (result == sonare::mixing::AutomationPushResult::NonMonotonic) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      if (result == sonare::mixing::AutomationPushResult::Full) {
        return SONARE_ERROR_OUT_OF_MEMORY;
      }
      return SONARE_OK;
    }

    SonareError sonare_strip_schedule_pan_automation(SonareStrip * strip, int64_t sample_pos,
                                                     float pan, int curve) {
      SONARE_C_API_ENTRY;
      if (!strip || !finite(pan)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      sonare::mixing::AutomationCurveType curve_enum;
      if (!parse_automation_curve(curve, &curve_enum)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      const auto result = strip->strip.schedule_pan_automation_result(sample_pos, pan, curve_enum);
      if (result == sonare::mixing::AutomationPushResult::NonMonotonic) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      if (result == sonare::mixing::AutomationPushResult::Full) {
        return SONARE_ERROR_OUT_OF_MEMORY;
      }
      return SONARE_OK;
    }

    SonareError sonare_strip_schedule_width_automation(SonareStrip * strip, int64_t sample_pos,
                                                       float width, int curve) {
      SONARE_C_API_ENTRY;
      if (!strip || !finite(width)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      sonare::mixing::AutomationCurveType curve_enum;
      if (!parse_automation_curve(curve, &curve_enum)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      const auto result =
          strip->strip.schedule_width_automation_result(sample_pos, width, curve_enum);
      if (result == sonare::mixing::AutomationPushResult::NonMonotonic) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      if (result == sonare::mixing::AutomationPushResult::Full) {
        return SONARE_ERROR_OUT_OF_MEMORY;
      }
      return SONARE_OK;
    }

    SonareError sonare_strip_schedule_send_automation(SonareStrip * strip, size_t send_index,
                                                      int64_t sample_pos, float db, int curve) {
      SONARE_C_API_ENTRY;
      if (!strip || !finite(db)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      sonare::mixing::AutomationCurveType curve_enum;
      if (!parse_automation_curve(curve, &curve_enum)) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      // Validate the send index up front so a bad argument is reported distinctly
      // from a capacity condition (mirrors the insert-automation convention above).
      if (send_index >= strip->strip.num_sends()) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      const auto result =
          strip->strip.schedule_send_automation_result(send_index, sample_pos, db, curve_enum);
      if (result == sonare::mixing::AutomationPushResult::NonMonotonic) {
        return SONARE_ERROR_INVALID_PARAMETER;
      }
      if (result == sonare::mixing::AutomationPushResult::Full) {
        return SONARE_ERROR_OUT_OF_MEMORY;
      }
      return SONARE_OK;
    }
