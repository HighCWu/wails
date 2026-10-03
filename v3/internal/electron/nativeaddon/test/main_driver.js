// Driver: feeds control lines to the harness child, asserts responses.
const { spawn } = require('child_process');
const child = spawn('node', [__dirname + '/main_harness.js'], { stdio: ['pipe', 'pipe', 'inherit'], env: { ...process.env, WAILS_ELECTRON_DEBUG: '1' } });
let buf = '';
const responses = [];
child.stdout.on('data', (d) => {
  buf += d.toString();
  let i;
  while ((i = buf.indexOf('\n')) >= 0) {
    const line = buf.slice(0, i);
    buf = buf.slice(i + 1);
    try { responses.push(JSON.parse(line)); } catch (e) {}
  }
});
function send(obj) { child.stdin.write(JSON.stringify(obj) + '\n'); }
function waitFor(pred, ms = 4000) {
  return new Promise((res, rej) => {
    const t0 = Date.now();
    const iv = setInterval(() => {
      const r = responses.find(pred);
      if (r) { clearInterval(iv); res(r); }
      else if (Date.now() - t0 > ms) { clearInterval(iv); rej(new Error('timeout')); }
    }, 20);
  });
}
(async () => {
  send({ t: 'req', id: 2, m: 'create', p: { id: 1, width: 900, height: 700, title: 'T', preload: '/tmp/fake-preload.js', url: 'http://x/' } });
  const cr = await waitFor((r) => r.id === 2);
  console.log('create resp:', JSON.stringify(cr));
  if (!(cr.ok && cr.r && cr.r.width === 900 && cr.r.height === 700)) {
    console.log('FAIL: create params did not flow (want 900x700)');
    process.exit(1);
  }
  send({ t: 'req', id: 4, m: 'setSize', p: { id: 1, width: 640, height: 480 } });
  const sr = await waitFor((r) => r.id === 4);
  console.log('setSize resp:', JSON.stringify(sr));
  if (!(sr.ok && sr.r === null)) { console.log('FAIL setSize'); process.exit(1); }
  send({ t: 'req', id: 6, m: 'isMaximised', p: { id: 1 } });
  send({ t: 'req', id: 8, m: 'maximise', p: { id: 1 } });
  send({ t: 'req', id: 10, m: 'isMaximised', p: { id: 1 } });
  const mx = await waitFor((r) => r.id === 10);
  console.log('maximise resp:', JSON.stringify(mx));
  if (!(mx.ok && mx.r === true)) { console.log('FAIL maximise'); process.exit(1); }
  send({ t: 'req', id: 12, m: 'setMinimumSize', p: { id: 1, width: 200, height: 150 } });
  await waitFor((r) => r.id === 12);
  // nested submenu rides the 'menu' JSON key (the serialized template is
  // what main.js's conv() walks — an 'items' key would silently drop it)
  send({ t: 'req', id: 20, m: 'setMenu', p: { id: 1, menu: [
    { type: 'submenu', label: 'Parent', uid: 8, menu: [{ type: 'normal', label: 'Nested', uid: 9 }] },
    { type: 'normal', label: 'X', uid: 10 },
  ] } });
  const sm = await waitFor((r) => r.id === 20);
  console.log('setMenu resp:', JSON.stringify(sm));
  if (!sm.ok) { console.log('FAIL setMenu'); process.exit(1); }
  const menudbg = responses.find((r) => r.e === 'menu-debug');
  if (!menudbg) { console.log('FAIL: __wailsSetMenu not invoked'); process.exit(1); }
  const spec = JSON.parse(menudbg.p.json);
  if (!spec.menu[0].menu || spec.menu[0].menu[0].label !== 'Nested') {
    console.log('FAIL: nested submenu lost in serialization:', menudbg.p.json.slice(0, 120));
    process.exit(1);
  }
  console.log('setMenu forwarded with nested submenu intact');
  send({ t: 'req', id: 22, m: 'trayOp', p: { id: 5, action: 'create', icon: 'data:image/png;base64,x' } });
  const to = await waitFor((r) => r.id === 22);
  console.log('trayOp create resp:', JSON.stringify(to));
  if (!to.ok) { console.log('FAIL trayOp create'); process.exit(1); }
  send({ t: 'req', id: 24, m: 'trayOp', p: { id: 5, action: 'bounds' } });
  const tb = await waitFor((r) => r.id === 24);
  console.log('trayOp bounds resp:', JSON.stringify(tb));
  if (!(tb.ok && tb.r && tb.r.width === 16)) { console.log('FAIL trayOp bounds'); process.exit(1); }
  send({ t: 'req', id: 13, m: 'showOpenDialog', p: { windowID: 1, title: 'Open', filters: [{ name: 'Text', extensions: ['txt'] }], properties: ['openFile', 'multiSelections'] } });
  const od = await waitFor((r) => r.id === 13);
  console.log('openDialog resp:', JSON.stringify(od));
  if (!(od.ok && od.r && od.r.filePaths && od.r.filePaths.length === 2)) {
    console.log('FAIL showOpenDialog');
    process.exit(1);
  }
  send({ t: 'req', id: 15, m: 'showMessageDialog', p: { windowID: 1, type: 'question', message: 'go?', buttons: ['Yes', 'No'], defaultId: 0, cancelId: 1 } });
  const md = await waitFor((r) => r.id === 15);
  console.log('messageDialog resp:', JSON.stringify(md));
  if (!(md.ok && md.r && md.r.response === 0)) { console.log('FAIL showMessageDialog'); process.exit(1); }
  // context menu round trip: Go serializes, addon builds + pops, the
  // fake click sends the select event back as a stdout ev line
  let ctxSelect = null;
  child.stdout.on('data', ctxTap);
  function ctxTap(d) {
    // reuse the main parser: onMsg already pushes to responses; we look
    // for the ev line below via responses scan
  }
  send({ t: 'req', id: 16, m: 'showContextMenu', p: { id: 1, x: 30, y: 40, menu: [
    { type: 'normal', label: 'Cut', enabled: true, uid: 101 },
    { type: 'separator' },
    { type: 'checkbox', label: 'Wrap', enabled: true, checked: true, uid: 102 },
    { type: 'submenu', label: 'More', enabled: true, uid: 103, items: [
      { type: 'normal', label: 'Deep', enabled: true, uid: 104 },
    ] },
  ] } });
  const cm = await waitFor((r) => r.id === 16);
  console.log('contextMenu resp:', JSON.stringify(cm));
  if (!cm.ok) { console.log('FAIL showContextMenu'); process.exit(1); }
  // the fake click fires the first clickable item (uid 101) → ev line
  const sel = await waitFor((r) => r.t === 'ev' && r.e === 'contextmenu-select' && r.p.uid === 101, 4000);
  console.log('contextmenu-select:', JSON.stringify(sel));
  if (!(sel.p.id === 1 && sel.p.uid === 101)) { console.log('FAIL select event'); process.exit(1); }
  send({ t: 'req', id: 14, m: 'getScreens', p: {} });
  const sc = await waitFor((r) => r.id === 14);
  console.log('getScreens resp:', JSON.stringify(sc));
  if (!(sc.ok && Array.isArray(sc.r) && sc.r.length === 2 && sc.r[0].IsPrimary === true &&
        sc.r[1].ScaleFactor === 2 && sc.r[1].X === 1920 &&
        sc.r[1].PhysicalBounds.Width === 2560)) {
    console.log('FAIL getScreens');
    process.exit(1);
  }
  // events: fire the maximize event on the window and expect an ev line
  console.log('MAIN-HARNESS PASS');
  child.kill();
  process.exit(0);
})().catch((e) => { console.log('DRIVER FAIL', e.message); process.exit(1); });
