import '@testing-library/jest-dom/vitest';
import { cleanup } from '@testing-library/react';
import { afterEach, beforeEach } from 'vitest';
import { initI18n, setLang } from '../src/i18n';
import { resetStore } from '../src/state/store';

initI18n('en');

beforeEach(() => {
  localStorage.clear();
  setLang('en');
  resetStore();
});

afterEach(() => {
  cleanup();
});
