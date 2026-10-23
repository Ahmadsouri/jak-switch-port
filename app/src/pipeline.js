'use strict';

// The whole install pipeline, with no Electron imports, so it can be exercised
// by plain `node --test` against a real extracted disc.

const fs = require('fs');
const fsp = fs.promises;
const path = require('path');
const https = require('https');
const { spawn } = require('child_process');

const EXTRACTOR_VERSION = 'v0.3.8';
const EXTRACTOR_URL =
  `https://github.com/open-goal/jak-project/releases/download/${EXTRACTOR_VERSION}` +
  `/opengoal-windows-${EXTRACTOR_VERSION}.zip`;

// A complete jak1 build. Anything well under this means a step died silently.
const EXPECTED = { obj: 1000, iso: 300, fr3: 20 };
const COMPLETE = { obj: 1039, iso: 322, fr3: 26 };

// Rough space requirements, in bytes.
const SPACE_WORK = 5.0 * 1024 ** 3;
const SPACE_OUT = 2.4 * 1024 ** 3;

const PHASES = [
  { id: 'tools', weight: 5 },
  { id: 'project', weight: 10 },
  { id: 'extract', weight: 30 },
  { id: 'decompile', weight: 12 },
  { id: 'compile', weight: 38 },
  { id: 'assemble', weight: 5 }
];
const TOTAL_WEIGHT = PHASES.reduce((sum, p) => sum + p.weight, 0);

class Cancelled extends Error {
  constructor() {
    super('cancelled');
    this.code = 'cancelled';
  }
}

class StepError extends Error {
  constructor(step, detail) {
    super(`${step}: ${detail}`);
    this.step = step;
    this.detail = detail;
  }
}

// ------------------------------------------------------------------ basics ---

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function countFiles(dir) {
  let total = 0;
  let entries;
  try {
    entries = await fsp.readdir(dir, { withFileTypes: true });
  } catch {
    return 0;
  }
  for (const entry of entries) {
    if (entry.isDirectory()) total += await countFiles(path.join(dir, entry.name));
    else if (entry.isFile()) total += 1;
  }
  return total;
}

async function dirBytes(dir) {
  let total = 0;
  let entries;
  try {
    entries = await fsp.readdir(dir, { withFileTypes: true });
  } catch {
    return 0;
  }
  for (const entry of entries) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) total += await dirBytes(full);
    else if (entry.isFile()) {
      try {
        total += (await fsp.stat(full)).size;
      } catch {
        /* vanished mid-walk */
      }
    }
  }
  return total;
}

async function listTree(dir, base = dir, out = []) {
  let entries;
  try {
    entries = await fsp.readdir(dir, { withFileTypes: true });
  } catch {
    return out;
  }
  for (const entry of entries) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) await listTree(full, base, out);
    else if (entry.isFile()) {
      let size = 0;
      try {
        size = (await fsp.stat(full)).size;
      } catch {
        /* ignore */
      }
      out.push({ from: full, rel: path.relative(base, full), size });
    }
  }
  return out;
}

async function freeSpace(target) {
  let probe = path.resolve(target);
  // statfs needs a path that exists; walk up to the nearest ancestor.
  while (probe && probe !== path.dirname(probe)) {
    try {
      const s = await fsp.statfs(probe);
      return { free: s.bavail * s.bsize, total: s.blocks * s.bsize };
    } catch {
      probe = path.dirname(probe);
    }
  }
  return null;
}

// Copies a tree, reporting bytes written. Returns the number of files copied.
async function copyTree(source, target, { onBytes, cancel, skipIdentical = false } = {}) {
  const files = await listTree(source);
  let done = 0;
  let copied = 0;
  for (const file of files) {
    if (cancel && cancel.cancelled) throw new Cancelled();
    const dest = path.join(target, file.rel);
    await fsp.mkdir(path.dirname(dest), { recursive: true });
    let skip = false;
    if (skipIdentical) {
      try {
        const st = await fsp.stat(dest);
        skip = st.size === file.size;
      } catch {
        /* not there yet */
      }
    }
    if (!skip) {
      await fsp.copyFile(file.from, dest);
      copied += 1;
    }
    done += file.size;
    if (onBytes) onBytes(done, file.size);
  }
  return copied;
}

async function removeTree(target) {
  await fsp.rm(target, { recursive: true, force: true, maxRetries: 3 });
}

// ---------------------------------------------------------------- download ---

function downloadFile(url, dest, { onProgress, cancel, redirects = 0 } = {}) {
  return new Promise((resolve, reject) => {
    if (redirects > 6) return reject(new Error('too many redirects'));
    const request = https.get(url, { headers: { 'User-Agent': 'jak1-switch-installer' } }, (res) => {
      const status = res.statusCode || 0;
      if (status >= 300 && status < 400 && res.headers.location) {
        res.resume();
        const next = new URL(res.headers.location, url).toString();
        return resolve(downloadFile(next, dest, { onProgress, cancel, redirects: redirects + 1 }));
      }
      if (status !== 200) {
        res.resume();
        return reject(new Error(`HTTP ${status} for ${url}`));
      }
      const total = Number(res.headers['content-length'] || 0);
      let received = 0;
      const file = fs.createWriteStream(dest);
      res.on('data', (chunk) => {
        if (cancel && cancel.cancelled) {
          res.destroy();
          file.destroy();
          return reject(new Cancelled());
        }
        received += chunk.length;
        if (onProgress) onProgress(received, total);
      });
      res.pipe(file);
      file.on('finish', () => file.close(() => resolve({ bytes: received, total })));
      file.on('error', reject);
      res.on('error', reject);
    });
    request.on('error', reject);
    request.setTimeout(120000, () => request.destroy(new Error('download timed out')));
  });
}

// --------------------------------------------------------------- processes ---

function runProcess(exe, args, { cwd, onLog, cancel } = {}) {
  return new Promise((resolve, reject) => {
    if (cancel && cancel.cancelled) return reject(new Cancelled());
    const child = spawn(exe, args, { cwd, windowsHide: true });
    if (cancel) cancel.child = child;
    let tail = '';
    const emit = (chunk) => {
      const text = chunk.toString('utf8');
      tail = (tail + text).slice(-4000);
      if (onLog) text.split(/\r?\n/).filter(Boolean).forEach((line) => onLog(line));
    };
    child.stdout.on('data', emit);
    child.stderr.on('data', emit);
    child.on('error', (err) => reject(err));
    child.on('close', (code) => {
      if (cancel) cancel.child = null;
      if (cancel && cancel.cancelled) return reject(new Cancelled());
      resolve({ code, tail });
    });
  });
}

function expandZip(zipPath, destDir) {
  const script =
    `Expand-Archive -LiteralPath '${zipPath.replace(/'/g, "''")}' ` +
    `-DestinationPath '${destDir.replace(/'/g, "''")}' -Force`;
  return runProcess('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', script]);
}

// ------------------------------------------------------------------ stages ---

function stageRunner(onEvent) {
  let completed = 0;
  let current = null;

  function report(fraction) {
    if (!current) return;
    const clamped = Math.max(0, Math.min(1, fraction || 0));
    const overall = (completed + current.weight * clamped) / TOTAL_WEIGHT;
    onEvent({ type: 'progress', phase: current.id, fraction: clamped, overall });
  }

  return {
    begin(id) {
      current = PHASES.find((p) => p.id === id) || { id, weight: 0 };
      onEvent({ type: 'phase', id, state: 'run' });
      report(0);
    },
    skip(id) {
      const phase = PHASES.find((p) => p.id === id);
      if (phase) completed += phase.weight;
      current = null;
      onEvent({ type: 'phase', id, state: 'skip' });
      onEvent({ type: 'progress', phase: id, fraction: 1, overall: completed / TOTAL_WEIGHT });
    },
    end(id) {
      const phase = PHASES.find((p) => p.id === id);
      if (phase) completed += phase.weight;
      current = null;
      onEvent({ type: 'phase', id, state: 'done' });
      onEvent({ type: 'progress', phase: id, fraction: 1, overall: completed / TOTAL_WEIGHT });
    },
    report
  };
}

async function sampleLoop(dirPath, expected, onFraction, cancel, ms = 900) {
  const timer = setInterval(async () => {
    if (cancel && cancel.cancelled) return;
    const n = await countFiles(dirPath);
    onFraction(Math.min(0.97, n / expected));
  }, ms);
  return () => clearInterval(timer);
}

// ------------------------------------------------------------------- main ---

/**
 * Runs the whole pipeline.
 *
 * @param {object} opts
 * @param {string} opts.isoPath        Path to the user's own disc image.
 * @param {string} opts.workDir        Scratch folder (~4 GB).
 * @param {string} opts.outDir         Where the finished `switch` folder is written.
 * @param {string} opts.projSource     Folder holding goal_src/ game/ decompiler/ custom_assets/.
 * @param {string} opts.runtimeSource  Folder holding gk.nro and data/ (shaders, fonts).
 * @param {boolean} opts.reuse         Keep an earlier extraction/compile when it is complete.
 * @param {object} opts.cancel         { cancelled, cancel() } token.
 * @param {function} opts.onEvent      Progress/log callback.
 */
async function prepare(opts) {
  const {
    isoPath,
    workDir,
    outDir,
    projSource,
    runtimeSource,
    reuse = true,
    recompile = false,
    cancel = { cancelled: false, cancel() { this.cancelled = true; } },
    onEvent = () => {}
  } = opts;

  const log = (line) => onEvent({ type: 'log', line });
  const stages = stageRunner(onEvent);

  if (!isoPath || !fs.existsSync(isoPath)) throw new StepError('noIso', 'missing');
  let iso = isoPath;
  if (fs.statSync(iso).isDirectory()) {
    throw new StepError('noIso', 'not a file');
  }
  await fsp.mkdir(workDir, { recursive: true });
  await fsp.mkdir(outDir, { recursive: true });

  const space = await freeSpace(workDir);
  if (space && space.free < SPACE_WORK) {
    throw new StepError('noSpace', 'work');
  }

  const proj = path.join(workDir, 'proj');
  const isoJak1 = path.join(proj, 'iso_data', 'jak1');
  const outJak1 = path.join(proj, 'out', 'jak1');

  // --- 1. the official OpenGOAL tools ---------------------------------------
  stages.begin('tools');
  const extractor = path.join(workDir, 'opengoal', 'extractor.exe');
  let exe = extractor;
  if (fs.existsSync(extractor)) {
    log(`using ${extractor}`);
  } else {
    const zip = path.join(workDir, `opengoal-${EXTRACTOR_VERSION}.zip`);
    if (!fs.existsSync(zip)) {
      log(`downloading ${EXTRACTOR_URL}`);
      await downloadFile(EXTRACTOR_URL, zip, {
        cancel,
        onProgress: (got, total) => stages.report(total ? got / total : 0)
      });
    }
    log('unpacking the tools');
    const unpack = path.join(workDir, 'opengoal');
    await removeTree(unpack);
    const res = await expandZip(zip, unpack);
    if (res.code !== 0) throw new StepError('download', `unzip exit ${res.code}`);
    if (!fs.existsSync(extractor)) {
      // The zip occasionally nests everything one level deeper.
      const nested = path.join(unpack, `opengoal-windows-${EXTRACTOR_VERSION}`, 'extractor.exe');
      if (fs.existsSync(nested)) exe = nested;
      else throw new StepError('download', 'extractor.exe missing after unzip');
    }
  }
  await checkCancel(cancel);
  stages.end('tools');

  // --- 2. a writable copy of the compiler project ---------------------------
  stages.begin('project');
  if (fs.existsSync(path.join(proj, 'goal_src'))) {
    stages.skip('project');
    log(`reusing ${proj}`);
  } else {
    log(`copying the compiler project to ${proj}`);
    await copyTree(projSource, proj, {
      cancel,
      onBytes: (done, total) => stages.report(total ? done / total : 0)
    });
    await fsp.mkdir(path.join(proj, 'iso_data'), { recursive: true });
    stages.end('project');
  }
  await checkCancel(cancel);

  // --- 3. extract the disc ---------------------------------------------------
  stages.begin('extract');
  if (reuse && fs.existsSync(path.join(isoJak1, 'DGO'))) {
    stages.skip('extract');
    log('the disc was already extracted');
  } else {
    log(`extracting ${iso}`);
    const stopSampler = await sampleLoop(isoJak1, 340, stages.report, cancel);
    let res;
    try {
      res = await runProcess(
        exe,
        ['--proj-path', proj, '--extract-path', path.join(proj, 'iso_data'), '-e', '-v', '--disable-ansi', iso],
        { cwd: proj, onLog: log, cancel }
      );
    } finally {
      stopSampler();
    }
    if (res.code !== 0) throw new StepError('extract', res.code);
    if (!fs.existsSync(path.join(isoJak1, 'DGO'))) throw new StepError('extract', 'no DGO folder');
    stages.end('extract');
  }
  await checkCancel(cancel);

  // --- 4. level data (this is what produces out/jak1/fr3) --------------------
  stages.begin('decompile');
  const decompilerOut = path.join(proj, 'decompiler_out');
  const fr3Dir = path.join(outJak1, 'fr3');
  const fr3Now = await countFiles(fr3Dir);
  if (reuse && !recompile && fs.existsSync(decompilerOut) && fr3Now >= EXPECTED.fr3) {
    stages.skip('decompile');
    log('the level data was already read');
  } else {
    log('reading the level data');
    const override = JSON.stringify({
      decompile_code: false,
      levels_extract: true,
      allowed_objects: []
    });
    const stopSampler = await sampleLoop(fr3Dir, COMPLETE.fr3, stages.report, cancel);
    let res;
    try {
      res = await runProcess(
        exe,
        ['--proj-path', proj, '-g', 'jak1', '-d', '--disable-ansi', '--decomp-config-override', override, isoJak1],
        { cwd: proj, onLog: log, cancel }
      );
    } finally {
      stopSampler();
    }
    if (res.code !== 0) throw new StepError('decompile', res.code);
    stages.end('decompile');
  }
  await checkCancel(cancel);

  // --- 5. compile for ARM64 --------------------------------------------------
  stages.begin('compile');
  for (const sub of ['obj', 'iso', 'fr3']) {
    await fsp.mkdir(path.join(outJak1, sub), { recursive: true });
  }
  const counts = {
    obj: await countFiles(path.join(outJak1, 'obj')),
    iso: await countFiles(path.join(outJak1, 'iso'))
  };
  if (reuse && !recompile && counts.obj >= EXPECTED.obj && counts.iso >= EXPECTED.iso) {
    stages.skip('compile');
    log('the game data was already compiled');
  } else {
    log('compiling the game data for ARM64');
    const stopSampler = await sampleLoop(
      path.join(outJak1, 'obj'),
      COMPLETE.obj,
      stages.report,
      cancel
    );
    let res;
    try {
      res = await runProcess(
        exe,
        ['--proj-path', proj, '-g', 'jak1', '-c', '--instruction-set', 'arm64', '--disable-ansi', isoJak1],
        { cwd: proj, onLog: log, cancel }
      );
    } finally {
      stopSampler();
    }
    if (res.code !== 0) throw new StepError('compile', res.code);
    stages.end('compile');
  }
  await checkCancel(cancel);

  const built = await verifyTree(outJak1);
  if (!built.ok) throw new StepError('incomplete', built.detail);

  // --- 6. collect everything into one folder ---------------------------------
  stages.begin('assemble');
  const install = path.join(outDir, 'switch', 'jak1');
  await fsp.mkdir(install, { recursive: true });
  log(`collecting ${install}`);
  await copyTree(runtimeSource, install, {
    cancel,
    onBytes: (done, total) => stages.report(total ? (done / total) * 0.5 : 0)
  });
  const compiledTarget = path.join(install, 'data', 'out', 'jak1');
  await removeTree(compiledTarget);
  await copyTree(outJak1, compiledTarget, {
    cancel,
    onBytes: (done, total) => stages.report(total ? 0.5 + (done / total) * 0.5 : 0)
  });
  await fsp.writeFile(path.join(outDir, 'HOW-TO-PLAY.txt'), howToPlay(), 'utf8');
  stages.end('assemble');
  onEvent({ type: 'phase', id: 'done', state: 'done' });
  onEvent({ type: 'progress', phase: 'done', fraction: 1, overall: 1 });

  const final = await verifyTree(compiledTarget);
  return {
    install,
    outDir,
    counts: final,
    bytes: await dirBytes(install)
  };
}

async function checkCancel(cancel) {
  if (cancel && cancel.cancelled) throw new Cancelled();
}

async function verifyTree(outJak1) {
  const counts = {
    obj: await countFiles(path.join(outJak1, 'obj')),
    iso: await countFiles(path.join(outJak1, 'iso')),
    fr3: await countFiles(path.join(outJak1, 'fr3'))
  };
  const kernel = fs.existsSync(path.join(outJak1, 'iso', 'KERNEL.CGO'));
  const ok =
    counts.obj >= EXPECTED.obj && counts.iso >= EXPECTED.iso && counts.fr3 >= EXPECTED.fr3 && kernel;
  return {
    ok,
    counts,
    detail: `obj=${counts.obj} iso=${counts.iso} fr3=${counts.fr3}${kernel ? '' : ' KERNEL.CGO missing'}`,
    bytes: await dirBytes(outJak1)
  };
}

function howToPlay() {
  return [
    'Jak and Daxter: The Precursor Legacy — Nintendo Switch',
    '',
    'WHAT TO DO',
    '  1. Copy the folder "switch" to the root of your SD card.',
    '  2. If the card already has a "switch" folder, merge them.',
    '  3. You should end up with sdmc:/switch/jak1/gk.nro',
    '  4. Launch "Jak 1" from the homebrew menu (hbmenu).',
    '  5. Hold R while launching if the console reports a memory error.',
    '',
    'WHAT TO DO (AR)',
    '  1. انسخ مجلد "switch" على جذر كرت الذاكرة.',
    '  2. إذا كان في مجلد "switch" من قبل، ادمج الاتنين.',
    '  3. لازم يطلع عندك sdmc:/switch/jak1/gk.nro',
    '  4. شغّل "Jak 1" من الـhomebrew menu (hbmenu).',
    '  5. اضغط R وقت التشغيل إذا طلع خطأ ذاكرة.',
    '',
    'Made with your own copy of the game. No game data is distributed with this installer.',
    ''
  ].join('\n');
}

module.exports = {
  prepare,
  verifyTree,
  countFiles,
  copyTree,
  dirBytes,
  freeSpace,
  downloadFile,
  listTree,
  Cancelled,
  StepError,
  EXPECTED,
  PHASES,
  EXTRACTOR_URL
};
