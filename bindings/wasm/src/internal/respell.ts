/**
 * The facade half of a converted argument's refusal: the native translation point attaches the
 * refusal's parts, and this re-renders them under the caller's name through the core renderer.
 */

import { getSonareModule } from '../module_state.js';
import {
  type RefusalRecord,
  type Respelled,
  type Respelling,
  refusalOf,
  retextRefusal,
} from './entry.js';

interface Rendered {
  message: string;
  refusal: RefusalRecord;
}

const RENDER = (refusal: RefusalRecord, subjects: string[], scales: number[]): Rendered =>
  (
    getSonareModule() as unknown as {
      renderRefusal: (r: RefusalRecord, s: string[], k: number[]) => Rendered;
    }
  ).renderRefusal(refusal, subjects, scales);

/**
 * Re-renders a core refusal under the name and unit the caller wrote, for an argument the facade
 * converted before the native call (seconds given in place of samples).
 */
export function respellRefusal(error: unknown, respelled: Respelled): unknown {
  const refusal = refusalOf(error);
  if (refusal === undefined || !(error instanceof Error)) {
    return error;
  }
  const { subjects } = refusal;
  const own = (subject: string): Respelling | undefined =>
    // biome-ignore lint/suspicious/noPrototypeBuiltins: Object.hasOwn is newer than the ES2020 target.
    Object.prototype.hasOwnProperty.call(respelled, subject) ? respelled[subject] : undefined;
  if (!subjects.some((subject) => own(subject) !== undefined)) {
    return error;
  }
  const convertedSubjects =
    refusal.raw === undefined ? undefined : subjects.map((subject) => own(subject)?.name ?? null);
  const rendered = RENDER(
    refusal,
    subjects.map((subject) => own(subject)?.name ?? subject),
    subjects.map((subject) => own(subject)?.scale ?? 1),
  );
  retextRefusal(error, rendered.refusal, rendered.message, convertedSubjects);
  return error;
}

/** Runs a native call whose converted arguments @p respelled declares; a promise is followed too. */
export function withRespelled<T>(respelled: Respelled, call: () => T): T {
  try {
    const result = call();
    if (result instanceof Promise) {
      return result.catch((error: unknown) => {
        throw respellRefusal(error, respelled);
      }) as T;
    }
    return result;
  } catch (error) {
    throw respellRefusal(error, respelled);
  }
}
