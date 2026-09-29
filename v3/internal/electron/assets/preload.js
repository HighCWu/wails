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

window.chrome = window.chrome || {};
window.chrome.webview = {
  postMessage: (msg) => { ipcRenderer.send('wails:message', msg); },
};

if (process.env.WAILS_ELECTRON_EXPERIMENT === 'fetch-ipc') {
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
}
