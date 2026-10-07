/**
 * Installs `Symbol.dispose` where the host lacks it (older browsers and Node),
 * so handle classes can declare `[Symbol.dispose]()` and work with `using`
 * once the host supports it. Import for side effect, before any class that
 * uses the key is evaluated.
 */
const symbolCtor = Symbol as { dispose?: symbol };
symbolCtor.dispose ??= Symbol.for('Symbol.dispose');

export {};
