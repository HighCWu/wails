// Renderer bridge: shim the WebView2-style postMessage transport so the
// Wails JS runtime works unmodified under Electron.
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('chrome', {
  webview: {
    postMessage: (msg) => { ipcRenderer.send('wails:message', msg); },
  },
});
