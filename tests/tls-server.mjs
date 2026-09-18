import tls from 'node:tls';
import net from 'node:net';
import { readFileSync, writeFileSync } from 'node:fs';
import { randomBytes, X509Certificate } from 'node:crypto';
import { spawn } from 'node:child_process';

const root = 'tests/out/tls-fixtures';
const exe = process.argv[2];
for (const [name, expected] of [['expired', 'ok'], ['future', 'ok'], ['sha384', 'ok'],
  ['wrong-host', 'reject'], ['untrusted', 'reject'], ['bad-signature', 'reject'], ['plain', 'protocol']]) {
  const sockets = new Set();
  let cert;
  if (name === 'bad-signature') {
    const der = new X509Certificate(readFileSync(`${root}/expired.pem`)).raw;
    der[der.length - 1] ^= 1;
    cert = `-----BEGIN CERTIFICATE-----\n${der.toString('base64').match(/.{1,64}/g).join('\n')}\n-----END CERTIFICATE-----\n`;
  } else if (name !== 'plain') cert = readFileSync(`${root}/${name}.pem`);
  const server = name === 'plain'
    ? net.createServer(s => s.end('HTTP/1.0 400 Bad Request\r\n\r\n'))
    : tls.createServer({ key: readFileSync(`${root}/key.pem`),
        cert,
        minVersion: 'TLSv1.2', maxVersion: 'TLSv1.2' }, s => s.end('OK\n'));
  server.on('tlsClientError', () => {}); // rejections are expected
  server.on('connection', s => {
    sockets.add(s);
    s.on('error', () => {}); // client aborts malformed/plaintext handshakes
    s.on('close', () => sockets.delete(s));
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  try {
    for (const rtc of [946684800, 3627936000]) { // 2000 and 2084
      writeFileSync(`${root}/tls-seed.bin`, randomBytes(64));
      const status = await new Promise((resolve, reject) => {
        const child = spawn(exe, [root, String(server.address().port), expected, String(rtc)],
          { stdio: 'inherit', windowsHide: true });
        const timer = setTimeout(() => { child.kill(); reject(new Error('TLS test timed out')); }, 15000);
        child.on('error', reject);
        child.on('exit', code => { clearTimeout(timer); resolve(code); });
      });
      if (status) throw new Error(`${name}, clock ${rtc}: exit ${status}`);
      console.log(`PASS TLS ${name}, clock ${rtc}`);
    }
  } finally {
    for (const socket of sockets) socket.destroy();
    await new Promise(resolve => server.close(resolve));
  }
}
