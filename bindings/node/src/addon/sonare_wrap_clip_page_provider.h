#ifndef SONARE_NODE_SONARE_WRAP_CLIP_PAGE_PROVIDER_H_
#define SONARE_NODE_SONARE_WRAP_CLIP_PAGE_PROVIDER_H_

#include <napi.h>
#include <sonare/sonare_c.h>

/// @brief N-API ObjectWrap over the paged clip source (@ref SonareClipPageProvider).
///
/// A provider belongs to no engine: a clip holds a shared reference to the page
/// storage, so destroying the handle while an engine still plays the clip is
/// safe. @ref Destroy is the deterministic release the facade exposes as
/// `destroy()`; the wrapper's destructor is the finalizer backstop.
class ClipPageProviderWrap : public Napi::ObjectWrap<ClipPageProviderWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  explicit ClipPageProviderWrap(const Napi::CallbackInfo& info);
  ~ClipPageProviderWrap();

  /// @brief Read the native provider a clip's `pageProvider` carries into @p out.
  ///
  /// A value that is not a ClipPageProvider instance is a TypeError and a
  /// destroyed one an InvalidState error, each naming `pageProvider` and
  /// leaving a false return so the caller stops before scheduling.
  static bool ReadHandle(Napi::Env env, const Napi::Value& value, SonareClipPageProvider** out);

 private:
  Napi::Value Supply(const Napi::CallbackInfo& info);
  Napi::Value Clear(const Napi::CallbackInfo& info);
  void Destroy(const Napi::CallbackInfo& info);

  SonareClipPageProvider* provider_ = nullptr;
};

#endif  // SONARE_NODE_SONARE_WRAP_CLIP_PAGE_PROVIDER_H_
