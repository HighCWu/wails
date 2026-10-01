// Renderer bridge. Two responsibilities:
//  1. Shim window.chrome.webview.postMessage (WebView2-style transport) so
//     the Wails JS runtime works unmodified.
//  2. native-http (the default with native-ipc): route /wails/runtime
//     fetches over the UDS data plane (frame v3), skipping the network
//     service and the loopback TCP hop. The earlier fetch-ipc variant
//     (ipcRenderer -> main -> stdio -> Go) has been retired.
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
  process.stderr.write(`[wails-electron preload] exp="${expModes.join(',')}" ppid=${process.ppid}\n`);
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
    if (url.indexOf('/wails/runtime') !== -1 && window.__wailsV8Body && window.__nativeInvoke) {
      url = new URL(url, location.href).toString();
      // Frame v3: the whole call object travels as ONE v8-serialized
      // message (id/channel/method/url/headers/body) — the same shape
      // as Electron's SerializeV8Value(arguments) on its Mojo invoke.
      const rawBody = init && init.body != null ? init.body : '';
      const callObj = {
        channel: 'http',
        method: (init && init.method) || 'GET',
        url: url,
        body: typeof rawBody === 'string' ? rawBody : new Uint8Array(rawBody),
        headers: {},
      };
      if (init && init.headers) {
        for (const [k, v] of new Headers(init.headers).entries()) callObj.headers[k] = v;
      }
      // The addon serializes the raw object via the injected v8.serialize
      // (one napi_call from C) — exactly the bytes Electron's renderer
      // hands its main process on an IPC invoke. Serializing here too
      // would double-wrap (buffer-of-buffer) — pass the object as is.
      const out = await window.__nativeInvoke(callObj);
      if (out.err) throw new Error(out.err);
      const headers = new Headers();
      if (out.contentType) headers.set('Content-Type', out.contentType);
      return new Response(out.body.length ? new Uint8Array(out.body) : null, {
        status: out.status,
        headers: headers,
      });
    }
    return origFetch(input, init);
  };
  window.__nativeHttpActive = true;
  console.log('[wails-electron preload] native-uds fetch override installed (D full UDS)');
}
// [retired] the fetch-ipc experiment (ipcRenderer -> main -> stdio ->
// Go) was removed once frame v3 over UDS became the default bindings
// data plane. Its history lives in the native-http override above.


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
  // The addon holds these as napi_refs and calls them from C on every
  // invoke — all serialization logic stays in JS's own v8 module, the
  // addon never re-implements the format.
  bridge.setSerde(require('v8').serialize, require('v8').deserialize);
  bridge.connect(initConfig.endpoint, initConfig.token);
  // Frame v3 is up: the call body travels v8-serialized (binary, args
  // object direct) instead of JSON.stringify'd — the same bytes
  // Electron's renderer hands its main process on an IPC invoke.
  // runtime.js gates its call-body encoding on this flag.
  window.__wailsV8Body = true;
  // runtime.js gates its call-body encoding on this: when present, the
  // call object is v8-serialized (binary, args object direct) instead
  // of JSON.stringify'd — eliminating the text-encode hop on the wire
  window.__wailsV8Serialize = (obj) => require('v8').serialize(obj);
  window.__wailsV8Deserialize = (buf) => require('v8').deserialize(buf);
  window.__nativeInvoke = (msgObj) => bridge.invoke(msgObj);
  // echo diagnostics ride the v3 invoke frame (channel: "echo") —
  // the message is v8-serialized like every other frame on the wire
  // echo diagnostics ride the v3 invoke frame (channel: "echo") — the
  // message is v8-serialized by the addon like every frame on the wire
  window.__nativeEcho = (payload) =>
    bridge.invoke({ id: 0, channel: 'echo', payload }).then((r) => r.payload);
  window.__nativeCall = (id, payload) => bridge.call(id, payload);
  console.log('[wails-electron preload] native-uds transport ready');
  // native-http is the DEFAULT data plane for the native-ipc mode
  // (frame v3, bindings over UDS). Opt out with 'no-native-http'.
  const wantNativeHttp = expModes.includes('native-http') ||
    (expModes.includes('native-ipc') && !expModes.includes('no-native-http'));
  if (wantNativeHttp) installNativeHttpFetch();
};
