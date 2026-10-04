// The entire main-process surface lives in the native addon (bridge.cc —
// pure N-API on every platform): window management, the stdio control
// protocol, event forwarding, and the orphan guards. This file only
// routes the config to the addon and loads it.
const cfg = JSON.parse(process.env.WAILS_ELECTRON_CONFIG || '{}');
if (cfg.nativeAddon) process.env.WAILS_ELECTRON_NATIVE_ADDON = cfg.nativeAddon;
if (cfg.bridgePath) process.env.WAILS_ELECTRON_BRIDGE_PATH = cfg.bridgePath;
const path = require('path');
// resolve relative addon paths against the host's cwd — a bare specifier
// would go through node_modules lookup and miss the file entirely
let addon = process.env.WAILS_ELECTRON_NATIVE_ADDON;
if (addon && !path.isAbsolute(addon)) addon = path.join(process.cwd(), addon);
if (!addon) {
  process.stderr.write(
    '[wails-electron] fatal: no native addon configured (WAILS_ELECTRON_NATIVE_ADDON)\n');
  process.exit(1);
}
// Context menus stay in pure JS on purpose: Menu.buildFromTemplate +
// popup() from a plain JS task renders correctly, while the same calls
// issued through the native addon never render the popup on linux.
// The addon forwards the serialized spec; clicks report back as raw
// contextmenu-select events on stdout (same channel as the addon's).
globalThis.__wailsShowContextMenu = (json) => {
  const spec = JSON.parse(json);
  const { Menu, BrowserWindow } = require('electron');
  const conv = (items) => items.map((it) => {
    if (it.type === 'separator') return { type: 'separator' };
    const out = { label: it.label, enabled: it.enabled };
    if (it.type === 'checkbox' || it.type === 'radio') {
      out.type = it.type;
      out.checked = !!it.checked;
    }
    if (it.accelerator) out.accelerator = it.accelerator;
    if (it.menu) out.submenu = conv(it.menu);
    out.click = () => {
      process.stdout.write(JSON.stringify({
        t: 'ev', e: 'contextmenu-select', p: { id: spec.id, uid: it.uid },
      }) + '\n');
    };
    return out;
  });
  const win = spec.id ? BrowserWindow.fromId(spec.id) : undefined;
  // defer into a fresh macrotask: popup() called synchronously from the
  // control-plane dispatch (a uv_async frame) never renders the menu on
  // linux — from a plain task it does (isolation test verified).
  setTimeout(() => {
    const menu = Menu.buildFromTemplate(conv(spec.menu));
    menu.popup({ window: win, x: spec.x, y: spec.y });
  }, 0);
};

// Menubar (window.setMenu / application menu): same pure-JS build path as
// the context menu. spec = {id, menu} — id 0 means the application menu
// (Menu.setApplicationMenu), any other id targets that BrowserWindow.
// Item clicks report back as menu-click events (uid-carried, same channel).
globalThis.__wailsSetMenu = (json) => {
  const spec = JSON.parse(json);
  const { Menu, BrowserWindow } = require('electron');
  const conv = (items) => items.map((it) => {
    if (it.type === 'separator') return { type: 'separator' };
    const out = { label: it.label, enabled: it.enabled };
    if (it.type === 'checkbox' || it.type === 'radio') {
      out.type = it.type;
      out.checked = !!it.checked;
    }
    if (it.accelerator) out.accelerator = it.accelerator;
    if (it.menu) out.submenu = conv(it.menu);
    out.click = () => {
      process.stdout.write(JSON.stringify({
        t: 'ev', e: 'menu-click', p: { id: spec.id, uid: it.uid },
      }) + '\n');
    };
    return out;
  });
  const menu = Menu.buildFromTemplate(conv(spec.menu));
  if (spec.id) {
    const win = BrowserWindow.fromId(spec.id);
    if (win) win.setMenu(menu);
  } else {
    Menu.setApplicationMenu(menu);
  }
  // report as a control-plane event: the Go side logs it, which keeps
  // the marker visible on every platform (electron stderr does not reach
  // the host log on darwin)
  process.stdout.write(JSON.stringify({
    t: 'ev', e: 'menu-set', p: { id: spec.id },
  }) + String.fromCharCode(10));
};

// System tray: same pure-JS build path as the menus. One entry point —
// action 'create' makes the Tray (icon/tooltip/menu), 'seticon' /
// 'settooltip' / 'setmenu' mutate it, 'bounds' returns getBounds(),
// 'destroy' tears it down. Menu clicks report tray-menu-click; bare
// clicks report tray-click etc. (click events only fire when no context
// menu is attached — the menu opens natively on left/right click then).
globalThis.__wailsTrays = new Map();
globalThis.__wailsTrayMenu = (items, trayId) => {
  const { Menu } = require('electron');
  const conv = (items) => items.map((it) => {
    if (it.type === 'separator') return { type: 'separator' };
    const out = { label: it.label, enabled: it.enabled };
    if (it.type === 'checkbox' || it.type === 'radio') {
      out.type = it.type;
      out.checked = !!it.checked;
    }
    if (it.accelerator) out.accelerator = it.accelerator;
    if (it.menu) out.submenu = conv(it.menu);
    out.click = () => {
      process.stdout.write(JSON.stringify({
        t: 'ev', e: 'tray-menu-click', p: { id: trayId, uid: it.uid },
      }) + '\n');
    };
    return out;
  });
  return Menu.buildFromTemplate(conv(items));
};
globalThis.__wailsTray = (json) => {
  const spec = JSON.parse(json);
  const { Tray, nativeImage } = require('electron');
  if (spec.action === 'create') {
    const tray = new Tray(nativeImage.createFromDataURL(spec.icon));
    if (spec.tooltip) tray.setToolTip(spec.tooltip);
    if (spec.menu) tray.setContextMenu(__wailsTrayMenu(spec.menu, spec.id));
    const emit = (name) => () => process.stdout.write(JSON.stringify({
      t: 'ev', e: name, p: { id: spec.id },
    }) + '\n');
    tray.on('click', emit('tray-click'));
    tray.on('right-click', emit('tray-right-click'));
    tray.on('double-click', emit('tray-double-click'));
    tray.on('mouse-enter', emit('tray-mouse-enter'));
    tray.on('mouse-leave', emit('tray-mouse-leave'));
    globalThis.__wailsTrays.set(spec.id, tray);
    return;
  }
  const tray = globalThis.__wailsTrays.get(spec.id);
  if (!tray) return;
  if (spec.action === 'seticon') tray.setImage(nativeImage.createFromDataURL(spec.icon));
  else if (spec.action === 'setlabel') tray.setTitle(spec.label || '');
  else if (spec.action === 'settooltip') tray.setToolTip(spec.tooltip || '');
  else if (spec.action === 'setmenu') tray.setContextMenu(spec.menu ? __wailsTrayMenu(spec.menu, spec.id) : null);
  else if (spec.action === 'bounds') return tray.getBounds();
  else if (spec.action === 'destroy') {
    tray.destroy();
    globalThis.__wailsTrays.delete(spec.id);
  }
};

require(addon).mainEntry(require('electron'), cfg);
