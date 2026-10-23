'use strict';

// Drives the real window over the DevTools protocol and asserts what the user
// would see: screens, the language switch (including RTL), and the empty-input
// guard. No test hooks live in the shipped code.
//
//   node tools/ui-check.js            (or: npm run ui-check)
//   JAK1_APP_EXE="dist/win-unpacked/Jak 1 Switch Installer.exe" node tools/ui-check.js
//                                     (same checks against a packaged build)

const { spawn } = require('child_process');
const path = require('path');

const PORT = 9333;
const ROOT = path.resolve(__dirname, '..');
const electron = require('electron');

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function target() {
  for (let attempt = 0; attempt < 60; attempt += 1) {
    try {
      const res = await fetch(`http://127.0.0.1:${PORT}/json/list`);
      const list = await res.json();
      const page = list.find((t) => t.type === 'page' && t.webSocketDebuggerUrl);
      if (page) return page.webSocketDebuggerUrl;
    } catch {
      /* not up yet */
    }
    await sleep(500);
  }
  throw new Error('the window never exposed a debugging target');
}

function connect(url) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    let id = 0;
    const pending = new Map();
    ws.addEventListener('open', () =>
      resolve({
        evaluate: (expression) =>
          new Promise((res, rej) => {
            const callId = ++id;
            pending.set(callId, { res, rej });
            ws.send(
              JSON.stringify({
                id: callId,
                method: 'Runtime.evaluate',
                params: { expression, awaitPromise: true, returnByValue: true }
              })
            );
          }),
        close: () => ws.close()
      })
    );
    ws.addEventListener('message', (event) => {
      const msg = JSON.parse(event.data);
      if (msg.id && pending.has(msg.id)) {
        const { res, rej } = pending.get(msg.id);
        pending.delete(msg.id);
        if (msg.error) rej(new Error(JSON.stringify(msg.error)));
        else res(msg.result.result.value);
      }
    });
    ws.addEventListener('error', () => reject(new Error('devtools socket failed')));
  });
}

const checks = [
  {
    name: 'four screens exist',
    expr: "document.querySelectorAll('.screen').length === 4"
  },
  {
    name: 'language buttons and drop zone are rendered',
    expr: "!!document.querySelector('.lang button[data-lang=\"ar\"]') && !!document.querySelector('#drop')"
  },
  {
    name: 'defaults: output folder is filled in',
    expr: "document.querySelector('#outDir').value.length > 3"
  },
  {
    name: 'switching to Arabic flips the layout to RTL',
    expr: `(async () => {
      document.querySelector('.lang button[data-lang="ar"]').click();
      await new Promise(r => setTimeout(r, 250));
      return document.documentElement.dir === 'rtl' &&
        /[\\u0600-\\u06FF]/.test(document.querySelector('#screen-choose h2').textContent);
    })()`
  },
  {
    name: 'switching back to English restores LTR',
    expr: `(async () => {
      document.querySelector('.lang button[data-lang="en"]').click();
      await new Promise(r => setTimeout(r, 250));
      return document.documentElement.dir === 'ltr' &&
        document.querySelector('#screen-choose h2').textContent.includes('Choose');
    })()`
  },
  {
    name: 'starting without a disc image is refused with a message',
    expr: `(async () => {
      document.querySelector('#startBtn').click();
      await new Promise(r => setTimeout(r, 300));
      const err = document.querySelector('#chooseError');
      const stillChoosing = document.querySelector('#screen-choose').classList.contains('is-active');
      return err.hidden === false && err.textContent.length > 5 && stillChoosing;
    })()`
  },
  {
    name: 'progress screen starts empty and has a cancel button',
    expr: `(async () => {
      document.querySelector('#screen-run').classList.add('is-active');
      document.querySelector('#screen-choose').classList.remove('is-active');
      const empty = document.querySelector('#barFill').style.width === '0%' ||
        document.querySelector('#barFill').style.width === '';
      const cancel = !!document.querySelector('#cancelBtn');
      document.querySelector('#screen-run').classList.remove('is-active');
      document.querySelector('#screen-choose').classList.add('is-active');
      return empty && cancel;
    })()`
  }
];

async function main() {
  const packaged = process.env.JAK1_APP_EXE;
  const command = packaged || electron;
  const args = packaged ? [`--remote-debugging-port=${PORT}`] : [ROOT, `--remote-debugging-port=${PORT}`];
  const child = spawn(command, args, { cwd: ROOT, stdio: 'ignore' });

  let failures = 0;
  try {
    const socketUrl = await target();
    const page = await connect(socketUrl);
    await sleep(1200);

    for (const check of checks) {
      let ok = false;
      try {
        ok = (await page.evaluate(check.expr)) === true;
      } catch (err) {
        console.log(`FAIL  ${check.name}\n      ${err.message}`);
        failures += 1;
        continue;
      }
      console.log(`${ok ? 'ok  ' : 'FAIL'}  ${check.name}`);
      if (!ok) failures += 1;
    }
    page.close();
  } finally {
    child.kill();
  }

  console.log(failures ? `${failures} check(s) failed` : 'all UI checks passed');
  process.exit(failures ? 1 : 0);
}

main().catch((err) => {
  console.error(err.message);
  process.exit(1);
});
