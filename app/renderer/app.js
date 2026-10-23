'use strict';

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => Array.from(document.querySelectorAll(selector));

const NEED_OUT = 2.4 * 1024 ** 3;
const NEED_WORK = 5.0 * 1024 ** 3;

const state = {
  locale: 'en',
  dict: {},
  info: null,
  iso: '',
  outDir: '',
  workDir: '',
  running: false,
  startedAt: 0,
  tick: null,
  logOpen: false,
  logLines: 0,
  drives: [],
  copyTarget: ''
};

// -------------------------------------------------------------- utilities ---

const nf = new Intl.NumberFormat('en-US');

function t(key, vars) {
  let text = state.dict[key] ?? key;
  if (vars) {
    for (const [name, value] of Object.entries(vars)) {
      text = text.replace(new RegExp(`\\{${name}\\}`, 'g'), String(value));
    }
  }
  return text;
}

function fmtBytes(bytes) {
  if (!bytes) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  const rounded = unit === 0 ? Math.round(value) : value >= 10 ? Math.round(value) : value.toFixed(1);
  return `${rounded} ${units[unit]}`;
}

function fmtDuration(ms) {
  const total = Math.max(0, Math.round(ms / 1000));
  const minutes = Math.floor(total / 60);
  const seconds = total % 60;
  if (minutes >= 60) {
    const hours = Math.floor(minutes / 60);
    return `${hours}h ${String(minutes % 60).padStart(2, '0')}m`;
  }
  return `${minutes}m ${String(seconds).padStart(2, '0')}s`;
}

function basename(p) {
  return String(p || '').split(/[\\/]/).filter(Boolean).pop() || p;
}

// -------------------------------------------------------------- i18n glue ---

function applyLocale(locale) {
  state.locale = locale === 'ar' ? 'ar' : 'en';
  state.dict = state.info.strings[state.locale];
  document.documentElement.lang = state.locale;
  document.documentElement.dir = state.dict.dir;
  $$('[data-i18n]').forEach((node) => {
    node.textContent = t(node.dataset.i18n);
  });
  $$('.lang button').forEach((button) => {
    button.classList.toggle('is-on', button.dataset.lang === state.locale);
  });
  $('#version').textContent = t('version', { v: state.info.version });
  refreshFileChip();
  refreshSpaces();
  if (!state.running) $('#runPhase').textContent = t('phaseTools');
  if (!$('#log').hidden) $('#logToggle').textContent = t('hideLog');
  if (state.info) buildPhases(true);
  renderDrives();
}

// ---------------------------------------------------------------- screens ---

function show(name) {
  $$('.screen').forEach((section) => section.classList.remove('is-active'));
  $(`#screen-${name}`).classList.add('is-active');
  const step = name === 'choose' ? 1 : name === 'run' ? 2 : 3;
  $$('.steps li').forEach((li) => {
    const n = Number(li.dataset.step);
    li.classList.toggle('is-on', n === step);
    li.classList.toggle('is-past', n < step);
  });
  $('main').scrollTop = 0;
}

function chooseError(message) {
  const node = $('#chooseError');
  node.textContent = message || '';
  node.hidden = !message;
}

// -------------------------------------------------------------- game file ---

function setIso(filePath) {
  if (!filePath) return;
  if (!/\.iso$/i.test(filePath)) {
    chooseError(t('notIso'));
    return;
  }
  chooseError('');
  state.iso = filePath;
  window.desktop.saveSettings({ isoPath: filePath });
  refreshFileChip();
}

function refreshFileChip() {
  const chip = $('#fileChip');
  if (!state.iso) {
    chip.hidden = true;
    return;
  }
  chip.hidden = false;
  $('#fileName').textContent = basename(state.iso);
  const dir = state.iso.replace(/[\\/][^\\/]+$/, '');
  $('#fileMeta').textContent = dir;
}

// -------------------------------------------------------------- disk space ---

async function refreshSpaces() {
  const outSpace = $('#outSpace');
  const workSpace = $('#workSpace');
  const outInfo = await window.desktop.space(state.outDir);
  const workInfo = await window.desktop.space(state.workDir);
  paintSpace(outSpace, outInfo, NEED_OUT);
  paintSpace(workSpace, workInfo, NEED_WORK);
}

function paintSpace(node, info, need) {
  if (!info) {
    node.textContent = t('spaceNeed', { need: fmtBytes(need) });
    node.classList.remove('is-low');
    return;
  }
  if (info.free < need) {
    node.textContent = t('spaceLow', { free: fmtBytes(info.free), need: fmtBytes(need) });
    node.classList.add('is-low');
  } else {
    node.textContent = t('spaceOk', { free: fmtBytes(info.free) });
    node.classList.remove('is-low');
  }
}

// ------------------------------------------------------------------ phases ---

const PHASES = ['tools', 'project', 'extract', 'decompile', 'compile', 'assemble'];

function phaseLabel(id) {
  const key = `phase${id.charAt(0).toUpperCase()}${id.slice(1)}`;
  return t(key);
}

function buildPhases(reset = false) {
  const list = $('#phases');
  if (!reset && list.dataset.lang === state.locale && list.children.length) return;
  list.textContent = '';
  list.dataset.lang = state.locale;
  PHASES.forEach((id) => {
    const li = document.createElement('li');
    li.dataset.phase = id;
    li.innerHTML = '<span class="state"></span><span class="label"></span><span class="tag"></span>';
    li.querySelector('.label').textContent = phaseLabel(id);
    list.appendChild(li);
  });
}

function setPhaseState(id, phaseState) {
  const li = $(`#phases li[data-phase="${id}"]`);
  if (!li) return;
  li.classList.remove('is-run', 'is-done', 'is-skip');
  const tag = li.querySelector('.tag');
  if (phaseState === 'run') {
    li.classList.add('is-run');
    tag.textContent = t('working');
  } else if (phaseState === 'done') {
    li.classList.add('is-done');
    tag.textContent = '';
    li.querySelector('.state').textContent = '✓';
  } else if (phaseState === 'skip') {
    li.classList.add('is-skip');
    tag.textContent = t('phaseSkipped');
    li.querySelector('.state').textContent = '·';
  } else {
    tag.textContent = '';
  }
}

function resetPhaseTags() {
  $$('#phases li').forEach((li) => {
    li.classList.remove('is-run', 'is-done', 'is-skip');
    li.querySelector('.tag').textContent = '';
    li.querySelector('.state').textContent = '';
  });
}

// ------------------------------------------------------------------ drives ---

async function loadDrives() {
  state.drives = await window.desktop.listDrives();
  renderDrives();
}

function renderDrives() {
  const select = $('#driveSelect');
  const previous = select.value;
  select.textContent = '';
  if (state.copyTarget) {
    const option = document.createElement('option');
    option.value = state.copyTarget;
    option.textContent = state.copyTarget;
    select.appendChild(option);
  }
  if (!state.drives.length && !state.copyTarget) {
    const option = document.createElement('option');
    option.value = '';
    option.textContent = t('noDrives');
    select.appendChild(option);
    select.disabled = true;
  } else {
    select.disabled = false;
    state.drives.forEach((drive) => {
      const option = document.createElement('option');
      option.value = drive.id;
      option.textContent = `${drive.label} — ${t('cardFree', { free: fmtBytes(drive.free) })}`;
      select.appendChild(option);
    });
    if (state.drives.some((d) => d.id === previous) || previous === state.copyTarget) {
      select.value = previous;
    }
  }
  $('#copyBtn').disabled = !select.value;
}

// --------------------------------------------------------------------- run ---

function startRun() {
  if (!state.iso) {
    chooseError(t('errNoIso'));
    return;
  }
  chooseError('');
  state.running = true;
  state.logLines = 0;
  $('#log').textContent = '';
  $('#logToggle').textContent = t('showLog');
  buildPhases(true);
  resetPhaseTags();
  $('#barFill').style.width = '0%';
  $('#pct').textContent = '0%';
  $('#eta').textContent = '';
  $('#elapsed').textContent = '';
  $('#runPhase').textContent = t('phaseTools');
  $('#cancelBtn').disabled = false;
  $('#cancelBtn').textContent = t('cancel');
  show('run');

  state.startedAt = Date.now();
  clearInterval(state.tick);
  state.tick = setInterval(() => {
    if (!state.running) return;
    const elapsed = Date.now() - state.startedAt;
    $('#elapsed').textContent = t('elapsed', { time: fmtDuration(elapsed) });
  }, 1000);

  window.desktop.start({
    isoPath: state.iso,
    outDir: state.outDir,
    workDir: state.workDir,
    reuse: $('#reuse').checked
  });
}

function onRunEvent(event) {
  if (event.type === 'phase') {
    setPhaseState(event.id, event.state);
    if (event.state === 'run') $('#runPhase').textContent = phaseLabel(event.id);
    return;
  }
  if (event.type === 'progress') {
    const pct = Math.min(100, Math.round((event.overall || 0) * 100));
    $('#barFill').style.width = `${pct}%`;
    $('#pct').textContent = `${pct}%`;
    const elapsed = event.elapsed || Date.now() - state.startedAt;
    if (event.overall > 0.035 && event.overall < 0.995) {
      const total = elapsed / event.overall;
      $('#eta').textContent = t('eta', { time: fmtDuration(total - elapsed) });
    } else {
      $('#eta').textContent = '';
    }
    return;
  }
  if (event.type === 'log') {
    const pre = $('#log');
    pre.textContent += `${event.line}\n`;
    state.logLines += 1;
    if (state.logLines > 700) {
      const lines = pre.textContent.split('\n').slice(-400);
      pre.textContent = lines.join('\n');
    }
    pre.scrollTop = pre.scrollHeight;
  }
}

function finishRun(payload) {
  state.running = false;
  clearInterval(state.tick);
  $('#statObj').textContent = nf.format(payload.counts.obj);
  $('#statIso').textContent = nf.format(payload.counts.iso);
  $('#statFr3').textContent = nf.format(payload.counts.fr3);
  $('#statSize').textContent = payload.size;
  $('#installPath').value = payload.install;
  $('#copyStatus').textContent = '';
  $('#copyStatus').classList.remove('is-error');
  $('#copyBarWrap').hidden = true;
  show('done');
  loadDrives();
}

function failRun(payload) {
  state.running = false;
  clearInterval(state.tick);
  const messages = {
    cancelled: () => t('errCancelled'),
    noIso: () => t('errNoIso'),
    noSpace: () => t('errNoSpace', { need: '5 GB' }),
    download: () => t('errDownload'),
    extract: () => t('errExtract', { code: payload.detail || '?' }),
    decompile: () => t('errDecompile', { code: payload.detail || '?' }),
    compile: () => t('errCompile', { code: payload.detail || '?' }),
    incomplete: () => t('errIncomplete', { detail: payload.detail || '' }),
    busy: () => t('errGeneric', { msg: 'busy' })
  };
  const builder = messages[payload.code] || (() => t('errGeneric', { msg: payload.detail || payload.message || '' }));
  $('#errMessage').textContent = builder();
  $('#errorLog').onclick = () => window.desktop.showFile(payload.logFile);
  show('error');
}

// -------------------------------------------------------------------- copy ---

async function copyToCard() {
  const drive = $('#driveSelect').value;
  if (!drive) return;
  const status = $('#copyStatus');
  status.classList.remove('is-error');
  status.textContent = t('copying', { done: '0 B', total: '…' });
  $('#copyBarWrap').hidden = false;
  $('#copyBar').style.width = '0%';
  $('#copyBtn').disabled = true;
  const res = await window.desktop.copy(drive);
  if (!res.ok) {
    status.classList.add('is-error');
    status.textContent = t('copyFail', { msg: res.error || '' });
  }
  $('#copyBtn').disabled = false;
}

// -------------------------------------------------------------------- init ---

async function init() {
  state.info = await window.desktop.info();
  applyLocale(state.info.settings.locale || 'en');

  state.iso = state.info.settings.isoPath || '';
  state.outDir = state.info.settings.outDir;
  state.workDir = state.info.settings.workDir;
  $('#outDir').value = state.outDir;
  $('#workDir').value = state.workDir;
  $('#reuse').checked = state.info.settings.reuse !== false;
  refreshFileChip();
  refreshSpaces();

  if (!state.info.resources.ready) {
    chooseError('The installer bundle is incomplete (compiler project or runtime missing). Reinstall it.');
  }

  $$('.lang button').forEach((button) => {
    button.addEventListener('click', () => {
      applyLocale(button.dataset.lang);
      window.desktop.saveSettings({ locale: button.dataset.lang });
    });
  });

  const drop = $('#drop');
  const openPicker = async () => {
    const picked = await window.desktop.pickIso();
    if (picked) setIso(picked);
  };
  drop.addEventListener('click', openPicker);
  drop.addEventListener('keydown', (event) => {
    if (event.key === 'Enter' || event.key === ' ') openPicker();
  });
  $('#changeFile').addEventListener('click', openPicker);

  ['dragenter', 'dragover'].forEach((type) =>
    drop.addEventListener(type, (event) => {
      event.preventDefault();
      drop.classList.add('is-hot');
    })
  );
  ['dragleave', 'drop'].forEach((type) =>
    drop.addEventListener(type, (event) => {
      event.preventDefault();
      drop.classList.remove('is-hot');
    })
  );
  drop.addEventListener('drop', (event) => {
    const file = event.dataTransfer.files && event.dataTransfer.files[0];
    if (!file) return;
    const p = window.desktop.pathForFile(file);
    if (p) setIso(p);
  });

  $$('[data-pick]').forEach((button) => {
    button.addEventListener('click', async () => {
      const which = button.dataset.pick;
      const current = which === 'outDir' ? state.outDir : state.workDir;
      const picked = await window.desktop.pickFolder(current);
      if (!picked) return;
      if (which === 'outDir') {
        state.outDir = picked;
        $('#outDir').value = picked;
      } else {
        state.workDir = picked;
        $('#workDir').value = picked;
      }
      window.desktop.saveSettings({ outDir: state.outDir, workDir: state.workDir });
      refreshSpaces();
    });
  });

  $('#reuse').addEventListener('change', (event) => {
    window.desktop.saveSettings({ reuse: event.target.checked });
  });

  $('#startBtn').addEventListener('click', startRun);

  $('#cancelBtn').addEventListener('click', () => {
    $('#cancelBtn').disabled = true;
    $('#cancelBtn').textContent = t('cancelling');
    window.desktop.cancel();
  });

  $('#logToggle').addEventListener('click', () => {
    state.logOpen = !state.logOpen;
    $('#log').hidden = !state.logOpen;
    $('#logToggle').textContent = state.logOpen ? t('hideLog') : t('showLog');
  });

  $('#openFolder').addEventListener('click', () => window.desktop.openPath($('#installPath').value));

  $('#refreshDrives').addEventListener('click', loadDrives);

  $('#pickTarget').addEventListener('click', async () => {
    const picked = await window.desktop.pickFolder(state.copyTarget || state.outDir);
    if (!picked) return;
    state.copyTarget = picked;
    renderDrives();
    $('#driveSelect').value = picked;
    $('#copyBtn').disabled = false;
    $('#copyStatus').textContent = '';
  });

  $('#copyBtn').addEventListener('click', copyToCard);

  $('#againBtn').addEventListener('click', () => show('choose'));

  $('#retryBtn').addEventListener('click', () => {
    show('choose');
    refreshSpaces();
  });

  $('#driveSelect').addEventListener('change', () => {
    $('#copyStatus').textContent = '';
  });

  window.desktop.on('run:event', onRunEvent);
  window.desktop.on('run:done', finishRun);
  window.desktop.on('run:error', failRun);
  window.desktop.on('copy:progress', (payload) => {
    const ratio = payload.total ? payload.done / payload.total : 0;
    $('#copyBar').style.width = `${Math.min(100, ratio * 100)}%`;
    $('#copyStatus').textContent = t('copying', {
      done: fmtBytes(payload.done),
      total: fmtBytes(payload.total)
    });
  });
  window.desktop.on('copy:done', (payload) => {
    $('#copyBar').style.width = '100%';
    const status = $('#copyStatus');
    status.classList.remove('is-error');
    status.textContent = t('copyDone', { n: nf.format(payload.count) });
  });
  window.desktop.on('copy:error', (payload) => {
    const status = $('#copyStatus');
    status.classList.add('is-error');
    status.textContent = t('copyFail', { msg: payload.message });
  });

  window.addEventListener('keydown', (event) => {
    if (event.key === 'Escape' && state.running) {
      $('#cancelBtn').click();
    }
  });

  window.desktop.ready();
}

window.addEventListener('error', (event) => {
  window.desktop.reportError(`${event.message} @ ${event.filename}:${event.lineno}`);
});

window.addEventListener('unhandledrejection', (event) => {
  const reason = event.reason;
  window.desktop.reportError(reason && reason.stack ? reason.stack : String(reason));
});

init().catch((err) => {
  window.desktop.reportError(err && err.stack ? err.stack : String(err));
  document.body.innerHTML =
    '<pre style="color:#ffb3a8;font:13px monospace;padding:32px;white-space:pre-wrap;direction:ltr;text-align:left">' +
    String(err && err.stack ? err.stack : err) +
    '</pre>';
});
