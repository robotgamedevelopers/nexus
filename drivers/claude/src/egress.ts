// The way out of the sandbox for the AI's commands: a proxy the driver runs, which Claude's sandbox uses in place of
// its own (sandbox.network.httpProxyPort and socksProxyPort, wall.ts). It connects anywhere but to this machine's own addresses
// (best effort: another machine that can reach this one is reachable too; docs/secrets.md, what stays open).
//
// Why: the sandbox lets a command's connections leave only through the proxy, so the proxy decides what a command can
// reach. Claude's own proxy refuses a name that resolves to one of this host's addresses, but connects to such an
// address given as a number (127.0.0.1, this machine's LAN or tailnet address), so a command could reach this
// machine's sshd, log in as the person, and read the token outside the sandbox (measured: docs/secrets.md). This one
// resolves every name itself, refuses when any address it gets is this machine's (loopback, unspecified, or an address
// of one of its interfaces, IPv4-mapped forms included), and connects to the address it checked, so no later lookup
// can change the answer. Everything else is let through: ssh to the fleet (git over ssh too), http and https. A client
// that has not finished its handshake kHandshakeMs after it connected is dropped. The handshakes are parsed by pure
// functions (socksGreeting, socksRequest, httpRequest), and a client is a seam (Client), so the unit tests drive them
// with no socket.
//
// One listening port on 127.0.0.1 speaks both protocols: SOCKS5 (CONNECT only, no authentication) when the first byte
// is 5, and HTTP otherwise (CONNECT, and plain requests with an absolute URL, one per connection).

import { lookup } from 'node:dns/promises';
import { createConnection, createServer, isIP, type Server, type Socket } from 'node:net';
import { networkInterfaces } from 'node:os';

/** This machine's own addresses, as the interfaces report them now. */
export function ownAddresses(): string[] {
    const out: string[] = [];
    for (const list of Object.values(networkInterfaces())) {
        for (const a of list ?? []) {
            out.push(a.address.replace(/%.*$/, ''));
        }
    }
    return out;
}

/** An IPv6 address as its eight groups in full (a dotted IPv4 tail made two groups); undefined if it is not one. */
function groups(a: string): number[] | undefined {
    let s = a;
    const dotted = /^(.*:)(\d+\.\d+\.\d+\.\d+)$/.exec(s);
    if (dotted?.[1] !== undefined && dotted[2] !== undefined) {
        const [b0 = 0, b1 = 0, b2 = 0, b3 = 0] = dotted[2].split('.').map(Number);
        s = `${dotted[1]}${((b0 << 8) | b1).toString(16)}:${((b2 << 8) | b3).toString(16)}`;
    }
    const halves = s.split('::');
    if (halves.length > 2) {
        return undefined;
    }
    const part = (h: string | undefined): string[] => (h === undefined || h === '' ? [] : h.split(':'));
    const head = part(halves[0]);
    const tail = part(halves[1]);
    const fill = halves.length === 2 ? 8 - head.length - tail.length : 0;
    if (fill < 0 || (halves.length === 1 && head.length !== 8)) {
        return undefined;
    }
    const all = [...head, ...Array<string>(fill).fill('0'), ...tail];
    return all.map((g) => parseInt(g, 16));
}

/**
 * The address in the form two spellings of it share: IPv4 dotted, an IPv4-mapped or IPv4-compatible IPv6 address as
 * its IPv4, any other IPv6 address as its eight groups in hex, lower case, without a zone.
 */
export function canonical(address: string): string {
    const a = address.toLowerCase().replace(/%.*$/, '');
    if (isIP(a) !== 6) {
        return a;
    }
    const g = groups(a);
    if (g === undefined) {
        return a;
    }
    const zeros = g.slice(0, 5).every((x) => x === 0);
    if (zeros && (g[5] === 0xffff || (g[5] === 0 && (g[6] ?? 0) !== 0))) {
        const hi = g[6] ?? 0;
        const lo = g[7] ?? 0;
        return `${String(hi >> 8)}.${String(hi & 255)}.${String(lo >> 8)}.${String(lo & 255)}`;
    }
    return g.map((x) => x.toString(16)).join(':');
}

/** Whether a connection to `address` (a numeric IPv4 or IPv6 address) would reach this machine. */
export function isThisMachine(address: string, own: readonly string[]): boolean {
    const a = canonical(address);
    if (isIP(a) === 4) {
        const first = Number(a.split('.')[0]);
        // 127/8 is loopback; 0/8 (0.0.0.0 above all) reaches this machine's listeners.
        if (first === 127 || first === 0) {
            return true;
        }
    } else if (isIP(a) === 6) {
        if (a === '0:0:0:0:0:0:0:1' || a === '0:0:0:0:0:0:0:0') {
            return true;
        }
    } else {
        return true;    // not an address: never connected to
    }
    return own.some((o) => canonical(o) === a);
}

export type Resolve = (host: string) => Promise<string[]>;

const realResolve: Resolve = async (host) => {
    if (isIP(host) !== 0) {
        return [host];
    }
    return (await lookup(host, { all: true, order: 'verbatim' })).map((r) => r.address);
};

/** Where to connect for `host`, or why not: every address it resolves to is checked, and the first is used. */
export async function decide(host: string, resolve: Resolve = realResolve, own: () => readonly string[] = ownAddresses):
    Promise<{ address: string } | { refused: string }> {
    const bare = host.replace(/^\[(.*)\]$/, '$1');
    let addresses: string[];
    try {
        addresses = await resolve(bare);
    } catch {
        return { refused: `${host} does not resolve` };
    }
    const mine = own();
    if (addresses.length === 0) {
        return { refused: `${host} does not resolve` };
    }
    if (addresses.some((a) => isThisMachine(a, mine))) {
        return { refused: `${host} is this machine: the AI's commands may reach other machines, never this one` };
    }
    return { address: addresses[0] ?? '' };
}


/** A SOCKS5 greeting (RFC 1928): how many bytes it takes, or how many more are needed to know. */
export function socksGreeting(buf: Buffer): { need: number } | { length: number } {
    if (buf.length < 2) {
        return { need: 2 };
    }
    const length = 2 + (buf[1] ?? 0);
    return buf.length < length ? { need: length } : { length };
}

/**
 * A SOCKS5 request after the greeting: where it asks to connect and the bytes after it, the reply code that refuses it
 * (7: not a CONNECT; 8: an address type there is none of), or how many bytes are needed to know.
 */
export function socksRequest(buf: Buffer): { need: number } | { reply: number } | { host: string; port: number; rest: Buffer } {
    if (buf.length < 5) {
        return { need: 5 };
    }
    if (buf[1] !== 1) {
        return { reply: 7 };
    }
    let host: string;
    let at: number;
    if (buf[3] === 1) {
        if (buf.length < 10) {
            return { need: 10 };
        }
        host = [...buf.subarray(4, 8)].join('.');
        at = 8;
    } else if (buf[3] === 3) {
        const n = buf[4] ?? 0;
        if (buf.length < 5 + n + 2) {
            return { need: 5 + n + 2 };
        }
        host = buf.subarray(5, 5 + n).toString('latin1');
        at = 5 + n;
    } else if (buf[3] === 4) {
        if (buf.length < 22) {
            return { need: 22 };
        }
        const g: string[] = [];
        for (let i = 0; i < 8; i++) {
            g.push(buf.readUInt16BE(4 + i * 2).toString(16));
        }
        host = g.join(':');
        at = 20;
    } else {
        return { reply: 8 };
    }
    return { host, port: buf.readUInt16BE(at), rest: buf.subarray(at + 2) };
}

/** The most an HTTP request's head may take before the proxy drops the connection. */
export const kMaxHead = 65536;

/**
 * An HTTP proxy request's head: a CONNECT's target, a plain request to forward (its host, port and the request line and
 * headers to send, proxy headers removed), why it is refused, or that more bytes are needed (`tooLong`: never enough).
 */
export function httpRequest(buf: Buffer):
    | { need: true } | { tooLong: true } | { refuse: string }
    | { connect: { host: string; port: number }; rest: Buffer }
    | { forward: { host: string; port: number; request: string }; rest: Buffer } {
    const end = buf.indexOf('\r\n\r\n');
    if (end < 0) {
        return buf.length > kMaxHead ? { tooLong: true } : { need: true };
    }
    const head = buf.subarray(0, end).toString('latin1').split('\r\n');
    const rest = buf.subarray(end + 4);
    const [method = '', target = '', version = 'HTTP/1.1'] = (head[0] ?? '').split(' ');
    if (method === 'CONNECT') {
        const m = /^(\[[^\]]+\]|[^:]+):(\d+)$/.exec(target);
        if (m?.[1] === undefined || m[2] === undefined) {
            return { refuse: `bad CONNECT target ${target}` };
        }
        return { connect: { host: m[1], port: Number(m[2]) }, rest };
    }
    let url: URL;
    try {
        url = new URL(target);
    } catch {
        return { refuse: `not a proxy request: ${method} ${target}` };
    }
    if (url.protocol !== 'http:') {
        return { refuse: `only http URLs are forwarded, not ${url.protocol}` };
    }
    const headers = head.slice(1).filter((h) => !/^(proxy-[a-z-]*|connection|keep-alive):/i.test(h));
    const request = [`${method} ${url.pathname}${url.search} ${version}`, ...headers, 'connection: close', '', ''].join('\r\n');
    return { forward: { host: url.hostname, port: Number(url.port === '' ? 80 : url.port), request }, rest };
}

/** Runs `fn` after `ms`; the returned function cancels it. A seam: the unit tests pass time by hand. */
export type After = (ms: number, fn: () => void) => () => void;

const realAfter: After = (ms, fn) => {
    const t = setTimeout(fn, ms);
    t.unref();
    return () => { clearTimeout(t); };
};

/** How long a client has to finish its handshake (SOCKS or HTTP) before the proxy drops it. */
export const kHandshakeMs = 10_000;

/** The part of a socket the proxy uses: a seam, so that the unit tests drive it with a stream and no socket. */
export type Client = Pick<Socket, 'on' | 'off' | 'once' | 'pause' | 'resume' | 'write' | 'end' | 'destroy' | 'pipe'>;

/** The proxy, listening on 127.0.0.1; `port()` once listening. */
export class Egress {
    private readonly server: Server;

    constructor(private readonly resolve: Resolve = realResolve, private readonly own: () => readonly string[] = ownAddresses,
        private readonly after: After = realAfter, private readonly handshakeMs = kHandshakeMs) {
        this.server = createServer((socket) => { this.accept(socket); });
    }

    /** Listens on a free port of 127.0.0.1, or rejects with why it cannot. */
    async listen(): Promise<number> {
        await new Promise<void>((done, fail) => {
            this.server.once('error', fail);
            this.server.listen(0, '127.0.0.1', () => {
                this.server.off('error', fail);
                done();
            });
        });
        this.server.unref();
        return this.port();
    }

    port(): number {
        const a = this.server.address();
        return typeof a === 'object' && a !== null ? a.port : 0;
    }

    close(): void {
        this.server.close();
    }

    /** One client: dropped if its handshake has not ended `handshakeMs` after it connected. */
    accept(client: Client): void {
        client.on('error', () => { client.destroy(); });
        const cancel = this.after(this.handshakeMs, () => { client.destroy(); });
        client.on('close', cancel);
        let buf = Buffer.alloc(0);
        let waiting: (() => void) | undefined;
        let ended = false;
        const onData = (d: Buffer): void => {
            buf = Buffer.concat([buf, d]);
            const w = waiting;
            waiting = undefined;
            w?.();
        };
        client.on('data', onData);
        client.on('end', () => {
            ended = true;
            const w = waiting;
            waiting = undefined;
            w?.();
        });
        // The bytes so far, after the next arrive; false when the client ended first.
        const more = (): Promise<boolean> => new Promise<boolean>((done) => {
            if (ended) {
                done(false);
                return;
            }
            waiting = () => { done(!ended || buf.length > 0); };
        });
        const take = (n: number): Buffer => {
            const out = buf.subarray(0, n);
            buf = buf.subarray(n);
            return out;
        };
        // The handshake is over: what is left goes to the other side, and the stream is the pipe's from here.
        const handOver = (): Buffer => {
            cancel();
            client.pause();
            client.off('data', onData);
            const rest = buf;
            buf = Buffer.alloc(0);
            return rest;
        };
        void (async () => {
            while (buf.length === 0) {
                if (!await more()) {
                    client.destroy();
                    return;
                }
            }
            if (buf[0] === 5) {
                await this.socks(client, more, take, handOver, () => buf);
            } else {
                await this.http(client, more, handOver, () => buf);
            }
        })();
    }

    private async open(host: string, port: number): Promise<Socket | string> {
        if (!Number.isInteger(port) || port < 1 || port > 65535) {
            return `bad port ${String(port)}`;
        }
        const d = await decide(host, this.resolve, this.own);
        if ('refused' in d) {
            return d.refused;
        }
        return await new Promise<Socket | string>((done) => {
            const s = createConnection({ host: d.address, port });
            s.once('connect', () => { done(s); });
            s.once('error', (e) => { done(`${host}:${String(port)}: ${e.message}`); });
        });
    }

    private static join(a: Client, b: Socket, rest: Buffer): void {
        b.on('error', () => { a.destroy(); });
        a.on('error', () => { b.destroy(); });
        a.on('close', () => { b.destroy(); });
        b.on('close', () => { a.destroy(); });
        if (rest.length > 0) {
            b.write(rest);
        }
        a.pipe(b);
        b.pipe(a as Socket);
        a.resume();
    }

    // SOCKS5 (RFC 1928): no authentication, CONNECT only.
    private async socks(client: Client, more: () => Promise<boolean>, take: (n: number) => Buffer, handOver: () => Buffer,
        now: () => Buffer): Promise<void> {
        for (let g = socksGreeting(now()); 'need' in g; g = socksGreeting(now())) {
            if (!await more()) {
                client.destroy();
                return;
            }
        }
        const greeting = socksGreeting(now());
        take('length' in greeting ? greeting.length : 0);
        client.write(Buffer.from([5, 0]));
        let r = socksRequest(now());
        while ('need' in r) {
            if (!await more()) {
                client.destroy();
                return;
            }
            r = socksRequest(now());
        }
        const reply = (code: number): void => { client.end(Buffer.from([5, code, 0, 1, 0, 0, 0, 0, 0, 0])); };
        if ('reply' in r) {
            reply(r.reply);
            return;
        }
        take(now().length - r.rest.length);
        const rest = handOver();
        const target = await this.open(r.host, r.port);
        if (typeof target === 'string') {
            reply(target.includes('this machine') ? 2 : 5);    // not allowed by the ruleset; refused
            return;
        }
        client.write(Buffer.from([5, 0, 0, 1, 0, 0, 0, 0, 0, 0]));
        Egress.join(client, target, rest);
    }

    // HTTP: CONNECT host:port, or one request with an absolute http URL.
    private async http(client: Client, more: () => Promise<boolean>, handOver: () => Buffer, now: () => Buffer): Promise<void> {
        let r = httpRequest(now());
        while ('need' in r) {
            if (!await more()) {
                client.destroy();
                return;
            }
            r = httpRequest(now());
        }
        const refuse = (why: string): void => {
            const body = `nexus: ${why}\n`;
            client.end(`HTTP/1.1 403 Forbidden\r\ncontent-type: text/plain\r\ncontent-length: ${String(Buffer.byteLength(body))}\r\nconnection: close\r\n\r\n${body}`);
        };
        if ('tooLong' in r) {
            client.destroy();
            return;
        }
        if ('refuse' in r) {
            refuse(r.refuse);
            return;
        }
        handOver();
        const to = 'connect' in r ? r.connect : r.forward;
        const s = await this.open(to.host, to.port);
        if (typeof s === 'string') {
            refuse(s);
            return;
        }
        if ('connect' in r) {
            client.write('HTTP/1.1 200 Connection established\r\n\r\n');
        } else {
            s.write(r.forward.request);
        }
        Egress.join(client, s, r.rest);
    }
}
