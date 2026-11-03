/* The built page against a real serve.py: what a listener does, end to end. */

import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { expect, type Page, test } from '@playwright/test';
import en from '../src/i18n/en.json' with { type: 'json' };
import ja from '../src/i18n/ja.json' with { type: 'json' };
import { FEEDBACK } from './env';

test.use({ launchOptions: { args: ['--autoplay-policy=no-user-gesture-required'] } });

const log = (set: string): Record<string, unknown>[] => {
  try {
    return readFileSync(join(FEEDBACK, `${set}.jsonl`), 'utf8')
      .split('\n')
      .filter(Boolean)
      .map((l) => JSON.parse(l) as Record<string, unknown>);
  } catch {
    return [];
  }
};

async function fresh(page: Page, hash = ''): Promise<void> {
  await page.goto(`/${hash}`);
  await page.evaluate(() => {
    localStorage.clear();
    sessionStorage.clear();
    localStorage.setItem('audition:lang', 'en');
  });
  await page.goto(`/${hash}`);
  await expect(page.locator('#versions .segmented button').first()).toBeVisible();
}

test.beforeEach(({ page }) => {
  page.on('pageerror', (err) => {
    throw err;
  });
});

test('opens the first set on its first take and names it in the address', async ({ page }) => {
  await fresh(page);
  await expect(page.locator('#title')).toHaveText('Test Grand');
  await expect(page.locator('nav#takes button')).toHaveCount(2);
  await expect(page).toHaveURL(/#e2e-grand\/music\/model\?c=gm_gs_product$/);
  await expect(page.locator('#identPath')).toHaveText('s/e2e-grand/music/model.wav');
});

test('switches sides with tab, plays, and follows the take list', async ({ page }) => {
  await fresh(page);
  await page.keyboard.press('Tab');
  await expect(page).toHaveURL(/\/music\/ref\?/);
  await expect(page.locator('#playBtn')).toHaveAttribute('aria-pressed', 'true');
  await expect
    .poll(async () => Number(await page.locator('#clockAt').textContent()))
    .toBeGreaterThan(0);
  await page.keyboard.press('ArrowDown');
  await expect(page).toHaveURL(/\/single-c4\/ref\?/);
  await expect(page.locator('nav#takes button[aria-current="true"] .take-name')).toHaveText('C4');
});

test('opens exactly the render a link names', async ({ page }) => {
  await fresh(page, '#e2e-grand/single-c4/ref?c=gm_gs_product');
  await expect(page.locator('#identPath')).toHaveText('s/e2e-grand/c4/ref.wav');
  await page.evaluate(() => {
    location.hash = '#e2e-organ/music/model';
  });
  await expect(page.locator('#title')).toHaveText('Test Organ');
  await expect(page.locator('body')).toHaveClass(/no-compare/);
});

test('speaks Japanese after a switch and keeps it across a reload', async ({ page }) => {
  await fresh(page);
  await page.locator('#langPick').selectOption('ja');
  await expect(page.locator('html')).toHaveAttribute('lang', 'ja');
  await expect(page.locator('#playBtn .tlabel')).toHaveText(ja['transport.play']);
  await page.reload();
  await expect(page.locator('#playBtn .tlabel')).toHaveText(ja['transport.play']);
  await expect(page.locator('body')).not.toContainText(/\b(transport|fb|ver|role)\.[a-z]+/);
});

test('writes a note to the log with what was sounding, and undoes it', async ({ page }) => {
  await fresh(page);
  const before = log('e2e-grand').length;
  await page.locator('.fb-choices button', { hasText: en['tree.off.ok'] }).click();
  await page.locator('#fbComment').fill('clean and even');
  await page.locator('#fbSend').click();
  await expect.poll(() => log('e2e-grand').length).toBe(before + 1);
  const note = log('e2e-grand').at(-1) ?? {};
  expect(note).toMatchObject({
    grade: 'ok',
    tag: 'ok',
    text: 'clean and even',
    lang: 'en',
    schema_version: 2,
  });
  expect(note.conditions).toMatchObject({
    set: 'e2e-grand',
    take: 'music',
    version: 'model',
    blind: false,
  });
  expect(note.evaluation).toMatchObject({ comparison_id: 'gm_gs_product', judged_source: 'model' });
  expect(note.evidence).toMatchObject({ set_generation: 'e'.repeat(64), take: 'music' });
  await expect(page.locator('#fbRecent .fb-entry').first()).toContainText('clean and even');
  await page.locator('#fbUndo').click();
  await expect.poll(() => log('e2e-grand').length).toBe(before);
});

test('walks the narrowing to a finer answer and records the trail', async ({ page }) => {
  await fresh(page);
  const choose = (key: keyof typeof en) =>
    page.locator('.fb-choices button', { hasText: en[key] }).click();
  await choose('tree.off.off');
  await choose('tree.grade.acceptable');
  await choose('tree.when.body');
  await choose('tree.body.tone');
  await choose('tree.tone.tone/dark');
  await expect(page.locator('#fbSend')).toHaveClass(/ready/);
  await page.locator('#fbSend').click();
  await expect.poll(() => log('e2e-grand').at(-1)?.tag).toBe('tone/dark');
  const note = log('e2e-grand').at(-1) ?? {};
  expect(note.grade).toBe('acceptable');
  expect((note.answers as { tag: string }[]).map((a) => a.tag)).toEqual([
    'off',
    'acceptable',
    'body',
    'tone',
    'tone/dark',
  ]);
});

test('shows the bank and comes back to listening', async ({ page }) => {
  await fresh(page);
  await page.locator('#viewToggle button[data-view="bank"]').click();
  await expect(page.locator('#bank')).toBeVisible();
  await expect(page.locator('body')).toHaveClass(/bank-view/);
  await page.locator('#viewToggle button[data-view="listen"]').click();
  await expect(page.locator('#bank')).toHaveCount(0);
  await expect(page.locator('#title')).toHaveText('Test Grand');
});

test('finds a voice in the palette by typing', async ({ page }) => {
  await fresh(page);
  await page.keyboard.press('v');
  await expect(page.locator('#voicePanel')).toBeVisible();
  await page.locator('#voiceFind').fill('organ');
  await page.keyboard.press('Enter');
  await expect(page.locator('#title')).toHaveText('Test Organ');
});

test('runs the queue: library side first, digits answer, enter sends and moves on', async ({
  page,
}) => {
  const queue = [
    {
      set: 'e2e-grand',
      name: 'Test Grand',
      program: 0,
      bank: 0,
      kit: false,
      tier: 'core',
      group: 'acoustic piano',
    },
    {
      set: 'e2e-organ',
      name: 'Test Organ',
      program: 19,
      bank: 0,
      kit: false,
      tier: 'core',
      group: 'pipe organ',
    },
  ].map((s) => ({ ...s, page: 'ready', ships: 'current', noted: 0, log: '', error: '' }));
  // The real queue names the real bank; the run is pointed at the synthetic sets instead.
  await page.route('**/queue.json', (r) => r.fulfill({ json: queue }));
  await page.route('**/render', (r) => r.fulfill({ json: queue }));
  await fresh(page);
  const before = log('e2e-grand').length;
  await page.locator('#viewToggle button[data-view="run"]').click();
  await expect(page.locator('#runName')).toHaveText('Test Grand');
  await expect(page.locator('#runCount')).toHaveText('1 / 2');
  await expect(page.locator('.run-side[aria-pressed="true"]')).toHaveClass(/role-model/);
  await expect(page.locator('#loopBtn')).toHaveAttribute('aria-pressed', 'true');
  await page.keyboard.press('Tab');
  await expect(page.locator('.run-side[aria-pressed="true"]')).toHaveClass(/role-reference/);
  await page.keyboard.press('2');
  await expect(page.locator('#fbSend')).toHaveText(en['run.keySend']);
  await page.keyboard.press('Enter');
  await expect(page.locator('#runName')).toHaveText('Test Organ');
  await expect.poll(() => log('e2e-grand').length).toBe(before + 1);
  expect(log('e2e-grand').at(-1)).toMatchObject({ grade: 'ok' });
  await page.keyboard.press('n');
  await expect(page.locator('#runName')).toHaveText(en['run.doneTitle']);
  await page.locator('#viewToggle button[data-view="listen"]').click();
  await expect(page.locator('#fbSend')).toHaveText(en['fb.send']);
});

test('the scratch root is the run’s own', () => {
  expect(readdirSync(FEEDBACK).every((f) => f.startsWith('e2e-'))).toBe(true);
});
