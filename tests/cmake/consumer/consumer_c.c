/* Compiles as C against the installed C ABI headers and links a single
 * exported archive rather than the aggregate, so the narrower link line stays
 * usable too. */
#include <sonare/sonare_c.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  const char* version = sonare_version();
  if (version == NULL || strlen(version) == 0) {
    fprintf(stderr, "sonare_version() returned nothing\n");
    return 1;
  }

  const float midi = sonare_hz_to_midi(440.0f);
  if (midi < 68.99f || midi > 69.01f) {
    fprintf(stderr, "sonare_hz_to_midi(440) = %f, expected 69\n", (double)midi);
    return 1;
  }

  printf("libsonare C ABI %s: A4 = MIDI %.1f\n", version, (double)midi);
  return 0;
}
