// The renderer surface lives entirely in the native addon (bridge.cc):
// postMessage shim, __wailsNativeInit, the fetch override, and all
// serialization. This file is only the loader.
const addon = process.env.WAILS_ELECTRON_NATIVE_ADDON;
if (addon) {
  try {
    require(addon).preloadInit(require('electron'));
  } catch (e) {
    try {
      process.stderr.write('[wails-electron preload] addon load failed: ' +
        (e && e.message || e) + '\n');
    } catch (_) {}
  }
}
