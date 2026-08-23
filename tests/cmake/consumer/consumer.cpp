// Compiles against the installed C++ headers and links the aggregate target.
// Both include spellings are exercised deliberately: the in-tree one, which the
// exported install-interface path has to keep working, and the namespaced one
// through the include root.
#include <sonare/cpp/core/convert.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include "sonare.h"

int main() {
  std::vector<float> samples(4096);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 44100.0f);
  }

  const auto audio = sonare::Audio::from_vector(std::move(samples), 44100);
  if (audio.size() != 4096) {
    std::fprintf(stderr, "unexpected sample count: %zu\n", audio.size());
    return 1;
  }

  const float midi = sonare::hz_to_midi(440.0f);
  if (std::fabs(midi - 69.0f) > 1e-3f) {
    std::fprintf(stderr, "hz_to_midi(440) = %f, expected 69\n", static_cast<double>(midi));
    return 1;
  }

  std::printf("libsonare %s: %zu samples at %d Hz, A4 = MIDI %.1f\n", SONARE_VERSION_STRING,
              audio.size(), audio.sample_rate(), static_cast<double>(midi));
  return 0;
}
