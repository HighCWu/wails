// Renderer bootstrap loader. The renderer surface itself lives in the
// addon selected by the platform:
//   - linux: bridge.cc (v8-direct) — ignores the second argument;
//   - windows: napi_bridge.cc (N-API) — takes require('v8') as the serde
//     injection (node's v8.serialize emits exactly the wire format the
//     Go decoder speaks).
const addon = process.env.WAILS_ELECTRON_NATIVE_ADDON;
if (addon) {
  try {
    require(addon).preloadInit(require('electron'), require('v8'));
  } catch (e) {
    try {
      process.stderr.write('[wails-electron preload] addon load failed: ' +
        (e && e.message || e) + '\n');
    } catch (_) {}
  }
}
