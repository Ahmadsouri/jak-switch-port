'use strict';

// Renders build/icon.png with the same emblem the window header uses, so the
// taskbar icon and the app never drift apart.
//
//   npm run icon

const { app, BrowserWindow } = require('electron');
const fs = require('fs');
const path = require('path');

const SIZE = 512;

const html = `<!doctype html><html><head><meta charset="utf-8"><style>
  html, body { margin: 0; width: 100vw; height: 100vh; overflow: hidden; background: transparent; }
  .plate {
    width: 100vw; height: 100vh; border-radius: 110px;
    background: radial-gradient(120% 120% at 20% 0%, #1b2534, #0a0e14 70%);
    display: grid; place-items: center;
    box-shadow: inset 0 0 0 6px rgba(255,255,255,0.06);
  }
  svg { width: 320px; height: 320px; }
</style></head><body>
<div class="plate">
  <svg viewBox="0 0 48 48" fill="none">
    <circle cx="24" cy="24" r="20" stroke="url(#a)" stroke-width="2.4"/>
    <circle cx="24" cy="24" r="11" stroke="url(#a)" stroke-width="1.4" opacity=".5"/>
    <path d="M24 10c7 5 7 23 0 28-7-5-7-23 0-28Z" stroke="url(#b)" stroke-width="2.2"/>
    <defs>
      <linearGradient id="a" x1="4" y1="6" x2="44" y2="44">
        <stop stop-color="#f7be6a"/><stop offset="1" stop-color="#d9791f"/>
      </linearGradient>
      <linearGradient id="b" x1="16" y1="8" x2="32" y2="40">
        <stop stop-color="#8ff4c8"/><stop offset="1" stop-color="#2bb087"/>
      </linearGradient>
    </defs>
  </svg>
</div>
</body></html>`;

app.disableHardwareAcceleration();

app.whenReady().then(async () => {
  const win = new BrowserWindow({
    width: SIZE,
    height: SIZE,
    useContentSize: true,
    show: false,
    transparent: true,
    frame: false,
    resizable: false,
    webPreferences: { offscreen: true }
  });
  win.setContentSize(SIZE, SIZE);
  await win.loadURL(`data:text/html;charset=utf-8,${encodeURIComponent(html)}`);
  await new Promise((resolve) => setTimeout(resolve, 900));
  const image = await win.webContents.capturePage({ x: 0, y: 0, width: SIZE, height: SIZE });
  const out = path.join(__dirname, '..', 'build', 'icon.png');
  fs.mkdirSync(path.dirname(out), { recursive: true });
  fs.writeFileSync(out, image.toPNG());
  console.log(`wrote ${out} (${image.getSize().width}x${image.getSize().height})`);
  app.quit();
});
