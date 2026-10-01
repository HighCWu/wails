// Wails Electron backend bootstrap: owns every BrowserWindow and speaks the
// JSON-lines control protocol on stdin/stdout.
//   request : {"t":"req","id":N,"m":"<method>","p":{...,"id":windowID}}
//   response: {"t":"resp","id":N,"ok":true,"r":{...}} | {"t":"resp","id":N,"ok":false,"err":"..."}
//   event   : {"t":"ev","e":"<name>","p":{...,"id":windowID}}
// id space: even ids are Go-initiated, odd ids are Electron-initiated.
// Quitting: when stdin closes the host is gone — exit immediately so no
// orphaned Electron processes outlive the Wails application.
const { app, BrowserWindow, ipcMain } = require('electron');
const fs = require('fs');
const controlBuf = Buffer.allocUnsafe(65536);

const cfg = JSON.parse(process.env.WAILS_ELECTRON_CONFIG || '{}');
app.on('preload-error', (_event, preloadPath, error) => {
  process.stderr.write('[wails-electron] preload-error ' + preloadPath + ': ' + (error && error.message || error) + '\n');
});
if (cfg.bridgePath) process.env.WAILS_ELECTRON_BRIDGE_PATH = cfg.bridgePath;
if (cfg.nativeAddon) process.env.WAILS_ELECTRON_NATIVE_ADDON = cfg.nativeAddon;
const windows = new Map(); // windowID -> BrowserWindow
// webContents.id -> windowID. Keyed by the numeric id, never by the
// webContents object: the 'closed' event fires after the window is
// destroyed, and touching win.webContents there throws "Object has been
// destroyed" — an uncaught throw inside the native destroy flow wedges
// the main process's Node integration for good (async/timer/threadpool
// callbacks stop dispatching; see history/electron-churning-repro/).
const byWebContents = new Map(); // webContents.id -> windowID

// Benchmark baseline: pure renderer<->main Electron IPC round trip, no Go.
ipcMain.handle('compat:ping', (_event, payload) => payload);

function send(obj) {
  try {
    process.stdout.write(JSON.stringify(obj) + '\n');
  } catch (e) {
    if (process.env.WAILS_ELECTRON_DEBUG === '1') {
      try { process.stderr.write('[wails-electron] send failed t=' + obj.t + ' id=' + obj.id + ': ' + e + '\n'); } catch (_) {}
    }
  }
}

function winEvent(id, name, extra) {
  const p = Object.assign({}, extra || {}, { id: id });
  send({ t: 'ev', e: name, p: p });
}

// Electron-initiated requests to the Go host (odd ids).
let reqSeq = 0;
const pendingGo = new Map();
function callGo(method, params) {
  return new Promise((resolve, reject) => {
    const id = ++reqSeq * 2 + 1;
    pendingGo.set(id, { resolve, reject });
    send({ t: 'req', id: id, m: method, p: params });
    setTimeout(() => {
      if (pendingGo.has(id)) {
        pendingGo.delete(id);
        reject(new Error('go ipc timeout: ' + method));
      }
    }, 15000);
  });
}

function respond(id, fn) {
  try {
    const r = fn();
    if (r && typeof r.then === 'function') {
      r.then(
        (v) => send({ t: 'resp', id: id, ok: true, r: v === undefined ? null : v }),
        (e) => send({ t: 'resp', id: id, ok: false, err: String(e) }),
      );
      return;
    }
    send({ t: 'resp', id: id, ok: true, r: r === undefined ? null : r });
  } catch (e) {
    send({ t: 'resp', id: id, ok: false, err: String(e) });
  }
}

function getWindow(p) {
  const w = windows.get(p.id);
  if (!w) throw new Error('unknown window ' + p.id);
  return w;
}

function createWindow(p) {
  const win = new BrowserWindow({
    x: p.x, y: p.y,
    width: p.width || 800, height: p.height || 600,
    title: p.title || '',
    frame: !p.frameless,
    transparent: !!p.transparent,
    resizable: p.resizable !== false,
    alwaysOnTop: !!p.alwaysOnTop,
    show: true,
    backgroundColor: p.transparent ? '#00000000' : undefined,
    webPreferences: {
      preload: p.preload || cfg.preload,
      contextIsolation: false,
      nodeIntegration: false,
      sandbox: false,
    },
  });
  windows.set(p.id, win);
  byWebContents.set(win.webContents.id, p.id);
  const id = p.id;
  // Renderer console → host stderr (the only observability channel for
  // preload/page JS errors on this backend).
  win.webContents.on('console-message', (_e, _level, message, _line, _source) => {
    try { process.stderr.write('[renderer] ' + message + '\n'); } catch (e) {}
  });
  // One-time bootstrap for the native transport addon: paths travel as
  // strings; after connect() the data plane is renderer addon <-> Go.
  // Same injection script as the addon's mainEntry (flags, serde-init
  // call, probe, draggable-region mirror) so both backends stay in
  // lockstep.
  win.webContents.on('did-finish-load', () => {
    const initCfg = JSON.stringify({
      addon: cfg.nativeAddon || '',
      endpoint: cfg.bridgePath || '',
      token: cfg.bridgeToken || '',
    });
    let script =
      'window._wails=window._wails||{};window._wails.flags=window._wails.flags||{};';
    script += 'window._wails.flags.enableFileDrop=' + (!!p.enableFileDrop) + ';';
    script += 'window._wails.flags.frameless=' + (!!p.frameless) + ';';
    script += ';if(window._wails.setResizable)window._wails.setResizable(' + (p.resizable !== false) + ');';
    script += ';typeof __wailsNativeInit === \'function\' && __wailsNativeInit(' + initCfg + ')';
    script += ';\'dnd=\' + typeof window.chrome?.webview?.postMessageWithAdditionalObjects + \';mirror=\' + !!window.__wailsAppRegionMirror';
    // Draggable-region mirror: Wails pages mark drag surfaces with the
    // --wails-draggable custom property, while Electron moves frameless
    // windows natively via -webkit-app-region (the WM-conducted protocol
    // cannot grab while the renderer holds the pointer). Mirror the
    // custom property onto the hovered element so Chromium conducts the
    // move itself — in-process, no cross-process grab contention.
    script += ';(function(){'
      + 'if(window.__wailsAppRegionMirror)return;'
      + 'window.__wailsAppRegionMirror=true;'
      + 'var el=null;'
      + 'function clear(){if(el){el.style.setProperty(\'-webkit-app-region\',\'\');el=null;}}'
      + 'window.addEventListener(\'mousemove\',function(ev){'
      + 'var t=ev.target;'
      + 'if(!(t instanceof Element)){clear();return;}'
      + 'var mode=getComputedStyle(t).getPropertyValue(\'--wails-draggable\').trim();'
      + 'if(mode===\'drag\'){'
      + 'if(el!==t){clear();el=t;t.style.setProperty(\'-webkit-app-region\',\'drag\');}'
      + '}else if(el){clear();}'
      + '},true);'
      + 'window.addEventListener(\'mousedown\',function(ev){'
      + 'if(el&&ev.target===el){el.style.setProperty(\'-webkit-app-region\',\'drag\');}'
      + '},true);'
      + '})()';
    win.webContents.executeJavaScript(script).then((r) => {
      try { process.stderr.write('[wails-electron] renderer probe: ' + r + '\n'); } catch (e) {}
    }, (e) => {
      try { process.stderr.write('[wails-electron] probe executeJavaScript failed: ' + e + '\n'); } catch (_) {}
    });
  });
  win.on('close', () => winEvent(id, 'close'));
  const wcId = win.webContents.id;
  win.on('closed', () => { winEvent(id, 'closed'); windows.delete(id); byWebContents.delete(wcId); });
  win.on('focus', () => winEvent(id, 'focus'));
  win.on('blur', () => winEvent(id, 'blur'));
  win.on('show', () => winEvent(id, 'show'));
  win.on('hide', () => winEvent(id, 'hide'));
  win.on('maximize', () => winEvent(id, 'maximise'));
  win.on('unmaximize', () => winEvent(id, 'unmaximise'));
  win.on('minimize', () => winEvent(id, 'minimise'));
  win.on('restore', () => winEvent(id, 'restore'));
  win.on('enter-full-screen', () => winEvent(id, 'fullscreen'));
  win.on('leave-full-screen', () => winEvent(id, 'unfullscreen'));
  win.on('resize', () => winEvent(id, 'resize', win.getBounds()));
  win.on('move', () => winEvent(id, 'move', win.getBounds()));
  win.webContents.on('render-process-gone', (_e, details) => winEvent(id, 'render-gone', { reason: details.reason }));
  if (p.url) win.loadURL(p.url);
  return win.getBounds();
}

const methods = {
  create: (p) => createWindow(p),
  // destroy(), not close(): the host's Close() is an unconditional tear
  // down (the window layer sets unconditionallyClose), and the graceful
  // close path has been observed to wedge the Electron main loop on X11
  // (async teardown racing the next window operation). destroy() tears
  // the window down synchronously and still fires 'closed'.
  // destroy() outside the IPC dispatch stack: a synchronous destroy in
  // the stdin data handler was observed to stall the uv stdin poll, so
  // the next host request would never be dispatched. Deferring to the
  // next tick lets the reply go out first and the loop keep flowing.
  close: (p) => getWindow(p).destroy(),
  destroy: (p) => getWindow(p).destroy(),
  show: (p) => getWindow(p).show(),
  hide: (p) => getWindow(p).hide(),
  focus: (p) => { const w = getWindow(p); if (w.isMinimized()) w.restore(); w.focus(); },
  setTitle: (p) => getWindow(p).setTitle(p.title),
  setPosition: (p) => getWindow(p).setPosition(p.x, p.y),
  setSize: (p) => getWindow(p).setSize(p.width, p.height),
  setBounds: (p) => getWindow(p).setBounds({ x: p.x, y: p.y, width: p.width, height: p.height }),
  getBounds: (p) => getWindow(p).getBounds(),
  center: (p) => getWindow(p).center(),
  setAlwaysOnTop: (p) => getWindow(p).setAlwaysOnTop(!!p.v),
  setResizable: (p) => getWindow(p).setResizable(!!p.v),
  setFullScreen: (p) => getWindow(p).setFullScreen(!!p.v),
  maximise: (p) => getWindow(p).maximize(),
  unmaximise: (p) => getWindow(p).unmaximize(),
  minimise: (p) => getWindow(p).minimize(),
  unminimise: (p) => getWindow(p).restore(),
  isVisible: (p) => getWindow(p).isVisible(),
  isFocused: (p) => getWindow(p).isFocused(),
  isMinimised: (p) => getWindow(p).isMinimized(),
  isMaximised: (p) => getWindow(p).isMaximized(),
  isFullScreen: (p) => getWindow(p).isFullScreen(),
  execJS: (p) => { getWindow(p).webContents.executeJavaScript(p.js); },
  loadURL: (p) => getWindow(p).loadURL(p.url),
  reload: (p) => getWindow(p).webContents.reload(),
  forceReload: (p) => getWindow(p).webContents.reloadIgnoringCache(),
  openDevTools: (p) => getWindow(p).webContents.openDevTools({ mode: 'detach' }),
  setZoom: (p) => getWindow(p).webContents.setZoomFactor(p.v),
  getZoom: (p) => getWindow(p).webContents.getZoomFactor(),
  setBackgroundColour: (p) => getWindow(p).setBackgroundColor(p.colour),
  setIgnoreMouseEvents: (p) => getWindow(p).setIgnoreMouseEvents(!!p.v),
  copy: (p) => getWindow(p).webContents.copy(),
  paste: (p) => getWindow(p).webContents.paste(),
  cut: (p) => getWindow(p).webContents.cut(),
  undo: (p) => getWindow(p).webContents.undo(),
  redo: (p) => getWindow(p).webContents.redo(),
  selectAll: (p) => getWindow(p).webContents.selectAll(),
  delete: (p) => getWindow(p).webContents.delete(),
  quit: () => app.quit(),
  // Forwards a frontend /wails/runtime HTTP call into the Go host over the
  // control protocol (preload fetch-ipc experiment), returning the response.
};

app.commandLine.appendSwitch('disable-features', 'CalculateNativeWinOcclusion');
for (const s of (process.env.WAILS_ELECTRON_SWITCHES || '').split(' ')) {
  if (s.trim()) app.commandLine.appendSwitch(s.trim());
}
if (process.env.WAILS_ELECTRON_DISABLE_GPU === '1') app.disableHardwareAcceleration();

app.whenReady().then(() => {
  const handleLine = (line) => {
    if (!line.trim()) return;
    let m;
    try { m = JSON.parse(line); } catch (e) { return; }
    if (m.t === 'resp') {
      // response to an Electron-initiated request (callGo)
      const p = pendingGo.get(m.id);
      if (p) {
        pendingGo.delete(m.id);
        if (m.ok) p.resolve(m.r);
        else p.reject(new Error(m.err || 'request failed'));
      }
      return;
    }
    const fn = methods[m.m];
    if (!fn) { send({ t: 'resp', id: m.id, ok: false, err: 'unknown method ' + m.m }); return; }
    if (process.env.WAILS_ELECTRON_DEBUG === '1') {
      try { process.stderr.write('[wails-electron] dispatch ' + m.m + ' id=' + m.id + '\n'); } catch (_) {}
    }
    respond(m.id, () => fn(m.p || {}));
  };
  // The control channel reads fd 0 through fs.read (libuv threadpool)
  // rather than the process.stdin stream: destroying a BrowserWindow has
  // been observed to stall the stream's poll handle, after which host
  // requests were never dispatched again. EOF (n=0) still means the host
  // is gone; orphan protection is additionally covered by the PPID poll.
  let buf = '';
  const controlFd = 0;
  const readControl = () => {
    fs.read(controlFd, controlBuf, 0, controlBuf.length, null, (err, n) => {
      if (err || n === 0) {
        if (process.env.WAILS_ELECTRON_DEBUG === '1') {
          try { process.stderr.write('[wails-electron] control read ended err=' + err + ' n=' + n + '\n'); } catch (_) {}
        }
        app.quit();
        return;
      }
      buf += controlBuf.toString('utf8', 0, n);
      let idx;
      while ((idx = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, idx);
        buf = buf.slice(idx + 1);
        handleLine(line);
      }
      readControl();
    });
  };
  readControl();
  send({ t: 'ev', e: 'ready', p: {} });
});

// The Wails host owns the application lifecycle: closing the last window
// must NOT quit the Electron process (the default), or the host's next
// window creation times out against a dying process.
app.on('window-all-closed', () => {});

process.on('SIGTERM', () => app.quit());
process.on('SIGINT', () => app.quit());

// Orphan protection: if the Wails host dies abruptly (SIGKILL/crash), stdin
// EOF is unreliable inside Electron — poll the parent PID and quit when the
// process gets re-parented.
const originalPpid = process.ppid;
if (process.env.WAILS_ELECTRON_DEBUG === '1') {
  process.stderr.write(`[wails-electron] guard armed, originalPpid=${originalPpid}\n`);
}
setInterval(() => {
  if (process.env.WAILS_ELECTRON_DEBUG === '1') {
    try { process.stderr.write(`[wails-electron] beat, ppid=${process.ppid}\n`); } catch (e) {}
  }
  if (process.ppid !== originalPpid) {
    try { process.stderr.write('[wails-electron] parent died, quitting\n'); } catch (e) {}
    app.quit();
  }
}, 2000);

ipcMain.on('wails:message', (event, msg) => {
  const id = byWebContents.get(event.sender.id) || 0;
  try {
    const probe = JSON.parse(msg);
    if (probe && typeof probe.name === 'string' && probe.name.indexOf('compat:') === 0) {
      process.stderr.write('[wails-electron] ' + probe.name + ' ' + String(probe.data) + '\n');
    }
  } catch (e) { /* not a compat diagnostic */ }
  send({ t: 'ev', e: 'message', p: { id: id, payload: msg } });
});
