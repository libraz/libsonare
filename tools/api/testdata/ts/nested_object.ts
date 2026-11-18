/** Fixture for ts_exports.cjs: properties whose types are spelled inline. */
export interface NestedHost {
  loudness?: {
    /** Ceiling in dBTP. */
    ceilingDb: number;
    inner: { depth: number };
  };
  bands: Array<{ gainDb: number }>;
  named: NamedPart;
  mixed?: { a: number } | { a: string; b: boolean };
}

/** A named interface: its members are recorded under its own name, not under the property. */
export interface NamedPart {
  x: number;
}
