// reconnect regression: a second __wailsNativeInit in the same context
// (simulating a re-injection / same-process reload) must (1) tear down the
// previous connection so the Go accept loop sees EOF, (2) keep the fetch
// chain intact — the second pass must not capture our own override as the
// "original" fetch, which would recurse infinitely on plain-http calls.
const { spawn } = require('child_process');
const assert = require('assert');

const SOCK = '/tmp/reconn-test-' + process.pid + '.sock';
const server = spawn('node', [__dirname + '/reconn_server.js', SOCK]);
const events = [];
let buf = '';
server.stdout.on('data', d => {
  buf += d.toString();
  let i;
  while ((i = buf.indexOf('\n')) >= 0) {
    const line = buf.slice(0, i); buf = buf.slice(i + 1);
    if (line.trim()) events.push(JSON.parse(line));
  }
});

const addon = require(process.env.WAILS_ELECTRON_TEST_ADDON || 'go_bridge_cpp.node');
addon.preloadInit({}, undefined);  // fake electron: ipcRenderer optional

const wait = ms => new Promise(r => setTimeout(r, ms));

async function main() {
  assert(await until(e => e.ev === 'listening'), 'server did not start');
  const cfg = { endpoint: SOCK, token: 'tok' };

  globalThis.__wailsNativeInit(cfg);
  const e1 = await globalThis.__nativeEcho({ a: 1 });
  assert.deepStrictEqual(e1, { a: 1 });

  // re-injection in the SAME context: must not leak conn #1
  globalThis.__wailsNativeInit(cfg);
  const e2 = await globalThis.__nativeEcho({ b: 2 });
  assert.deepStrictEqual(e2, { b: 2 });
  await wait(300);

  const hellos = events.filter(e => e.ev === 'hello').length;
  const closes = events.filter(e => e.ev === 'close').length;
  assert.strictEqual(hellos, 2, `expected 2 handshakes, got ${hellos}; ${JSON.stringify(events)}`);
  assert.strictEqual(closes, 1, `conn1 must have EOF'd exactly once, closes=${closes}; ${JSON.stringify(events)}`);

  // fetch chain guard: a plain-http fetch must resolve through the real
  // original fetch (a poisoned chain throws RangeError from recursion)
  const http = require('http');
  const hs = http.createServer((req, res) => res.end('plain-ok'));
  await new Promise(r => hs.listen(0, '127.0.0.1', r));
  const port = hs.address().port;
  const out = await fetch(`http://127.0.0.1:${port}/x`).then(r => r.text());
  assert.strictEqual(out, 'plain-ok');
  hs.close();

  console.log('RECONN-TEST PASS');
  console.log('events:', events.map(e => e.ev + (e.payload !== undefined ? ':' + JSON.stringify(e.payload) : '')).join(' | '));
  try { addon.close(); } catch {}
  server.kill();
  process.exit(0);
}

async function until(pred, what, tries = 100) {
  for (let i = 0; i < tries; i++) {
    if (events.some(pred)) return true;
    await wait(50);
  }
  console.error('timeout waiting:', what, JSON.stringify(events));
  return false;
}

main().catch(e => {
  console.error('RECONN-TEST FAIL:', e.message);
  server.kill();
  process.exit(1);
});
