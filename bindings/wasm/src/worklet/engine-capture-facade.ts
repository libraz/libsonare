import type { EngineCaptureStatus, RealtimeEngine } from '../index.js';
import { commitCommand, commitSync, type EngineCommitContext } from './engine-commit.js';
import type { SonareRealtimeEngineNode } from './engine-node.js';
import { buildCaptureConfig, type CaptureOptions } from './engine-offline.js';
import type { SonareEngineSyncCaptureMessage, SonareEngineSyncMessage } from './messages.js';
import { SonareEngineCommandType } from './protocol.js';

type CaptureConfig = Omit<SonareEngineSyncCaptureMessage, 'type'>;

/**
 * Collaborator surface the capture/record/punch setters need from the owning
 * {@link SonareEngine}: the offline engine they mirror into, the realtime node
 * they command and query, the channel count, the out-of-band sync poster, the
 * capture-config accessor, and the target-id resolver.
 */
export interface EngineCaptureContext extends EngineCommitContext {
  readonly offlineEngine: RealtimeEngine;
  readonly realtimeNode: SonareRealtimeEngineNode;
  readonly offlineChannelCount: number;
  postSync(message: SonareEngineSyncMessage): void;
  getCaptureConfig(): CaptureConfig | undefined;
  setCaptureConfig(config: CaptureConfig): void;
}

export function configureCapture(ctx: EngineCaptureContext, options: CaptureOptions): void {
  // Every field is validated here, so the buffer replacement below cannot be
  // followed by a refusal that leaves the earlier recording discarded.
  const config = buildCaptureConfig(options, ctx.offlineChannelCount);
  commitSync(
    ctx,
    (offline) => {
      offline.setCaptureBuffer(config.channels, config.bufferFrames);
      offline.setCaptureSource(config.source);
      offline.setRecordOffsetSamples(config.recordOffsetSamples);
      offline.setInputMonitor(config.inputMonitor.enabled, config.inputMonitor.gain);
    },
    () => {
      ctx.setCaptureConfig(config);
      ctx.postSync({ type: 'syncCapture', ...config });
    },
  );
}

export function armRecord(
  ctx: EngineCaptureContext,
  trackId: string | number,
  enabled: boolean,
): boolean {
  if (trackId !== 0) {
    throw new RangeError('Capture is global; armRecord only accepts trackId 0');
  }
  if (enabled && !ctx.getCaptureConfig()) {
    throw new Error('Capture buffer is not configured');
  }
  return commitCommand(
    ctx,
    {
      type: SonareEngineCommandType.ArmRecord,
      targetId: 0,
      sampleTime: -1,
      argInt: enabled ? 1 : 0,
    },
    (offline) => offline.armCapture(enabled),
  );
}

export function punch(ctx: EngineCaptureContext, inPpq: number, outPpq: number): boolean {
  const inSample = ctx.offlineEngine.sampleAtPpq(inPpq);
  const outSample = ctx.offlineEngine.sampleAtPpq(outPpq);
  // Carry BOTH endpoints as already-converted SAMPLES so the realtime engine
  // agrees with the offline engine. The previous code sent the raw PPQ out
  // point and let the consumer multiply by sampleRate (treating PPQ as
  // seconds), which ignored tempo and produced a punch-out ~2x too large at
  // 120 BPM. argInt = in sample, argFloat = out sample (full-precision double).
  return commitCommand(
    ctx,
    { type: SonareEngineCommandType.Punch, sampleTime: -1, argInt: inSample, argFloat: outSample },
    (offline) => offline.setCapturePunch(inSample, outSample, true),
  );
}

export function captureStatus(ctx: EngineCaptureContext): Promise<EngineCaptureStatus> {
  return ctx.realtimeNode.requestCaptureStatus();
}

export function capturedAudio(ctx: EngineCaptureContext): Promise<Float32Array[]> {
  return ctx.realtimeNode.requestCapturedAudio();
}

export async function resetCapture(ctx: EngineCaptureContext): Promise<void> {
  ctx.offlineEngine.resetCapture();
  await ctx.realtimeNode.requestCaptureReset();
}
