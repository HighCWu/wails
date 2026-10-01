// Entry router. Two control-plane backends:
//   - linux (default): the v8-direct addon's mainEntry — the full
//     main-process surface lives in bridge.cc.
//   - windows: the v8-direct addon cannot exist there (electron.exe
//     exports only the N-API surface, no node.lib is published), so v1
//     runs this JS control plane (main-js.js) + the N-API renderer addon
//     (napi_bridge.cc). The control protocol is low-frequency JSON lines
//     where N-API buys nothing.
// WAILS_ELECTRON_MAIN_JS=1 forces the JS plane everywhere — it lets the
// linux dev/CI run the exact windows combo end-to-end.
const cfg = JSON.parse(process.env.WAILS_ELECTRON_CONFIG || '{}');
if (cfg.nativeAddon) process.env.WAILS_ELECTRON_NATIVE_ADDON = cfg.nativeAddon;
if (cfg.bridgePath) process.env.WAILS_ELECTRON_BRIDGE_PATH = cfg.bridgePath;
if (process.platform === 'win32' || process.env.WAILS_ELECTRON_MAIN_JS === '1') {
  require('./main-js.js')(require('electron'), cfg);
  return;
}
if (!process.env.WAILS_ELECTRON_NATIVE_ADDON) {
  process.stderr.write(
    '[wails-electron] fatal: no native addon configured (WAILS_ELECTRON_NATIVE_ADDON)\n');
  process.exit(1);
}
require(process.env.WAILS_ELECTRON_NATIVE_ADDON).mainEntry(
  require('electron'), cfg);
