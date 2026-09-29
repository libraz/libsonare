#pragma once

/// @file model_format.h
/// @brief Generated-data format of the GS classic realization: whole-type node graphs at 32 kHz.
///
/// A type is a run of nodes grouped into components. The generator emits nodes in
/// execution order: components after every component they read from, and nodes
/// inside a looped component in the order their same-sample reads allow.
///
/// Signal numbering: 0 is `in_l`, 1 is `in_r`, then every node's ports in node-pool
/// order — `pan` has two (left, right), every other kind one. `GsClassicInput::signal`,
/// `GsClassicValue::control_ref` and `GsClassicType::out_l`/`out_r` use this numbering.
///
/// Byte maps ship as specs and are expanded once into `GsClassicLut` when a registry
/// is built; the audio path only indexes an expanded LUT. Every `*_begin` is an
/// offset into the pool of the owning `GsClassicModelSet`; the generator stops if a
/// pool outgrows its index type.
///
/// Two configurations share one binary. The generator emits `.inc` files of one shape
/// whose every symbol is named through `GS_CLASSIC_SET_NAME`, and each `.inc` defines
/// exactly one function `GsClassicModelSet GS_CLASSIC_SET_PREFIX()` returning the view
/// (with `luts` null). An includer names the set, includes, and undefines:
///
/// @code
///   #define GS_CLASSIC_SET_PREFIX gs_classic_models_default
///   #include "midi/synth/gs_classic_models.inc"   // with overlays
///   #undef GS_CLASSIC_SET_PREFIX
///   #define GS_CLASSIC_SET_PREFIX gs_classic_models_raw
///   #include "midi/gs_classic_models_raw.inc"     // without overlays
///   #undef GS_CLASSIC_SET_PREFIX
///   // inside the .inc:
///   namespace { const GsClassicNode GS_CLASSIC_SET_NAME(_nodes)[] = {...}; }
///   GsClassicModelSet GS_CLASSIC_SET_PREFIX() { return {GS_CLASSIC_SET_NAME(_types), ...}; }
/// @endcode

#include <cstddef>
#include <cstdint>

/// Pastes the current set prefix onto a symbol suffix (see the file comment).
#define GS_CLASSIC_SET_CAT_(a, b) a##b
#define GS_CLASSIC_SET_CAT(a, b) GS_CLASSIC_SET_CAT_(a, b)
#define GS_CLASSIC_SET_NAME(suffix) GS_CLASSIC_SET_CAT(GS_CLASSIC_SET_PREFIX, suffix)

namespace sonare::midi::synth::gs_classic {

/// The rate every classic model is drawn at, in Hz.
inline constexpr double kGsClassicSampleRateHz = 32000.0;

/// Realtime byte slots a type reads (EFX parameter slots 0..19).
inline constexpr std::size_t kGsClassicByteSlots = 20;

/// Entries of an expanded byte map: one per 7-bit byte value.
inline constexpr std::size_t kGsClassicLutSize = 128;

/// Samples of a control curve between its `lo` and `hi`.
inline constexpr std::size_t kGsClassicCurveSize = 256;

/// Most audio inputs one node reads (a `mix`).
inline constexpr std::size_t kGsClassicMaxInputs = 16;

/// Key standing for `*` in a `states` map: the value of every byte not named.
inline constexpr uint8_t kGsClassicStateWildcard = 255;

/// Index meaning "none" in any 16-bit pool reference.
inline constexpr uint16_t kGsClassicNone = 0xFFFF;

/// Offset meaning "absent" in a `GsClassicSection` value offset.
inline constexpr uint8_t kGsClassicAbsent = 0xFF;

/// The five byte-map rules.
enum class GsClassicMapKind : uint8_t {
  kSteppedTable,  ///< values = entries; index min(byte / per_entry, n - 1)
  kWindow,        ///< keys = {low, high}, values = {at_low, at_high}; clamped linear
  kStates,        ///< keys = named bytes (255 = `*`), values = their quantities
  kPoints,        ///< keys = bytes, values = quantities; linear, or in log if `log`
  kTable,         ///< values = entries; a byte past the end reads entry `out_of_range`
};

/// One byte map as the model states it, plus the protocol domain of the slot reading it.
///
/// `accept_lo`/`accept_hi`/`power_on` decide the entries a `states` map without `*`
/// names nothing for: a byte the protocol accepts takes the nearest named value, one it
/// rejects takes the power-on byte's value. Specs are deduplicated over all fields.
struct GsClassicMapSpec {
  GsClassicMapKind kind;
  uint8_t log;            ///< points only: interpolate in the log of the values
  uint16_t key_begin;     ///< into `map_keys` (window, states, points)
  uint16_t value_begin;   ///< into `map_values`
  uint16_t n;             ///< keys and values alike (window: 2)
  uint16_t per_entry;     ///< stepped-table stride
  uint16_t out_of_range;  ///< table: entry read past the end
  uint8_t accept_lo;
  uint8_t accept_hi;
  uint8_t power_on;
};

/// A byte map expanded to one quantity per byte value, on the heap.
struct GsClassicLut {
  float v[kGsClassicLutSize];
};

/// A control-signal map sampled uniformly over [lo, hi]; clamped at both ends.
struct GsClassicCurve {
  float v[kGsClassicCurveSize];
  float lo;
  float hi;
};

/// One point of an lfo `points` shape (x = phase in cycles) or a shaper curve (x = input).
struct GsClassicPoint {
  double x;
  double y;
};

/// A run of `GsClassicPoint`, sorted by x.
struct GsClassicPoints {
  uint16_t begin;
  uint16_t n;
};

/// One audio input of a node, as a signal number.
struct GsClassicInput {
  uint16_t signal;
};

/// A section stored at full scale and blended with the dry path.
struct GsClassicReachedBy {
  float full_db;
  uint16_t stored_at;   ///< 0 full-boost, 1 full-cut
  uint16_t other_side;  ///< 0 the-same-section-inverted, 1 a-second-section-stored-at-the-other-end
};

/// A pan law as two multipliers over the position axis 0..127, interpolated between entries.
struct GsClassicPanLaw {
  float left[kGsClassicLutSize];
  float right[kGsClassicLutSize];
};

enum class GsClassicValueKind : uint8_t {
  kConst,    ///< `constant`
  kByte,     ///< LUT `table` indexed by the byte in `slot`
  kControl,  ///< signal `control_ref`, through curve `table` unless it is kGsClassicNone
};

struct GsClassicValue {
  GsClassicValueKind kind;
  uint8_t slot;
  uint16_t table;
  uint16_t control_ref;
  double constant;
};

/// Node kinds; the soundings vocabulary plus the overlay-only `x-noise`.
///
/// Values (offsets from `value_begin`), `aux` and `flags` per kind:
/// - kGain: [gain (linear; a `db` gain is folded by the generator)]
/// - kMix: [weight of input 0 .. weight of input n-1]
/// - kPan: [position on the law's 0..127 axis]; aux = pan law
/// - kDelay: [time_ms] or [time_ms, depth_ms, modulating control (raw kControl)];
///   flags = GsClassicInterpolation
/// - kLfo: [rate_hz, phase_offset (cycles)]; flags = GsClassicLfoShape; aux = points (kPoints)
/// - kSection: offsets named by the GsClassicSection at aux
/// - kShaper: [drive_db]; flags = GsClassicShaperCurve | oversample << 4; aux = points (kPoints)
/// - kEnvelope: [attack_ms, release_ms] (+ [floor_db] in the log domain); flags = envelope bits
/// - kGainComputer: [threshold_db, ratio, knee_db]
/// - kVca: [control (raw kControl)]
/// - kHold: [rate_hz]
/// - kQuantize: []; aux = bits
/// - kPitch: [ratio, window_ms]; flags = GsClassicInterpolation | GsClassicCrossfade << 2
/// - kXNoise: [level, parameter]; flags = GsClassicNoise; aux = seed
/// kMix reads n inputs, kLfo and kXNoise none, every other kind exactly one.
enum class GsClassicNodeKind : uint8_t {
  kGain,
  kMix,
  kPan,
  kDelay,
  kLfo,
  kSection,
  kShaper,
  kEnvelope,
  kGainComputer,
  kVca,
  kHold,
  kQuantize,
  kPitch,
  kXNoise,
};

enum class GsClassicInterpolation : uint8_t { kNone, kLinear };
enum class GsClassicCrossfade : uint8_t { kHann, kLinear, kSCurve };
enum class GsClassicLfoShape : uint8_t { kSine, kTriangle, kSquare, kSaw, kPoints };
enum class GsClassicShaperCurve : uint8_t { kTanh, kHard, kCubic, kPoints };

/// Envelope flag bits; each clear bit is the first-named alternative (peak, branching, linear).
inline constexpr uint8_t kGsClassicEnvelopeRms = 1u << 0;
inline constexpr uint8_t kGsClassicEnvelopeDecoupled = 1u << 1;
inline constexpr uint8_t kGsClassicEnvelopeLogDomain = 1u << 2;

/// Overlay noise sources; `parameter` is a low-pass corner (radio), clicks per second
/// (disc) or the mains frequency (hum), and unused by white and pink.
enum class GsClassicNoise : uint8_t { kWhite, kPink, kRadio, kDisc, kHum };

struct GsClassicNode {
  GsClassicNodeKind kind;
  uint8_t flags;
  uint16_t aux;
  uint16_t input_begin;
  uint8_t n_inputs;
  uint8_t value_count;
  uint16_t value_begin;
};

enum class GsClassicStage : uint8_t { kShelf, kPeaking, kPole, kAllpassChain };
enum class GsClassicSectionForm : uint8_t { kBilinear, kOneMultiply };
enum class GsClassicSide : uint8_t { kLow, kHigh };

/// A section node's stage. Each value field is an offset into the node's values, or
/// kGsClassicAbsent: `corner` (the centre of a peaking stage), `gain` (dB), `q`,
/// `count` (pole: a byte or constant; allpass-chain: a constant) and `mix` (allpass-chain).
struct GsClassicSection {
  GsClassicStage stage;
  GsClassicSectionForm form;
  uint8_t order;
  GsClassicSide side;
  uint8_t corner;
  uint8_t gain;
  uint8_t q;
  uint8_t count;
  uint8_t mix;
  uint16_t reached_by;  ///< into `reached_by`, or kGsClassicNone
};

/// A strongly connected component; a looped one is drawn a sample at a time.
struct GsClassicComponent {
  uint16_t node_begin;
  uint16_t node_end;
  uint8_t looped;
};

/// Parallel-2 arrangement of a type's two halves.
enum class GsClassicTopology : uint8_t { kNotApplicable, kSideBySide, kInSeries };

struct GsClassicType {
  uint16_t type;  ///< MSB << 8 | LSB
  uint16_t node_begin;
  uint16_t node_end;
  uint16_t comp_begin;
  uint16_t comp_end;
  uint16_t out_l;              ///< signal
  uint16_t out_r;              ///< signal
  uint32_t max_delay_samples;  ///< longest read-back of any delay or pitch node, in samples
  GsClassicTopology topology;
  char model_sha256[65];
  float gross_residual;
  /// First and last printed byte of each slot (a state list's ends, a range's ends, a
  /// column pointer's whole byte); both 0 for a slot the type does not print.
  uint8_t printed_lo[kGsClassicByteSlots];
  uint8_t printed_hi[kGsClassicByteSlots];
};

/// A view over one configuration's pools. `luts` holds one expansion per map spec and
/// is filled when a registry is built; the generated function leaves it null.
struct GsClassicModelSet {
  const GsClassicType* types;
  std::size_t n_types;
  const GsClassicNode* nodes;
  const GsClassicComponent* components;
  const GsClassicInput* inputs;
  const GsClassicValue* values;
  const GsClassicSection* sections;
  const GsClassicReachedBy* reached_by;
  const GsClassicPoints* point_lists;
  const GsClassicPoint* points;
  const GsClassicPanLaw* pan_laws;
  const GsClassicCurve* curves;
  const GsClassicMapSpec* map_specs;
  std::size_t n_map_specs;
  const uint8_t* map_keys;
  const double* map_values;
  const GsClassicLut* luts;
};

}  // namespace sonare::midi::synth::gs_classic
