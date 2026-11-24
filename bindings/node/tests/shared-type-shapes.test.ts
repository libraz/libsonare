/**
 * Public type names the Node and WASM packages both export must mean the same
 * shape.
 *
 * `DynamicsResult` used to be the analysis metrics in one package and the
 * processor envelope `{ samples, latencySamples }` in the other, so a shared
 * TypeScript module annotated with it type-checked against one package and
 * failed against the other — the identifier resolved either way and only the
 * member list gave it away.
 *
 * The two packages are separate npm packages and cannot import one module, so
 * what makes a drift red is this: each package builds a sample object with
 * `satisfies`, which is exact (a missing field is a compile error, an extra one
 * is an excess-property error), and then compares that object's keys against
 * `tests/conformance/shared_type_shapes.json`. Adding, removing or renaming a
 * field on either side fails that package's own suite against the shared file,
 * and the sibling suite reads the same file.
 */

import { readFileSync } from 'node:fs';

import { describe, expect, expectTypeOf, it } from 'vitest';
import type {
  AudioFormatCapabilities,
  Capabilities,
  CapabilityCatalog,
  CapabilityCatalogChain,
  CapabilityCatalogChainList,
  CapabilityCatalogChainStage,
  CapabilityCatalogPresets,
  ChordSymbolFields,
  DynamicsProcessorResult,
  DynamicsResult,
  EngineBounceResult,
  EngineCaptureStatus,
  MasteringInsertParamChoice,
  MasteringInsertParamDependency,
  MasteringInsertParamInfo,
  MasteringInsertSlot,
  MasteringInsertTiming,
  MasteringPresetCatalogEntry,
  MasteringProcessorCatalogEntry,
  MeterEstimate,
  ProjectChordSymbol,
  ProjectClip,
  ProjectDiagnostic,
  ProjectMarker,
  ProjectSource,
  ProjectTempoCandidate,
  ProjectTrack,
  VocalStateToken,
} from '../src/index.js';

interface SharedTypeShapes {
  types: Record<string, string[]>;
  aliases: Record<string, string>;
}

const shapes = JSON.parse(
  readFileSync(
    new URL('../../../tests/conformance/shared_type_shapes.json', import.meta.url),
    'utf8',
  ),
) as SharedTypeShapes;

type Equal<Left, Right> =
  (<Value>() => Value extends Left ? 1 : 2) extends <Value>() => Value extends Right ? 1 : 2
    ? (<Value>() => Value extends Right ? 1 : 2) extends <Value>() => Value extends Left ? 1 : 2
      ? true
      : false
    : false;
type IsReadonly<T, Key extends keyof T> = Equal<Pick<T, Key>, Readonly<Pick<T, Key>>>;

// Exact by construction: `satisfies` rejects a missing or extra field, so these
// literals cannot drift from the interfaces without a compile error.
const dynamicsResult = {
  dynamicRangeDb: 0,
  peakDb: 0,
  rmsDb: 0,
  crestFactor: 0,
  loudnessRangeLu: 0,
  isCompressed: false,
  loudnessTimesSec: new Float32Array(0),
  loudnessRmsDb: new Float32Array(0),
} satisfies DynamicsResult;

const dynamicsProcessorResult = {
  samples: new Float32Array(0),
  latencySamples: 0,
} satisfies DynamicsProcessorResult;

const meterEstimate = {
  timeSignature: { numerator: 4, denominator: 4, confidence: 0 },
  downbeatPhase: 0,
  searched: false,
  grouping: [4],
  candidateScores: [],
  candidates: [],
  unobservedBeats: [0, 6],
} satisfies MeterEstimate;

const engineCaptureStatus = {
  capturedFrames: 128,
  droppedFrames: 32,
  armed: true,
  punchEnabled: false,
  source: 'output',
  recordOffsetSamples: 0,
} satisfies EngineCaptureStatus;

describe('shared public type shapes', () => {
  it('EngineCaptureStatus is the shape the shared corpus declares', () => {
    expect(Object.keys(engineCaptureStatus).sort()).toEqual(
      [...shapes.types.EngineCaptureStatus].sort(),
    );
  });
  it('MeterEstimate is the shape the shared corpus declares', () => {
    expect(Object.keys(meterEstimate).sort()).toEqual([...shapes.types.MeterEstimate].sort());
  });

  it('DynamicsResult is the analysis shape the shared corpus declares', () => {
    expect(Object.keys(dynamicsResult).sort()).toEqual([...shapes.types.DynamicsResult].sort());
  });

  it('DynamicsProcessorResult is the processor envelope the shared corpus declares', () => {
    expect(Object.keys(dynamicsProcessorResult).sort()).toEqual(
      [...shapes.types.DynamicsProcessorResult].sort(),
    );
  });

  it('the two shapes stay disjoint, so a mix-up cannot be silent', () => {
    const analysis = new Set(shapes.types.DynamicsResult);
    const overlap = shapes.types.DynamicsProcessorResult.filter((key) => analysis.has(key));
    expect(overlap).toEqual([]);
  });
});

// The catalog's part types are exported from the entry so a host can name them
// directly; importing them here is what turns a dropped export into a type error.
const catalogParameter = {
  name: 'driveDb',
  id: undefined,
  rtSafe: false,
  type: 'number',
  min: undefined,
  max: undefined,
  minExclusive: false,
  maxExclusive: false,
  maxRelativeTo: undefined,
  default: undefined,
  unit: 'Hz',
  uiMin: undefined,
  uiMax: undefined,
  scale: 'linear',
  choices: undefined,
  slot: 'band1',
  dependsOn: [{ key: 'maxHz', relation: 'le', factor: 1 }],
  coupledWith: ['minHz'],
  domainMin: undefined,
  domainMinExclusive: undefined,
  domainMax: undefined,
  domainMaxExclusive: undefined,
} satisfies MasteringInsertParamInfo;

const catalogSlot = {
  name: 'band1',
  parent: undefined,
  activation: 'always',
  minCrossoverCutoffs: 1,
} satisfies MasteringInsertSlot;

const catalogProcessor = {
  id: 'saturation.softClipper',
  description: 'Soft-clips peaks above a threshold.',
  kind: 'realtime',
  forms: ['realtime'],
  realtimeInsertable: true,
  stereoOnly: false,
  latencySamples: 0,
  tailSamples: 0,
  realtimeCost: undefined,
  channelPolicy: 'perChannel',
  category: 'saturation',
  causal: true,
  params: [catalogParameter],
  offlineParams: undefined,
  slots: [catalogSlot],
} satisfies MasteringProcessorCatalogEntry;

const catalogPresets = {
  mastering: [],
  synth: [],
  mixingScene: [],
  voiceChanger: [],
  playbackRoom: [],
} satisfies CapabilityCatalogPresets;

const catalogMasteringPreset = {
  name: 'vinyl',
  kind: 'restoration',
  targetLufs: undefined,
  truePeakCeilingDbtp: undefined,
  maxLimiterGainReductionDb: undefined,
} satisfies MasteringPresetCatalogEntry;

const catalogChainStage = {
  prefix: 'dynamics.compressor',
  processor: 'dynamics.compressor',
  form: 'offline',
  enabledKey: 'dynamics.compressor.enabled',
} satisfies CapabilityCatalogChainStage;

const catalogChainList = {
  key: 'dynamics.multibandComp.bands',
  processor: 'dynamics.compressor',
  form: 'offline',
  minItems: 2,
  maxItems: 64,
} satisfies CapabilityCatalogChainList;

const catalogChain = {
  stages: [catalogChainStage],
  lists: [catalogChainList],
  params: [catalogParameter],
} satisfies CapabilityCatalogChain;

const catalogEncode = {
  builtin: ['wav'],
  ffmpeg: [],
} satisfies AudioFormatCapabilities;

const capabilityCatalog = {
  version: '',
  abi: null as unknown as CapabilityCatalog['abi'],
  processors: [catalogProcessor],
  chain: catalogChain,
  presets: catalogPresets,
  masteringPresets: [catalogMasteringPreset],
  encode: catalogEncode,
} satisfies CapabilityCatalog;

describe('capability catalog type shapes', () => {
  it.each([
    ['CapabilityCatalog', capabilityCatalog],
    ['CapabilityCatalogChain', catalogChain],
    ['CapabilityCatalogChainList', catalogChainList],
    ['CapabilityCatalogChainStage', catalogChainStage],
    ['CapabilityCatalogPresets', catalogPresets],
    ['MasteringInsertParamInfo', catalogParameter],
    ['MasteringInsertSlot', catalogSlot],
    ['MasteringPresetCatalogEntry', catalogMasteringPreset],
    ['MasteringProcessorCatalogEntry', catalogProcessor],
    ['AudioFormatCapabilities', catalogEncode],
  ] as const)('%s is the shape the shared corpus declares', (name, sample) => {
    expect(Object.keys(sample).sort()).toEqual([...shapes.types[name]].sort());
  });
});

describe('readonly result record migration', () => {
  it('CapabilityCatalogPresets migration: readonly records and arrays require copies', () => {
    expectTypeOf<IsReadonly<Capabilities, 'version'>>().toEqualTypeOf<true>();
    expectTypeOf<IsReadonly<CapabilityCatalog, 'version'>>().toEqualTypeOf<true>();
    expectTypeOf<IsReadonly<CapabilityCatalogChain, 'stages'>>().toEqualTypeOf<true>();
    expectTypeOf<IsReadonly<CapabilityCatalogChainList, 'key'>>().toEqualTypeOf<true>();
    expectTypeOf<IsReadonly<CapabilityCatalogChainStage, 'prefix'>>().toEqualTypeOf<true>();
    expectTypeOf<IsReadonly<CapabilityCatalogPresets, 'mastering'>>().toEqualTypeOf<true>();
    expectTypeOf<CapabilityCatalogPresets['mastering']>().toEqualTypeOf<readonly string[]>();
    expectTypeOf<CapabilityCatalogPresets['synth']>().toEqualTypeOf<readonly string[]>();
    expectTypeOf<CapabilityCatalogPresets['mixingScene']>().toEqualTypeOf<readonly string[]>();
    expectTypeOf<CapabilityCatalogPresets['voiceChanger']>().toEqualTypeOf<readonly string[]>();
    expectTypeOf<CapabilityCatalogPresets['playbackRoom']>().toEqualTypeOf<readonly string[]>();
    expectTypeOf<CapabilityCatalog['processors']>().toEqualTypeOf<
      readonly MasteringProcessorCatalogEntry[]
    >();
    expectTypeOf<CapabilityCatalog['masteringPresets']>().toEqualTypeOf<
      readonly MasteringPresetCatalogEntry[]
    >();
    expectTypeOf<CapabilityCatalogChain['lists']>().toEqualTypeOf<
      readonly CapabilityCatalogChainList[]
    >();
    expectTypeOf<CapabilityCatalogChain['params']>().toEqualTypeOf<
      readonly MasteringInsertParamInfo[]
    >();
    expectTypeOf<IsReadonly<MasteringInsertParamChoice, 'name'>>().toEqualTypeOf<true>();
    expectTypeOf<IsReadonly<MasteringInsertParamDependency, 'key'>>().toEqualTypeOf<true>();
    expectTypeOf<IsReadonly<MasteringInsertSlot, 'name'>>().toEqualTypeOf<true>();
    expectTypeOf<IsReadonly<MasteringInsertTiming, 'latencySamples'>>().toEqualTypeOf<true>();
    expectTypeOf<IsReadonly<EngineBounceResult, 'channels'>>().toEqualTypeOf<true>();
    expectTypeOf<IsReadonly<ProjectClip, 'id'>>().toEqualTypeOf<true>();
    expectTypeOf<IsReadonly<ProjectDiagnostic, 'code'>>().toEqualTypeOf<true>();
    expectTypeOf<IsReadonly<ProjectMarker, 'id'>>().toEqualTypeOf<true>();
    expectTypeOf<IsReadonly<ProjectSource, 'id'>>().toEqualTypeOf<true>();
    expectTypeOf<IsReadonly<ProjectTempoCandidate, 'bpm'>>().toEqualTypeOf<true>();
    expectTypeOf<IsReadonly<ProjectTrack, 'id'>>().toEqualTypeOf<true>();
    expectTypeOf<IsReadonly<VocalStateToken, 'sessionEpoch'>>().toEqualTypeOf<true>();
    expectTypeOf<ProjectChordSymbol['extensions']>().toEqualTypeOf<
      ReadonlyArray<number> | undefined
    >();
    expectTypeOf<ChordSymbolFields['extensions']>().toEqualTypeOf<ReadonlyArray<number>>();
  });
});
