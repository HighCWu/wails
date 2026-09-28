// Runtime operations from examples/window-api, using the real browser bridge.
import { Call, Window } from '/wails/runtime.js';

export async function checkWindowAPI(report) {
 await report('upstream:start');
 const position = await Window.Position();
 const cocoa = new URLSearchParams(location.search).get('platform') === 'darwin';
 const settle = async (name, query, expected) => {
  const deadline = performance.now() + 10000;
  while (performance.now() < deadline) {
   if (await query() === expected) { await report('upstream:' + name); return; }
   await new Promise(resolve => setTimeout(resolve, 100));
  }
  throw Error(name + ' did not reach ' + expected);
 };
 await Window.Maximise();
 await settle('maximise', () => Window.IsMaximised(), true);
 await Window.UnMaximise();
 await settle('unmaximise', () => Window.IsMaximised(), false);
 await Window.ToggleMaximise();
 await settle('toggle-maximise', () => Window.IsMaximised(), true);
 await Window.ToggleMaximise();
 await settle('toggle-restore', () => Window.IsMaximised(), false);
 await Window.Fullscreen();
 // AppKit changes its style mask before finishing the animation. A reverse
 // toggle then is rejected. Require the host's completion event as well.
 if (cocoa) await settle('fullscreen-event', () => window.cefNativeFullscreen, true);
 await settle('fullscreen', () => Window.IsFullscreen(), true);
 await Window.UnFullscreen();
 if (cocoa) await settle('unfullscreen-event', () => window.cefNativeFullscreen, false);
 await settle('unfullscreen', () => Window.IsFullscreen(), false);
 await Window.Center();
 await Call.ByName('main.ProbeService.WindowLifecycle');
 await Window.SetPosition(position.x, position.y);
 await report('upstream:window-api:passed');
}
