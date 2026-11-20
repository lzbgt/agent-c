// Run from repository root. Uses the existing UI Playwright installation.
// Live mode is strictly read-only: every POST is blocked in the browser.
import { createRequire } from 'node:module';
import { mkdir, writeFile } from 'node:fs/promises';
import assert from 'node:assert/strict';
const require = createRequire(new URL('../../ui/package.json', import.meta.url));
const { chromium } = require('@playwright/test');
const origin = process.env.SUPPORT_TEST_ORIGIN || 'http://127.0.0.1:21961';
const live = process.env.SUPPORT_LIVE === '1';
if (!live && !origin.startsWith('http://127.0.0.1:')) throw new Error('Payment fixtures must use loopback');
const directory = `build/support-browser-${live ? 'live' : 'fixture'}`;
await mkdir(directory, { recursive: true });
const browser = await chromium.launch({ headless: true, executablePath: process.env.CHROMIUM_PATH || chromium.executablePath() });
const evidence = [];
try {
  for (const width of [1440, 768, 390, 320]) {
    const context = await browser.newContext({ viewport: { width, height: 980 }, reducedMotion: 'reduce' });
    const page = await context.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    const submissions = [];
    await context.route('**/*', async route => {
      if (live && route.request().method() !== 'GET' && route.request().method() !== 'HEAD') {
        submissions.push(route.request().url());
        return route.abort();
      }
      return route.continue();
    });
    const response = await page.goto(origin + '/projects/agent-c');
    assert.equal(response.status(), 200);
    await page.getByRole('heading', { name: 'Support Agent-C', exact: true }).waitFor();
    assert.equal(await page.locator('#amount').inputValue(), '15.00');
    await page.getByRole('button', { name: '$30', exact: true }).click();
    assert.equal(await page.locator('#amount').inputValue(), '30.00');
    await page.getByLabel('Amount USD').fill('12.50');
    assert.equal(await page.locator('[data-amount][aria-pressed=true]').count(), 0);
    const dimensions = await page.evaluate(() => ({ width: innerWidth, scroll: document.documentElement.scrollWidth }));
    assert.ok(dimensions.scroll <= dimensions.width, `Horizontal overflow at ${width}`);
    await page.screenshot({ path: `${directory}/${width}.png`, fullPage: true });
    assert.deepEqual(errors, []);
    assert.deepEqual(submissions, []);
    evidence.push({ viewport: width, page_status: response.status(), horizontal_overflow: false, script_errors: 0, live_posts: submissions.length });
    if (!live && width === 1440) {
      let localOrder;
      page.on('response', response => {
        if (response.request().method() === 'POST' && response.url().endsWith('/checkout')) {
          localOrder = response.headers()['location']?.match(/\/orders\/([a-f0-9]{32})/)?.[1];
        }
      });
      // Emulate only the provider approval page; the loopback fixture validates
      // the production return, POST capture, GET evidence and receipt paths.
      await context.route('https://www.sandbox.paypal.com/checkoutnow?*', async route => {
        const token = new URL(route.request().url()).searchParams.get('token');
        assert.ok(localOrder);
        await route.fulfill({ contentType: 'text/html', body: `<a href="${origin}/return?order=${localOrder}&token=${token}">Approve fixture payment</a>` });
      });
      await page.getByRole('button', { name: 'Continue with PayPal' }).click();
      await page.getByRole('link', { name: 'Approve fixture payment' }).click();
      await page.getByRole('heading', { name: 'Thank you for your support.' }).waitFor();
      assert.equal(await page.locator('details[open]').count(), 0);
      assert.equal(await page.locator('.receipt-amount').innerText(), '$12.50\nUSD · One time');
      await page.screenshot({ path: `${directory}/receipt.png`, fullPage: true });
      evidence.push({ fixture_checkout: 'passed', confirmation: 'server-validated capture', metadata_collapsed: true });
    }
    await context.close();
  }
} finally {
  await browser.close();
}
await writeFile(`${directory}/receipt.json`, JSON.stringify({ origin, live, evidence }, null, 2) + '\n');
console.log(JSON.stringify({ origin, live, evidence }));
