// Corpus test: byte-compare the addon's fast serde against node's
// v8.serialize, and round-trip both directions.
const v8 = require('v8');
const util = require('util');
const addon = require(process.env.WAILS_ELECTRON_TEST_ADDON || 'go_bridge_cpp.node');

const long = 'x'.repeat(4096);
const cjk = '中文测试'.repeat(50);
const corpus = [
  undefined, null, true, false,
  0, 1, -1, 42, -42, 127, 128, 255, 256,
  2 ** 31 - 1, -(2 ** 31), 2 ** 31, 4294967295, 2 ** 32,
  0.5, -3.14, 1e300, NaN, Infinity, -Infinity,
  '', 'hello', long, 'é', 'ÿ', cjk, '👨‍👩‍👧‍👦', 'mixed 中文 ascii',
  [], [1, 2, 3], [[]], [[1, [2]], 3], ['a', 1, true, null],
  {}, { a: 1 }, { a: 'x', b: 2.5, c: null, d: [1, { e: false }] },
  { 中文: '值' }, { '': 0 }, { long: long, cjk: cjk },
  new Uint8Array(0), new Uint8Array([1, 2, 3]),
  Buffer.from('buffer-data'), Buffer.alloc(1024, 7),
  { payload: new Uint8Array([9, 8, 7]), n: 1, s: 'str', arr: [new Uint8Array([1])] },
  // nested depth
  (() => { let o = { leaf: 1 }; for (let i = 0; i < 30; i++) o = { w: o }; return o; })(),
];

let pass = 0, skip = 0, fail = 0;
for (const v of corpus) {
  const want = v8.serialize(v);
  const got = addon._fastSerialize(v);
  if (got === undefined) {
    skip++;
    continue;
  }
  if (Buffer.compare(want, got) !== 0) {
    fail++;
    console.log('MISMATCH for', JSON.stringify(v).slice(0, 80));
    console.log('  node:', want.toString('hex').slice(0, 120));
    console.log('  fast:', got.toString('hex').slice(0, 120));
    continue;
  }
  // round-trip through our decoder on node's bytes. Deep-equality (not
  // byte re-serialization): node 25 re-encodes decoded arrays with its
  // new 0x61 tags, and our decoder deliberately returns Buffer for
  // 0x5C host objects (same as the v8-direct path / Go's []byte).
  if (v !== undefined) {
    const back = addon._fastDeserialize(want);
    const wantVal = v8.deserialize(want);
    const eq = (a, b) => {
      if (a instanceof Uint8Array && b instanceof Uint8Array) {
        return Buffer.compare(Buffer.from(a), Buffer.from(b)) === 0;
      }
      if (Array.isArray(a) && Array.isArray(b)) {
        return a.length === b.length && a.every((x, i) => eq(x, b[i]));
      }
      if (a && b && typeof a === 'object' && typeof b === 'object') {
        const ka = Object.keys(a), kb = Object.keys(b);
        return ka.length === kb.length && ka.every((k) => eq(a[k], b[k]));
      }
      return util.isDeepStrictEqual(a, b) ||
        (a === undefined && b === null) || (a === null && b === undefined);
    };
    if (!eq(back, wantVal)) {
      fail++;
      const label = JSON.stringify(v, (k, x) => x === undefined ? null : x);
      console.log('ROUNDTRIP MISMATCH for', String(label).slice(0, 80));
      console.log('  back:', JSON.stringify(back).slice(0, 90));
      console.log('  node:', JSON.stringify(wantVal).slice(0, 90));
      continue;
    }
  }
  pass++;
}
console.log(`corpus: ${pass} pass, ${skip} fallback, ${fail} fail`);

// fallback correctness: unsupported shapes must hit the JS serde path
// inside the real endpoints — simulate by checking the frame-level echo
// through a live connection in the smoke test instead.

// Go-encoder fixtures (from internal/v8serde tests) decode identically
const goFixtures = [
  ['ff0f6f22016149027b01', { a: 1 }],
  ['ff0f4103490249044906240003', [1, 2, 3]],
  ['ff0f6f2201645c01030102037b01', { d: Buffer.from([1, 2, 3]) }],
  ['ff0f4102220576616c75650063082d4e8765938f6551240002', ['value', '中文输入']],
];
let gfail = 0;
for (const [hex, want] of goFixtures) {
  const got = addon._fastDeserialize(Buffer.from(hex, 'hex'));
  const a = JSON.stringify(got), b = JSON.stringify(want);
  if (a !== b) { gfail++; console.log('GO-FIXTURE MISMATCH', hex, a, b); }
}
console.log(`go fixtures: ${goFixtures.length - gfail}/${goFixtures.length} pass`);
process.exit(fail || gfail ? 1 : 0);
