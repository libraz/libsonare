#include "engine/strip_spec.h"

#include <memory>
#include <string>
#include <utility>

#include "engine/track_mixer_internal.h"
#include "mastering/api/insert_factory.h"
#include "mastering/api/named_processor.h"
#include "mixing/channel_strip_eq.h"
#include "mixing/pan_law.h"
#include "util/json.h"

namespace sonare::engine {

namespace {

// Every strip scalar the constructor config does not carry.
void apply_shared_strip_scalars(mixing::ChannelStrip& strip, const mixing::api::Strip& spec) {
  strip.set_vca_offset_db(spec.vca_offset_db);
  strip.set_width(spec.width);
  strip.set_muted(spec.muted);
  strip.set_soloed(spec.soloed);
  strip.set_solo_safe(spec.solo_safe);
  strip.set_pan_mode(to_pan_mode(spec.pan_mode));
  strip.set_dual_pan(spec.dual_pan_left, spec.dual_pan_right);
  strip.set_polarity_invert(spec.polarity_invert_left, spec.polarity_invert_right);
  strip.set_channel_delay_samples(spec.channel_delay_samples);
  strip.set_surround_pan_params({spec.surround_pan.azimuth, spec.surround_pan.elevation,
                                 spec.surround_pan.divergence, spec.surround_pan.lfe,
                                 spec.surround_pan.distance});
}

}  // namespace

std::unique_ptr<rt::ProcessorBase> make_insert_processor(const mixing::api::Insert& insert,
                                                         double build_sample_rate,
                                                         bool* stereo_pair_only) {
  auto processor =
      mastering::api::make_insert(insert.processor_name, insert.params_json, nullptr,
                                  resource::kDefaultProjectImportResourceLimits, build_sample_rate);
  if (!processor) return nullptr;
  *stereo_pair_only = mastering::api::channel_policy(insert.processor_name) ==
                      mastering::api::ChannelPolicy::StereoPairOnly;
  return processor;
}

std::unique_ptr<mixing::ChannelStrip> make_channel_strip_from_spec(const mixing::api::Strip& spec,
                                                                   double build_sample_rate) {
  mixing::ChannelStripConfig config{spec.fader_db,
                                    spec.pan,
                                    mixing::pan_law_from_index(spec.pan_law),
                                    5.0f,
                                    mixing::EqPosition::PreFader,
                                    spec.input_trim_db,
                                    false};
  config.mono_pan_centre_unity = true;
  auto strip = std::make_unique<mixing::ChannelStrip>(config);
  apply_shared_strip_scalars(*strip, spec);
  mixing::apply_strip_eq(*strip, spec.eq, nullptr);
  for (const auto& insert : spec.inserts) {
    bool spo = false;
    auto processor = make_insert_processor(insert, build_sample_rate, &spo);
    if (!processor) {
      return nullptr;
    }
    if (insert.slot == mixing::api::InsertSlot::PreFader) {
      strip->add_pre_insert(std::move(processor), spo);
    } else {
      strip->add_post_insert(std::move(processor), spo);
    }
  }
  return strip;
}

const std::string* strip_insert_processor_name_at(const mixing::api::Strip& spec,
                                                  unsigned int insert_index) noexcept {
  unsigned int index = 0;
  for (const auto& insert : spec.inserts) {
    if (insert.slot != mixing::api::InsertSlot::PreFader) continue;
    if (index == insert_index) return &insert.processor_name;
    ++index;
  }
  for (const auto& insert : spec.inserts) {
    if (insert.slot != mixing::api::InsertSlot::PostFader) continue;
    if (index == insert_index) return &insert.processor_name;
    ++index;
  }
  return nullptr;
}

namespace {

util::json::Value canonicalize_json_zeroes(const util::json::Value& value) {
  if (value.is_number()) {
    const double number = value.as_number();
    // The WASM facade's JSON round trip maps -0 to 0; match it so a scene resend compares equal.
    return util::json::Value(number == 0.0 ? 0.0 : number);
  }
  if (value.is_array()) {
    util::json::Array normalized;
    normalized.reserve(value.as_array().size());
    for (const util::json::Value& child : value.as_array()) {
      normalized.push_back(canonicalize_json_zeroes(child));
    }
    return util::json::Value(std::move(normalized));
  }
  if (value.is_object()) {
    util::json::Object normalized;
    for (const auto& [key, child] : value.as_object()) {
      normalized.emplace(key, canonicalize_json_zeroes(child));
    }
    return util::json::Value(std::move(normalized));
  }
  return value;
}

std::string canonical_insert_params(const std::string& params_json) {
  // Empty text (omitted params) stays distinct from `{}`; invalid text keeps its exact bytes.
  if (params_json.empty()) return std::string(1, '\0');
  try {
    return std::string(1, '\1') +
           util::json::dump(canonicalize_json_zeroes(util::json::parse(params_json)));
  } catch (...) {
    return std::string(1, '\2') + params_json;
  }
}

bool inserts_match(const mixing::api::Insert& left, const mixing::api::Insert& right) {
  return left.processor_name == right.processor_name &&
         canonical_insert_params(left.params_json) == canonical_insert_params(right.params_json) &&
         left.sidechain_key == right.sidechain_key;
}

}  // namespace

bool strip_inserts_equal(const std::vector<mixing::api::Insert>& a,
                         const std::vector<mixing::api::Insert>& b, bool canonical_track_order) {
  if (a.size() != b.size()) return false;
  if (!canonical_track_order) {
    for (size_t i = 0; i < a.size(); ++i) {
      if (a[i].slot != b[i].slot || !inserts_match(a[i], b[i])) return false;
    }
    return true;
  }

  // Track/master strips store pre and post chains separately. Scene JSON may
  // interleave their entries, so compare each stage in its stable within-stage
  // order rather than comparing the raw scene vector positions.
  const auto stage_matches = [&a, &b](mixing::api::InsertSlot slot) {
    size_t left_index = 0;
    size_t right_index = 0;
    while (true) {
      while (left_index < a.size() && a[left_index].slot != slot) ++left_index;
      while (right_index < b.size() && b[right_index].slot != slot) ++right_index;
      if (left_index == a.size() || right_index == b.size()) {
        return left_index == a.size() && right_index == b.size();
      }
      if (!inserts_match(a[left_index], b[right_index])) return false;
      ++left_index;
      ++right_index;
    }
  };
  return stage_matches(mixing::api::InsertSlot::PreFader) &&
         stage_matches(mixing::api::InsertSlot::PostFader);
}

void apply_strip_scalars(mixing::ChannelStrip& strip, const mixing::api::Strip& next,
                         const mixing::api::Strip& previous) {
  strip.set_fader_db(next.fader_db);
  strip.set_pan(next.pan);
  strip.set_pan_law(mixing::pan_law_from_index(next.pan_law));
  strip.set_input_trim_db(next.input_trim_db);
  apply_shared_strip_scalars(strip, next);
  mixing::apply_strip_eq(strip, next.eq, &previous.eq);
}

bool strip_eq_acceptable(const mixing::api::StripEq& eq, double sample_rate) noexcept {
  try {
    mixing::validate_eq(eq);
    for (const mastering::eq::EqBand& band : eq.bands) {
      (void)mastering::eq::design_eq_biquad(band, sample_rate);
    }
  } catch (...) {
    return false;
  }
  return true;
}

void store_eq_band(mixing::api::StripEq& eq, size_t band_index, const mastering::eq::EqBand& band) {
  if (eq.bands.size() <= band_index) eq.bands.resize(band_index + 1);
  eq.bands[band_index] = band;
}

}  // namespace sonare::engine
