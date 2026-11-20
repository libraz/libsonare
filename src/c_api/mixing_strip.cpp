#include <algorithm>
#include <array>

#include "c_api/mixing_internal.h"
#include "mixing/alignment_delay.h"
#include "mixing/api/scene.h"
#include "mixing/meter.h"
#include "util/constants.h"
#include "util/parameter_checks.h"

using namespace sonare_c_mixing_detail;
using sonare::constants::kDegreesPerHalfTurn;

namespace {

namespace check = sonare::util::check;

SonareError require_finite(float value, const char* subject) {
  return sonare_c_detail::run_validation([&] { check::finite_float(value, subject); });
}

// An index at or past @p count names nothing; @p thing names what it indexes.
SonareError require_index_below(size_t index, size_t count, const char* subject,
                                const char* thing) {
  return sonare_c_detail::run_validation([&] {
    if (count == 0) {
      check::refuse(
          subject, std::string("must be the index of ") + thing + " in this strip, which has none");
    }
    check::integer_in_range(static_cast<long long>(index), 0, static_cast<long long>(count) - 1,
                            subject);
  });
}

SonareError require_curve(int curve, sonare::mixing::AutomationCurveType* out) {
  constexpr int kLastCurve = static_cast<int>(sonare::mixing::AutomationCurveType::SCurve);
  const SonareError err = sonare_c_detail::run_validation(
      [&] { check::integer_in_range(curve, 0, kLastCurve, "curve"); });
  if (err == SONARE_OK) *out = static_cast<sonare::mixing::AutomationCurveType>(curve);
  return err;
}

SonareError refuse_sample_pos_order() {
  return sonare_c_detail::run_validation([] {
    check::refuse("sample_pos",
                  "must not be earlier than the last event scheduled for this target");
  });
}

SonareError refuse_out_of_domain(const char* subject, float lo, float hi) {
  return sonare_c_detail::run_validation(
      [&] { check::refuse(subject, sonare::util::refusal::range_predicate(lo, hi)); });
}

}  // namespace

SonareError sonare_mixer_create(int sample_rate, int max_block_size, SonareMixer** out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  if (max_block_size == 0) max_block_size = sonare::mixing::api::kMixerDefaultMaxBlock;
  if (const SonareError err = sonare_c_detail::run_validation([&] {
        check::integer_in_range(sample_rate, static_cast<long long>(sonare::kMinAudioSampleRate),
                                static_cast<long long>(sonare::kMaxAudioSampleRate), "sample_rate");
        check::integer_in_range(max_block_size, 1, sonare::kMaxRealtimeBlockSize, "max_block_size");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  auto mixer = std::make_unique<SonareMixer>();
  mixer->sample_rate = sample_rate;
  mixer->max_block_size = max_block_size;
  *out = mixer.release();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareStrip* sonare_mixer_add_strip(SonareMixer* mixer, const char* id) {
  SONARE_C_API_ENTRY;
  // The historical full metering configuration: LUFS + true peak at 4x.
  return sonare_mixer_add_strip_ex(mixer, id, 1, 1, 1, 0);
}

SonareStrip* sonare_mixer_add_strip_ex(SonareMixer* mixer, const char* id, int enable_metering,
                                       int measure_lufs, int measure_true_peak,
                                       int true_peak_oversample) {
  SONARE_C_API_ENTRY;
  if (!mixer) {
    sonare_c_detail::refuse_null("mixer");
    return nullptr;
  }
  if (!id) {
    sonare_c_detail::refuse_null("id");
    return nullptr;
  }
  if (true_peak_oversample != 0 &&
      !sonare::mixing::is_realtime_true_peak_oversample(true_peak_oversample)) {
    sonare_c_detail::run_validation([&] {
      check::one_of(true_peak_oversample, std::array<int, 3>{0, 4, 8}, "true_peak_oversample");
    });
    return nullptr;
  }
  SONARE_C_TRY
  const std::string strip_id = id;
  for (const auto& existing : mixer->strips) {
    if (existing->id == strip_id) {
      sonare_c_detail::run_validation(
          [] { check::refuse("id", "must not be the id of a strip already in this mixer"); });
      return nullptr;
    }
  }
  sonare::mixing::ChannelStripConfig config;
  config.enable_metering = enable_metering != 0;
  config.meter.measure_lufs = measure_lufs != 0;
  config.meter.measure_true_peak = measure_true_peak != 0;
  // Zero is the "use the default" sentinel, matching the rest of the C ABI.
  config.meter.true_peak_oversample =
      true_peak_oversample != 0 ? true_peak_oversample : config.meter.true_peak_oversample;
  auto strip = std::make_unique<SonareStrip>(config);
  strip->id = strip_id;
  strip->scene_strip.id = strip->id;
  strip->scene_strip.metering.enabled = config.enable_metering;
  strip->scene_strip.metering.lufs = config.meter.measure_lufs;
  strip->scene_strip.metering.true_peak = config.meter.measure_true_peak;
  strip->scene_strip.metering.true_peak_oversample = config.meter.true_peak_oversample;
  strip->owner = mixer;
  strip->strip.prepare(static_cast<double>(mixer->sample_rate), mixer->max_block_size);
  for (const auto& group : mixer->vca_groups) {
    if (std::find(group.members.begin(), group.members.end(), strip->id) != group.members.end()) {
      strip->strip.add_vca_group_offset_db(group.gain_db);
    }
  }
  SonareStrip* raw = strip.get();
  mixer->strips.push_back(std::move(strip));
  mixer->compiled_dirty = true;
  return raw;
  SONARE_C_CATCH_RETURN(nullptr)
}

SonareError sonare_strip_set_fader_db(SonareStrip* strip, float db) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  if (const SonareError err = require_finite(db, "db"); err != SONARE_OK) return err;
  SONARE_C_TRY
  strip->strip.set_fader_db(db);
  strip->scene_strip.fader_db = db;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_input_trim_db(SonareStrip* strip, float db) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  if (const SonareError err = require_finite(db, "db"); err != SONARE_OK) return err;
  SONARE_C_TRY
  strip->strip.set_input_trim_db(db);
  strip->scene_strip.input_trim_db = db;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_pan(SonareStrip* strip, float pan, int pan_mode) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  if (const SonareError err = sonare_c_detail::run_validation(
          [&] { check::in_range(pan, sonare::mixing::kPanMin, sonare::mixing::kPanMax, "pan"); });
      err != SONARE_OK) {
    return err;
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
  strip->scene_strip.pan = pan;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_dual_pan(SonareStrip* strip, float left_pan, float right_pan) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  if (const SonareError err = sonare_c_detail::run_validation([&] {
        check::in_range(left_pan, sonare::mixing::kPanMin, sonare::mixing::kPanMax, "left_pan");
        check::in_range(right_pan, sonare::mixing::kPanMin, sonare::mixing::kPanMax, "right_pan");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  strip->strip.set_pan_mode(sonare::mixing::PanMode::DualPan);
  strip->strip.set_dual_pan(left_pan, right_pan);
  // The nominal pan round-trips the value sonare_mixer_to_scene_json exports for a DualPan strip.
  strip->scene_strip.pan = 0.5f * (left_pan + right_pan);
  strip->scene_strip.pan_mode = SONARE_PAN_MODE_DUAL_PAN;
  strip->scene_strip.dual_pan_left = left_pan;
  strip->scene_strip.dual_pan_right = right_pan;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_surround_pan(SonareStrip* strip, const SonareSurroundPan* pan) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  SONARE_C_REQUIRE_NONNULL(pan);
  if (const SonareError err = sonare_c_detail::run_validation([&] {
        check::in_range(pan->azimuth, -kDegreesPerHalfTurn, kDegreesPerHalfTurn, "pan.azimuth");
        check::in_range(pan->divergence, 0.0, 1.0, "pan.divergence");
        check::in_range(pan->lfe, 0.0, 1.0, "pan.lfe");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  // Stored on the scene strip and mirrored onto the live strip below. The
  // engine's track mixer reads it when the lane feeds a >2-channel
  // destination; the stereo-only mixer block entry points do not.
  const sonare::mixing::SurroundPanParams stored =
      sonare::mixing::clamp_surround_pan_params({pan->azimuth, pan->divergence, pan->lfe});
  strip->scene_strip.surround_pan.azimuth = stored.azimuth;
  strip->scene_strip.surround_pan.divergence = stored.divergence;
  strip->scene_strip.surround_pan.lfe = stored.lfe;
  strip->strip.set_surround_pan_params(stored);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_width(SonareStrip* strip, float width) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  if (const SonareError err = sonare_c_detail::run_validation([&] {
        check::in_range(width, sonare::mixing::kWidthMin, sonare::mixing::kWidthMax, "width");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  strip->strip.set_width(width);
  strip->scene_strip.width = width;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_muted(SonareStrip* strip, int muted) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  SONARE_C_TRY
  strip->strip.set_muted(muted != 0);
  strip->scene_strip.muted = muted != 0;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_soloed(SonareStrip* strip, int soloed) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
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

SonareError sonare_strip_set_solo_safe(SonareStrip* strip, int solo_safe) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
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

SonareError sonare_strip_set_polarity_invert(SonareStrip* strip, int invert_left,
                                             int invert_right) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  SONARE_C_TRY
  strip->strip.set_polarity_invert(invert_left != 0, invert_right != 0);
  strip->scene_strip.polarity_invert_left = invert_left != 0;
  strip->scene_strip.polarity_invert_right = invert_right != 0;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_pan_law(SonareStrip* strip, int pan_law) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  SONARE_C_TRY
  strip->strip.set_pan_law(to_pan_law(pan_law));  // validates pan_law (throws on unknown)
  strip->scene_strip.pan_law = pan_law;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_channel_delay_samples(SonareStrip* strip, int delay_samples) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  if (const SonareError err = sonare_c_detail::run_validation([&] {
        check::integer_in_range(delay_samples, 0, sonare::mixing::kMaxAlignmentDelaySamples,
                                "delay_samples");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  strip->strip.set_channel_delay_samples(delay_samples);
  strip->scene_strip.channel_delay_samples = delay_samples;
  // Channel delay changes the strip's tail (not its latency); mark the graph
  // dirty so the next compile re-derives the mixer tail.
  if (strip->owner != nullptr) {
    strip->owner->compiled_dirty = true;
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_set_vca_offset_db(SonareStrip* strip, float offset_db) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  if (const SonareError err = require_finite(offset_db, "offset_db"); err != SONARE_OK) return err;
  SONARE_C_TRY
  strip->strip.set_vca_offset_db(offset_db);
  strip->scene_strip.vca_offset_db = offset_db;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_settle(SonareStrip* strip) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  SONARE_C_TRY
  // Nothing is mirrored into scene_strip: settling moves the smoothers to
  // the values the scene already records, so the scene is unchanged by it.
  strip->strip.settle();
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_add_send(SonareStrip* strip, const char* id,
                                  const char* destination_bus_id, float send_db, int timing,
                                  size_t* index_out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(index_out);
  SONARE_C_REQUIRE_NONNULL(strip);
  SONARE_C_REQUIRE_NONNULL(destination_bus_id);
  if (const SonareError err = require_finite(send_db, "send_db"); err != SONARE_OK) return err;
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

SonareError sonare_strip_set_send_db(SonareStrip* strip, size_t index, float send_db) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  // Reject an out-of-range send index rather than silently returning OK after a
  // no-op (the underlying ChannelStrip ignores unknown indices). Validate
  // against the live ChannelStrip's send count, the single source of truth
  // shared with sonare_strip_schedule_send_automation; add_send/remove_send keep
  // the live strip and the scene_strip mirror index-parallel.
  if (const SonareError err =
          require_index_below(index, strip->strip.num_sends(), "index", "a send");
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = require_finite(send_db, "send_db"); err != SONARE_OK) return err;
  SONARE_C_TRY
  strip->strip.set_send_db(index, send_db);
  strip->scene_strip.sends[index].send_db = send_db;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_remove_send(SonareStrip* strip, unsigned int index) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  // Validate against the live ChannelStrip's send count, the single source of
  // truth (see sonare_strip_set_send_db / sonare_strip_schedule_send_automation).
  if (const SonareError err = require_index_below(static_cast<size_t>(index),
                                                  strip->strip.num_sends(), "index", "a send");
      err != SONARE_OK) {
    return err;
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
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(strip);
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  copy_meter_snapshot(strip->strip.meter_snapshot(), out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_meter_tap(const SonareStrip* strip, int tap, SonareMixMeterSnapshot* out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(strip);
  sonare::mixing::TapPoint tap_point;
  switch (tap) {
    case SONARE_METER_TAP_PRE_FADER:
      tap_point = sonare::mixing::TapPoint::PreFader;
      break;
    case SONARE_METER_TAP_POST_FADER:
      tap_point = sonare::mixing::TapPoint::PostFader;
      break;
    default:
      return sonare_c_detail::run_validation([&] {
        check::integer_in_range(tap, SONARE_METER_TAP_PRE_FADER, SONARE_METER_TAP_POST_FADER,
                                "tap");
      });
  }
  SONARE_C_REQUIRE_NONNULL(out);
  SONARE_C_TRY
  copy_meter_snapshot(strip->strip.meter_snapshot(tap_point), out);
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_strip_non_finite_discard_count(const SonareStrip* strip, uint32_t* out_count) {
  // A single relaxed atomic load, so it is safe to poll from the audio
  // thread alongside the meters; it touches no diagnostic string.
  SONARE_C_RT_API_ENTRY;
  if (!strip || !out_count) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  *out_count = strip->strip.non_finite_discard_count();
  return SONARE_OK;
}

size_t sonare_strip_read_goniometer_latest(const SonareStrip* strip, SonareMixGoniometerPoint* out,
                                           size_t max_points) {
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
SonareError sonare_strip_schedule_insert_automation(SonareStrip* strip, unsigned int insert_index,
                                                    unsigned int param_id, int64_t sample_pos,
                                                    float value, int curve) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  // Bound insert_index against the combined [pre... post...] insert count so
  // callers get an explicit bad-argument classification.
  const size_t insert_count = strip->strip.num_pre_inserts() + strip->strip.num_post_inserts();
  if (const SonareError err =
          require_index_below(insert_index, insert_count, "insert_index", "an insert");
      err != SONARE_OK) {
    return err;
  }
  // Reject obviously corrupt ids before descriptor lookup.
  constexpr unsigned int kMaxReasonableParamId = 65535u;
  if (const SonareError err = sonare_c_detail::run_validation(
          [&] { check::integer_in_range(param_id, 0, kMaxReasonableParamId, "param_id"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  const auto descriptors = strip->strip.insert_processor(insert_index)->parameter_descriptors();
  if (std::none_of(descriptors.begin(), descriptors.end(),
                   [param_id](const auto& d) { return d.id == param_id; })) {
    return sonare_c_detail::run_validation(
        [] { check::refuse_unknown_id("param_id", "a parameter", "insert"); });
  }
  if (const SonareError err = require_finite(value, "value"); err != SONARE_OK) return err;
  sonare::mixing::AutomationCurveType curve_enum;
  if (const SonareError err = require_curve(curve, &curve_enum); err != SONARE_OK) return err;
  switch (strip->strip.schedule_insert_automation_result(insert_index, param_id, sample_pos, value,
                                                         curve_enum)) {
    case sonare::mixing::InsertAutomationScheduleResult::Success:
      return SONARE_OK;
    case sonare::mixing::InsertAutomationScheduleResult::InvalidParameter:
      return refuse_sample_pos_order();
    case sonare::mixing::InsertAutomationScheduleResult::NotSupported:
      return SONARE_ERROR_NOT_SUPPORTED;
    case sonare::mixing::InsertAutomationScheduleResult::OutOfMemory:
      return SONARE_ERROR_OUT_OF_MEMORY;
  }
  return SONARE_ERROR_UNKNOWN;
  SONARE_C_CATCH
}

SonareError sonare_strip_schedule_fader_automation(SonareStrip* strip, int64_t sample_pos,
                                                   float fader_db, int curve) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  if (const SonareError err = require_finite(fader_db, "fader_db"); err != SONARE_OK) return err;
  sonare::mixing::AutomationCurveType curve_enum;
  if (const SonareError err = require_curve(curve, &curve_enum); err != SONARE_OK) return err;
  const auto result =
      strip->strip.schedule_fader_automation_result(sample_pos, fader_db, curve_enum);
  if (result == sonare::mixing::AutomationPushResult::NonMonotonic) {
    return refuse_sample_pos_order();
  }
  if (result == sonare::mixing::AutomationPushResult::Full) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  }
  return SONARE_OK;
}

SonareError sonare_strip_schedule_pan_automation(SonareStrip* strip, int64_t sample_pos, float pan,
                                                 int curve) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  if (const SonareError err = require_finite(pan, "pan"); err != SONARE_OK) return err;
  sonare::mixing::AutomationCurveType curve_enum;
  if (const SonareError err = require_curve(curve, &curve_enum); err != SONARE_OK) return err;
  const auto result = strip->strip.schedule_pan_automation_result(sample_pos, pan, curve_enum);
  if (result == sonare::mixing::AutomationPushResult::NonMonotonic) {
    return refuse_sample_pos_order();
  }
  if (result == sonare::mixing::AutomationPushResult::OutOfDomain) {
    return refuse_out_of_domain("pan", sonare::mixing::kPanMin, sonare::mixing::kPanMax);
  }
  if (result == sonare::mixing::AutomationPushResult::Full) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  }
  return SONARE_OK;
}

SonareError sonare_strip_schedule_width_automation(SonareStrip* strip, int64_t sample_pos,
                                                   float width, int curve) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  if (const SonareError err = require_finite(width, "width"); err != SONARE_OK) return err;
  sonare::mixing::AutomationCurveType curve_enum;
  if (const SonareError err = require_curve(curve, &curve_enum); err != SONARE_OK) return err;
  const auto result = strip->strip.schedule_width_automation_result(sample_pos, width, curve_enum);
  if (result == sonare::mixing::AutomationPushResult::NonMonotonic) {
    return refuse_sample_pos_order();
  }
  if (result == sonare::mixing::AutomationPushResult::OutOfDomain) {
    return refuse_out_of_domain("width", sonare::mixing::kWidthMin, sonare::mixing::kWidthMax);
  }
  if (result == sonare::mixing::AutomationPushResult::Full) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  }
  return SONARE_OK;
}

SonareError sonare_strip_schedule_send_automation(SonareStrip* strip, size_t send_index,
                                                  int64_t sample_pos, float db, int curve) {
  SONARE_C_API_ENTRY;
  SONARE_C_REQUIRE_NONNULL(strip);
  // Validate the send index up front so a bad argument is reported distinctly
  // from a capacity condition (mirrors the insert-automation convention above).
  if (const SonareError err =
          require_index_below(send_index, strip->strip.num_sends(), "send_index", "a send");
      err != SONARE_OK) {
    return err;
  }
  if (const SonareError err = require_finite(db, "db"); err != SONARE_OK) return err;
  sonare::mixing::AutomationCurveType curve_enum;
  if (const SonareError err = require_curve(curve, &curve_enum); err != SONARE_OK) return err;
  const auto result =
      strip->strip.schedule_send_automation_result(send_index, sample_pos, db, curve_enum);
  if (result == sonare::mixing::AutomationPushResult::NonMonotonic) {
    return refuse_sample_pos_order();
  }
  if (result == sonare::mixing::AutomationPushResult::Full) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  }
  return SONARE_OK;
}
