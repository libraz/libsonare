import type { EngineCaptureStatus, RealtimeEngine } from '../index.js';
import { invalidState } from '../internal/entry.js';
import type { SonareEngineSyncMessage } from '../internal/worklet_messages.js';
import type { CaptureOptions } from '../public_types_engine.js';
import { requestObject, requireBoolean } from '../validation.js';
import { commitCommand, commitSync, type EngineCommitContext } from './engine-commit.js';
import type { SonareRealtimeEngineNode } from './engine-node.js';
import { buildCaptureConfig, type CaptureSyncConfig } from './engine-offline.js';
import { SonareEngineCommandType } from './protocol.js';

type CaptureConfig = CaptureSyncConfig;

/**
 * Collaborator surface the capture/record/punch setters need from the owning
 * {@link SonareEngine}: the offline engine they mirror into, the realtime node
 * they command and query, the channel count, the out-of-band sync poster, the
 * capture-config accessor, and the target-id resolver.
 */
export interface EngineCaptureContext extends EngineCommitContext {
  readonly offlineEngine: RealtimeEngine;
  readonly realtimeNode: SonareRealtimeEngineNode;
  readonly sampleRate: number;
  readonly offlineNumChannels: number;
  postSync(message: SonareEngineSyncMessage): void;
  getCaptureConfig(): CaptureConfig | undefined;
  setCaptureConfig(config: CaptureConfig): void;
}

export function configureCapture(ctx: EngineCaptureContext, options: CaptureOptions): void {
  // Every field is validated here, so the buffer replacement below cannot be
  // followed by a refusal that leaves the earlier recording discarded.
  const { config, offlineOptions } = buildCaptureConfig(
    options,
    ctx.offlineNumChannels,
    ctx.sampleRate,
    (ppq) => ctx.offlineEngine.sampleAtPpq(ppq),
  );
  commitSync(
    ctx,
    (offline) => offline.configureCapture(offlineOptions),
    () => {
      ctx.setCaptureConfig(config);
      ctx.postSync({ type: 'syncCapture', ...config });
    },
  );
}

/** Arms or disarms the engine-global capture path; per-track capture is not implemented. */
export function armCapture(ctx: EngineCaptureContext, state: { armed: boolean }): void {
  requestObject('armCapture', state, 'state', true);
  const armed = requireBoolean(state.armed, 'armed');
  if (armed && !ctx.getCaptureConfig()) {
    throw invalidState('Capture buffer is not configured');
  }
  commitCommand(
    ctx,
    {
      type: SonareEngineCommandType.ArmRecord,
      targetId: 0,
      renderFrame: -1,
      argInt: armed ? 1 : 0,
    },
    (offline) => offline.armCapture({ armed }),
  );
}

export function captureStatus(ctx: EngineCaptureContext): Promise<EngineCaptureStatus> {
  return ctx.realtimeNode.requestCaptureStatus();
}

export function capturedAudio(ctx: EngineCaptureContext): Promise<Float32Array[]> {
  return ctx.realtimeNode.requestCapturedAudio();
}

export async function resetCapture(ctx: EngineCaptureContext): Promise<void> {
  // The live reset is confirmed first, so a refused or failed request leaves the offline recording.
  await ctx.realtimeNode.requestCaptureReset();
  ctx.offlineEngine.resetCapture();
}
