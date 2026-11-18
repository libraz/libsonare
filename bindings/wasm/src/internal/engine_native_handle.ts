import type { RealtimeEngine } from '../realtime_engine.js';
import type { WasmRealtimeEngine } from '../sonare.js';

/**
 * The embind object behind an engine, for the worklet processor's allocation-free
 * telemetry scratch path and its scene-text strip path.
 */
export function engineNativeHandle(engine: RealtimeEngine): WasmRealtimeEngine {
  return (engine as unknown as { native: WasmRealtimeEngine }).native;
}
