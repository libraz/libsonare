import { describe, expect, expectTypeOf, it } from 'vitest';
import type {
  Capabilities,
  CapabilityCatalog,
  MasteringInsertParamInfo,
  MasteringPresetCatalogEntry,
  MasteringProcessorCatalogEntry,
  MasteringProcessorCategory,
  Matrix2D,
} from '../../dist/index.js';

describe('core public type names', () => {
  it('exports Matrix2D and Capabilities', () => {
    expectTypeOf<Matrix2D>().toEqualTypeOf<{ data: Float32Array; rows: number; cols: number }>();
    expectTypeOf<Capabilities['features']['ffmpeg']>().toEqualTypeOf<boolean>();
    expect(true).toBe(true);
  });

  it('shares one catalog type family between capabilityCatalog and the mastering catalog', () => {
    expectTypeOf<
      CapabilityCatalog['processors'][number]
    >().toEqualTypeOf<MasteringProcessorCatalogEntry>();
    expectTypeOf<
      CapabilityCatalog['chain']['params'][number]
    >().toEqualTypeOf<MasteringInsertParamInfo>();
    expectTypeOf<
      CapabilityCatalog['masteringPresets'][number]
    >().toEqualTypeOf<MasteringPresetCatalogEntry>();
    expectTypeOf<
      MasteringProcessorCatalogEntry['category']
    >().toEqualTypeOf<MasteringProcessorCategory>();
    const entry = {} as MasteringProcessorCatalogEntry;
    // @ts-expect-error catalog entries are readonly
    entry.id = 'x';
    expect(true).toBe(true);
  });

  it('no longer exports the twin or internal type names', () => {
    // @ts-expect-error removed
    type B = import('../../dist/index.js').Matrix2dResult;
    // @ts-expect-error removed
    type C = import('../../dist/index.js').SonareModule;
    // @ts-expect-error removed
    type D = import('../../dist/index.js').SonareCapabilities;
    // @ts-expect-error removed
    type E = import('../../dist/index.js').CapabilityCatalogProcessor;
    // @ts-expect-error removed
    type F = import('../../dist/index.js').CapabilityCatalogParameter;
    // @ts-expect-error removed
    type G = import('../../dist/index.js').CapabilityCatalogMasteringPreset;
    expectTypeOf<[B, C, D, E, F, G]>().not.toBeNever();
    expect(true).toBe(true);
  });
});
