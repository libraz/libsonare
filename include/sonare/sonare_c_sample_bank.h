#pragma once

/// @file sonare_c_sample_bank.h
/// @brief Host-supplied PCM for the sample synthesis engine: a bank of float
///        samples plus key/velocity keymaps, bound alongside a synth patch.
///
/// The SoundFont path (@ref sonare_project_load_soundfont) parses a container
/// and brings its own generator model. This is the other door: PCM the caller
/// prepared elsewhere, described by nothing but a keymap, so a host that
/// already has its own waveforms does not have to author an SF2 to play them.
/// Decoding is the caller's job — the bank takes float frames.
///
/// A bank is built on the CONTROL thread and then read as immutable data. Add
/// every sample and zone before a render starts: the pool is contiguous and
/// moves as it grows, so a sample added while something sounds invalidates the
/// voices reading it.

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Opaque bank of samples and keymaps.
typedef struct SonareSampleBank SonareSampleBank;

/// @brief Tuning and looping of one sample, in units relative to that sample.
/// Zero-initialize then override: a zero-init desc is an unlooped sample rooted
/// at middle C and played at the render's own rate.
typedef struct {
  uint8_t root_key; /* key at which it sounds at its recorded pitch; 0 => 60 */
  float fine_tune_cents;
  double source_rate;  /* rate it was recorded at; 0 => the render's */
  uint32_t loop_start; /* frame offsets inside this sample */
  uint32_t loop_end;
  /* SoundFont sampleModes, the same values an SF2 zone carries: 0 = no loop,
     1 = continuous, 3 = loop while the key is held. This is the sample's own
     looping; SonareSynthPatch.sample_loop overrides it per patch. */
  int loop_mode;
} SonareSampleDesc;

/// @brief One key/velocity rectangle mapped onto a sample.
/// Zero-initialize then override: every bound defaults on its own, so narrowing
/// one edge never collapses another into an empty range. An upper bound of zero
/// reads as its maximum (127) and @c vel_lo of zero reads as 1, velocity zero
/// being a note-off rather than a dynamic; @c key_lo of zero is simply the
/// lowest key. So `{ .key_lo = 48 }` is 48..127 at every velocity, and an
/// untouched rectangle is the whole keyboard. The one rectangle this cannot
/// express is the single key 0.
typedef struct {
  uint8_t key_lo;
  uint8_t key_hi;
  uint8_t vel_lo;
  uint8_t vel_hi;
  uint32_t sample_index; /* as returned by sonare_sample_bank_add_sample */
  float tune_cents;      /* added to the sample's own fine tuning */
  float gain;            /* linear; 0 => 1.0 */
  float pan_units;       /* -500..500, the scale the voice mixer uses */
} SonareSampleZoneDesc;

/// @brief Creates an empty bank. NULL on allocation failure.
/// @details The caller owns the handle and must release it with @ref sonare_sample_bank_destroy,
///          which is what invalidates it; no other call does.
SonareSampleBank* sonare_sample_bank_create(void);

/// @brief Releases a bank. NULL is a no-op. Nothing rendering may still hold it.
void sonare_sample_bank_destroy(SonareSampleBank* bank);

/// @brief CONTROL thread: copies @p n_frames mono float frames into the bank and
///        writes the new sample's index to @p out_index (optional).
/// @details Loop points are clamped inside the sample and a loop mode whose loop
///          survives the clamp empty is dropped, so a malformed loop plays as an
///          unlooped sample rather than as a wrap over nothing.
///          SONARE_ERROR_INVALID_PARAMETER for a NULL bank/@p data, a zero
///          @p n_frames or a NULL @p desc; SONARE_ERROR_OUT_OF_MEMORY when the
///          bank would exceed 67,108,864 sample points, the same ceiling the
///          SoundFont loader applies so neither door is the cheaper way to
///          exhaust memory.
SonareError sonare_sample_bank_add_sample(SonareSampleBank* bank, const float* data,
                                          size_t n_frames, const SonareSampleDesc* desc,
                                          uint32_t* out_index);

/// @brief CONTROL thread: appends a zone to keymap set @p set_index, creating any
///        sets below it. A patch names a set; the first zone in it covering a
///        note is the one that sounds.
/// @details SONARE_ERROR_INVALID_PARAMETER for a NULL bank/@p zone, a
///          sample_index the bank does not have, an inverted key or velocity
///          range, or a @p set_index at or above 4096.
SonareError sonare_sample_bank_add_zone(SonareSampleBank* bank, uint32_t set_index,
                                        const SonareSampleZoneDesc* zone);

/// @brief Samples added so far.
SonareError sonare_sample_bank_sample_count(const SonareSampleBank* bank, size_t* out_count);

/// @brief Keymap sets the bank has (one past the highest index used).
SonareError sonare_sample_bank_set_count(const SonareSampleBank* bank, size_t* out_count);

#ifdef __cplusplus
}
#endif
