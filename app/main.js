'use strict';

const { app, BrowserWindow, ipcMain, dialog, shell } = require('electron');
const fs = require('fs');
const fsp = fs.promises;
const path = require('path');
const os = require('os');
const { spawn } = require('child_process');

const pipeline = require('./src/pipeline');
const strings = require('./src/strings');

const SMOKE = process.argv.includes('--smoke');

// ------------------------------------------------------------------ paths ---

// In development the compiler project and the runtime live in app/resources;
// packaged, electron-builder copies them next to the app as extraResources.
function resourceDir(name) {
  const packaged = path.join(process.resourcesPath || '', name);
  if (app.isPackaged && fs.existsSync(packaged)) return packaged;
  return path.join(__dirname, 'resources', name);
}

const defaults = () => ({
  locale: (app.getLocale() || 'en').toLowerCase().startsWith('ar') ? 'ar' : 'en',
  isoPath: '',
  outDir: path.join(app.getPath('documents'), 'Jak1-Switch'),
  workDir: path.join(process.env.LOCALAPPDATA || app.getPath('userData'), 'Jak1Switch', 'work'),
  reuse: true
});

let settings = defaults();
let mainWindow = null;
let state = { running: false, cancel: null, result: null };

function settingsFile() {
  return path.join(app.getPath('userData'), 'settings.json');
}

function loadSettings() {
  try {
    const raw = JSON.parse(fs.readFileSync(settingsFile(), 'utf8'));
    settings = { ...defaults(), ...raw };
  } catch {
    settings = defaults();
  }
  return settings;
}

async function saveSettings(patch) {
  settings = { ...settings, ...patch };
  await fsp.mkdir(path.dirname(settingsFile()), { recursive: true });
  await fsp.writeFile(settingsFile(), JSON.stringify(settings, null, 2), 'utf8');
  return settings;
}

// --------------------------------------------------------------- helpers ----

function send(channel, payload) {
  if (mainWindow && !mainWindow.isDestroyed()) mainWindow.webContents.send(channel, payload);
}

function humanSize(bytes) {
  if (!bytes) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return `${value >= 10 || unit === 0 ? Math.round(value) : value.toFixed(1)} ${units[unit]}`;
}

function listDrives() {
  return new Promise((resolve) => {
    const script =
      'Get-CimInstance Win32_LogicalDisk -Filter "DriveType=2" | ' +
      'Select-Object DeviceID,VolumeName,FreeSpace,Size | ConvertTo-Json -Compress';
    const child = spawn('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', script], {
      windowsHide: true
    });
    let out = '';
    child.stdout.on('data', (d) => (out += d.toString()));
    child.on('error', () => resolve([]));
    child.on('close', () => {
      try {
        const parsed = JSON.parse(out.trim() || '[]');
        const list = Array.isArray(parsed) ? parsed : [parsed];
        resolve(
          list
            .filter((d) => d && d.DeviceID)
            .map((d) => ({
              id: d.DeviceID,
              label: `${d.DeviceID}\\${d.VolumeName ? ` ${d.VolumeName}` : ''}`,
              free: Number(d.FreeSpace || 0),
              size: Number(d.Size || 0)
            }))
        );
      } catch {
        resolve([]);
      }
    });
  });
}

async function appendLog(file, line) {
  try {
    await fsp.appendFile(file, `${line}\n`, 'utf8');
  } catch {
    /* logging must never break a build */
  }
}

// ------------------------------------------------------------------- IPC ----

ipcMain.handle('app:info', async () => ({
  version: app.getVersion(),
  settings: loadSettings(),
  strings,
  resources: {
    proj: resourceDir('proj'),
    runtime: resourceDir('runtime'),
    ready: fs.existsSync(path.join(resourceDir('proj'), 'goal_src')) &&
      fs.existsSync(path.join(resourceDir('runtime'), 'gk.nro'))
  },
  smoke: SMOKE
}));

ipcMain.handle('settings:save', (_e, patch) => saveSettings(patch || {}));

ipcMain.handle('dialog:iso', async () => {
  const res = await dialog.showOpenDialog(mainWindow, {
    title: 'Jak and Daxter ISO',
    properties: ['openFile'],
    filters: [
      { name: 'Disc image', extensions: ['iso'] },
      { name: 'All files', extensions: ['*'] }
    ]
  });
  return res.canceled ? null : res.filePaths[0];
});

ipcMain.handle('dialog:folder', async (_e, current) => {
  const res = await dialog.showOpenDialog(mainWindow, {
    title: 'Folder',
    defaultPath: current || undefined,
    properties: ['openDirectory', 'createDirectory']
  });
  return res.canceled ? null : res.filePaths[0];
});

ipcMain.handle('drives:list', () => listDrives());

ipcMain.handle('space:check', async (_e, target) => {
  const info = await pipeline.freeSpace(target || settings.workDir);
  return info ? { free: info.free, total: info.total } : null;
});

ipcMain.handle('shell:open', async (_e, target) => {
  if (!target) return false;
  const err = await shell.openPath(target);
  return !err;
});

ipcMain.handle('shell:showFile', (_e, target) => {
  if (target && fs.existsSync(target)) shell.showItemInFolder(target);
});

ipcMain.handle('run:cancel', () => {
  if (state.cancel) state.cancel.cancel();
  return true;
});

ipcMain.handle('run:start', async (_e, payload) => {
  if (state.running) return { ok: false, error: 'busy' };
  const cfg = { ...settings, ...(payload || {}) };
  await saveSettings(cfg);

  const logFile = path.join(cfg.workDir, 'install.log');
  await fsp.mkdir(path.dirname(logFile), { recursive: true });
  await fsp.writeFile(logFile, `Jak 1 Switch Installer ${app.getVersion()}\n${new Date().toISOString()}\n`, 'utf8');

  const cancel = { cancelled: false, child: null, cancel() { this.cancelled = true; if (this.child) this.child.kill(); } };
  state.running = true;
  state.cancel = cancel;

  const started = Date.now();
  const emit = (event) => {
    if (event.type === 'log') appendLog(logFile, event.line);
    send('run:event', { ...event, elapsed: Date.now() - started });
  };

  try {
    const result = await pipeline.prepare({
      isoPath: cfg.isoPath,
      workDir: cfg.workDir,
      outDir: cfg.outDir,
      projSource: resourceDir('proj'),
      runtimeSource: resourceDir('runtime'),
      reuse: cfg.reuse !== false,
      cancel,
      onEvent: emit
    });
    state.result = { ...result, outDir: cfg.outDir, logFile };
    send('run:done', {
      install: result.install,
      outDir: result.outDir,
      counts: result.counts.counts,
      bytes: result.bytes,
      size: humanSize(result.bytes),
      logFile
    });
    return { ok: true };
  } catch (err) {
    const code = err.code === 'cancelled' ? 'cancelled' : err.step || 'generic';
    send('run:error', {
      code,
      detail: err.detail || '',
      message: err.message,
      logFile
    });
    return { ok: false, code };
  } finally {
    state.running = false;
    state.cancel = null;
  }
});

ipcMain.handle('copy:start', async (_e, target) => {
  const result = state.result;
  if (!result) return { ok: false, error: 'no-result' };
  const source = path.join(result.outDir, 'switch');
  const dest = path.join(target, 'switch');
  const cancel = { cancelled: false, cancel() { this.cancelled = true; } };
  state.cancel = cancel;
  try {
    const files = await pipeline.listTree(source);
    const totalBytes = files.reduce((sum, f) => sum + f.size, 0);
    let last = 0;
    const copied = await pipeline.copyTree(source, dest, {
      cancel,
      skipIdentical: true,
      onBytes: (done, size) => {
        if (done - last > 6 * 1024 * 1024 || done === totalBytes) {
          last = done;
          send('copy:progress', {
            done,
            total: totalBytes,
            label: `${humanSize(done)} / ${humanSize(totalBytes)}`
          });
        }
      }
    });
    const count = await pipeline.countFiles(path.join(dest, 'jak1'));
    send('copy:done', { count, dest });
    return { ok: true, count, dest };
  } catch (err) {
    send('copy:error', { message: err.message });
    return { ok: false, error: err.message };
  } finally {
    state.cancel = null;
  }
});

ipcMain.on('ui:ready', () => {
  if (SMOKE) {
    process.stdout.write(`SMOKE OK ${app.getVersion()} resources=${resourceDir('runtime')}\n`);
    setTimeout(() => app.quit(), 120);
  }
});

ipcMain.on('ui:render-error', (_e, message) => {
  process.stderr.write(`RENDER ERROR: ${message}\n`);
  if (SMOKE) app.exit(3);
});

// ---------------------------------------------------------------- window ----

function createWindow() {
  const icon = path.join(__dirname, 'build', 'icon.png');
  mainWindow = new BrowserWindow({
    width: 1180,
    height: 840,
    minWidth: 940,
    minHeight: 700,
    backgroundColor: '#0a0d12',
    show: false,
    autoHideMenuBar: true,
    title: 'Jak 1 Switch Installer',
    icon: fs.existsSync(icon) ? icon : undefined,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false
    }
  });

  mainWindow.once('ready-to-show', () => {
    if (!SMOKE) mainWindow.show();
  });
  mainWindow.loadFile(path.join(__dirname, 'renderer', 'index.html'));

  mainWindow.webContents.setWindowOpenHandler(({ url }) => {
    shell.openExternal(url);
    return { action: 'deny' };
  });

  mainWindow.on('closed', () => {
    mainWindow = null;
  });
}

app.whenReady().then(() => {
  loadSettings();
  createWindow();
  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});
