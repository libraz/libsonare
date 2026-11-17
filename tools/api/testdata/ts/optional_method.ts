/** Fixture for ts_exports.cjs: an interface whose methods are optional or not. */
export interface OptionalMethodHost {
  required(group: number, offsetInBlockSamples?: number): void;
  optional?(group: number, offsetInBlockSamples?: number): void;
}
