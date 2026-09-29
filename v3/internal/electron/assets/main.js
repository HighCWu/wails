// Wails Electron backend bootstrap: owns every BrowserWindow and speaks the
// JSON-lines control protocol on stdin/stdout.
//   request : {"t":"req","id":N,"m":"<method>","p":{...,"id":windowID}}
//   response: {"t":"resp","id":N,"ok":true,"r":{...}} | {"t":"resp","id":N,"ok":false,"err":"..."}
//   event   : {"t":"ev","e":"<name>","p":{...,"id":windowID}}
// id space: even ids are Go-initiated, odd ids are Electron-initiated.
// Quitting: when stdin closes the host is gone — exit immediately so no
// orphaned Electron processes outlive the Wails application.
const { app, BrowserWindow, ipcMain } = require('electron');

const cfg = JSON.parse(process.env.WAILS_ELECTRON_CONFIG || '{}');
const windows = new Map(); // windowID -> BrowserWindow
const byWebContents = new Map(); // webContents -> windowID

function send(obj) {
  try { process.stdout.write(JSON.stringify(obj) + '\n'); } catch (e) { /* host gone */ }
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
  byWebContents.set(win.webContents, p.id);
  const id = p.id;
  win.on('close', () => winEvent(id, 'close'));
  win.on('closed', () => { winEvent(id, 'closed'); windows.delete(id); byWebContents.delete(win.webContents); });
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
  close: (p) => getWindow(p).close(),
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
  webviewRequest: (p) => callGo('webviewRequest', p),
};

app.commandLine.appendSwitch('disable-features', 'CalculateNativeWinOcclusion');
for (const s of (process.env.WAILS_ELECTRON_SWITCHES || '').split(' ')) {
  if (s.trim()) app.commandLine.appendSwitch(s.trim());
}
if (process.env.WAILS_ELECTRON_DISABLE_GPU === '1') app.disableHardwareAcceleration();

app.whenReady().then(() => {
  let buf = '';
  process.stdin.setEncoding('utf8');
  process.stdin.on('data', (chunk) => {
    buf += chunk;
    let idx;
    while ((idx = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, idx);
      buf = buf.slice(idx + 1);
      if (!line.trim()) continue;
      let m;
      try { m = JSON.parse(line); } catch (e) { continue; }
      if (m.t === 'resp') {
        // response to an Electron-initiated request (callGo)
        const p = pendingGo.get(m.id);
        if (p) {
          pendingGo.delete(m.id);
          if (m.ok) p.resolve(m.r);
          else p.reject(new Error(m.err || 'request failed'));
        }
        continue;
      }
      const fn = methods[m.m];
      if (!fn) { send({ t: 'resp', id: m.id, ok: false, err: 'unknown method ' + m.m }); continue; }
      respond(m.id, () => fn(m.p || {}));
    }
  });
  process.stdin.on('end', () => app.quit());
  process.stdin.on('close', () => app.quit());
  send({ t: 'ev', e: 'ready', p: {} });
});

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
  const id = byWebContents.get(event.sender) || 0;
  send({ t: 'ev', e: 'message', p: { id: id, payload: msg } });
});

// Preload fetch-ipc experiment: a /wails/runtime call forwarded from the
// renderer is routed over the control protocol to the Go host.
ipcMain.handle('wails:runtime', (_event, payload) => callGo('webviewRequest', payload || {}));
