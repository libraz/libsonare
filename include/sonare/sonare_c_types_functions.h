#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sonare_c_types_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

// Audio functions
/// @note Release @p out with @ref sonare_audio_destroy; it is a handle, not a
///       sonare_free_* buffer.
SonareError sonare_audio_from_buffer(const float* data, size_t length, int sample_rate,
                                     SonareAudio** out);
/// @note Release @p out with @ref sonare_audio_destroy; it is a handle, not a
///       sonare_free_* buffer.
SonareError sonare_audio_from_memory(const uint8_t* data, size_t length, SonareAudio** out);

/// @brief Decodes audio bytes once and returns every source channel, without a
///        downmix.
/// @details Same format set, size ceiling and decoded-buffer contract as
///          @ref sonare_audio_from_memory (an empty decode or a non-finite sample
///          is @c SONARE_ERROR_DECODE_FAILED, a declared rate outside the
///          supported range is @c SONARE_ERROR_INVALID_FORMAT); only the channel
///          fold differs. @p out_samples holds @p out_channels planes of
///          @p out_frames samples each, plane after plane in the source's own
///          channel order (5.1 = L R C LFE Ls Rs). A NULL @p data or a zero
///          @p length returns @c SONARE_ERROR_INVALID_PARAMETER. Every output
///          is written only on @c SONARE_OK; on failure @p out_samples is NULL
///          and the counts are 0.
/// @note Free @p out_samples with @ref sonare_free_floats.
SonareError sonare_decode_channels(const uint8_t* data, size_t length, float** out_samples,
                                   size_t* out_frames, int* out_channels, int* out_sample_rate);

/// @brief Audio metadata copied from a decoded container.
/// @details @c keys and @c values each hold @c count separately allocated
///          NUL-terminated strings. An empty result has both pointers NULL.
///          Release both arrays and their strings with
///          @ref sonare_free_audio_tags.
typedef struct {
  char** keys;    ///< Heap strings for the metadata keys; count entries.
  char** values;  ///< Heap strings for the metadata values; count entries.
  size_t count;   ///< Number of key/value pairs.
} SonareAudioTags;

#ifdef __cplusplus
static_assert(offsetof(SonareAudioTags, keys) == 0, "SonareAudioTags.keys offset");
static_assert(offsetof(SonareAudioTags, values) == sizeof(char**), "SonareAudioTags.values offset");
static_assert(offsetof(SonareAudioTags, count) ==
                  ((2u * sizeof(char**) + alignof(size_t) - 1u) / alignof(size_t)) *
                      alignof(size_t),
              "SonareAudioTags.count offset");
#endif

/// @brief Reads container metadata without decoding the audio samples.
/// @details Tags use the normalized names documented by the audio codec
///          contract. Malformed individual tag frames are skipped. On failure,
///          @p out is cleared. A NULL or empty @p data buffer is refused.
SonareError sonare_read_audio_tags(const uint8_t* data, size_t size, SonareAudioTags* out);
/// @brief Releases every key and value in @p tags, then clears it; NULL is ignored.
void sonare_free_audio_tags(SonareAudioTags* tags);

/// @brief Options for @ref sonare_encode_audio.
/// @details Start from @ref sonare_encode_options_init. A NULL options pointer
///          selects its defaults. Pointer members remain caller-owned and are
///          copied by the call. The format tokens are lowercase. WAV and FLAC
///          accept bit depths 16 or 24, and WAV also accepts 32-bit float;
///          lossy formats accept quality in [0, 1] and reject bit_depth.
typedef struct {
  uint32_t struct_size;         ///< sizeof this struct; set by sonare_encode_options_init.
  const char* format;           ///< Exact lowercase token: wav, flac, mp3, ogg, opus, or m4a.
  int32_t bit_depth;            ///< 0 selects the format default; otherwise format-specific.
  float quality;                ///< NaN selects the format default; otherwise in [0, 1].
  const SonareAudioTags* tags;  ///< Optional tags to write; NULL writes no tags.
} SonareEncodeOptions;

#ifdef __cplusplus
static_assert(offsetof(SonareEncodeOptions, struct_size) == 0,
              "SonareEncodeOptions.struct_size offset");
static_assert(offsetof(SonareEncodeOptions, format) ==
                  ((sizeof(uint32_t) + alignof(const char*) - 1u) / alignof(const char*)) *
                      alignof(const char*),
              "SonareEncodeOptions.format offset");
static_assert(offsetof(SonareEncodeOptions, bit_depth) ==
                  offsetof(SonareEncodeOptions, format) + sizeof(const char*),
              "SonareEncodeOptions.bit_depth offset");
static_assert(offsetof(SonareEncodeOptions, quality) ==
                  offsetof(SonareEncodeOptions, bit_depth) + sizeof(int32_t),
              "SonareEncodeOptions.quality offset");
static_assert(offsetof(SonareEncodeOptions, tags) ==
                  ((offsetof(SonareEncodeOptions, quality) + sizeof(float) +
                    alignof(const SonareAudioTags*) - 1u) /
                   alignof(const SonareAudioTags*)) *
                      alignof(const SonareAudioTags*),
              "SonareEncodeOptions.tags offset");
static_assert(sizeof(SonareEncodeOptions) ==
                  ((offsetof(SonareEncodeOptions, tags) + sizeof(const SonareAudioTags*) +
                    alignof(const SonareAudioTags*) - 1u) /
                   alignof(const SonareAudioTags*)) *
                      alignof(const SonareAudioTags*),
              "SonareEncodeOptions layout");
#endif

/// @brief Writes the defaults and sizeof into @p options; NULL is ignored.
/// @details Defaults are WAV, bit_depth 0 (the format default), NaN quality
///          (the format default), and no tags.
///          Initialization does not transfer ownership.
void sonare_encode_options_init(SonareEncodeOptions* options);

/// @brief Encodes planar samples into an in-memory audio file.
/// @details @p samples contains @p num_channels contiguous planes of @p frames
///          samples each. A NULL @p options selects 16-bit WAV defaults. WAV
///          is built in; optional formats require the corresponding FFmpeg
///          encoder and muxer and return @c SONARE_ERROR_NOT_SUPPORTED when
///          unavailable. Outputs are cleared on failure.
/// @note Release @p out with @ref sonare_free_bytes.
SonareError sonare_encode_audio(const float* samples, size_t frames, int num_channels,
                                int sample_rate, const SonareEncodeOptions* options, uint8_t** out,
                                size_t* out_size);

/// @brief Fold options for @ref sonare_downmix.
/// @details Fill with @ref sonare_downmix_options_init before setting fields.
typedef struct {
  /// Size of this struct in bytes, written by @ref sonare_downmix_options_init.
  uint32_t struct_size;
  /// Non-zero folds the LFE plane into the front pair at -3 dB instead of
  /// dropping it. Default 0.
  int32_t include_lfe;
  /// Non-zero scales the output so the worst case, every input at full scale and
  /// in phase, stays at or below unity. Default 0.
  int32_t normalize;
} SonareDownmixOptions;

/// @brief Writes the defaults (both options off) and @c struct_size into
///        @p options; a NULL @p options is ignored.
void sonare_downmix_options_init(SonareDownmixOptions* options);

/// @brief Downmixes planar channels to a narrower layout (ITU-R BS.775).
/// @details @p channels holds @p num_channels planes of @p frames samples each. The
///          source layout is the one @p num_channels names (1 mono, 2 stereo, 6 5.1,
///          8 7.1; plane order as in @ref sonare_decode_channels). Center and
///          surround enter the front pair at -3 dB and the LFE plane is dropped
///          unless @p options asks to fold it. The BS.775 fold is not normalised, so
///          a source with several full-scale planes can fold past full scale (six
///          planes of 1.0 reach 2.414 on a stereo target) unless @p options asks to
///          normalise. A stereo/5.1/7.1 source folds to mono through the same matrix
///          the decoders use. @p target_layout is a @ref SonareChannelLayout value.
///          Supported targets narrow the bed: 7.1 to 5.1, 5.1 and 7.1 to stereo,
///          stereo/5.1/7.1 to mono, and the identity copy. A channel count outside
///          1/2/6/8 folds to @c SONARE_CHANNEL_LAYOUT_MONO as the unweighted mean of
///          its planes whatever the options, which is what the decoders do for such
///          a source, and has no other target. A NULL @p options takes the defaults.
///          Anything else (an upmix, an unknown @p target_layout, a zero @p frames or
///          @p num_channels, a NULL pointer, an @p options whose @c struct_size is
///          smaller than this struct) returns @c SONARE_ERROR_INVALID_PARAMETER.
///          @p out_samples receives the target layout's channel count of planes,
///          @p frames samples each.
/// @note Free @p out_samples with @ref sonare_free_floats.
SonareError sonare_downmix(const float* channels, size_t frames, size_t num_channels,
                           int target_layout, const SonareDownmixOptions* options,
                           float** out_samples);

#ifndef __EMSCRIPTEN__
/// @brief @ref SonareAudioFileOptions::channel_index value that downmixes every channel.
#define SONARE_AUDIO_CHANNEL_DOWNMIX (-1)

/// @brief Options for @ref sonare_audio_from_file.
/// @details Fill with @ref sonare_audio_file_options_init before setting fields.
typedef struct {
  /// Size of this struct in bytes, written by @ref sonare_audio_file_options_init.
  uint32_t struct_size;
  /// Channel to load, or @ref SONARE_AUDIO_CHANNEL_DOWNMIX (the default) to downmix every
  /// channel. Any other negative value, or an index the file has no channel for, is refused
  /// with @c SONARE_ERROR_INVALID_PARAMETER.
  int32_t channel_index;
} SonareAudioFileOptions;

/// @brief Writes the defaults and @c struct_size into @p options; a NULL @p options is ignored.
void sonare_audio_file_options_init(SonareAudioFileOptions* options);

/// @brief Loads an audio file into a one-plane handle.
/// @details A handle carries one plane, so a multi-channel source is downmixed into it unless
///          @p options selects one channel, which is what a stereo render needs: pair it with
///          @ref sonare_audio_file_num_channels and load each channel in turn. The file is
///          decoded once per call. A NULL @p options downmixes. An empty decode or a
///          non-finite sample in the requested plane is @c SONARE_ERROR_DECODE_FAILED, a
///          declared rate outside the supported range is @c SONARE_ERROR_INVALID_FORMAT.
/// @note Release @p out with @ref sonare_audio_destroy; it is a handle, not a
///       sonare_free_* buffer.
SonareError sonare_audio_from_file(const char* path, const SonareAudioFileOptions* options,
                                   SonareAudio** out);
/// @brief Returns the positive source channel count reported by an audio file.
/// @details @p out_channels is set to 0 immediately when it is non-NULL, before
///          argument validation or file inspection. On @c SONARE_OK it receives
///          a positive channel count and is left at 0 on every failure. Both
///          @p path and @p out_channels must be non-NULL; either NULL argument
///          returns @c SONARE_ERROR_INVALID_PARAMETER. A missing/unreadable file
///          returns @c SONARE_ERROR_FILE_NOT_FOUND. A malformed recognized file
///          or decoder/probe failure returns @c SONARE_ERROR_DECODE_FAILED.
///          An unknown or unsupported format returns @c SONARE_ERROR_INVALID_FORMAT
///          when no FFmpeg decoder is enabled; with FFmpeg enabled, failures
///          while probing that format return @c SONARE_ERROR_DECODE_FAILED.
SonareError sonare_audio_file_num_channels(const char* path, int* out_channels);
#endif

/// @brief Releases @p audio; a NULL @p audio is ignored.
void sonare_audio_destroy(SonareAudio* audio);
/// @brief Borrowed view of the handle's mono samples, or NULL for a NULL
///        @p audio.
/// @details Owned by the @ref SonareAudio handle, not the caller: valid until
///          @ref sonare_audio_destroy, and never to be freed separately. Use
///          @ref sonare_audio_length for the element count, which is a sample
///          count and not frames times channels: a handle carries one plane, and
///          a multi-channel source is downmixed as the handle is built.
///          @ref sonare_audio_file_num_channels reports the channel count of a
///          file on disk, never the handle's.
const float* sonare_audio_data(const SonareAudio* audio);
/// @brief Sample count of @p audio's one plane, or 0 for a NULL @p audio.
size_t sonare_audio_length(const SonareAudio* audio);
/// @brief Sample rate of @p audio in Hz, or 0 for a NULL @p audio.
int sonare_audio_sample_rate(const SonareAudio* audio);
/// @brief Duration of @p audio in seconds, or 0 for a NULL @p audio.
float sonare_audio_duration_sec(const SonareAudio* audio);
/// @brief Detects the tempo of @p audio within a search range.
/// @details @p bpm_min and @p bpm_max follow @ref sonare_detect_bpm.
SonareError sonare_audio_detect_bpm(const SonareAudio* audio, float bpm_min, float bpm_max,
                                    float* out_bpm);
/// @brief Detects the key of @p audio, measuring the recording's tuning first.
SonareError sonare_audio_detect_key(const SonareAudio* audio, SonareKey* out_key);
/// @details @p bpm_min and @p bpm_max follow @ref sonare_detect_bpm.
/// @note Free @p out_times with @ref sonare_free_floats.
SonareError sonare_audio_detect_beats(const SonareAudio* audio, float bpm_min, float bpm_max,
                                      float** out_times, size_t* out_count);
/// @note Free @p out_times with @ref sonare_free_floats.
SonareError sonare_audio_detect_downbeats(const SonareAudio* audio, float** out_times,
                                          size_t* out_count);
/// @note Free @p out_times with @ref sonare_free_floats.
SonareError sonare_audio_detect_onsets(const SonareAudio* audio, float** out_times,
                                       size_t* out_count);
// Quick detection functions
/// @brief Detects the tempo within a search range.
/// @param bpm_min Lower bound of the tempo search in BPM; finite and positive.
/// @param bpm_max Upper bound in BPM; finite and above @p bpm_min. A range that is
///        not (non-positive, non-finite or inverted) is INVALID_PARAMETER, as for
///        @ref sonare_analyze_bpm. Pass 60 and 200 for the search the other
///        public tempo entries default to. Any other range is searched as given,
///        and the 60-200 estimate is kept when it lies inside it.
/// @param out_bpm Receives the tempo, or NaN (not measured) when none was found.
SonareError sonare_detect_bpm(const float* samples, size_t length, int sample_rate, float bpm_min,
                              float bpm_max, float* out_bpm);
/// @brief Options for sonare_detect_key and sonare_detect_key_candidates. Boolean fields are 0/1.
/// @details Start from sonare_detect_key_options_init; its values are the defaults named below.
/// @note Pointer members stay the caller's; the call does not transfer ownership.
typedef struct {
  uint32_t struct_size;  ///< sizeof of this struct; set by sonare_detect_key_options_init.
  /// FFT size in samples at 22050 Hz, rescaled to the analysis rate. Default 2048.
  int n_fft;
  /// Hop in samples of the analysed signal: the input resampled to 22050 Hz when its rate is above
  /// 22050 Hz, the input itself otherwise. Default 512.
  size_t hop_length;
  /// Non-zero applies harmonic-percussive separation before the chroma. Default 0.
  int use_hpss;
  /// Non-zero weights chroma frames by loudness. Default 0.
  int loudness_weighted;
  /// High-pass cutoff for the chroma in Hz; 0 disables it. Default 80.
  float chroma_high_pass_hz;
  /// Modes the key may take, @c mode_count entries; NULL with 0 selects major and minor.
  const SonareMode* modes;
  size_t mode_count;  ///< Number of entries of @c modes. Default 0.
  /// Key-profile family the chroma is correlated against. Default SONARE_KEY_PROFILE_AUTO.
  SonareKeyProfileType profile_type;
  /// Optional genre name biasing the profile choice; NULL or empty means automatic.
  const char* genre_hint;
  /// Recording tuning in cents, in [-50, 50) where 0 is concert A440; read only when
  /// @c tuning_auto is 0. Default 0.
  float tuning_cents;
  /// Non-zero measures the tuning from the audio instead of reading @c tuning_cents. Default 1.
  int tuning_auto;
} SonareDetectKeyOptions;

/// @brief Writes the defaults and @c struct_size into @p options; a NULL @p options is ignored.
void sonare_detect_key_options_init(SonareDetectKeyOptions* options);

/// @brief Detects the key.
/// @param options NULL selects the defaults of @ref sonare_detect_key_options_init.
/// @param out_tuning_cents Optional; receives the tuning used, given or measured, in cents.
SonareError sonare_detect_key(const float* samples, size_t length, int sample_rate,
                              const SonareDetectKeyOptions* options, SonareKey* out_key,
                              float* out_tuning_cents);
/// @brief Lists the key candidates, best first.
/// @details @p options and @p out_tuning_cents follow @ref sonare_detect_key.
/// @note Free @p out_candidates with @ref sonare_free_key_candidates.
SonareError sonare_detect_key_candidates(const float* samples, size_t length, int sample_rate,
                                         const SonareDetectKeyOptions* options,
                                         SonareKeyCandidate** out_candidates, size_t* out_count,
                                         float* out_tuning_cents);
/// @brief Detects beat times within a tempo search range.
/// @details @p bpm_min and @p bpm_max follow @ref sonare_detect_bpm.
/// @note Free @p out_times with @ref sonare_free_floats.
SonareError sonare_detect_beats(const float* samples, size_t length, int sample_rate, float bpm_min,
                                float bpm_max, float** out_times, size_t* out_count);
/// @note Free @p out_times with @ref sonare_free_floats.
SonareError sonare_detect_downbeats(const float* samples, size_t length, int sample_rate,
                                    float** out_times, size_t* out_count);
/// @brief Controls peak-picking for @ref sonare_detect_onsets.
/// @details Start from @ref sonare_onset_detect_config_init, then set the fields to change.
///
///          @p pre_max, @p post_max, @p pre_avg, @p post_avg, @p wait and
///          @p backtrack_range are FRAME counts, and a frame is @p hop_length
///          divided by the analysis rate. That rate is 22050 Hz on both entry
///          points -- input above it is resampled down before detection -- so a
///          field means the same span of audio whatever rate the caller's buffer
///          carries.
typedef struct {
  uint32_t struct_size;  ///< sizeof of this struct; set by sonare_onset_detect_config_init.
  /// FFT window length in samples at 22050 Hz, rescaled to the input rate. Default 2048.
  int32_t n_fft;
  /// Hop in samples at the analysis rate. Default 512.
  size_t hop_length;
  /// Minimum onset strength a peak must reach, greater than 0; NaN (the default) selects the
  /// adaptive path, where the threshold follows the running mean of the envelope.
  /// Expressed in the units of the onset envelope this call computes internally,
  /// which is not normalized, so the usable range depends on the magnitudes the
  /// caller's own audio produces and cannot be derived from the signature. The
  /// envelope is the same one sonare_onset_strength returns, so run that on
  /// representative audio to see the scale before setting this.
  float threshold;
  /// Frames before a peak that must not exceed it. Default 1.
  int32_t pre_max;
  /// Frames after a peak that must not exceed it. Default 1.
  int32_t post_max;
  /// Frames before a peak averaged for the adaptive threshold. Default 3.
  int32_t pre_avg;
  /// Frames after a peak averaged for the adaptive threshold. Default 4.
  int32_t post_avg;
  /// Offset added to the running mean of the onset envelope, in that envelope's
  /// own units -- the same quantity and the same caveat as sonare_peak_pick's
  /// delta, which this field ultimately feeds. Not a normalized or dB value.
  /// Take a small fraction of the envelope's mean or median and adjust. Default 0.06.
  float delta;
  /// Minimum frames between consecutive onsets. Default 1.
  int32_t wait;
  /// Non-zero moves each onset back to the preceding local minimum of the envelope; only 0 and 1
  /// are accepted. Default 0.
  uint8_t backtrack;
  /// Padding; set to 0.
  uint8_t reserved[3];
  /// Search range in frames for the backtracking. Default 10.
  int32_t backtrack_range;
} SonareOnsetDetectConfig;

#ifdef __cplusplus
static_assert(offsetof(SonareOnsetDetectConfig, hop_length) == 8u,
              "SonareOnsetDetectConfig hop_length offset");
static_assert(offsetof(SonareOnsetDetectConfig, backtrack_range) == 8u + sizeof(size_t) + 32u,
              "SonareOnsetDetectConfig backtrack_range offset");
static_assert(sizeof(SonareOnsetDetectConfig) ==
                  (8u + sizeof(size_t) + 36u + alignof(size_t) - 1u) / alignof(size_t) *
                      alignof(size_t),
              "SonareOnsetDetectConfig layout drift");
#endif

/// @brief Fills @p config with sizeof and the library defaults, the threshold being NaN
///        (adaptive). A NULL @p config is ignored.
void sonare_onset_detect_config_init(SonareOnsetDetectConfig* config);

/// @brief Detects onset times.
/// @param config NULL selects the defaults of @ref sonare_onset_detect_config_init.
/// @note Free @p out_times with @ref sonare_free_floats.
SonareError sonare_detect_onsets(const float* samples, size_t length, int sample_rate,
                                 const SonareOnsetDetectConfig* config, float** out_times,
                                 size_t* out_count);

/// Progress callback for the JSON analysis: progress in [0,1] plus a
/// stage label. user_data is the opaque pointer passed to the analyze call.
typedef void (*SonareAnalyzeProgressCallback)(float progress, const char* stage, void* user_data);

/// Cooperative cancellation callback for long-running offline operations.
/// Return nonzero to stop after the current progress-report boundary.
typedef int (*SonareCancelCallback)(void* user_data);

/// Capacity of SonareMusicAnalyzeOptions::meter_candidate_numerators. Part of
/// the public contract: a caller cannot request more numerators than this, and
/// every binding rejects an over-long list rather than truncating it.
#define SONARE_MAX_METER_CANDIDATE_NUMERATORS 16

/// @brief Options for sonare_analyze_json. Boolean fields are 0/1 ints.
/// @details Start from sonare_music_analyze_options_init(): a zeroed
///   meter_candidate_numerator_count is rejected, not treated as "use the default set".
///   Defaults below are those of that function.
/// @note Pointer members stay the caller's; the call does not transfer ownership.
typedef struct {
  uint32_t struct_size;  ///< sizeof of this struct; set by sonare_music_analyze_options_init.
  /// Analysis window in samples at 22050 Hz, rescaled to the analysis rate. Default 2048.
  int n_fft;
  /// Hop in samples of the analysed signal: the input resampled to 22050 Hz when its rate is above
  /// 22050 Hz, the input itself otherwise. Default 512.
  size_t hop_length;
  /// Lower bound of the tempo search in BPM. Default 60.
  float bpm_min;
  /// Upper bound of the tempo search in BPM. Default 200.
  float bpm_max;
  /// Finite, positive BPM prior that seeds beat tracking if the tempo search finds none.
  /// Analysis reports measured tempo or absence. Default 120.
  float start_bpm;
  /// Non-zero restricts chord detection to triads, with no seventh chords. Default 1.
  int use_triads_only;
  /// Non-zero applies harmonic-percussive separation before the chord chroma. Default 0.
  int use_hpss;
  /// High-pass cutoff for the chroma in Hz; 0 disables it. Default 80.
  float chroma_high_pass_hz;
  /// Non-zero weights the chord chroma toward the bass.
  int use_bass_weighted;
  /// Multiplier on hop_length for the chroma; larger is faster. Default 4.
  int chroma_hop_multiplier;
  /// Non-zero smooths chords with a Viterbi HMM. Default 1.
  int use_chord_hmm;
  /// Non-zero biases the chord HMM transitions using the detected key. Default 0.
  int use_chord_key_context;
  /// Candidate beam width of the chord HMM smoothing. Default 24.
  int chord_hmm_beam_width;
  /// Non-zero estimates slash-chord bass notes. Default 0.
  int detect_chord_inversions;
  /// Non-zero tracks a locally updated tempo prior through beat tracking. Default 0.
  int adaptive_tempo;
  /// Local tempo context length in beats; used only when adaptive_tempo is 1. Default 8.
  int tempo_update_interval_beats;
  /// Non-zero decodes a per-beat local tempo curve into the result's beatLocalBpm array.
  /// Off by default: it is an extra output rather than a better analysis, so a
  /// caller that does not read the curve would pay a decode over the beat grid
  /// for nothing. The curve describes the beat grid it was decoded from, and
  /// beat tracking holds a fixed tempo prior unless adaptive_tempo is also 1, so
  /// measuring a tempo that moves needs both set. The values are continuous,
  /// each a weighted average in log tempo of the surrounding beat intervals: a
  /// steady tempo reads within about 1% at every beat, and a moving one is
  /// followed a few beats late.
  int compute_tempo_curve;
  /// Meter numerators the estimator scores. Only the first
  /// meter_candidate_numerator_count entries are read; each must be in [2, 32].
  /// Widening the set does not force a wider meter; the default {3, 4, 6}
  /// reproduces the library's standard result.
  int meter_candidate_numerators[SONARE_MAX_METER_CANDIDATE_NUMERATORS];
  /// Number of entries of meter_candidate_numerators that are read; at least 1.
  size_t meter_candidate_numerator_count;
  /// Beat unit reported for the detected meter; a power of two in [1, 32]. The
  /// estimator still reports 8 on its own when it resolves a compound meter. Default 4.
  int meter_denominator;
  /// Tuning offset of the recording in cents; 0 is concert A440. Must be in
  /// [-50, 50). Every chroma the analysis builds (key, chords, sections) is
  /// shifted by it. sonare_reference_hz_to_tuning converts a reference pitch
  /// to this unit.
  float tuning_cents;
  /// Non-zero measures the tuning from the audio instead of reading
  /// tuning_cents. Default 1. The value used, given or measured, is
  /// the result's "tuningCents".
  int tuning_auto;
  /// Per-stage progress report; NULL for none. Default NULL.
  SonareAnalyzeProgressCallback progress_callback;
  /// Passed back to @c progress_callback unchanged. Default NULL.
  void* progress_user_data;
  /// Polled at progress boundaries; a non-zero return stops the analysis with
  /// SONARE_ERROR_CANCELLED and leaves the output NULL. NULL for none. Default NULL.
  SonareCancelCallback cancel_callback;
  /// Passed back to @c cancel_callback unchanged. Default NULL.
  void* cancel_user_data;
} SonareMusicAnalyzeOptions;

/// @brief Fills @p options with sizeof and the library defaults. A NULL @p options is ignored.
void sonare_music_analyze_options_init(SonareMusicAnalyzeOptions* options);
/// @brief Full analysis serialized to a camelCase JSON object.
/// @details Returns the complete result: tempo, key, beats with per-beat strength, chords,
///   sections, timbre, dynamics, rhythm, melody and form.
/// @param options NULL selects the defaults of @ref sonare_music_analyze_options_init. No
///   callback or user pointer in it is retained past the call.
/// @param out_json Receives a heap-allocated string; release it with @ref sonare_free_string.
SonareError sonare_analyze_json(const float* samples, size_t length, int sample_rate,
                                const SonareMusicAnalyzeOptions* options, char** out_json);

/// @brief Scoring options for sonare_estimate_meter_json.
/// @details Start from sonare_meter_options_init(): a zeroed candidate_numerator_count is
///   rejected, not treated as "use the default set".
typedef struct {
  uint32_t struct_size;  ///< sizeof of this struct; set by sonare_meter_options_init.
  /// Meter numerators to score. Only the first candidate_numerator_count
  /// entries are read; each must be in [2, 32].
  int candidate_numerators[SONARE_MAX_METER_CANDIDATE_NUMERATORS];
  /// Number of entries of candidate_numerators that are read; at least 1.
  size_t candidate_numerator_count;
  /// Beat unit reported for the detected meter; a power of two in [1, 32]. Default 4.
  int denominator;
  /// Weight of the downbeat accent in a candidate's score. Default 1.
  float downbeat_weight;
  /// Weight of the bar-to-bar consistency in a candidate's score. Default 0.5.
  float measure_weight;
  /// Weight of the subdivision support in a candidate's score. Default 0.15.
  float subdivision_weight;
  /// Mean accent between the beats, as a fraction of the mean accent on them, at and above
  /// which a six (or a three) is read as compound; only a midpoint holding an attack
  /// contributes. Default 0.5. Consulted when subdivision accents are supplied.
  float compound_subdivision_threshold;
} SonareMeterOptions;

/// @brief Fills @p options with sizeof and the library defaults. A NULL @p options is ignored.
void sonare_meter_options_init(SonareMeterOptions* options);

/// @brief Caller-supplied evidence for meter estimation.
typedef struct {
  uint32_t struct_size;               ///< Set by sonare_meter_beats_init.
  const float* accent;                ///< One nonnegative accent per beat, required.
  const uint8_t* has_attack;          ///< Optional zero-or-one flag per beat.
  const float* low_frequency_energy;  ///< Optional energy per beat for masked-beat scoring.
  const float* subdivision_accent;    ///< Optional accent per interval between beats.
  size_t beat_count;                  ///< Length of accent and the two optional per-beat arrays.
} SonareMeterBeats;

/// @brief Initializes @p beats to an empty record with its size set. NULL is ignored.
/// @details Initialization does not transfer ownership.
void sonare_meter_beats_init(SonareMeterBeats* beats);

/// @brief Estimates meter over beat observations without re-running audio analysis.
/// @details Use the accent, hasAttack, lowFrequencyEnergy and subdivisionAccent streams from
///   analysis JSON. A NULL has_attack treats every beat as attacked. A NULL low_frequency_energy
///   skips masked-beat detection. A NULL subdivision_accent leaves subdivision unmeasured.
///   When supplied for two or more beats, subdivision_accent contains beat_count - 1 entries.
///   The JSON includes the grouping and a searched flag. For too few beats to score a candidate,
///   searched is false and the fallback has zero confidence. The timeSignature confidence is the
///   margin over the runner-up. Each candidate's confidence is its share of the summed support.
///   unobservedBeats lists zero-based masked beat indices in ascending order. It is an empty
///   array when no per-beat energy was supplied.
/// @param options Required scoring options, initialized by sonare_meter_options_init.
/// @param[out] out_json Receives JSON on success or NULL on error. Free with sonare_free_string.
SonareError sonare_estimate_meter_json(const SonareMeterBeats* beats,
                                       const SonareMeterOptions* options, char** out_json);

/// @brief Releases a float buffer the library allocated; NULL is ignored.
void sonare_free_floats(float* ptr);
/// @brief Releases an int buffer the library allocated; NULL is ignored.
void sonare_free_ints(int* ptr);
/// @brief Releases a string the library handed to the caller; NULL is ignored.
void sonare_free_string(char* ptr);
/// @brief Releases a byte buffer the library allocated (an encoded file, an exported SMF);
///        NULL is ignored.
void sonare_free_bytes(uint8_t* ptr);
/// @brief Releases a key-candidate array; NULL is ignored.
void sonare_free_key_candidates(SonareKeyCandidate* ptr);

// Error handling
/// @brief Returns a short static description of @p error.
/// @details A string literal with static storage duration: never NULL, never
///          invalidated by any later call, and never to be freed. Contrast
///          @ref sonare_last_error_message, which is thread-local and
///          overwritten.
const char* sonare_error_message(SonareError error);

/// @brief Returns the detailed message for the most recent error on the calling thread.
/// @details The returned string is owned by libsonare and valid until the next API call on
///   the same thread. The pointer is never NULL; returns an empty string ("") when no
///   detailed message is currently recorded.
///
///   CONTRACT (read carefully):
///   - Public C-ABI calls that return SonareError CLEAR the thread-local
///     message on entry, so a detailed message can never leak into a later
///     error-code result. Diagnostic accessors and void cleanup helpers do not
///     clear it, allowing callers to release partial outputs before inspection.
///   - AUDIO-THREAD entry points are the exception: they neither clear nor
///     record a message. The first use of a thread-local object can allocate
///     TLS storage on some platforms, which would break their no-allocation
///     contract. They also never throw, so they have no exception-path detail
///     to report. The consequence is that after one of them returns an error
///     code, this function still returns whatever an earlier control-thread
///     call left behind -- do not read it as belonging to the failed call.
///     Validate and configure on the control thread instead. This class is the
///     engine block-render entry points and the realtime voice-changer
///     process / latency entry points.
///   - A message is recorded on every caught-C++-exception return path (the
///     library mapped a thrown sonare::SonareException / std::exception to a
///     SonareError); under normal memory conditions this returns the exact
///     exception's what() text. If storing that diagnostic fails, this remains
///     non-NULL and returns a static fallback while
///     @ref sonare_last_error_code retains the mapped error code. A few
///     validation paths record a message of their own.
///   - Most validation early-returns that produce an error code WITHOUT throwing
///     (e.g. a NULL out-pointer, an out-of-range sample rate, a non-finite input
///     sample) record NO message: this returns "" even though the call failed.
///     Use sonare_error_message(SonareError) for a human-readable string for ANY
///     error code; use this only for the extra detail of exception-path errors.
///   - Functions that return a handle pointer record a code (see
///     @ref sonare_last_error_code) and a message on EVERY NULL return,
///     validation returns included, because the pointer cannot carry a code.
///   - A refusal reported by a core diagnostic (an invalid room in
///     @ref sonare_synthesize_rir) records its first Error as
///     ``acoustic.code: explanation``.
///   - Like every sonare_last_* accessor, it records no error.
/// @return Pointer to a NUL-terminated thread-local message string.
const char* sonare_last_error_message(void);

/// @brief Returns the error code recorded together with the detailed message on
///        the calling thread, or SONARE_OK when nothing is recorded.
/// @details Functions returning a handle pointer report failure as NULL; this is
///   how their caller learns the code. The same calls that clear
///   @ref sonare_last_error_message on entry clear this code, and every path
///   that records a message records the code beside it (for a SonareError
///   return, the code returned). Audio-thread entry points neither clear nor
///   record it, exactly as for the message. Diagnostic accessors do not clear it. Like every
///   sonare_last_* accessor, it records no error.
/// @return The recorded SonareError, or SONARE_OK.
SonareError sonare_last_error_code(void);

/// @brief Returns the lower-camel-case reason token attached to the last error, or NULL.
/// @details The reason is part of the thread-local last-error record and is cleared or replaced
///          together with @ref sonare_last_error_code. The returned token has static storage.
///          Like every sonare_last_* accessor, this records no error.
const char* sonare_last_error_reason(void);

/// @brief One entry of the structured diagnostic channel. Caller-allocated: fill
///        it with @ref sonare_diagnostic_init before passing it to
///        @ref sonare_last_diagnostic.
/// @details @c code and @c message are borrowed: owned by the library, never
///   NULL, never freed by the caller, and valid until the next API call that records or clears a
///   diagnostic on the same thread.
///
///   The warning codes a call publishes are stable strings a caller may switch on:
///   | code                           | published by                                   |
///   |---|---|
///   | `mixing.undeclared_send`       | sonare_mixer_from_scene_json                   |
///   | `acoustic.*`                   | sonare_synthesize_rir, sonare_room_morph,      |
///   |                                | sonare_room_morph_stereo                       |
typedef struct {
  uint32_t struct_size;               ///< Written by sonare_diagnostic_init.
  const char* code;                   ///< Stable dotted code, such as `mixing.undeclared_send`.
  const char* message;                ///< Human-readable detail.
  SonareDiagnosticSeverity severity;  ///< How serious the entry is.
} SonareDiagnostic;

/// @brief Fills @p diagnostic with empty strings, SONARE_DIAGNOSTIC_INFO and its
///        @c struct_size. A NULL @p diagnostic is ignored.
void sonare_diagnostic_init(SonareDiagnostic* diagnostic);

/// @brief Number of structured diagnostics the most recent call published on the
///        calling thread, or 0 when it published none.
/// @details The only warning channel: a call that succeeds with a recoverable
///   problem (a clamped RIR tail) publishes it here, never
///   through sonare_last_error_message.
///   - Cleared at the entry of every call that may publish, so a count read after
///     a later clean call is 0 rather than stale.
///   - Reading does not consume: the list stays valid until the next API call
///     that records or clears a diagnostic on the same thread.
///   - Like every sonare_last_* accessor, it records no error.
size_t sonare_last_diagnostic_count(void);

/// @brief Copies diagnostic @p index of the most recent call into @p out.
/// @return SONARE_ERROR_INVALID_PARAMETER when @p out is NULL, its @c struct_size
///   is below sizeof(SonareDiagnostic), or @p index is not below
///   sonare_last_diagnostic_count(); @p out is left untouched then. Like every
///   sonare_last_* accessor, it records no error, so the message of the call
///   being inspected survives the read.
SonareError sonare_last_diagnostic(size_t index, SonareDiagnostic* out);

/// @brief Number of subjects in the parameter refusal the most recent failing call on the calling
///        thread recorded, or 0 when that failure was not a parameter refusal.
/// @details A subject is the path of what was refused, rooted at the argument the call was given:
///   `sample_rate`, `config.cutoff_hz`, `stages[2].stage`. The list holds the subjects in the
///   order the message names them. When a string argument supplies an undeclared key, the
///   record holds the argument name and the message prints the supplied key. A binding that
///   passes a struct or document under a name its caller never wrote drops that leading path
///   segment from each subject and calls @ref sonare_last_refusal_render. Like every
///   sonare_last_* accessor, it records no error, and it clears none.
size_t sonare_last_refusal_subject_count(void);

/// @brief Subject @p index of the last parameter refusal, or NULL when @p index is not below
///        @ref sonare_last_refusal_subject_count.
/// @details Borrowed: owned by the library and valid until the next API call that records or
///   clears an error on the same thread. Like every sonare_last_* accessor, it records no error.
const char* sonare_last_refusal_subject(size_t index);

/// @brief Key of the C entry that recorded the last parameter refusal, or the name of the
///        entry refused as compiled out, without the "sonare_" prefix.
/// @details NULL when neither cause is recorded. Bindings use the key together with the
///   parameter subjects to look up their own spelling. A compiled-out refusal has no parameter
///   subjects. Borrowed and thread-local, valid until the next API call that records or clears an
///   error on the same thread. This accessor records no error and clears none.
const char* sonare_last_refusal_entry(void);

/// @brief 1 for a recorded wrong type, unknown key, or missing or unexpected argument.
/// @return 0 for a value-domain refusal or when no refusal is recorded.
/// @note Reading this thread-local status does not clear the error.
int sonare_last_refusal_is_shape(void);

/// @brief Renders the last parameter refusal's message with @p subjects in place of its own.
/// @param subjects @p subject_count replacement subjects, in the order of
///   @ref sonare_last_refusal_subject.
/// @param scales NULL, or @p subject_count factors: how many of the refused parameter's units
///   one unit of the caller's replacement holds (the sample rate, for seconds given in place of
///   samples). A refusal stating a numeric domain then states it divided by its subject's
///   factor, as a real interval whose bounds are rounded inward to four significant digits when
///   they have no shorter exact decimal ("must be finite and in [0, 20]"). Any other refusal
///   changes only its names. Each factor must be finite and greater than 0; 1 leaves a subject's
///   domain as it is.
/// @param subject_count Must equal @ref sonare_last_refusal_subject_count.
/// @param spelling How the message words a value being left out or supplied: NULL as the last
///   error reads with SONARE_PRESENCE_SPELLING_C, None or omitted / given for a binding.
/// @param[out] out_text Receives the message, released with @ref sonare_free_string; set to NULL
///   on failure.
/// @return SONARE_ERROR_INVALID_STATE when no parameter refusal is recorded;
///   SONARE_ERROR_INVALID_PARAMETER when @p out_text is NULL, @p subject_count differs from the
///   recorded count, a subject is NULL, a factor is not finite and greater than 0 or
///   @p spelling is not a SonarePresenceSpelling;
///   SONARE_ERROR_OUT_OF_MEMORY when the copy cannot be allocated. Like every sonare_last_*
///   accessor, it records no error, so the refusal being rendered survives the call.
SonareError sonare_last_refusal_render(const char* const* subjects, const double* scales,
                                       size_t subject_count, SonarePresenceSpelling spelling,
                                       char** out_text);

// Version
/// @brief Returns the library version string (e.g. "1.2.3").
/// @details A string literal with static storage duration: never NULL, never
///          invalidated by any later call, and never to be freed.
const char* sonare_version(void);
/// @brief Returns SONARE_ENGINE_ABI_VERSION as compiled into the library.
uint32_t sonare_engine_abi_version(void);

/// @brief Builds a JSON document describing the capabilities of this build.
/// @details The document contains the library version, project and engine ABI
///   versions, platform, enabled feature families, available decode and encode
///   backends, SIMD mode, and reported hardware concurrency.
///   Live-device support ships outside the core archive and is reported separately by
///   @ref sonare_has_live_devices.
/// @param[out] out_json Receives a NUL-terminated UTF-8 document the caller
///   releases with @ref sonare_free_string; set to NULL on failure.
/// @return SONARE_OK, or an error code with the reason in the last error.
SonareError sonare_capabilities_json(char** out_json);

/// @brief Builds a JSON document describing the host this library runs on.
/// @details Holds @c cpu (@c logical_cores, @c physical_cores), @c memory
///   (@c total_gb, @c available_gb, bytes / 1024^3) and @c parallel
///   (@c enabled, @c workers, @c strategy). A fact the platform cannot report
///   is absent from its object, never 0; @c parallel is always present and
///   assumes one core when the logical count is unknown.
/// @param[out] out_json Receives a NUL-terminated UTF-8 document the caller
///   releases with @ref sonare_free_string; set to NULL on failure.
/// @return SONARE_OK, or an error code with the reason in the last error.
SonareError sonare_system_info_json(char** out_json);

/// @brief Returns 1 when the named feature is compiled into this build, 0 otherwise.
/// @details Reads the same table as the @c features object of
///   sonare_capabilities_json(), and takes its key names (@c "ffmpeg",
///   @c "vocalEdit", @c "mastering", ...). An unknown name or NULL returns 0.
int sonare_has_feature(const char* name);

#ifdef __cplusplus
}
#endif
