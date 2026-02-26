/**
 * Ainoiceguard - Preload Script
 *
 * Bridges the Electron main process and renderer via contextBridge.
 * The renderer cannot directly access Node.js or the native addon.
 * Instead, it calls these safe IPC wrappers.
 */

const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("ainoiceguard", {
  getDevices: () => ipcRenderer.invoke("audio:get-devices"),
  start: (inputIdx, outputIdx) =>
    ipcRenderer.invoke("audio:start", inputIdx, outputIdx),
  stop: () => ipcRenderer.invoke("audio:stop"),
  setLevel: (level) => ipcRenderer.invoke("audio:set-level", level),
  getStatus: () => ipcRenderer.invoke("audio:get-status"),
  getMetrics: () => ipcRenderer.invoke("audio:get-metrics"),
  setVadThreshold: (threshold) =>
    ipcRenderer.invoke("audio:set-vad-threshold", threshold),
  openExternal: (url) => ipcRenderer.invoke("app:open-external", url),
});
