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

require(addon).mainEntry(require('electron'), cfg);
