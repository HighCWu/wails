// Renderer bootstrap loader. The renderer surface itself lives in the
// addon selected by the platform build (bridge.cc — one N-API source on
// every platform; the second argument is node's v8 module, the serde
// fallback path).
const path = require('path');
let addon = process.env.WAILS_ELECTRON_NATIVE_ADDON;
if (addon && !path.isAbsolute(addon)) addon = path.join(process.cwd(), addon);
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
