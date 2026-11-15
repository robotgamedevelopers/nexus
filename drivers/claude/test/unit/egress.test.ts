// What the driver's proxy lets the AI's commands reach (egress.ts): every spelling of this machine refused, any other
// machine allowed. Names resolve through a made-up resolver and the interfaces are made up: no network, no sockets.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { canonical, decide, isThisMachine, type Resolve } from '../../src/egress.js';

const kOwn = ['127.0.0.1', '::1', 'fe80::1%lo0', '192.168.1.22', '100.76.29.55', 'fd7a:115c:a1e0::1:2', 'fe80::aede:48ff:fe00:1122%en0'];

void test('two spellings of one address meet', () => {
    assert.equal(canonical('::ffff:127.0.0.1'), '127.0.0.1');
    assert.equal(canonical('::FFFF:7f00:1'), '127.0.0.1');
    assert.equal(canonical('0:0:0:0:0:ffff:c0a8:116'), '192.168.1.22');
    assert.equal(canonical('::127.0.0.1'), '127.0.0.1');
    assert.equal(canonical('FD7A:115C:A1E0:0:0:0:1:2'), canonical('fd7a:115c:a1e0::1:2'));
    assert.equal(canonical('fe80::1%en0'), canonical('fe80:0::1'));
    assert.equal(canonical('10.0.0.1'), '10.0.0.1');
});

void test('loopback, unspecified and this machine\'s own addresses are this machine', () => {
    for (const a of ['127.0.0.1', '127.255.0.9', '0.0.0.0', '0.1.2.3', '::1', '0:0:0:0:0:0:0:1', '::', '::ffff:127.0.0.1',
        '::ffff:7f00:1', '192.168.1.22', '::ffff:192.168.1.22', '100.76.29.55', 'fd7a:115c:a1e0:0::1:2', 'fe80::1',
        'FE80::AEDE:48FF:FE00:1122']) {
        assert.ok(isThisMachine(a, kOwn), a);
    }
});

void test('another machine is not', () => {
    for (const a of ['100.77.65.125', '192.168.1.23', '8.8.8.8', '2606:4700::1111', 'fd7a:115c:a1e0::1:3', '::ffff:8.8.8.8']) {
        assert.ok(!isThisMachine(a, kOwn), a);
    }
});

void test('anything that is not an address is never connected to', () => {
    assert.ok(isThisMachine('localhost', kOwn));
    assert.ok(isThisMachine('', kOwn));
});

const kNames: Record<string, string[]> = {
    'hulk': ['100.77.65.125'],
    'studio': ['100.76.29.55'],
    'localhost': ['::1', '127.0.0.1'],
    'both': ['8.8.8.8', '127.0.0.1'],
    'none': [],
};
const fakeResolve: Resolve = (host) => {
    const found = kNames[host];
    if (found !== undefined) {
        return Promise.resolve(found);
    }
    return /^[\d.:a-f]+$/i.test(host) ? Promise.resolve([host]) : Promise.reject(new Error('ENOTFOUND'));
};

void test('a name is decided by every address it resolves to, and connects to the one checked', async () => {
    assert.deepEqual(await decide('hulk', fakeResolve, () => kOwn), { address: '100.77.65.125' });
    for (const host of ['studio', 'localhost', 'both', '127.0.0.1', '[::1]', '192.168.1.22']) {
        const d = await decide(host, fakeResolve, () => kOwn);
        assert.ok('refused' in d, host);
        assert.match(d.refused, /this machine/);
    }
    const missing = await decide('nowhere', fakeResolve, () => kOwn);
    assert.ok('refused' in missing);
    assert.match(missing.refused, /does not resolve/);
});
