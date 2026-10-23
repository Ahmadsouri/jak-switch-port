'use strict';

const { contextBridge, ipcRenderer, webUtils } = require('electron');

const EVENTS = ['run:event', 'run:done', 'run:error', 'copy:progress', 'copy:done', 'copy:error'];

contextBridge.exposeInMainWorld('desktop', {
  info: () => ipcRenderer.invoke('app:info'),
  saveSettings: (patch) => ipcRenderer.invoke('settings:save', patch),
  pickIso: () => ipcRenderer.invoke('dialog:iso'),
  pickFolder: (current) => ipcRenderer.invoke('dialog:folder', current),
  listDrives: () => ipcRenderer.invoke('drives:list'),
  space: (target) => ipcRenderer.invoke('space:check', target),
  openPath: (target) => ipcRenderer.invoke('shell:open', target),
  showFile: (target) => ipcRenderer.invoke('shell:showFile', target),
  start: (payload) => ipcRenderer.invoke('run:start', payload),
  cancel: () => ipcRenderer.invoke('run:cancel'),
  copy: (target) => ipcRenderer.invoke('copy:start', target),
  pathForFile: (file) => {
    try {
      return webUtils.getPathForFile(file) || '';
    } catch {
      return '';
    }
  },
  on: (channel, handler) => {
    if (!EVENTS.includes(channel)) return () => {};
    const wrapped = (_event, payload) => handler(payload);
    ipcRenderer.on(channel, wrapped);
    return () => ipcRenderer.removeListener(channel, wrapped);
  },
  ready: () => ipcRenderer.send('ui:ready'),
  reportError: (message) => ipcRenderer.send('ui:render-error', message)
});
