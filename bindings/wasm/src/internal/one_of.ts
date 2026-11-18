/**
 * Refuses unless exactly one of `names` is given, and returns the index of the one that is.
 * None given is a TypeError (`<fn>: give a or b`); more than one is a RangeError with the
 * same text plus `, not both` for two names or `, not more than one` for three or more.
 */
export function requireOneOf(
  fnName: string,
  names: readonly string[],
  given: readonly boolean[],
): number {
  const choices = `${names.slice(0, -1).join(', ')} or ${names[names.length - 1]}`;
  const count = given.filter(Boolean).length;
  if (count === 0) {
    throw new TypeError(`${fnName}: give ${choices}`);
  }
  if (count > 1) {
    const excess = names.length === 2 ? 'not both' : 'not more than one';
    throw new RangeError(`${fnName}: give ${choices}, ${excess}`);
  }
  return given.indexOf(true);
}
