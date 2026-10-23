'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');

const pipeline = require('../src/pipeline');
const strings = require('../src/strings');

function tmpdir(name) {
  return fs.mkdtempSync(path.join(os.tmpdir(), `jak-${name}-`));
}

function touch(file, size = 0) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, Buffer.alloc(size, 7));
}

test('every English string has an Arabic twin', () => {
  const en = Object.keys(strings.en).sort();
  const ar = Object.keys(strings.ar).sort();
  assert.deepEqual(ar, en);
});

test('every placeholder survives translation', () => {
  const places = (value) => (String(value).match(/\{\w+\}/g) || []).sort();
  for (const key of Object.keys(strings.en)) {
    assert.deepEqual(
      places(strings.ar[key]),
      places(strings.en[key]),
      `placeholder mismatch on "${key}"`
    );
  }
});

test('Arabic is the only right-to-left locale', () => {
  assert.equal(strings.ar.dir, 'rtl');
  assert.equal(strings.en.dir, 'ltr');
});

test('countFiles and dirBytes walk nested folders', async () => {
  const dir = tmpdir('count');
  touch(path.join(dir, 'a.bin'), 10);
  touch(path.join(dir, 'nested', 'b.bin'), 32);
  touch(path.join(dir, 'nested', 'deeper', 'c.bin'), 6);
  assert.equal(await pipeline.countFiles(dir), 3);
  assert.equal(await pipeline.dirBytes(dir), 48);
  assert.equal(await pipeline.countFiles(path.join(dir, 'missing')), 0);
});

test('copyTree mirrors the tree and reports bytes', async () => {
  const src = tmpdir('src');
  const dest = tmpdir('dest');
  touch(path.join(src, 'one', 'a.txt'), 4);
  touch(path.join(src, 'b.txt'), 8);

  let last = 0;
  const copied = await pipeline.copyTree(src, path.join(dest, 'out'), {
    onBytes: (done) => {
      last = done;
    }
  });

  assert.equal(copied, 2);
  assert.equal(last, 12);
  assert.equal(fs.readFileSync(path.join(dest, 'out', 'one', 'a.txt')).length, 4);
});

test('copyTree can skip files that are already identical in size', async () => {
  const src = tmpdir('src2');
  const dest = tmpdir('dest2');
  touch(path.join(src, 'a.bin'), 5);
  touch(path.join(dest, 'a.bin'), 5);
  const copied = await pipeline.copyTree(src, dest, { skipIdentical: true });
  assert.equal(copied, 0);
});

test('copyTree stops when cancelled', async () => {
  const src = tmpdir('src3');
  const dest = tmpdir('dest3');
  touch(path.join(src, 'a.bin'), 1);
  const cancel = { cancelled: true };
  await assert.rejects(() => pipeline.copyTree(src, dest, { cancel }), /cancelled/);
});

test('verifyTree rejects an incomplete build', async () => {
  const dir = tmpdir('verify-bad');
  touch(path.join(dir, 'obj', 'a.o'), 1);
  touch(path.join(dir, 'iso', 'KERNEL.CGO'), 1);
  const res = await pipeline.verifyTree(dir);
  assert.equal(res.ok, false);
  assert.match(res.detail, /obj=1/);
});

test('verifyTree accepts a complete build', async () => {
  const dir = tmpdir('verify-good');
  for (const sub of ['obj', 'iso', 'fr3']) {
    for (let i = 0; i < 6; i += 1) touch(path.join(dir, sub, `f${i}.bin`), 2);
  }
  touch(path.join(dir, 'iso', 'KERNEL.CGO'), 3);
  const res = await pipeline.verifyTree(dir);
  assert.equal(res.ok, false, 'six files per folder is below the threshold');

  for (let i = 0; i < 1200; i += 1) touch(path.join(dir, 'obj', `x${i}.o`), 1);
  for (let i = 0; i < 400; i += 1) touch(path.join(dir, 'iso', `y${i}.bin`), 1);
  for (let i = 0; i < 30; i += 1) touch(path.join(dir, 'fr3', `z${i}.bin`), 1);
  const ok = await pipeline.verifyTree(dir);
  assert.equal(ok.ok, true, ok.detail);
  assert.equal(ok.counts.obj, 1206);
});

test('prepare refuses a missing disc image', async () => {
  await assert.rejects(
    () =>
      pipeline.prepare({
        isoPath: path.join(os.tmpdir(), 'definitely-not-here.iso'),
        workDir: tmpdir('work'),
        outDir: tmpdir('out'),
        projSource: tmpdir('proj'),
        runtimeSource: tmpdir('runtime')
      }),
    (err) => err.code === 'noIso' || err.step === 'noIso'
  );
});

test('prepare refuses a directory instead of an ISO', async () => {
  const iso = tmpdir('folder-not-iso');
  await assert.rejects(
    () =>
      pipeline.prepare({
        isoPath: iso,
        workDir: tmpdir('work2'),
        outDir: tmpdir('out2'),
        projSource: tmpdir('proj2'),
        runtimeSource: tmpdir('runtime2')
      }),
    (err) => err.step === 'noIso'
  );
});
