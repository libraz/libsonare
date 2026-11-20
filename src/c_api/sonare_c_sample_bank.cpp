#include <sonare/sonare_c.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "midi/synth/sample_bank.h"
#include "sample_bank_internal.h"
#include "sonare_c_internal.h"
#include "util/parameter_checks.h"

#if defined(SONARE_WITH_ARRANGEMENT)
namespace {

/// Total sample points one bank may hold, matching the SoundFont loader's cap
/// so neither door is the cheaper way to exhaust memory.
constexpr size_t kMaxBankSamplePoints = 67108864u;
/// Keymap sets are dense, so an index also sizes the table it creates.
constexpr uint32_t kMaxKeymapSets = 4096u;

}  // namespace
#endif  // defined(SONARE_WITH_ARRANGEMENT)

extern "C" {

SonareError sonare_sample_bank_create(SonareSampleBank** out) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out);
  SONARE_C_REQUIRE_NONNULL(out);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_TRY
  *out = new SonareSampleBank();
  return SONARE_OK;
  SONARE_C_CATCH
#else
  sonare_c_detail::set_last_error(SONARE_ERROR_NOT_SUPPORTED,
                                  "sample bank: arrangement support is not built");
  return SONARE_ERROR_NOT_SUPPORTED;
#endif
}

void sonare_sample_bank_destroy(SonareSampleBank* bank) { delete bank; }

SonareError sonare_sample_bank_add_sample(SonareSampleBank* bank, const float* data,
                                          size_t n_frames, const SonareSampleDesc* desc,
                                          uint32_t* out_index) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_index);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(bank);
  SONARE_C_REQUIRE_NONNULL(data);
  if (const SonareError err = sonare_c_detail::run_validation(
          [&] { sonare::util::check::integer_at_least(n_frames, 1, "n_frames"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(desc);
  // loop_mode is a SoundFont sampleModes value, 0..3; 2 is the spec's "unused, no loop".
  if (const SonareError err = sonare_c_detail::run_validation(
          [&] { sonare::util::check::integer_in_range(desc->loop_mode, 0, 3, "desc.loop_mode"); });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_TRY
  if (n_frames > kMaxBankSamplePoints ||
      bank->bank->pool_size() + n_frames > kMaxBankSamplePoints) {
    return SONARE_ERROR_OUT_OF_MEMORY;
  }
  if (const SonareError err = sonare_c_detail::run_validation([&] {
        for (size_t frame = 0; frame < n_frames; ++frame) {
          if (!std::isfinite(data[frame])) {
            const std::string subject = "data[" + std::to_string(frame) + "]";
            sonare::util::check::finite_float(data[frame], subject.c_str());
          }
        }
        sonare::util::check::finite_float(desc->fine_tune_cents, "desc.fine_tune_cents");
        sonare::util::check::finite(desc->source_rate, "desc.source_rate");
      });
      err != SONARE_OK) {
    return err;
  }

  sonare::midi::synth::SampleDesc cpp;
  // A sample rooted at note 0 is not a thing anyone records, so zero reads as
  // the ABI's usual "unset" and lands on middle C.
  cpp.root_key = desc->root_key != 0 ? desc->root_key : 60;
  cpp.fine_tune_cents = desc->fine_tune_cents;
  cpp.source_rate = desc->source_rate;
  cpp.loop_start = desc->loop_start;
  cpp.loop_end = desc->loop_end;
  cpp.loop_mode = desc->loop_mode;

  // Every refusal the bank can state was made above.
  (void)bank->bank->add_sample(data, n_frames, cpp, out_index);
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(bank, data, n_frames, desc, out_index);
#endif
}

SonareError sonare_sample_bank_add_zone(SonareSampleBank* bank, uint32_t set_index,
                                        const SonareSampleZoneDesc* zone) {
  SONARE_C_API_ENTRY;
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(bank);
  if (const SonareError err = sonare_c_detail::run_validation([&] {
        sonare::util::check::integer_in_range(set_index, 0, kMaxKeymapSets - 1, "set_index");
      });
      err != SONARE_OK) {
    return err;
  }
  SONARE_C_REQUIRE_NONNULL(zone);
  SONARE_C_TRY
  sonare::midi::synth::SampleZoneDesc cpp;
  // Every bound defaults on its own, so narrowing one edge never collapses
  // another into an empty range. An upper bound of zero is the unset one --
  // it would otherwise describe a zone that ends before it starts -- and
  // velocity zero is a note-off rather than a dynamic. A lower key bound of
  // zero is simply the lowest key and needs no rule. The one rectangle this
  // cannot express is the single key 0.
  cpp.key_lo = zone->key_lo;
  cpp.key_hi = zone->key_hi != 0 ? zone->key_hi : 127;
  cpp.vel_lo = zone->vel_lo != 0 ? zone->vel_lo : 1;
  cpp.vel_hi = zone->vel_hi != 0 ? zone->vel_hi : 127;
  cpp.sample_index = zone->sample_index;
  cpp.tune_cents = zone->tune_cents;
  cpp.gain = zone->gain != 0.0f ? zone->gain : 1.0f;
  cpp.pan_units = std::clamp(zone->pan_units, -500.0f, 500.0f);

  if (const SonareError err = sonare_c_detail::run_validation([&] {
        namespace check = sonare::util::check;
        if (cpp.key_lo > cpp.key_hi) {
          check::refuse_relation({"zone.key_lo", "zone.key_hi"}, "{0} must not exceed {1}");
        }
        if (cpp.vel_lo > cpp.vel_hi) {
          check::refuse_relation({"zone.vel_lo", "zone.vel_hi"}, "{0} must not exceed {1}");
        }
        if (cpp.sample_index >= bank->bank->sample_count()) {
          check::refuse_unknown_id("zone.sample_index", "a sample", "bank");
        }
        check::finite_float(zone->tune_cents, "zone.tune_cents");
        check::finite_float(zone->gain, "zone.gain");
        check::finite_float(zone->pan_units, "zone.pan_units");
      });
      err != SONARE_OK) {
    return err;
  }
  if (!bank->bank->add_zone(set_index, cpp)) {
    return sonare_c_detail::run_validation([&] {
      sonare::util::check::refuse_relation(
          {"zone.tune_cents", "zone.sample_index"},
          "{0} plus the fine tuning of the sample {1} names must be finite");
    });
  }
  return SONARE_OK;
  SONARE_C_CATCH
#else
  SONARE_C_STUB_NOT_SUPPORTED(bank, set_index, zone);
#endif
}

SonareError sonare_sample_bank_sample_count(const SonareSampleBank* bank, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(bank);
  SONARE_C_REQUIRE_NONNULL(out_count);
  *out_count = bank->bank->sample_count();
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(bank, out_count);
#endif
}

SonareError sonare_sample_bank_set_count(const SonareSampleBank* bank, size_t* out_count) {
  SONARE_C_API_ENTRY;
  SONARE_C_ZERO_OUTS(out_count);
#if defined(SONARE_WITH_ARRANGEMENT)
  SONARE_C_REQUIRE_NONNULL(bank);
  SONARE_C_REQUIRE_NONNULL(out_count);
  *out_count = bank->bank->set_count();
  return SONARE_OK;
#else
  SONARE_C_STUB_NOT_SUPPORTED(bank, out_count);
#endif
}

}  // extern "C"
