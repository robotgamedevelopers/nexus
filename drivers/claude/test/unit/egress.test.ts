// What the driver's proxy lets the AI's commands reach (egress.ts): every spelling of this machine refused, any other
// machine allowed; the SOCKS and HTTP handshakes parsed, malformed ones refused, and a handshake that never ends dropped at
// its deadline. Names resolve through a made-up resolver, the interfaces are made up, a client is a stream and time is
// passed by hand: no network, no sockets, no clock.

import assert from 'node:assert/strict';
import { Duplex } from 'node:stream';
import { test } from 'node:test';

import {
    canonical, decide, Egress, httpRequest, isThisMachine, kMaxHead, socksGreeting, socksRequest, type After, type Client, type Resolve,
} from '../../src/egress.js';

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

void test('a SOCKS5 greeting and request are parsed, and a short one asks for more', () => {
    assert.deepEqual(socksGreeting(Buffer.from([5])), { need: 2 });
    assert.deepEqual(socksGreeting(Buffer.from([5, 2, 0])), { need: 4 });
    assert.deepEqual(socksGreeting(Buffer.from([5, 1, 0])), { length: 3 });
    const v4 = socksRequest(Buffer.from([5, 1, 0, 1, 10, 0, 0, 7, 0, 22, 9]));
    assert.ok('host' in v4);
    assert.deepEqual([v4.host, v4.port, [...v4.rest]], ['10.0.0.7', 22, [9]]);
    const name = socksRequest(Buffer.concat([Buffer.from([5, 1, 0, 3, 4]), Buffer.from('hulk'), Buffer.from([1, 187])]));
    assert.ok('host' in name);
    assert.deepEqual([name.host, name.port], ['hulk', 443]);
    const v6 = socksRequest(Buffer.from([5, 1, 0, 4, ...Array<number>(15).fill(0), 1, 0, 22]));
    assert.ok('host' in v6);
    assert.equal(canonical(v6.host), '0:0:0:0:0:0:0:1');
    assert.deepEqual(socksRequest(Buffer.from([5, 1, 0, 1, 10])), { need: 10 });
    assert.deepEqual(socksRequest(Buffer.from([5, 1, 0, 3, 200, 104])), { need: 207 });
    assert.deepEqual(socksRequest(Buffer.from([5, 2, 0, 1, 0])), { reply: 7 }, 'BIND is not served');
    assert.deepEqual(socksRequest(Buffer.from([5, 1, 0, 9, 0])), { reply: 8 }, 'no such address type');
});

void test('an HTTP proxy request is parsed; a malformed or unforwardable one is refused, an endless head dropped', () => {
    const c = httpRequest(Buffer.from('CONNECT [::1]:22 HTTP/1.1\r\nhost: x\r\n\r\nSSH'));
    assert.ok('connect' in c);
    assert.deepEqual([c.connect, c.rest.toString()], [{ host: '[::1]', port: 22 }, 'SSH']);
    const f = httpRequest(Buffer.from('GET http://hulk:8080/a?b HTTP/1.1\r\nproxy-authorization: x\r\nkeep-alive: 1\r\naccept: */*\r\n\r\n'));
    assert.ok('forward' in f);
    assert.deepEqual([f.forward.host, f.forward.port], ['hulk', 8080]);
    assert.equal(f.forward.request, 'GET /a?b HTTP/1.1\r\naccept: */*\r\nconnection: close\r\n\r\n');
    assert.deepEqual(httpRequest(Buffer.from('GET http://x/ HTTP/1.1\r\n')), { need: true });
    assert.deepEqual(httpRequest(Buffer.alloc(kMaxHead + 1, 'a')), { tooLong: true });
    assert.match((httpRequest(Buffer.from('CONNECT nohost HTTP/1.1\r\n\r\n')) as { refuse: string }).refuse, /bad CONNECT target/);
    assert.match((httpRequest(Buffer.from('GET /relative HTTP/1.1\r\n\r\n')) as { refuse: string }).refuse, /not a proxy request/);
    assert.match((httpRequest(Buffer.from('GET file:///etc/hosts HTTP/1.1\r\n\r\n')) as { refuse: string }).refuse, /only http URLs/);
    assert.match((httpRequest(Buffer.from('\u0000ÿ\r\n\r\n')) as { refuse: string }).refuse, /not a proxy request/);
});

/** A client that is a stream: what the proxy writes to it is kept, and `send` is what the client sends. */
function fakeClient() {
    const written: Buffer[] = [];
    const d = new Duplex({
        read() { /* bytes come from send */ },
        write(chunk: Buffer, _enc, done) { written.push(Buffer.from(chunk)); done(); },
    });
    return { client: d as unknown as Client, stream: d, written, send: (b: Buffer | string) => { d.push(b); } };
}

/** Time by hand: the deadlines set, and `fire` runs them. */
function fakeAfter() {
    const pending: { ms: number; fn: () => void; cancelled: boolean }[] = [];
    const after: After = (ms, fn) => {
        const p = { ms, fn, cancelled: false };
        pending.push(p);
        return () => { p.cancelled = true; };
    };
    return { after, pending, fire: () => { for (const p of pending) { if (!p.cancelled) { p.fn(); } } } };
}

const settle = (): Promise<void> => new Promise((done) => { setImmediate(done); });

void test('a handshake that stops part way is dropped at its deadline, never held open', async () => {
    for (const part of [Buffer.from([5, 1]), Buffer.from([5, 1, 0, 5, 1, 0, 3, 200]), Buffer.from('CONNECT hulk:22 HTTP/1.1\r\n')]) {
        const t = fakeAfter();
        const egress = new Egress(() => Promise.resolve(['10.0.0.9']), () => kOwn, t.after, 5000);
        const c = fakeClient();
        egress.accept(c.client);
        c.send(part);
        await settle();
        assert.equal(c.stream.destroyed, false, 'not before its deadline');
        assert.deepEqual(t.pending.map((p) => p.ms), [5000]);
        t.fire();
        assert.equal(c.stream.destroyed, true, `dropped at its deadline: ${part.toString('latin1')}`);
    }
});

void test('a refused request is answered and closed with no connection made, and its deadline is dropped', async () => {
    const t = fakeAfter();
    const egress = new Egress(() => Promise.resolve(['127.0.0.1']), () => kOwn, t.after);
    const c = fakeClient();
    egress.accept(c.client);
    c.send('CONNECT localhost:22 HTTP/1.1\r\n\r\n');
    await settle();
    await settle();
    assert.match(Buffer.concat(c.written).toString(), /^HTTP\/1\.1 403 Forbidden[\s\S]*localhost is this machine/);
    assert.ok(t.pending.every((p) => p.cancelled), 'the deadline is cancelled once the handshake is over');
    const s = fakeClient();
    const socks = new Egress(() => Promise.resolve(['127.0.0.1']), () => kOwn, fakeAfter().after);
    socks.accept(s.client);
    s.send(Buffer.from([5, 1, 0, 5, 1, 0, 1, 127, 0, 0, 1, 0, 22]));
    await settle();
    await settle();
    // The greeting's answer, then "not allowed by the ruleset".
    assert.deepEqual([...Buffer.concat(s.written)].slice(0, 4), [5, 0, 5, 2]);
});
