/**
 * Installs `Symbol.dispose` and `Symbol.asyncDispose` where the host lacks them
 * (older browsers and Node), so classes can declare `[Symbol.dispose]()` /
 * `[Symbol.asyncDispose]()` and work with `using` / `await using` once the host
 * supports them. Import for side effect, before any class that uses the keys
 * is evaluated.
 */
const symbolCtor = Symbol as { dispose?: symbol; asyncDispose?: symbol };
symbolCtor.dispose ??= Symbol.for('Symbol.dispose');
symbolCtor.asyncDispose ??= Symbol.for('Symbol.asyncDispose');

export {};
