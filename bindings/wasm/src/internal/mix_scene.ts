import type {
  MixMeterSnapshot,
  MixSceneBus,
  MixSceneBusInput,
  MixSceneDocument,
  MixSceneDocumentInput,
  MixSceneSend,
  MixSceneSendInput,
  MixSceneStrip,
  MixSceneStripInput,
  SendTiming,
} from '../public_types_mixing.js';

// The native scene document still spells these as ints and 'pre' / 'post'; the public document uses tokens.

/** Pan-mode tokens, indexed by the native ordinal. */
const PAN_MODES: readonly string[] = ['balance', 'stereoPan', 'dualPan'];
/** Pan-law tokens, indexed by the native ordinal. */
const PAN_LAWS: readonly string[] = ['const3Db', 'const4.5Db', 'const6Db', 'linear0Db'];
const SEND_TIMING_TO_NATIVE: Readonly<Record<SendTiming, string>> = {
  preFader: 'pre',
  postFader: 'post',
};
const SEND_TIMING_FROM_NATIVE: Readonly<Record<string, SendTiming>> = {
  pre: 'preFader',
  post: 'postFader',
};

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

/** A pan-law token as the native ordinal; a non-string is a `TypeError`, an unknown token a `RangeError`. */
export function panLawToNative(fnName: string, value: unknown, argName = 'panLaw'): number {
  return tokenOrdinal(fnName, value, argName, PAN_LAWS);
}

/** A send-timing token as the native scene spelling; a non-string is a `TypeError`, an unknown token a `RangeError`. */
export function sendTimingToNative(fnName: string, value: unknown, argName = 'timing'): string {
  tokenOrdinal(fnName, value, argName, Object.keys(SEND_TIMING_TO_NATIVE));
  return SEND_TIMING_TO_NATIVE[value as SendTiming];
}

function tokenFromNative(tokens: readonly string[], value: unknown): unknown {
  return tokens[value as number] ?? value;
}

function sendToNative(fnName: string, send: MixSceneSendInput, path: string): Loose {
  if (send === null || typeof send !== 'object') {
    throw new TypeError(`${fnName}: ${path} must be an object`);
  }
  if (send.timing === undefined || send.timing === null) {
    return { ...send };
  }
  return { ...send, timing: sendTimingToNative(fnName, send.timing, `${path}.timing`) };
}

function panFieldsToNative(fnName: string, out: Loose, path: string): void {
  if (out.panMode !== undefined && out.panMode !== null) {
    out.panMode = panModeToNative(fnName, out.panMode, `${path}.panMode`);
  }
  if (out.panLaw !== undefined && out.panLaw !== null) {
    out.panLaw = panLawToNative(fnName, out.panLaw, `${path}.panLaw`);
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

/** The native scene document for a public one: tokens become ordinals and `'pre'` / `'post'`. */
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

function sendFromNative(send: Loose): MixSceneSend {
  return {
    ...send,
    timing: SEND_TIMING_FROM_NATIVE[send.timing as string] ?? send.timing,
  } as unknown as MixSceneSend;
}

function stripFromNative(strip: Loose): MixSceneStrip {
  const out: Loose = { ...strip };
  if (out.panMode !== undefined) {
    out.panMode = tokenFromNative(PAN_MODES, out.panMode);
  }
  if (out.panLaw !== undefined) {
    out.panLaw = tokenFromNative(PAN_LAWS, out.panLaw);
  }
  if (Array.isArray(out.sends)) {
    out.sends = (out.sends as Loose[]).map(sendFromNative);
  }
  return out as unknown as MixSceneStrip;
}

function busFromNative(bus: Loose): MixSceneBus {
  const out: Loose = { ...bus };
  if (out.panMode !== undefined) {
    out.panMode = tokenFromNative(PAN_MODES, out.panMode);
  }
  if (out.panLaw !== undefined) {
    out.panLaw = tokenFromNative(PAN_LAWS, out.panLaw);
  }
  return out as unknown as MixSceneBus;
}

/** The public scene document for a parsed native one: ordinals and `'pre'` / `'post'` become tokens. */
export function mixSceneFromNative(document: unknown): MixSceneDocument {
  const out: Loose = { ...(document as Loose) };
  if (Array.isArray(out.strips)) {
    out.strips = (out.strips as Loose[]).map(stripFromNative);
  }
  if (Array.isArray(out.buses)) {
    out.buses = (out.buses as Loose[]).map(busFromNative);
  }
  return out as unknown as MixSceneDocument;
}

/** The public meter snapshot for a native one, which still spells the true-peak and channel-count fields without the unit. */
export function mixMeterSnapshotFromNative(native: unknown): MixMeterSnapshot {
  const { truePeakDbL, truePeakDbR, maxTruePeakDb, truePeakDb, channelCount, ...rest } =
    native as Loose;
  const out: Loose = {
    ...rest,
    truePeakDbtpL: truePeakDbL,
    truePeakDbtpR: truePeakDbR,
    maxTruePeakDbtp: maxTruePeakDb,
  };
  if (channelCount !== undefined) {
    out.numChannels = channelCount;
  }
  if (truePeakDb !== undefined) {
    out.truePeakDbtp = truePeakDb;
  }
  return out as unknown as MixMeterSnapshot;
}

/** A public scene JSON string as the native document: tokens become ordinals; text that does not parse is left for native to refuse. */
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

/** A native scene JSON string as the public document text: ordinals become tokens. */
export function mixSceneJsonFromNative(json: string): string {
  return JSON.stringify(mixSceneFromNative(JSON.parse(json)));
}
