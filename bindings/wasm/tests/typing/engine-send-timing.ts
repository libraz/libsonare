import type { WasmRealtimeEngine } from '../../src/sonare.js';

export function configureNativeLaneSends(engine: WasmRealtimeEngine): void {
  engine.setTrackLanes([
    { trackId: 10, sends: [{ busId: 1, sendTiming: 1 }] },
    { trackId: 20, sends: [{ busId: 1, sendTiming: 0 }] },
  ]);
  engine.setTrackLanes([{ trackId: 30, sends: [{ busId: 1 }] }]);
}
