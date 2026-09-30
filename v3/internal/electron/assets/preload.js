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
    if (url.indexOf('/wails/runtime') !== -1 && window.__nativeCallBin) {
      url = new URL(url, location.href).toString();
      // Aligned with Electron's Invoke: the serialized payload carries
      // ONLY the call arguments ([body] — a single-element array, same
      // shape as the args list ipcRenderer passes to SerializeV8Value).
      // Transport metadata (id/channel/method/url/headers) lives in the
      // frame header, the equivalent of Electron's Mojo parameters.
      const rawBody = init && init.body != null ? init.body : '';
      const bodyBuf = typeof rawBody === 'string' ? Buffer.from(rawBody) : Buffer.from(rawBody);
      // headers must ride along: the runtime's chunked upload protocol
      // (x-wails-chunk-*) and call association (x-wails-call-id) live there
      const hdrObj = {};
      if (init && init.headers) {
        for (const [k, v] of new Headers(init.headers).entries()) hdrObj[k] = v;
      }
      const payload = require('v8').serialize([bodyBuf]);
      const hdr = JSON.stringify({
        id: __nativeSeq++,
        type: 'http',
        method: (init && init.method) || 'GET',
        url: url,
        headers: hdrObj,
        payloadLen: payload.length,
      });
      // frame = header line + raw payload bytes (one Invoke on the wire)
      const frame = Buffer.concat([Buffer.from(hdr + '\n'), Buffer.from(payload)]);
      const respFrame = await window.__nativeCallBin(__nativeSeq - 1, frame, Buffer.alloc(0));
      // response frame: header line + raw payload bytes
      const nl = respFrame.indexOf(0x0A);
      const respHdr = JSON.parse(respFrame.slice(0, nl).toString());
      const respPayload = respFrame.slice(nl + 1);
      const decoded = require('v8').deserialize(respPayload);
      const respBody = decoded[0];
      const headers = new Headers();
      if (respHdr.contentType) headers.set('Content-Type', respHdr.contentType);
      return new Response(respBody.length ? new Uint8Array(respBody) : null, {
        status: respHdr.status,
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
  bridge.connect(initConfig.endpoint, initConfig.token);
  // runtime.js gates its call-body encoding on this: when present, the
  // call object is v8-serialized (binary, args object direct) instead
  // of JSON.stringify'd — eliminating the text-encode hop on the wire
  window.__wailsV8Serialize = (obj) => require('v8').serialize(obj);
  // echo diagnostics ride the v3 invoke frame (channel: "echo") —
  // the message is v8-serialized like every other frame on the wire
  window.__nativeEcho = (payload) => {
    const msg = require('v8').serialize({
      id: 0, channel: 'echo', payload: payload,
    });
    return bridge.invoke(msg).then((r) => {
      const decoded = require('v8').deserialize(r);
      return decoded.payload;
    });
  };
  window.__nativeCall = (id, payload) => bridge.call(id, payload);
  console.log('[wails-electron preload] native-uds transport ready');
  // native-http is the DEFAULT data plane for the native-ipc mode
  // (frame v3, bindings over UDS). Opt out with 'no-native-http'.
  const wantNativeHttp = expModes.includes('native-http') ||
    (expModes.includes('native-ipc') && !expModes.includes('no-native-http'));
  if (wantNativeHttp) installNativeHttpFetch();
};
