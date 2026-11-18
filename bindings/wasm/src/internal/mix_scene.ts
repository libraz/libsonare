import type {
  MixSceneBusInput,
  MixSceneDocument,
  MixSceneDocumentInput,
  MixSceneSendInput,
  MixSceneStripInput,
  SendTiming,
} from '../public_types_mixing.js';

// The native scene document reads and writes the public tokens; these helpers refuse a wrong type or an unknown token
// with TypeError / RangeError before the document reaches native, which would refuse it as an invalid format.

/** Pan-mode tokens, indexed by the native ordinal. */
const PAN_MODES: readonly string[] = ['balance', 'stereoPan', 'dualPan'];
/** Pan-law tokens, indexed by the native ordinal. */
const PAN_LAWS: readonly string[] = ['const3Db', 'const4.5Db', 'const6Db', 'linear0Db'];
const SEND_TIMINGS: readonly SendTiming[] = ['preFader', 'postFader'];

type Loose = Record<string, unknown>;

function tokenOrdinal(
  fnName: string,
  value: unknown,
  argName: string,
  tokens: readonly string[],
): number {
  if (typeof value !== 'string') {
    throw new TypeError(`${fnName}: ${argName} must be a string`);
  }
  const ordinal = tokens.indexOf(value);
  if (ordinal < 0) {
    throw new RangeError(`${fnName}: ${argName} must be one of ${tokens.join(', ')}`);
  }
  return ordinal;
}

/** A pan-mode token as the native ordinal; a non-string is a `TypeError`, an unknown token a `RangeError`. */
export function panModeToNative(fnName: string, value: unknown, argName = 'panMode'): number {
  return tokenOrdinal(fnName, value, argName, PAN_MODES);
}

function sendToNative(fnName: string, send: MixSceneSendInput, path: string): Loose {
  if (send === null || typeof send !== 'object') {
    throw new TypeError(`${fnName}: ${path} must be an object`);
  }
  if (send.timing === undefined || send.timing === null) {
    return { ...send };
  }
  tokenOrdinal(fnName, send.timing, `${path}.timing`, SEND_TIMINGS);
  return { ...send };
}

function panFieldsToNative(fnName: string, out: Loose, path: string): void {
  if (out.panMode !== undefined && out.panMode !== null) {
    tokenOrdinal(fnName, out.panMode, `${path}.panMode`, PAN_MODES);
  }
  if (out.panLaw !== undefined && out.panLaw !== null) {
    tokenOrdinal(fnName, out.panLaw, `${path}.panLaw`, PAN_LAWS);
  }
}

function listOf(fnName: string, value: unknown, path: string): unknown[] | undefined {
  if (value === undefined || value === null) {
    return undefined;
  }
  if (!Array.isArray(value)) {
    throw new TypeError(`${fnName}: ${path} must be an array`);
  }
  return value;
}

function objectAt(fnName: string, value: unknown, path: string): Loose {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new TypeError(`${fnName}: ${path} must be an object`);
  }
  return value as Loose;
}

/** Refuses an explicit 0 oversampling factor: omit the field for the library default. */
export function assertStripMetering(fnName: string, metering: unknown, path = 'metering'): void {
  if (metering === undefined || metering === null) {
    return;
  }
  const { truePeakOversample } = objectAt(fnName, metering, path);
  if (truePeakOversample === 0) {
    throw new RangeError(
      `${fnName}: ${path}.truePeakOversample must be 4 or 8 (omit for the default)`,
    );
  }
}

/** A strip entry of a scene document as the native document spells it. */
function stripToNative(fnName: string, strip: unknown, path: string): Loose {
  const out: Loose = { ...objectAt(fnName, strip, path) };
  panFieldsToNative(fnName, out, path);
  assertStripMetering(fnName, out.metering, `${path}.metering`);
  const sends = listOf(fnName, out.sends, `${path}.sends`);
  if (sends !== undefined) {
    out.sends = sends.map((send, i) =>
      sendToNative(fnName, send as MixSceneSendInput, `${path}.sends[${i}]`),
    );
  }
  return out;
}

function busToNative(fnName: string, bus: unknown, path: string): Loose {
  const out: Loose = { ...objectAt(fnName, bus, path) };
  panFieldsToNative(fnName, out, path);
  return out;
}

/** The native scene document for a public strip, as the engine's per-strip setters read it. */
export function mixSceneStripToNative(
  fnName: string,
  strip: MixSceneStripInput,
  path = 'strip',
): Loose {
  return stripToNative(fnName, strip, path);
}

/** The native scene document for a public bus, as the engine's per-bus setters read it. */
export function mixSceneBusToNative(fnName: string, bus: MixSceneBusInput, path = 'bus'): Loose {
  return busToNative(fnName, bus, path);
}

/** The native scene document for a public one, with every token checked. */
export function mixSceneToNative(
  fnName: string,
  document: MixSceneDocument | MixSceneDocumentInput,
): Loose {
  const out: Loose = { ...objectAt(fnName, document, 'scene') };
  const strips = listOf(fnName, out.strips, 'scene.strips');
  if (strips !== undefined) {
    out.strips = strips.map((strip, i) => stripToNative(fnName, strip, `scene.strips[${i}]`));
  }
  const buses = listOf(fnName, out.buses, 'scene.buses');
  if (buses !== undefined) {
    out.buses = buses.map((bus, i) => busToNative(fnName, bus, `scene.buses[${i}]`));
  }
  return out;
}

/** A public scene JSON string with every token checked; text that does not parse is left for native to refuse. */
export function mixSceneJsonToNative(fnName: string, json: string): string {
  let parsed: unknown;
  try {
    parsed = JSON.parse(json);
  } catch {
    return json;
  }
  if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed)) {
    return json;
  }
  return JSON.stringify(mixSceneToNative(fnName, parsed as MixSceneDocumentInput));
}
