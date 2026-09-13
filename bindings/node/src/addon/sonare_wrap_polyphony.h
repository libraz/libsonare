#ifndef SONARE_NODE_SONARE_WRAP_POLYPHONY_H_
#define SONARE_NODE_SONARE_WRAP_POLYPHONY_H_

#include <napi.h>
#include <sonare/sonare_c.h>

#include <cstdint>
#include <vector>

namespace sonare_node {

/// @brief N-API ObjectWrap over the opaque polyphonic-analysis handle
///        (@ref SonarePolyphonicAnalysis).
///
/// Mirrors the @ref MixerWrap pattern: the constructor analyses and takes the
/// native handle, the destructor and @c destroy() release it, and every method
/// routes through the @c sonare_polyphonic_* C ABI. JS surface:
///   const analysis = new sonare.PolyphonicAnalysis(samples, sampleRate, config);
///   analysis.noteCount();  analysis.frameCount();
///   analysis.notes();      analysis.polyphony();
///   analysis.noteF0(i);    analysis.noteAmplitude(i);  analysis.noteSalience(i);
///   analysis.noteEnvelope(i);  analysis.noteInharmonicity();
///   analysis.setNoteEdit(i, edit, envelope);
///   analysis.render(options);
///   analysis.destroy();
///
/// Every count the C ABI exposes is a query a caller makes before sizing a
/// buffer, and a buffer shorter than the data is clamped rather than refused. So
/// the two-step is done here, once, and JS never sees a capacity: the note count,
/// the frame count and each note's frame span are read at construction, and the
/// spans cannot change because an edit is the only thing a host writes.
class PolyphonicAnalysisWrap : public Napi::ObjectWrap<PolyphonicAnalysisWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  explicit PolyphonicAnalysisWrap(const Napi::CallbackInfo& info);
  ~PolyphonicAnalysisWrap();

  PolyphonicAnalysisWrap(const PolyphonicAnalysisWrap&) = delete;
  PolyphonicAnalysisWrap& operator=(const PolyphonicAnalysisWrap&) = delete;
  PolyphonicAnalysisWrap(PolyphonicAnalysisWrap&&) = delete;
  PolyphonicAnalysisWrap& operator=(PolyphonicAnalysisWrap&&) = delete;

 private:
  /// One of the four per-note curve accessors, all of which share a signature.
  using CurveReader = SonareError (*)(const SonarePolyphonicAnalysis*, size_t, float*, size_t,
                                      size_t*);

  Napi::Value NoteCount(const Napi::CallbackInfo& info);
  Napi::Value FrameCount(const Napi::CallbackInfo& info);
  Napi::Value Notes(const Napi::CallbackInfo& info);
  Napi::Value SetNoteEdit(const Napi::CallbackInfo& info);
  Napi::Value Polyphony(const Napi::CallbackInfo& info);
  Napi::Value NoteF0(const Napi::CallbackInfo& info);
  Napi::Value NoteAmplitude(const Napi::CallbackInfo& info);
  Napi::Value NoteSalience(const Napi::CallbackInfo& info);
  Napi::Value NoteEnvelope(const Napi::CallbackInfo& info);
  /// Not a curve accessor: one entry per note rather than per frame, and sized
  /// by the note count, so it takes no note index and shares no signature with
  /// @ref CurveReader.
  Napi::Value NoteInharmonicity(const Napi::CallbackInfo& info);
  Napi::Value Render(const Napi::CallbackInfo& info);
  void Destroy(const Napi::CallbackInfo& info);

  /// Throws and returns false once the handle has been released, so a use after
  /// destroy is a JS error rather than a NULL dereference in the C ABI.
  bool RequireOpen(Napi::Env env);
  /// Shared body of the three measured curve accessors: reads the note index,
  /// sizes the buffer from the note's own frame span, and marshals what was
  /// written. The envelope is not one of them -- its points are indexed from 0,
  /// not over the span -- so @ref NoteEnvelope sizes itself.
  Napi::Value Curve(const Napi::CallbackInfo& info, CurveReader reader);
  /// Reads one note's envelope point count, which is the capacity its points
  /// need. Reports 0 for an index past the last note, which the C ABI then
  /// refuses on its own.
  bool ReadEnvelopeCount(Napi::Env env, size_t note, size_t* out);
  void Release();

  SonarePolyphonicAnalysis* analysis_ = nullptr;
  /// Each note's frame span, the documented length of its three measured curves.
  /// Held unsigned so it can be a buffer size without a cast that would turn an
  /// inverted span into a four-billion-element allocation.
  std::vector<size_t> spans_;
  int32_t frame_count_ = 0;
};

}  // namespace sonare_node

#endif  // SONARE_NODE_SONARE_WRAP_POLYPHONY_H_
