// The entire main-process surface lives in the native addon (bridge.cc —
// pure N-API on every platform): window management, the stdio control
// protocol, event forwarding, and the orphan guards. This file only
// routes the config to the addon and loads it.
const cfg = JSON.parse(process.env.WAILS_ELECTRON_CONFIG || '{}');
if (cfg.nativeAddon) process.env.WAILS_ELECTRON_NATIVE_ADDON = cfg.nativeAddon;
if (cfg.bridgePath) process.env.WAILS_ELECTRON_BRIDGE_PATH = cfg.bridgePath;
if (!process.env.WAILS_ELECTRON_NATIVE_ADDON) {
  process.stderr.write(
    '[wails-electron] fatal: no native addon configured (WAILS_ELECTRON_NATIVE_ADDON)\n');
  process.exit(1);
}
require(process.env.WAILS_ELECTRON_NATIVE_ADDON).mainEntry(
  require('electron'), cfg);
