/* The bank's data and the choices made over it.
 *
 * Kept apart from the app store: the listening surface never writes here, and the bank's query
 * survives leaving the view and coming back. The cursor is a slug, not a row number, so a filter
 * change that leaves the voice on screen does not move the inspector to whatever slid into place.
 */

import { useStore } from 'zustand';
import { createStore } from 'zustand/vanilla';
import { getJSON } from '../api/client';
import { currentLang } from '../i18n';
import { getState as appState } from '../state/store';
import { type BankQuery, cursorIndex, emptyQuery, pageContext, shownVoices } from './logic';
import { BankFile, type BankVoiceRow } from './schema';

export interface BankState {
  voices: BankVoiceRow[];
  loaded: boolean;
  query: BankQuery;
  cursor: string | null;
}

const initial = (): BankState => ({ voices: [], loaded: false, query: emptyQuery(), cursor: null });

export const bankStore = createStore<BankState>()(initial);

export const getBank = bankStore.getState;

export function useBank<T>(select: (s: BankState) => T): T {
  return useStore(bankStore, select);
}

/** Put the bank back to its first state; for tests. */
export function resetBank(over: Partial<BankState> = {}): void {
  bankStore.setState({ ...initial(), ...over }, true);
}

export function setQuery(patch: Partial<BankQuery>): void {
  bankStore.setState((s) => ({ query: { ...s.query, ...patch } }));
}

export function setCursor(cursor: string | null): void {
  bankStore.setState({ cursor });
}

/** Read `bank.json` once; a missing or malformed file leaves the bank empty, which the view says. */
export async function loadBank(): Promise<void> {
  if (getBank().loaded) {
    return;
  }
  let voices: BankVoiceRow[] = [];
  try {
    voices = (await getJSON('bank.json', BankFile)).voices;
  } catch {
    voices = [];
  }
  bankStore.setState({ voices, loaded: true });
}

/** The rows the current query shows, from the stores' present state. */
export function currentShown(): BankVoiceRow[] {
  const { voices, query } = getBank();
  return shownVoices(voices, query, pageContext(appState().sets), currentLang());
}

/** The row the cursor is on, or null with nothing shown. */
export function currentRow(): BankVoiceRow | null {
  const shown = currentShown();
  return shown[cursorIndex(shown, getBank().cursor)] ?? null;
}
