import { assertDeclaredKeys, refuse, refuseType } from './internal/entry.js';
import { requireListBudget, stringifyWithinBudget } from './internal/list_budget.js';
import { tokenName, tokenOrdinal } from './internal/project_tokens.js';
import { refuseSynthPatchOrdinals } from './internal/synth_tokens.js';
import { isAbsent, isNonArrayObject } from './internal/value_guards.js';
import { SampleBank } from './sample_bank.js';
import type {
  AutomationCurve,
  MeterTap,
  PanLaw,
  PanMode,
  PartRigInsert,
  PartRigKey,
  PartRigMode,
  PartRigRequest,
  ProjectAutomationLaneDesc,
  ProjectAutomationPoint,
  SendTiming,
  SynthPatch,
} from './types.js';
import type { ChannelLayout } from './types_engine.js';

/** `SonareChannelLayout` ordinals of the channel-layout tokens. */
export const CHANNEL_LAYOUT_VALUES: Readonly<Record<ChannelLayout, number>> = {
  mono: 0,
  stereo: 1,
  '5.1': 2,
  '7.1': 3,
};

/**
 * Internal coercion helpers shared by the Project, Mixer and RealtimeEngine
 * facades: they map the public string/union enum spellings to the integer
 * ordinals the C ABI expects, and reshape the few descriptors whose public form
 * differs from the one the addon reads. Not part of the public surface.
 *
 * The SampleBank unwrap at the foot is the module's only runtime import;
 * everything else here is a pure function over type-only inputs.
 */

/**
 * Resolve a public enum spelling to its C ABI ordinal. Only the exact declared
 * spelling is accepted: anything that is not a string (a number included) is a
 * `TypeError`, an unknown string a `RangeError`.
 */
export function resolveEnumOrdinal(
  value: unknown,
  values: Readonly<Record<string, number>>,
  key: string,
): number {
  const allowed = Object.keys(values).join(', ');
  if (typeof value !== 'string') {
    throw refuseType(`${key} must be one of ${allowed}`);
  }
  if (Object.hasOwn(values, value)) {
    return values[value] as number;
  }
  throw refuse(`${key} must be one of ${allowed}`);
}

/**
 * Automation curve spellings and their `SonareAutomationCurve` ordinals.
 *
 * Single source of truth for the vocabulary: `AutomationCurve` is derived
 * from these keys, so a spelling
 * cannot be documented without resolving, or resolve without being documented.
 */
export const AUTOMATION_CURVE_VALUES = {
  linear: 0,
  exponential: 1,
  hold: 2,
  sCurve: 3,
} as const;

/** One automation breakpoint as the native layer reads it: the curve as its ordinal. */
export type NativeAutomationPoint = Omit<ProjectAutomationPoint, 'curveToNext'> & {
  curveToNext: number;
};

/** A breakpoint the native layer produced, with its curve ordinal named. */
export function projectAutomationPointFromNative(native: {
  ppq: number;
  value: number;
  curveToNext: number;
}): ProjectAutomationPoint {
  return {
    ...native,
    curveToNext: tokenName(AUTOMATION_CURVE_VALUES, native.curveToNext, 'automation curve'),
  };
}

/** The fields an automation breakpoint declares. */
export const AUTOMATION_POINT_KEYS = ['ppq', 'value', 'curveToNext'];

/** Resolve a breakpoint's curve token; a key outside the point's fields is refused by name. */
export function projectAutomationPointValue(
  point: ProjectAutomationPoint,
  index: number,
): NativeAutomationPoint {
  assertDeclaredKeys(point, AUTOMATION_POINT_KEYS, `points[${index}]`);
  const curveToNext = tokenOrdinal(
    point.curveToNext ?? 'linear',
    AUTOMATION_CURVE_VALUES,
    'curveToNext',
  );
  return { ...point, curveToNext };
}

export type NativeAutomationLaneDesc = Omit<ProjectAutomationLaneDesc, 'targetKind' | 'points'> & {
  targetKind?: 0 | 1 | 2;
  points: NativeAutomationPoint[];
};

export function projectAutomationLaneValue(
  desc: Omit<ProjectAutomationLaneDesc, 'targetKind'> & { targetKind?: 0 | 1 | 2 },
): NativeAutomationLaneDesc {
  if (desc.targetParamId === 0) {
    throw refuse('project automation lane targetParamId must be non-zero');
  }
  if (Array.isArray(desc.points)) {
    requireListBudget(desc.points, 'points');
  }
  return {
    ...desc,
    points: desc.points.map((point, index) => projectAutomationPointValue(point, index)),
  };
}

export const PAN_LAW_VALUES: Record<PanLaw, number> = {
  const3Db: 0,
  'const4.5Db': 1,
  const6Db: 2,
  linear0Db: 3,
};

export const PAN_MODE_VALUES: Record<PanMode, number> = {
  balance: 0,
  stereoPan: 1,
  dualPan: 2,
};

const METER_TAP_VALUES: Record<MeterTap, number> = {
  preFader: 0,
  postFader: 1,
};

// Mirrors SonareSendTiming: post-fader is 0 so a zero-initialized C ABI send defaults to post-fader.
// Keys follow the scene document's token order, which every refusal lists.
export const SEND_TIMING_VALUES: Record<SendTiming, number> = {
  preFader: 1,
  postFader: 0,
};

export function automationCurveValue(curve: AutomationCurve, key: string): number {
  return resolveEnumOrdinal(curve, AUTOMATION_CURVE_VALUES, key);
}

export function panLawValue(panLaw: PanLaw, key: string): number {
  return resolveEnumOrdinal(panLaw, PAN_LAW_VALUES, key);
}

export function panModeValue(panMode: PanMode, key: string): number {
  return resolveEnumOrdinal(panMode, PAN_MODE_VALUES, key);
}

export function meterTapValue(tap: MeterTap, key: string): number {
  return resolveEnumOrdinal(tap, METER_TAP_VALUES, key);
}

export function sendTimingValue(timing: SendTiming, key: string): number {
  return resolveEnumOrdinal(timing, SEND_TIMING_VALUES, key);
}

/**
 * Swaps a facade {@link SampleBank} in a synth patch descriptor for the addon
 * handle the native layer expects, leaving every other field alone. Shared by
 * the offline bounce and the realtime engine, so `sampleBank` means the same
 * thing on both.
 */
export function normalizeSynthInstrument(patch: SynthPatch): SynthPatch {
  if (!isNonArrayObject(patch)) {
    throw refuseType(`patch must be a SynthPatch object; a preset is named by its preset field`);
  }
  refuseSynthPatchOrdinals(patch);
  if (!(patch.sampleBank instanceof SampleBank)) {
    // An omitted bank passes through for the native layer to read as "no bank",
    // and a value that is not a bank at all for it to reject as a TypeError.
    return patch;
  }
  // Reads SampleBank's private `native`. TypeScript's private is a compile-time
  // rule, so one reviewed read beats widening the class with a handle accessor
  // no caller wants. The field holds the addon's own bank object, which has no
  // TypeScript type of its own; only the native layer looks at it again.
  const handle = (patch.sampleBank as unknown as { native: SampleBank }).native;
  return { ...patch, sampleBank: handle };
}

/**
 * Serialize a part-rig request's inserts to the C ABI JSON form, refusing a
 * wrong shape by field name. Returns `undefined` when there are none.
 */
export function partRigInsertsJson(inserts: PartRigInsert[] | undefined): string | undefined {
  if (isAbsent(inserts)) {
    return undefined;
  }
  if (!Array.isArray(inserts)) {
    throw refuseType('inserts must be an array');
  }
  requireListBudget(inserts, 'inserts');
  return stringifyWithinBudget(inserts, 'inserts');
}

/** A part-rig call resolved to the native argument list; `mode` is unset for a key. */
export interface NormalizedPartRig {
  destinationId: number;
  part: number;
  mode: PartRigMode | undefined;
  insertsJson: string | undefined;
}

/**
 * Resolve either part-rig call form — a {@link PartRigRequest} / {@link PartRigKey}
 * object, or positional `(destinationId, part, mode?, inserts?)` — to the native
 * arguments, so both forms share one validation path. A request object takes no
 * further arguments.
 */
export function normalizePartRig(
  requestOrDestinationId: PartRigRequest | PartRigKey | number,
  part?: number,
  mode?: PartRigMode,
  inserts?: PartRigInsert[],
): NormalizedPartRig {
  if (typeof requestOrDestinationId === 'object' && requestOrDestinationId !== null) {
    if (part !== undefined || mode !== undefined || inserts !== undefined) {
      throw refuseType(`a request object takes no further arguments`);
    }
    const request = requestOrDestinationId as Partial<PartRigRequest>;
    return {
      destinationId: request.destinationId as number,
      part: request.part as number,
      mode: request.mode,
      insertsJson: partRigInsertsJson(request.inserts),
    };
  }
  return {
    destinationId: requestOrDestinationId,
    part: part as number,
    mode,
    insertsJson: partRigInsertsJson(inserts),
  };
}
