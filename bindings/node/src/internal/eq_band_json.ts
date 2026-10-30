/** Serializes an EQ band to the JSON the native strip setters take; a string passes through. */
export function eqBandJson(band: object | string): string {
  return typeof band === 'string' ? band : JSON.stringify(band);
}
