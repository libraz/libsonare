/* audition — listen to one voice against the thing it is measured against. */

import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { App } from './app/App';
import { boot } from './app/boot';
import { initI18n } from './i18n';
import './styles/base.css';
import './styles/header.css';
import './styles/listen.css';
import './styles/console.css';
import './styles/feedback.css';
import './styles/run.css';
import './styles/bank.css';

initI18n();
const root = document.getElementById('root');
if (root) {
  createRoot(root).render(
    <StrictMode>
      <App />
    </StrictMode>,
  );
}
void boot();
