const assert = require('node:assert/strict');
const { createHash, createHmac } = require('node:crypto');
const { readFileSync } = require('node:fs');
const { resolve } = require('node:path');
const { runInNewContext } = require('node:vm');
const test = require('node:test');

const source = readFileSync(resolve(__dirname, '../shared/WifiOta/src/OtaPage.h'), 'utf8');
const script = source.match(/<script>([\s\S]*?)<\/script>/)?.[1];
assert.ok(script, 'embedded OTA page script exists');
const helpers = script.split('let current;')[0];
const crypto = runInNewContext(`${helpers}\n({ sha256, hmac, hex, enc, manifestMessage })`, { TextEncoder });

test('browser manifest bytes match the signed release format', () => {
  const manifest = {board: 'device-controller', hardware: 'esp32-s3-n16r8', build: 2,
    version: '0.1.0-ota2', size: 123456, sha256: 'ab'.repeat(32)};
  assert.equal(crypto.manifestMessage(manifest),
    'BABYTECH-OTA-V1\ndevice-controller\nesp32-s3-n16r8\n2\n0.1.0-ota2\n123456\n' + 'ab'.repeat(32) + '\n');
});

test('embedded SHA-256 agrees with Node for short, long and Unicode input', () => {
  for (const text of ['', 'abc', 'x'.repeat(130), '固件升级']) {
    const expected = createHash('sha256').update(text).digest('hex');
    assert.equal(crypto.hex(crypto.sha256(crypto.enc.encode(text))), expected);
  }
});

test('embedded HMAC agrees with Node for admin challenge messages', () => {
  for (const key of ['0123456789abcdef0123456789abcdef', 'x'.repeat(90)]) {
    const message = 'BABYTECH-OTA-AUTH-V1\nnonce\nBABYTECH-OTA-V1\nmain-controller\n';
    const expected = createHmac('sha256', key).update(message).digest('hex');
    assert.equal(crypto.hex(crypto.hmac(key, message)), expected);
  }
});
