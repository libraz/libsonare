#include <sonare/sonare_c.h>

#include <algorithm>
#include <memory>

#include "midi/synth/sample_bank.h"
#include "sample_bank_internal.h"
#include "sonare_c_internal.h"

namespace {

/// Total sample points one bank may hold, matching the SoundFont loader's cap
/// so neither door is the cheaper way to exhaust memory.
constexpr size_t kMaxBankSamplePoints = 67108864u;
/// Keymap sets are dense, so an index also sizes the table it creates.
constexpr uint32_t kMaxKeymapSets = 4096u;

}  // namespace

extern "C" {

SonareSampleBank* sonare_sample_bank_create(void) {
  try {
    return new SonareSampleBank();
  } catch (...) {
    return nullptr;
  }
}

void sonare_sample_bank_destroy(SonareSampleBank* bank) { delete bank; }

SonareError sonare_sample_bank_add_sample(SonareSampleBank* bank, const float* data,
                                          size_t n_frames, const SonareSampleDesc* desc,
                                          uint32_t* out_index) {
  if (bank == nullptr || data == nullptr || n_frames == 0 || desc == nullptr) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  SONARE_C_TRY
  if (n_frames > kMaxBankSamplePoints ||
      bank->bank->pool_size() + n_frames > kMaxBankSamplePoints) {
    return SONARE_ERROR_OUT_OF_MEMORY;
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

  if (!bank->bank->add_sample(data, n_frames, cpp, out_index)) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_sample_bank_add_zone(SonareSampleBank* bank, uint32_t set_index,
                                        const SonareSampleZoneDesc* zone) {
  if (bank == nullptr || zone == nullptr || set_index >= kMaxKeymapSets) {
    return SONARE_ERROR_INVALID_PARAMETER;
  }
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

  if (!bank->bank->add_zone(set_index, cpp)) return SONARE_ERROR_INVALID_PARAMETER;
  return SONARE_OK;
  SONARE_C_CATCH
}

SonareError sonare_sample_bank_sample_count(const SonareSampleBank* bank, size_t* out_count) {
  if (bank == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = bank->bank->sample_count();
  return SONARE_OK;
}

SonareError sonare_sample_bank_set_count(const SonareSampleBank* bank, size_t* out_count) {
  if (bank == nullptr || out_count == nullptr) return SONARE_ERROR_INVALID_PARAMETER;
  *out_count = bank->bank->set_count();
  return SONARE_OK;
}

}  // extern "C"
