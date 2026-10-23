'use strict';

// Stages everything the installer hands to the player into app/resources/:
//
//   resources/proj/     the GOAL compiler project (goal_src, game, decompiler, custom_assets)
//   resources/runtime/  gk.nro plus the game/ and custom_assets/ folders it loads at start
//
// electron-builder copies these next to the installed app as extraResources.
// They are git-ignored: only this script creates them.

const fs = require('fs');
const path = require('path');

const APP = path.resolve(__dirname, '..');
const ROOT = path.resolve(APP, '..');
const RES = path.join(APP, 'resources');

function copyDir(from, to) {
  if (!fs.existsSync(from)) throw new Error(`missing: ${from}`);
  fs.mkdirSync(to, { recursive: true });
  fs.cpSync(from, to, { recursive: true, force: true });
}

function findNro() {
  const candidates = [
    process.env.JAK1_NRO,
    path.join(ROOT, 'build-switch', 'gk.nro'),
    path.join(ROOT, 'gk.nro'),
    path.join(APP, 'gk.nro')
  ].filter(Boolean);
  return candidates.find((p) => fs.existsSync(p));
}

function main() {
  const nro = findNro();
  if (!nro) {
    console.error(
      'gk.nro not found. Build it with tools/switch/build-switch.sh + tools/switch/package-nro.sh,\n' +
        'or point JAK1_NRO at an existing build.'
    );
    process.exit(1);
  }

  fs.rmSync(RES, { recursive: true, force: true });

  console.log('staging the compiler project');
  for (const name of ['goal_src', 'game', 'decompiler', 'custom_assets']) {
    copyDir(path.join(ROOT, name), path.join(RES, 'proj', name));
  }

  console.log(`staging the runtime (${path.basename(nro)})`);
  const runtime = path.join(RES, 'runtime');
  fs.mkdirSync(path.join(runtime, 'data'), { recursive: true });
  fs.copyFileSync(nro, path.join(runtime, 'gk.nro'));
  copyDir(path.join(ROOT, 'game'), path.join(runtime, 'data', 'game'));
  copyDir(path.join(ROOT, 'custom_assets'), path.join(runtime, 'data', 'custom_assets'));

  const files = countFiles(RES);
  console.log(`done: ${files} files in ${RES}`);
}

function countFiles(dir) {
  let total = 0;
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    if (entry.isDirectory()) total += countFiles(path.join(dir, entry.name));
    else total += 1;
  }
  return total;
}

main();
