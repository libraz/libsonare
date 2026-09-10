#ifndef SONARE_NODE_SONARE_WRAP_SAMPLE_BANK_H_
#define SONARE_NODE_SONARE_WRAP_SAMPLE_BANK_H_

#include <napi.h>
#include <sonare/sonare_c.h>

/// @brief N-API ObjectWrap over the opaque host-PCM sample bank
///        (@ref SonareSampleBank): the other door into the sample engine, for a
///        host that already has float waveforms and does not want to author an
///        SF2 for them.
///
/// A bank is built on the control thread and then read as immutable data, so
/// every sample and zone has to be added before the bounce that binds it starts.
/// The C handle is not GC-aware: @ref Destroy is the deterministic release the
/// facade exposes as `destroy()` / `Symbol.dispose`, and the wrapper's
/// destructor is the finalizer backstop.
class SampleBankWrap : public Napi::ObjectWrap<SampleBankWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  explicit SampleBankWrap(const Napi::CallbackInfo& info);
  ~SampleBankWrap();

  /// @brief Read the native bank a JS value carries into @p out.
  ///
  /// undefined / null leave @p out as nullptr, which the C ABI reads as "no
  /// bank" — a sample patch bound without one renders silence rather than
  /// failing. A value that is not a SampleBank instance, and an already
  /// destroyed bank, are each exactly one catchable TypeError and a false
  /// return, so the caller bails out before its C-ABI call.
  static bool ReadHandle(Napi::Env env, const Napi::Value& value, SonareSampleBank** out);

 private:
  Napi::Value AddSample(const Napi::CallbackInfo& info);
  Napi::Value AddZone(const Napi::CallbackInfo& info);
  Napi::Value SampleCount(const Napi::CallbackInfo& info);
  Napi::Value SetCount(const Napi::CallbackInfo& info);
  void Destroy(const Napi::CallbackInfo& info);

  SonareSampleBank* bank_ = nullptr;
};

#endif  // SONARE_NODE_SONARE_WRAP_SAMPLE_BANK_H_
