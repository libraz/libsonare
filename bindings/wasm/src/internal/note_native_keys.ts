// Native spells the note edit's transpose fields `pitchShiftSemitones` / `formantShiftSemitones`.

const EDIT_TO_NATIVE: Readonly<Record<string, string>> = {
  pitchSemitones: 'pitchShiftSemitones',
  formantSemitones: 'formantShiftSemitones',
};

function renameKeys(value: unknown, table: Readonly<Record<string, string>>): unknown {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    return value;
  }
  const out: Record<string, unknown> = {};
  for (const [key, entry] of Object.entries(value)) {
    out[table[key] ?? key] = entry;
  }
  return out;
}

const EDIT_FROM_NATIVE: Readonly<Record<string, string>> = Object.fromEntries(
  Object.entries(EDIT_TO_NATIVE).map(([pub, native]) => [native, pub]),
);

/** A note edit with the keys native reads; anything that is not a plain object passes through. */
export function noteEditToNative<T>(edit: T): T {
  return renameKeys(edit, EDIT_TO_NATIVE) as T;
}

/** A note with its `edit` carrying the keys native reads. */
export function noteToNative<T>(note: T): T {
  if (note === null || typeof note !== 'object' || !('edit' in note)) {
    return note;
  }
  return { ...note, edit: noteEditToNative((note as { edit: unknown }).edit) };
}

/** A note as native reports it, with its `edit` under the public key names. */
export function noteFromNative<T>(note: T): T {
  if (note === null || typeof note !== 'object' || !('edit' in note)) {
    return note;
  }
  return { ...note, edit: renameKeys((note as { edit: unknown }).edit, EDIT_FROM_NATIVE) as never };
}

/** Maps every entry of a note list; a non-array passes through. */
export function notesToNative<T>(notes: T): T {
  return (Array.isArray(notes) ? notes.map(noteToNative) : notes) as T;
}

/** Maps every entry of a note list reported by native; a non-array passes through. */
export function notesFromNative<T>(notes: T): T {
  return (Array.isArray(notes) ? notes.map(noteFromNative) : notes) as T;
}
