#pragma once

/// @file sample_bank_wasm.h
/// @brief The embind sample-bank handle, shared by the offline bounce and the
///        realtime engine so a patch's `sampleBank` means the same on both.
///
/// A binding descriptor names a bank by the id its handle carries rather than
/// by a pointer: an embind class instance cannot travel inside the plain JS
/// object the C++ readers walk field by field, and an id turns a released or
/// fabricated handle into an InvalidParameter instead of a use-after-free. The
/// TS facade does the swap, so hosts still pass the `SampleBank` itself.

#ifdef __EMSCRIPTEN__

#include <sonare/sonare_c.h>

#include <cmath>
#include <cstdint>

#include "wasm/bindings/common/common.h"

#if defined(SONARE_WITH_ARRANGEMENT)

// Host-supplied PCM for the sample synthesis engine, wrapping the opaque
// SonareSampleBank. Non-copyable: the JS handle is the sole owner, and a copy
// would destroy the bank twice.
//
// Built on the control thread and read as immutable data during a render, so
// every sample and zone belongs in the bank before the render that binds it
// starts. WASM has no host filesystem, so the frames arrive as a Float32Array
// the caller decoded itself.
class SampleBankWasm {
 public:
  SampleBankWasm();
  ~SampleBankWasm();
  SampleBankWasm(const SampleBankWasm&) = delete;
  SampleBankWasm& operator=(const SampleBankWasm&) = delete;

  // Identity a binding names this bank by (never zero).
  uint32_t id() const { return id_; }

  // Copies a mono Float32Array into the bank and returns the new sample's
  // index. The descriptor carries { rootKey, fineTuneCents, sourceRate,
  // loopStart, loopEnd, loopMode }; every field is optional and zero means the
  // C ABI's own default. loopMode takes the raw SoundFont sampleModes number or
  // its name ("none" / "continuous" / "key-down"), matching the Node reader.
  uint32_t addSample(val data, val desc);

  // Appends a { sampleIndex, keyLo, keyHi, velLo, velHi, tuneCents, gain,
  // panUnits } rectangle to keymap set @p set_index. Every bound defaults on
  // its own in the C ABI, so an absent field is left at zero here and narrowing
  // one axis leaves the other whole. The TS facade takes setIndex as a field of
  // the same object and splits it here, as the Node addon does.
  void addZone(const val& set_index_val, val zone);

  double sampleCount() const;
  double setCount() const;

  // The live bank for @p id, or nullptr once its handle has been released.
  static SonareSampleBank* lookup(uint32_t id);

  // The bank a synth-instrument descriptor's `sampleBankId` names, or nullptr
  // when the field is absent or zero. Throws for a malformed id and for one
  // whose handle is gone, so a bank released too early is a clear error rather
  // than silence.
  static SonareSampleBank* fromDescriptor(val desc) {
    if (!hasProperty(desc, "sampleBankId")) return nullptr;
    // Asked for its TYPE check: checkedUintFromVal reads through as<double>(),
    // which coerces a numeric string into an id rather than refusing it.
    requireNumberProperty(desc, "sampleBankId", "synth instrument");
    const uint32_t id = checkedUintFromVal(desc["sampleBankId"], "synth instrument sampleBankId");
    if (id == 0) return nullptr;
    SonareSampleBank* bank = lookup(id);
    if (bank == nullptr) {
      throw sonare::SonareException(sonare::ErrorCode::InvalidParameter,
                                    "synth instrument sampleBank has been released");
    }
    return bank;
  }

 private:
  SonareSampleBank* bank_ = nullptr;
  uint32_t id_ = 0;
};

// The sample bank is its own class_ handle rather than a slice of Project's:
// one bank outlives any single render and can be bound to several. Registered
// alongside the project because its C-ABI translation unit ships with the
// arrangement sources.
void registerSampleBank();

#endif  // SONARE_WITH_ARRANGEMENT

#endif  // __EMSCRIPTEN__
