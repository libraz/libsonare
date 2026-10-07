#include <cstring>
#include <string>
#include <vector>

#include "features/common.h"
#include "sonare_wrap.h"
#include "sonare_wrap_options.h"
#include "sonare_wrap_utils.h"

using namespace sonare_node;
using namespace sonare_node::features;

namespace {

Napi::Object SegmentMatrixResult(Napi::Env env, SonareSegmentMatrix* result) {
  Napi::Object out = Napi::Object::New(env);
  out.Set("rows", Napi::Number::New(env, result->rows));
  out.Set("cols", Napi::Number::New(env, result->cols));
  auto values = Napi::Float32Array::New(env, static_cast<size_t>(result->rows) * result->cols);
  if (result->values != nullptr) {
    std::memcpy(values.Data(), result->values, values.ElementLength() * sizeof(float));
  }
  out.Set("values", values);
  sonare_free_segment_matrix(result);
  return out;
}

Napi::Int32Array SegmentIndicesResult(Napi::Env env, SonareSegmentIndices* result) {
  auto values = Napi::Int32Array::New(env, result->count);
  if (result->values != nullptr) {
    std::memcpy(values.Data(), result->values, result->count * sizeof(int));
  }
  sonare_free_segment_indices(result);
  return values;
}

bool SegmentMatrixInput(Napi::Env env, const char* name, const Napi::Float32Array& values, int rows,
                        int cols) {
  if (rows <= 0 || cols <= 0) {
    Napi::RangeError::New(env, std::string(name) + ": matrix dimensions must be positive")
        .ThrowAsJavaScriptException();
    return false;
  }
  return ValidateMatrixDims(env, name, rows, cols, values.ElementLength());
}

}  // namespace

Napi::Value SonareWrap::SegmentCrossSimilarity(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "Expected X Float32Array") ||
      !RequireFloat32Array(info, 3, "Expected Y Float32Array"))
    return env.Undefined();
  const auto x = info[0].As<Napi::Float32Array>();
  int x_rows{};
  if (!OptionalIntArg(env, info, 1, "xRows", 0, &x_rows)) return env.Undefined();
  int x_cols{};
  if (!OptionalIntArg(env, info, 2, "xCols", 0, &x_cols)) return env.Undefined();
  const auto y = info[3].As<Napi::Float32Array>();
  int y_rows{};
  if (!OptionalIntArg(env, info, 4, "yRows", 0, &y_rows)) return env.Undefined();
  int y_cols{};
  if (!OptionalIntArg(env, info, 5, "yCols", 0, &y_cols)) return env.Undefined();
  int k{};
  if (!OptionalIntArg(env, info, 6, "k", 0, &k)) return env.Undefined();
  std::string metric;
  if (!OptionalStringArg(env, info, 7, "metric", "cosine", &metric)) {
    return env.Undefined();
  }
  std::string mode;
  if (!OptionalStringArg(env, info, 8, "mode", "connectivity", &mode)) {
    return env.Undefined();
  }
  if (x_rows <= 0 || x_cols <= 0 || y_rows != x_rows || y_cols <= 0 || k < 0 ||
      x.ElementLength() != static_cast<size_t>(x_rows) * x_cols ||
      y.ElementLength() != static_cast<size_t>(y_rows) * y_cols) {
    Napi::TypeError::New(env, "segmentCrossSimilarity: invalid matrix dimensions")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SonareSegmentMatrix result{};
  const SonareError err = sonare_segment_cross_similarity(
      x.Data(), x_rows, x_cols, y.Data(), y_rows, y_cols, k, metric.c_str(), mode.c_str(), &result);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return SegmentMatrixResult(env, &result);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SegmentRecurrenceMatrix(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "Expected data Float32Array")) return env.Undefined();
  const auto data = info[0].As<Napi::Float32Array>();
  int rows{};
  if (!OptionalIntArg(env, info, 1, "rows", 0, &rows)) return env.Undefined();
  int cols{};
  if (!OptionalIntArg(env, info, 2, "cols", 0, &cols)) return env.Undefined();
  if (!SegmentMatrixInput(env, "segmentRecurrenceMatrix", data, rows, cols)) return env.Undefined();
  int k{};
  if (!OptionalIntArg(env, info, 3, "k", 0, &k)) return env.Undefined();
  int width{};
  if (!OptionalIntArg(env, info, 4, "width", 1, &width)) return env.Undefined();
  bool sym{};
  if (!OptionalBoolArg(env, info, 5, "sym", false, &sym)) return env.Undefined();
  std::string metric;
  if (!OptionalStringArg(env, info, 6, "metric", "euclidean", &metric)) {
    return env.Undefined();
  }
  std::string mode;
  if (!OptionalStringArg(env, info, 7, "mode", "connectivity", &mode)) {
    return env.Undefined();
  }
  SonareSegmentMatrix result{};
  const SonareError err = sonare_segment_recurrence_matrix(
      data.Data(), rows, cols, k, width, sym ? 1 : 0, metric.c_str(), mode.c_str(), &result);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return SegmentMatrixResult(env, &result);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SegmentRecurrenceToLag(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "Expected recurrence Float32Array")) return env.Undefined();
  const auto recurrence = info[0].As<Napi::Float32Array>();
  int n{};
  if (!OptionalIntArg(env, info, 1, "n", 0, &n)) return env.Undefined();
  if (!SegmentMatrixInput(env, "segmentRecurrenceToLag", recurrence, n, n)) return env.Undefined();
  bool pad{};
  if (!OptionalBoolArg(env, info, 2, "pad", false, &pad)) return env.Undefined();
  SonareSegmentMatrix result{};
  const SonareError err =
      sonare_segment_recurrence_to_lag(recurrence.Data(), n, pad ? 1 : 0, &result);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return SegmentMatrixResult(env, &result);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SegmentLagToRecurrence(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "Expected lag Float32Array")) return env.Undefined();
  const auto lag = info[0].As<Napi::Float32Array>();
  int rows{};
  if (!OptionalIntArg(env, info, 1, "rows", 0, &rows)) return env.Undefined();
  int lags{};
  if (!OptionalIntArg(env, info, 2, "lags", 0, &lags)) return env.Undefined();
  if (!SegmentMatrixInput(env, "segmentLagToRecurrence", lag, rows, lags)) return env.Undefined();
  SonareSegmentMatrix result{};
  const SonareError err = sonare_segment_lag_to_recurrence(lag.Data(), rows, lags, &result);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return SegmentMatrixResult(env, &result);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SegmentSubsegment(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!RequireFloat32Array(info, 0, "Expected data Float32Array")) return env.Undefined();
  // A short argument list used to short-circuit ahead of the reader, so the call
  // returned undefined with nothing pending and the caller saw no error at all.
  if (info.Length() < 4) {
    Napi::TypeError::New(env, "Expected (data, rows, cols, boundaries, nSegments?)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  SONARE_NODE_TRY
  const auto data = info[0].As<Napi::Float32Array>();
  int rows{};
  if (!OptionalIntArg(env, info, 1, "rows", 0, &rows)) return env.Undefined();
  int cols{};
  if (!OptionalIntArg(env, info, 2, "cols", 0, &cols)) return env.Undefined();
  if (!SegmentMatrixInput(env, "segmentSubsegment", data, rows, cols)) return env.Undefined();
  const std::vector<int> boundaries = IntVectorFromValue(info[3], "boundaries");
  int n_segments{};
  if (!OptionalIntArg(env, info, 4, "nSegments", 4, &n_segments)) return env.Undefined();
  SonareSegmentIndices result{};
  const SonareError err = sonare_segment_subsegment(data.Data(), rows, cols, boundaries.data(),
                                                    boundaries.size(), n_segments, &result);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return SegmentIndicesResult(env, &result);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SegmentAgglomerative(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "Expected data Float32Array")) return env.Undefined();
  const auto data = info[0].As<Napi::Float32Array>();
  int rows{};
  if (!OptionalIntArg(env, info, 1, "rows", 0, &rows)) return env.Undefined();
  int cols{};
  if (!OptionalIntArg(env, info, 2, "cols", 0, &cols)) return env.Undefined();
  if (!SegmentMatrixInput(env, "segmentAgglomerative", data, rows, cols)) return env.Undefined();
  int k{};
  if (!OptionalIntArg(env, info, 3, "k", 0, &k)) return env.Undefined();
  std::string linkage;
  if (!OptionalStringArg(env, info, 4, "linkage", "average", &linkage)) {
    return env.Undefined();
  }
  SonareSegmentIndices result{};
  const SonareError err =
      sonare_segment_agglomerative(data.Data(), rows, cols, k, linkage.c_str(), &result);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return SegmentIndicesResult(env, &result);
  SONARE_NODE_CATCH(env)
}

Napi::Value SonareWrap::SegmentPathEnhance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  SONARE_NODE_TRY
  if (!RequireFloat32Array(info, 0, "Expected recurrence Float32Array")) return env.Undefined();
  const auto recurrence = info[0].As<Napi::Float32Array>();
  int n{};
  if (!OptionalIntArg(env, info, 1, "n", 0, &n)) return env.Undefined();
  if (!SegmentMatrixInput(env, "segmentPathEnhance", recurrence, n, n)) return env.Undefined();
  int win{};
  if (!OptionalIntArg(env, info, 2, "win", 0, &win)) return env.Undefined();
  int max_ratio{};
  if (!OptionalIntArg(env, info, 3, "maxRatio", 2, &max_ratio)) return env.Undefined();
  int min_ratio{};
  if (!OptionalIntArg(env, info, 4, "minRatio", 0, &min_ratio)) return env.Undefined();
  int n_filters{};
  if (!OptionalIntArg(env, info, 5, "nFilters", 7, &n_filters)) return env.Undefined();
  SonareSegmentMatrix result{};
  const SonareError err = sonare_segment_path_enhance(recurrence.Data(), n, win, max_ratio,
                                                      min_ratio, n_filters, &result);
  if (err != SONARE_OK) return CheckCResult(env, err);
  return SegmentMatrixResult(env, &result);
  SONARE_NODE_CATCH(env)
}
