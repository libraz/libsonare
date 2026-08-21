/**
 * Compile-only: `SonareError` stayed source-compatible when it became a class.
 *
 * It shipped as a type-only `interface` and is now a runtime class, so that the
 * Node and WASM packages export the same *kind* of thing under one name. A
 * class type is structural in TypeScript only while it has no `private` or
 * `protected` members; adding one would make it nominal and would silently
 * reject every value in this file — all of which an existing consumer could
 * have written against the interface. Type-checked by `tsconfig.test.json`;
 * vitest does not collect it, because it asserts nothing at runtime.
 */

import { isSonareError, type SonareError } from '../../src/index.js';

// An object literal assigned to the type. This is the case a nominal class
// rejects, so it is the load-bearing one.
const fromLiteral: SonareError = {
  name: 'SonareError',
  message: 'boom',
  code: 4,
  codeName: 'InvalidParameter',
};

// The shape the addon actually raises: a plain Error with the fields attached.
const fromNativeThrow: SonareError = Object.assign(new Error('boom'), {
  name: 'SonareError' as const,
  code: 4,
  codeName: 'InvalidParameter',
});

// Used as a parameter annotation.
function describeError(error: SonareError): string {
  return `${error.codeName}(${error.code}): ${error.message}`;
}

// Extended by a consumer interface.
interface AppError extends SonareError {
  retryable: boolean;
}
const extended: AppError = { ...fromLiteral, retryable: true };

// Declared as implemented by a consumer class.
class ConsumerError extends Error implements SonareError {
  override readonly name = 'SonareError' as const;
  readonly code = 7;
  readonly codeName = 'InvalidState';
}

// Narrowed out of `unknown` by the shipped guard.
function handle(caught: unknown): string {
  return isSonareError(caught) ? describeError(caught) : 'not ours';
}

export const errorCompatProbe: string[] = [
  describeError(fromLiteral),
  describeError(fromNativeThrow),
  describeError(extended),
  describeError(new ConsumerError()),
  handle(fromLiteral),
];
