globalThis.__wailsTray = (json) => { const spec = JSON.parse(json); if (spec.action === 'bounds') return { x: 1, y: 2, width: 16, height: 16 }; };
// Main-surface harness: fake electron + mainEntry; runs as a child whose
// stdin/stdout ARE the control protocol the addon reads/writes.
const v8 = require('v8');
const addon = require(process.env.WAILS_ELECTRON_TEST_ADDON || 'go_bridge_cpp.node');

const windows = [];
function makeWin(opts) {
  const wc = {
    id: 7, _ev: [],
    on(ev, fn) { wc._ev.push([ev, fn]); },
    executeJavaScript(s) { return Promise.resolve('probe-ok:' + s.length); },
  };
  const win = {
    _opts: opts, _calls: [], webContents: wc, _ev: [],
    on(ev, fn) { win._ev.push([ev, fn]); },
    destroy() { win._calls.push('destroy'); },
    show() { win._calls.push('show'); },
    setSize(w, h) { win._calls.push(['setSize', w, h]); opts.width = w; opts.height = h; },
    getBounds() { return { x: opts.x || 0, y: opts.y || 0, width: opts.width, height: opts.height }; },
    isMinimized() { return false; },
    focus() { win._calls.push('focus'); },
    loadURL(u) { win._calls.push(['loadURL', u]); return Promise.resolve(); },
    setParentWindow(p) { win._calls.push(['setParent', !!p]); },
    setMinimumSize(w, h) { win._calls.push(['setMin', w, h]); },
    setMaximumSize(w, h) { win._calls.push(['setMax', w, h]); },
    maximize() { opts._max = true; },
    isMaximized() { return !!opts._max; },
  };
  windows.push(win);
  return win;
}

const electronStub = {
  Menu: function (template) {
    this._template = template;
  },
  dialog: {
    showOpenDialog(win, opts) {
      return Promise.resolve({ canceled: false, filePaths: ['/tmp/a.txt', '/tmp/b.txt'] });
    },
    showSaveDialog(win, opts) {
      return Promise.resolve({ canceled: true, filePath: undefined });
    },
    showMessageBox(win, opts) {
      return Promise.resolve({ response: opts.defaultId || 0, checkboxChecked: false });
    },
  },
  screen: {
    getAllDisplays() {
      return [
        { id: 1, label: 'DP-1', bounds: { x: 0, y: 0, width: 1920, height: 1080 },
          workArea: { x: 0, y: 27, width: 1920, height: 1053 }, scaleFactor: 1, rotation: 0 },
        { id: 2, label: 'HDMI-0', bounds: { x: 1920, y: 0, width: 1280, height: 720 },
          workArea: { x: 1920, y: 0, width: 1280, height: 720 }, scaleFactor: 2, rotation: 0 },
      ];
    },
    getPrimaryDisplay() { return this.getAllDisplays()[0]; },
  },
  app: {
    _cb: {},
    on(ev, fn) { this._cb[ev] = fn; },
    commandLine: { appendSwitch() {} },
    whenReady() { return Promise.resolve(); },
    quit() { process.exit(0); },
    disableHardwareAcceleration() {},
  },
  BrowserWindow: function (opts) { return makeWin(opts); },
  ipcMain: { on() {}, handle() {} },
};
electronStub.BrowserWindow.prototype = {};
electronStub.Menu.buildFromTemplate = function (template) {
  return template.map((it) => ({ ...it,
    click: it.click || (() => {}) }));
};
globalThis.__wailsSetMenu = (json) => { process.stdout.write(JSON.stringify({ t: 'ev', e: 'menu-debug', p: { json: json } }) + String.fromCharCode(10)); };
globalThis.__wailsShowContextMenu = (json) => {
  const spec = JSON.parse(json);
  process.stderr.write('[fake] ctxmenu popup id=' + spec.id + ' x=' + spec.x +
    ' y=' + spec.y + ' menu=' + JSON.stringify(spec.menu).slice(0, 160) + '\n');
  const find = (items) => {
    for (const it of items) {
      if (it.type === 'separator') continue;
      if (it.items) { const r = find(it.items); if (r) return r; }
      return it;
    }
    return null;
  };
  const hit = find(spec.menu);
  if (hit) setImmediate(() => {
    process.stdout.write(JSON.stringify({
      t: 'ev', e: 'contextmenu-select', p: { id: spec.id, uid: hit.uid },
    }) + '\n');
  });
};
electronStub.Menu.prototype.popup = function (opts) {
  process.stderr.write('[fake] popup x=' + opts.x + ' y=' + opts.y + ' items=' +
    JSON.stringify(this._template).slice(0, 160) + '\n');
  // simulate a user click on the first clickable item
  const find = (items) => {
    for (const it of items) {
      if (it.type === 'separator') continue;
      if (it.submenu) { const r = find(it.submenu); if (r) return r; }
      else if (it.click) return it;
    }
    return null;
  };
  const hit = find(this._template);
  if (hit) setImmediate(() => hit.click());
};

process.on('message', () => {});
// drain our own stdout noise? none.
addon.mainEntry(electronStub, {
  preload: __dirname + '/fake-preload.js',
  bridgePath: '/tmp/wails-test-bridge-' + process.pid + '.sock',
  bridgeToken: 'tok',
  nativeAddon: process.env.WAILS_ELECTRON_TEST_ADDON || 'go_bridge_cpp.node',
});
process.stderr.write('[harness] mainEntry returned\n');
// keep alive
setInterval(() => {}, 1000);
