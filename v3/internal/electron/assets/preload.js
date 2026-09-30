// Renderer bridge. Two responsibilities:
//  1. Shim window.chrome.webview.postMessage (WebView2-style transport) so
//     the Wails JS runtime works unmodified.
//  2. Optional experiment (WAILS_ELECTRON_EXPERIMENT=fetch-ipc): route
//     /wails/runtime fetches over ipcRenderer -> main -> stdio -> Go,
//     skipping the network service and the loopback TCP hop.
// Runs in the renderer main world (contextIsolation is off for this
// backend); the preload is authored by us and the renderer still gets no
// Node integration.
const { ipcRenderer } = require('electron');
// [debug] surface unhandled rejections with stacks (native-http investigation)
process.on('unhandledRejection', (r) => {
  try { process.stderr.write('[ur] ' + (r && r.stack || r) + '\n'); } catch (e) {}
});

const expModes = (process.env.WAILS_ELECTRON_EXPERIMENT || '').split(',');
try {
  process.stderr.write(`[wails-electron preload] exp="${expMode}" ppid=${process.ppid}\n`);
} catch (e) {}

window.chrome = window.chrome || {};
window.chrome.webview = {
  postMessage: (msg) => { ipcRenderer.send('wails:message', msg); },
};

// Native UDS bindings data plane (native-ipc mode): /wails/runtime
// fetches go over the addon's direct socket to the asset server —
// no network stack, no Electron IPC. Takes precedence over fetch-ipc.
let __nativeHttpInstalled = false;
let __nativeSeq = 1;
function installNativeHttpFetch() {
  if (__nativeHttpInstalled) return;
  __nativeHttpInstalled = true;
  const origFetch = window.fetch.bind(window);
  window.fetch = async (input, init) => {
    let url = typeof input === 'string' ? input : (input && input.url) || String(input);
    if (url.indexOf('/wails/runtime') !== -1 && window.__nativeCall) {
      url = new URL(url, location.href).toString();
      const body = init && init.body != null ? String(init.body) : '';
      const resp = await window.__nativeCall(__nativeSeq++, JSON.stringify({
        type: 'http',
        method: (init && init.method) || 'GET',
        url: url,
        body: body,
      }));
      const parsed = JSON.parse(resp);
      if (parsed.err) throw new Error(parsed.err);
      const headers = new Headers();
      if (parsed.contentType) headers.set('Content-Type', parsed.contentType);
      const bin = atob(parsed.payload || '');
      const bytes = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
      return new Response(bytes.length ? bytes : null, {
        status: parsed.status,
        headers: headers,
      });
    }
    return origFetch(input, init);
  };
  window.__nativeHttpActive = true;
  console.log('[wails-electron preload] native-uds fetch override installed');
}

if (expModes.includes('fetch-ipc')) {
  const origFetch = window.fetch.bind(window);
  window.fetch = async (input, init) => {
    let url = typeof input === 'string' ? input : (input && input.url) || String(input);
    if (url.indexOf('/wails/runtime') !== -1) {
      url = new URL(url, location.href).toString();
      const body = init && init.body != null ? String(init.body) : '';
      const resp = await ipcRenderer.invoke('wails:runtime', {
        method: (init && init.method) || 'GET',
        url: url,
        body: body,
      });
      const headers = new Headers();
      if (resp.contentType) headers.set('Content-Type', resp.contentType);
      return new Response(resp.body ? Buffer.from(resp.body, 'base64') : null, {
        status: resp.status,
        headers: headers,
      });
    }
    return origFetch(input, init);
  };
  console.log('[wails-electron preload] fetch-ipc override installed');
}

function reportPreloadError(msg) {
  try { process.stderr.write('[wails-electron preload] ERROR ' + msg + '\n'); } catch (e) {}
  try { ipcRenderer.send('wails:message', JSON.stringify({ name: 'compat:preload-error', data: msg })); } catch (e) {}
}

if (expModes.includes('invoke-baseline')) {
  // Benchmark baseline: pure renderer<->main Electron IPC round trip, no Go.
  window.__compatInvoke = (payload) => ipcRenderer.invoke('compat:ping', payload);
}

// Native transport (WAILS_ELECTRON_EXPERIMENT includes native-ipc): the
// bootstrap is a one-time string injection from the main process
// (executeJavaScript after did-finish-load) — the data plane after
// connect() is the addon's own socket; main is not involved. The page
// waits briefly for __nativeCall before benching the native row.
window.__wailsNativeInit = (initConfig) => {
  console.log('[wails-electron preload] __wailsNativeInit called addon=' + initConfig.addon + ' endpoint=' + initConfig.endpoint);
  if (window.__nativeCall) return; // idempotent across navigations
  const bridge = require(initConfig.addon);
  bridge.connect(initConfig.endpoint, initConfig.token);
  window.__nativeCall = (id, payload) => bridge.call(id, payload);
  console.log('[wails-electron preload] native-uds transport ready');
  // gated: the ipcRenderer onMessage path SIGABRTs under this mode
  // (see promoted5/6 logs) — enable with native-http to keep debugging
  if (expModes.includes('native-http')) installNativeHttpFetch();
};
