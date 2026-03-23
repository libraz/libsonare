#include <napi.h>

#include "sonare_wrap.h"

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  SonareWrap::Init(env, exports);
  return exports;
}

NODE_API_MODULE(sonare, Init)
