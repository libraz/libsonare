#pragma once

/// @file spectrum.h
/// @brief STFT/iSTFT and Spectrogram class.

#include <algorithm>
#include <complex>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "core/audio.h"
#include "util/constants.h"
#include "util/types.h"

namespace sonare {

/// @brief Progress callback type for iterative operations.
/// @param progress Progress value (0.0 to 1.0)
using SpectrogramProgressCallback = std::function<void(float progress)>;

namespace detail {

/// @brief Fills @p magnitude for the complex spectrum @p data as abs(z), unconditionally.
/// @details Shared by the complex-spectrum holders that cache magnitude and
///   power side by side (Spectrogram, CqtResult). @p magnitude is resized to
///   @p data's length. The unnamed power parameter is accepted for call-site
///   compatibility and ignored -- never derives from it, so each accessor is one
///   fixed formula of the complex spectrum, independent of which one ran first.
void fill_magnitude_cache(const std::vector<std::complex<float>>& data, const std::vector<float>&,
                          std::vector<float>& magnitude);

/// @brief Fills @p power for the complex spectrum @p data as re² + im², unconditionally.
/// @details Never derives from the (unnamed, ignored) magnitude parameter, for
///   the same reason as @ref fill_magnitude_cache.
void fill_power_cache(const std::vector<std::complex<float>>& data, const std::vector<float>&,
                      std::vector<float>& power);

/// @brief Writes magnitude for one contiguous run of @p count elements.
/// @details The primitive @ref fill_magnitude_cache is expressed in terms of, so a
///          caller producing a subrange takes the same code path rather than a copy
///          of its branch. Pure elementwise map with no accumulator and no
///          multiply-add, so the result does not depend on @p count.
void magnitude_run(const std::complex<float>* data, std::size_t count, float* out);

}  // namespace detail

/// @brief STFT output format.
enum class StftFormat {
  Complex,    ///< Complex spectrum (default)
  Magnitude,  ///< Magnitude spectrum
  Power,      ///< Power spectrum (magnitude squared)
};

enum class PadMode {
  Constant,  ///< Zero padding, matching current librosa.stft default
  Reflect,   ///< Reflect input edges before/after the signal
};

/// @brief Largest accepted @ref StftConfig::n_fft.
/// @details Bin spacing is 1/(window seconds) Hz, so the useful window ends near one
/// second: 1 Hz already resolves a semitone at the bottom of the audible range, and the
/// time smear past it exceeds any event. A backstop rather than a domain bound -- one
/// second at @ref kMaxAudioSampleRate rounded up to a power of two, so it sits above
/// every consumer's useful maximum instead of binding one.
inline constexpr int kMaxStftNFft = 524288;
static_assert(kMaxStftNFft >= kMaxAudioSampleRate,
              "the STFT ceiling must hold a one-second window at the highest accepted rate");

/// @brief Configuration for STFT computation.
struct StftConfig {
  int n_fft = 2048;                      ///< FFT size; in [1, @ref kMaxStftNFft]
  int hop_length = 512;                  ///< Hop length between frames
  int win_length = 0;                    ///< Window length (0 = n_fft)
  WindowType window = WindowType::Hann;  ///< Window function
  bool center = true;                    ///< Pad signal to center frames
  PadMode pad_mode = PadMode::Constant;  ///< Padding mode used when center=true

  /// @brief Returns actual window length (defaults to n_fft if 0).
  int actual_win_length() const { return win_length > 0 ? win_length : n_fft; }
};

/// @brief Validation rules for @ref StftConfig.
/// @details Found by argument-dependent lookup from @ref Validated, which
///          @ref Spectrogram::compute uses before it touches the framing loop.
///          Every surface reaches the STFT through that entry point, so the
///          rules do not need repeating per binding.
/// @throws SonareException(InvalidParameter) for a non-positive size, a size above
///         @ref kMaxStftNFft, a negative window length, or a window longer than the
///         FFT. The size ceiling is checked here so it is rejected by name before the
///         framing loop allocates; reaching the allocator instead reports the failure
///         as OutOfMemory, which names nothing the caller passed.
void validate_config(const StftConfig& config);

/// @brief Validates an analysis geometry whose spectrum is resynthesized to audio.
/// @details The single source of truth for the STFT geometry the spectral
///          effects accept. Two rules on top of @ref validate_config's shape
///          checks:
///          - @p n_fft must be even and >= 2, which is what the real FFT accepts
///            (a one-sided spectrum has no n_fft/2 + 1 layout otherwise). Any
///            even size works; the FFT is mixed-radix, not radix-2 only, so a
///            power of two is a performance preference and not a requirement.
///          - @p hop_length must lie in (0, n_fft / 2], i.e. frames overlap by
///            at least half a window.
/// @details The half-window bound is deliberately stricter than where
///          reconstruction breaks: @ref Spectrogram::to_audio divides by the true
///          analysis*synthesis window sum, so a mask-and-invert round trip stays
///          exact well past half overlap and only collapses at hop >= n_fft. Half
///          overlap is what the phase-coherent paths were written to, and the
///          bound below which a phase vocoder's frame-to-frame phase advance
///          stops being resolvable.
/// @details An analysis-only STFT reconstructs nothing and so is not subject to
///          the overlap rule, which is why this is separate from
///          @ref validate_config rather than folded into it.
/// @param n_fft FFT size.
/// @param hop_length Hop length between frames.
/// @throws SonareException(InvalidParameter) if either rule is violated.
void validate_cola_geometry(int n_fft, int hop_length);

/// @brief Builds an StftConfig with the given FFT and hop sizes.
/// @details Convenience helper for the common case where callers only need to
/// override @p n_fft and @p hop_length. All other fields (window, win_length,
/// center, pad_mode) remain at their StftConfig defaults so changing the
/// defaults still propagates to every call site.
/// @param n_fft FFT size
/// @param hop_length Hop length between frames
/// @return StftConfig with the specified n_fft / hop_length and default
///         window/centering settings.
inline StftConfig make_stft_config(int n_fft, int hop_length) {
  StftConfig config;
  config.n_fft = n_fft;
  config.hop_length = hop_length;
  return config;
}

/// @brief Frame count @ref Spectrogram::compute produces for a signal of
///        @p signal_length samples under @p config.
/// @details The framing loop's own count, shared with it rather than restated.
///          Exposed so a consumer handed a spectrogram it did not build can tie
///          it back to the signal it is about to be read alongside.
/// @param signal_length Length of the unpadded input signal in samples.
/// @param config Analysis geometry.
/// @return Number of frames; 1 for a signal shorter than one padded frame.
/// @throws SonareException(InvalidParameter) if the count exceeds int range.
int stft_frame_count(std::size_t signal_length, const StftConfig& config);

/// @brief Configuration for Griffin-Lim algorithm.
/// @details Griffin-Lim iteratively estimates phase from magnitude spectrogram.
///          Momentum accelerates convergence but may cause instability if too high.
struct GriffinLimConfig {
  int n_iter = 32;         ///< Number of iterations (typically 16-64)
  float momentum = 0.99f;  ///< Momentum factor [0.0, 1.0). 0 disables, 0.99 is typical.
                           ///< Higher values converge faster but may oscillate.
};

/// @brief Spectrogram computed from audio via STFT.
/// @details Stores complex spectrum and provides views for magnitude, power, and dB.
///
/// Memory Layout:
/// - Data is stored as [n_bins x n_frames] in row-major order
/// - Access pattern: data[bin * n_frames + frame]
/// - bin index: 0 to n_bins-1 (frequency bins, n_bins = n_fft/2 + 1)
/// - frame index: 0 to n_frames-1 (time frames)
///
/// This layout is optimized for frequency-domain processing where
/// operations typically iterate over all frames for a given bin.
///
/// @note Thread safety: A single Spectrogram instance is NOT thread-safe for
///       concurrent access to magnitude()/power() methods due to lazy caching.
///       Each thread should have its own Spectrogram instance, or external
///       synchronization is required. Spectrogram::compute() itself is thread-safe.
class Spectrogram {
 public:
  /// @brief Default constructor creates empty spectrogram.
  Spectrogram();

  /// @brief Computes STFT of audio signal.
  /// @param audio Input audio
  /// @param config STFT configuration
  /// @param progress_callback Optional progress callback (0.0 to 1.0)
  /// @return Spectrogram object
  static Spectrogram compute(const Audio& audio, const StftConfig& config = StftConfig(),
                             SpectrogramProgressCallback progress_callback = nullptr);

  /// @brief Creates Spectrogram from existing complex spectrum data.
  /// @param data Complex spectrum data [n_bins x n_frames]
  /// @param n_fft Original FFT size
  /// @param hop_length Hop length used
  /// @param sample_rate Sample rate of original audio
  /// @param center Whether the source signal was center-padded
  /// @param win_length Analysis window length (0 = n_fft)
  /// @return Spectrogram object
  /// @param window Window the caller's frequency-domain data was analyzed with.
  ///        Carried so @ref to_audio normalizes with it. Deliberately has no
  ///        default: a wrong window here produces a reconstruction gain ripple
  ///        rather than an error, so every caller has to state which one
  ///        produced its data instead of inheriting a guess.
  /// @param pad_mode Padding the source signal was centered with. Defaults to the
  ///        @ref StftConfig default, which is what a spectrum assembled from a
  ///        library STFT carries; a caller whose data came from reflect padding
  ///        states it so @ref validate_reused_geometry can tell the two apart.
  static Spectrogram from_complex(const std::complex<float>* data, int n_bins, int n_frames,
                                  int n_fft, int hop_length, int sample_rate, WindowType window,
                                  bool center = true, int win_length = 0,
                                  PadMode pad_mode = PadMode::Constant);

  /// @brief Returns number of frequency bins (n_fft/2 + 1).
  int n_bins() const { return n_bins_; }

  /// @brief Returns number of time frames.
  int n_frames() const { return n_frames_; }

  /// @brief Returns FFT size.
  int n_fft() const { return n_fft_; }

  /// @brief Returns hop length.
  int hop_length() const { return hop_length_; }

  /// @brief Returns window length used for analysis.
  /// @details Defaults to n_fft if not explicitly set.
  int win_length() const { return win_length_; }

  /// @brief Returns true if the input was center-padded before STFT.
  bool center() const { return center_; }

  /// @brief Returns sample rate of original audio.
  int sample_rate() const { return sample_rate_; }

  /// @brief Returns duration in seconds.
  float duration() const;

  /// @brief Returns true if spectrogram is empty.
  bool empty() const { return n_frames_ == 0 || n_bins_ == 0; }

  /// @brief Returns view of complex spectrum [n_bins x n_frames].
  MatrixView<std::complex<float>> complex_view() const;

  /// @brief Returns pointer to complex data.
  const std::complex<float>* complex_data() const;

  /// @brief Returns magnitude spectrum [n_bins x n_frames].
  /// @details Computed lazily and cached.
  const std::vector<float>& magnitude() const;

  /// @brief Returns power spectrum [n_bins x n_frames].
  /// @details Computed lazily and cached.
  const std::vector<float>& power() const;

  /// @brief Runs @p fn over the magnitude spectrum in bounded frame tiles.
  /// @details Lets a single-pass consumer read magnitude without materializing the
  ///          whole [n_bins x n_frames] array, which for a full track is the larger
  ///          of this object's two derived caches.
  ///
  ///          Two states, and the tile holds whatever @ref detail::magnitude_run
  ///          would have written for the same frames -- the same code path over a
  ///          subrange, not an expression equal to it:
  ///            - magnitude already cached: one call with the whole cache. The memory
  ///              is already spent, and a frame tile is not contiguous in a row-major
  ///              [n_bins x n_frames] buffer, so tiling would copy an array we hold.
  ///            - not cached: tiles of abs(z), regardless of whether power is cached.
  ///
  ///          @p overlap leading frames are repeated at the head of each tile for a
  ///          consumer that needs the previous frame (spectral flux). @p fn receives
  ///          (tile, tile_frames, discard): the tile is [n_bins x tile_frames]
  ///          row-major and the first @p discard frames are overlap whose outputs the
  ///          caller drops. Emitted frames are contiguous and ascending across calls.
  ///
  ///          Tiling changes a consumer's trip count. That is safe here because every
  ///          consumer is elementwise across frames with a per-frame accumulator, so
  ///          no reassociation is possible; it is *not* safe by that argument alone
  ///          when a consumer's accumulate is a multiply-add, because the same
  ///          statement can fuse in a vectorized body and not in the scalar epilogue.
  ///          Verified for the consumers this is used by rather than assumed.
  template <typename Fn>
  void for_each_magnitude_tile(int tile_frames, int overlap, Fn&& fn) const {
    if (n_bins_ <= 0 || n_frames_ <= 0 || data_.empty()) return;

    if (!magnitude_cache_.empty()) {
      fn(magnitude_cache_.data(), n_frames_, 0);
      return;
    }

    // A tile must hold its overlap plus at least one emitted frame.
    const int step = std::max(1, std::max(tile_frames, overlap + 1));
    const int span = step + std::max(0, overlap);

    std::vector<float> tile(static_cast<std::size_t>(n_bins_) * static_cast<std::size_t>(span));
    for (int first = 0; first < n_frames_; first += step) {
      const int lo = std::max(0, first - overlap);
      const int hi = std::min(n_frames_, first + step);
      const int len = hi - lo;
      const int discard = first - lo;
      for (int bin = 0; bin < n_bins_; ++bin) {
        const std::size_t src =
            static_cast<std::size_t>(bin) * static_cast<std::size_t>(n_frames_) +
            static_cast<std::size_t>(lo);
        detail::magnitude_run(
            data_.data() + src, static_cast<std::size_t>(len),
            tile.data() + static_cast<std::size_t>(bin) * static_cast<std::size_t>(len));
      }
      fn(static_cast<const float*>(tile.data()), len, discard);
    }
  }

  /// @brief Returns magnitude in decibels [n_bins x n_frames].
  /// @param ref Reference value (default 1.0)
  /// @param amin Minimum amplitude to avoid log(0) (default constants::kEpsilon)
  /// @param top_db Threshold below max dB to clamp (default constants::kDefaultTopDb, negative to
  /// disable)
  /// @return dB values
  std::vector<float> to_db(float ref = 1.0f, float amin = constants::kEpsilon,
                           float top_db = constants::kDefaultTopDb) const;

  /// @brief Reconstructs audio from spectrogram via iSTFT.
  /// @param length Target length in samples (0 = auto)
  /// @return Reconstructed audio
  /// @details Normalizes with the analysis window this spectrogram carries. The
  ///          iSTFT divides the overlap-add by the analysis*synthesis window
  ///          sum, so the divisor is only correct when it is built from the
  ///          window the frequency-domain data was actually produced with.
  Audio to_audio(int length = 0) const;

  /// @brief Reconstructs audio via iSTFT, overriding the analysis window.
  /// @param length Target length in samples (0 = auto)
  /// @param window Window the frequency-domain data was analyzed with
  /// @return Reconstructed audio
  /// @details Only for data whose analysis window this spectrogram cannot know,
  ///          such as a buffer assembled outside @ref from_complex. Passing a
  ///          window other than the true analysis one makes the normalization
  ///          divisor wrong and imprints a gain ripple at the hop rate.
  Audio to_audio(int length, WindowType window) const;

  /// @brief Returns the analysis window this spectrogram was produced with.
  WindowType window() const { return window_; }

  /// @brief Returns the padding mode the analysis centered the signal with.
  /// @details Only meaningful when @ref center is true. Carried so the whole
  ///          analysis geometry is observable from the object: a consumer handed
  ///          a spectrogram it did not build can otherwise not tell what padding
  ///          produced it, and reuse then rests on the framings happening to
  ///          agree rather than on a check.
  PadMode pad_mode() const { return pad_mode_; }

  /// @brief Access complex value at (bin, frame).
  const std::complex<float>& at(int bin, int frame) const;

 private:
  Spectrogram(std::vector<std::complex<float>> data, int n_bins, int n_frames, int n_fft,
              int hop_length, int sample_rate, int win_length = 0, bool center = true,
              WindowType window = WindowType::Hann, PadMode pad_mode = PadMode::Constant);

  std::vector<std::complex<float>> data_;  ///< Complex spectrum [n_bins * n_frames]
  int n_bins_;
  int n_frames_;
  int n_fft_;
  int hop_length_;
  int sample_rate_;
  int win_length_;  ///< Window length used for analysis (defaults to n_fft)
  bool center_;
  WindowType window_;  ///< Window family used for analysis; drives iSTFT normalization
  PadMode pad_mode_;   ///< Padding applied when center_ is true

  // Cached derived data (computed lazily)
  mutable std::vector<float> magnitude_cache_;
  mutable std::vector<float> power_cache_;
};

/// @brief Validates that @p spec is the STFT @p config over that signal produces.
/// @details The guard a consumer applies to a spectrogram it did not build. Every
///          field of the analysis geometry is compared, plus the sample rate and
///          the frame count the signal implies, so a framing that merely shares
///          today's defaults cannot pass for the one the consumer asked for. Two
///          independently computed STFTs that disagree only produce two slightly
///          differently framed measurements; a reused one that disagrees produces
///          a measurement read off frames it never asked for.
/// @param spec Spectrogram to check.
/// @param config Geometry the consumer needs.
/// @param sample_rate Sample rate of the signal the consumer holds.
/// @param signal_length Length of that signal in samples.
/// @throws SonareException(InvalidParameter) on any mismatch.
void validate_reused_geometry(const Spectrogram& spec, const StftConfig& config, int sample_rate,
                              std::size_t signal_length);

/// @brief Magnitude + phase decomposition of a complex spectrum.
struct MagPhase {
  std::vector<float> magnitude;            ///< |D|^power, length n_bins * n_frames (row-major)
  std::vector<std::complex<float>> phase;  ///< D / |D|, unit-modulus complex, same layout
};

/// @brief Separate a complex spectrogram into magnitude (^power) and phase.
/// @param spec Complex spectrum, row-major [n_bins x n_frames]
/// @param n Total number of complex entries (n_bins * n_frames)
/// @param power Exponent applied to magnitude (default 1.0; e.g. 2.0 for power)
/// @return MagPhase. phase[i] = spec[i] / max(|spec[i]|, eps); magnitude[i] = |spec[i]|^power.
/// @throw sonare::SonareException if n > 0 and spec is null, or power <= 0.
MagPhase magphase(const std::complex<float>* spec, std::size_t n, float power = 1.0f);

/// @brief Convenience overload accepting a Spectrogram.
MagPhase magphase(const Spectrogram& spec, float power = 1.0f);

/// @brief Reconstructs audio from magnitude spectrogram using Griffin-Lim algorithm.
/// @param magnitude Magnitude spectrum [n_bins x n_frames]
/// @param n_bins Number of frequency bins
/// @param n_frames Number of time frames
/// @param n_fft FFT size
/// @param hop_length Hop length
/// @param sample_rate Sample rate
/// @param config Griffin-Lim configuration
/// @return Reconstructed audio
/// @throw sonare::SonareException (InvalidParameter) on a null @p magnitude, a
///        non-positive dimension, an @p n_bins that is not n_fft/2 + 1, or any
///        non-finite element of @p magnitude. The finiteness precondition lives
///        here so every surface reports it identically.
Audio griffin_lim(const float* magnitude, int n_bins, int n_frames, int n_fft, int hop_length,
                  int sample_rate, const GriffinLimConfig& config = GriffinLimConfig());

/// @brief Reconstructs audio from magnitude spectrogram using Griffin-Lim algorithm.
/// @param magnitude Magnitude values as vector
/// @param n_fft FFT size
/// @param hop_length Hop length
/// @param sample_rate Sample rate
/// @param config Griffin-Lim configuration
/// @return Reconstructed audio
Audio griffin_lim(const std::vector<float>& magnitude, int n_bins, int n_frames, int n_fft,
                  int hop_length, int sample_rate,
                  const GriffinLimConfig& config = GriffinLimConfig());

/// @brief Output of @ref reassigned_spectrogram.
struct ReassignedSpectrogram {
  std::vector<float> magnitude;    ///< [n_bins x n_frames] row-major
  std::vector<float> times;        ///< [n_bins x n_frames] reassigned times (seconds)
  std::vector<float> frequencies;  ///< [n_bins x n_frames] reassigned frequencies (Hz)
};

/// @brief Computes the reassigned spectrogram of @p audio.
/// @details Implements an Auger-Flandrin style reassignment: STFTs are computed
/// with a Hann window, a time-weighted Hann (t*w(t)), and a derivative window
/// (dw/dt). The reassigned time/frequency for each bin is derived from those
/// three transforms. When `S * conj(S)` falls below @p ref_power, the affected
/// bin's time/frequency entries are set to the un-reassigned values (or NaN
/// if @p fill_nan is true), matching `librosa.reassigned_spectrogram`.
/// @param audio Input audio (mono).
/// @param config STFT configuration (n_fft, hop_length, window).
/// @param ref_power Power threshold below which bins are not reassigned.
/// @param fill_nan If true, low-power bins are filled with NaN.
ReassignedSpectrogram reassigned_spectrogram(const Audio& audio,
                                             const StftConfig& config = StftConfig(),
                                             float ref_power = 1e-6f, bool fill_nan = false);

/// @brief Reassigned per-bin frequencies (Hz).
/// @details Mirrors `librosa.core.spectrum.reassign_frequencies`. Returns just
/// the [n_bins x n_frames] frequency map computed from `Sw` and a
/// derivative-window STFT. Bins whose power falls below @p ref_power are
/// returned with their un-reassigned center frequencies (or NaN if
/// @p fill_nan is true).
std::vector<float> reassign_frequencies(const Audio& audio, const StftConfig& config = StftConfig(),
                                        float ref_power = 1e-6f, bool fill_nan = false);

/// @brief Reassigned per-bin times (seconds).
/// @details Mirrors `librosa.core.spectrum.reassign_times`. Returns just the
/// [n_bins x n_frames] time map computed from `Sw` and a time-weighted-window
/// STFT. Bins below @p ref_power are returned with their un-reassigned center
/// times (or NaN if @p fill_nan is true).
std::vector<float> reassign_times(const Audio& audio, const StftConfig& config = StftConfig(),
                                  float ref_power = 1e-6f, bool fill_nan = false);

}  // namespace sonare
