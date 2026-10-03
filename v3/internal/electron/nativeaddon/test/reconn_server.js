// fake go-bridge accept server (child process): hello handshake + echo
// roundtrip over the v8serde wire, events as JSON lines on stdout
const net = require('net');
const v8 = require('v8');

const SOCK = process.argv[2];
try { require('fs').unlinkSync(SOCK); } catch {}

const server = net.createServer(sock => {
  sock.on('data', d => {
    let off = 0;
    while (off + 4 <= d.length) {
      const len = d.readUInt32LE(off);
      const frame = d.slice(off + 4, off + 4 + len);
      off += 4 + len;
      const text = frame.toString('utf8');
      if (text.includes('"hello"')) {
        say({ ev: 'hello' });
        reply(sock, Buffer.from(JSON.stringify({ t: 'ready' })));
      } else {
        const msg = v8.deserialize(frame);
        say({ ev: 'echo', payload: msg.payload });
        reply(sock, v8.serialize({ t: 'resp', id: msg.id, ok: true, payload: msg.payload }));
      }
    }
  });
  sock.on('close', () => say({ ev: 'close' }));
  sock.on('error', e => say({ ev: 'sockerr', msg: e.code || e.message }));
});

function reply(sock, body) {
  const h = Buffer.alloc(4);
  h.writeUInt32LE(body.length);
  sock.write(Buffer.concat([h, body]));
}
function say(o) { process.stdout.write(JSON.stringify(o) + '\n'); }

server.listen(SOCK, () => say({ ev: 'listening', sock: SOCK }));
