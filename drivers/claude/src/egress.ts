// The way out of the sandbox for the AI's commands: a proxy the driver runs, which Claude's sandbox uses in place of
// its own (sandbox.network.httpProxyPort and socksProxyPort, wall.ts). It connects anywhere except this machine.
//
// Why: the sandbox lets a command's connections leave only through the proxy, so the proxy decides what a command can
// reach. Claude's own proxy refuses a name that resolves to one of this host's addresses, but connects to such an
// address given as a number (127.0.0.1, this machine's LAN or tailnet address), so a command could reach this
// machine's sshd, log in as the person, and read the token outside the sandbox (measured: docs/secrets.md). This one
// resolves every name itself, refuses when any address it gets is this machine's (loopback, unspecified, or an address
// of one of its interfaces, IPv4-mapped forms included), and connects to the address it checked, so no later lookup
// can change the answer. Everything else is let through: ssh to the fleet (git over ssh too), http and https.
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

/** The proxy, listening on 127.0.0.1; `port()` once listening. */
export class Egress {
    private readonly server: Server;

    constructor(private readonly resolve: Resolve = realResolve, private readonly own: () => readonly string[] = ownAddresses) {
        this.server = createServer((socket) => { this.accept(socket); });
    }

    async listen(): Promise<number> {
        await new Promise<void>((done) => { this.server.listen(0, '127.0.0.1', done); });
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

    private accept(client: Socket): void {
        client.on('error', () => { client.destroy(); });
        client.once('data', (first: Buffer) => {
            client.pause();
            if (first[0] === 5) {
                void this.socks(client, first);
            } else {
                this.http(client, first);
            }
        });
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

    private static join(a: Socket, b: Socket, rest: Buffer): void {
        b.on('error', () => { a.destroy(); });
        a.on('error', () => { b.destroy(); });
        a.on('close', () => { b.destroy(); });
        b.on('close', () => { a.destroy(); });
        if (rest.length > 0) {
            b.write(rest);
        }
        a.pipe(b);
        b.pipe(a);
        a.resume();
    }

    // SOCKS5 (RFC 1928): no authentication, CONNECT only.
    private async socks(client: Socket, first: Buffer): Promise<void> {
        let buf = first;
        const need = async (n: number): Promise<boolean> => {
            while (buf.length < n) {
                const more = await new Promise<Buffer | undefined>((done) => {
                    client.once('data', (d: Buffer) => { client.pause(); done(d); });
                    client.once('end', () => { done(undefined); });
                    client.resume();
                });
                if (more === undefined) {
                    return false;
                }
                buf = Buffer.concat([buf, more]);
            }
            return true;
        };
        if (!await need(2) || !await need(2 + (buf[1] ?? 0))) {
            client.destroy();
            return;
        }
        buf = buf.subarray(2 + (buf[1] ?? 0));
        client.write(Buffer.from([5, 0]));
        if (!await need(5)) {
            client.destroy();
            return;
        }
        const reply = (code: number): void => { client.end(Buffer.from([5, code, 0, 1, 0, 0, 0, 0, 0, 0])); };
        if (buf[1] !== 1) {
            reply(7);    // command not supported
            return;
        }
        let host: string;
        let at: number;
        if (buf[3] === 1 && await need(10)) {
            host = [...buf.subarray(4, 8)].join('.');
            at = 8;
        } else if (buf[3] === 3 && await need(5 + (buf[4] ?? 0) + 2)) {
            host = buf.subarray(5, 5 + (buf[4] ?? 0)).toString('latin1');
            at = 5 + (buf[4] ?? 0);
        } else if (buf[3] === 4 && await need(22)) {
            const g: string[] = [];
            for (let i = 0; i < 8; i++) {
                g.push(buf.readUInt16BE(4 + i * 2).toString(16));
            }
            host = g.join(':');
            at = 20;
        } else {
            reply(8);    // address type not supported
            return;
        }
        const port = buf.readUInt16BE(at);
        const rest = buf.subarray(at + 2);
        const target = await this.open(host, port);
        if (typeof target === 'string') {
            reply(target.includes('this machine') ? 2 : 5);    // not allowed by the ruleset; refused
            return;
        }
        client.write(Buffer.from([5, 0, 0, 1, 0, 0, 0, 0, 0, 0]));
        Egress.join(client, target, rest);
    }

    // HTTP: CONNECT host:port, or one request with an absolute http URL.
    private http(client: Socket, first: Buffer): void {
        let buf = first;
        const go = (): void => {
            const end = buf.indexOf('\r\n\r\n');
            if (end < 0) {
                if (buf.length > 65536) {
                    client.destroy();
                    return;
                }
                client.once('data', (d: Buffer) => { client.pause(); buf = Buffer.concat([buf, d]); go(); });
                client.resume();
                return;
            }
            const head = buf.subarray(0, end).toString('latin1').split('\r\n');
            const rest = buf.subarray(end + 4);
            const [method = '', target = '', version = 'HTTP/1.1'] = (head[0] ?? '').split(' ');
            const refuse = (why: string): void => {
                const body = `nexus: ${why}\n`;
                client.end(`HTTP/1.1 403 Forbidden\r\ncontent-type: text/plain\r\ncontent-length: ${String(Buffer.byteLength(body))}\r\nconnection: close\r\n\r\n${body}`);
            };
            if (method === 'CONNECT') {
                const m = /^(\[[^\]]+\]|[^:]+):(\d+)$/.exec(target);
                if (m?.[1] === undefined || m[2] === undefined) {
                    refuse(`bad CONNECT target ${target}`);
                    return;
                }
                void this.open(m[1], Number(m[2])).then((s) => {
                    if (typeof s === 'string') {
                        refuse(s);
                        return;
                    }
                    client.write('HTTP/1.1 200 Connection established\r\n\r\n');
                    Egress.join(client, s, rest);
                });
                return;
            }
            let url: URL;
            try {
                url = new URL(target);
            } catch {
                refuse(`not a proxy request: ${method} ${target}`);
                return;
            }
            if (url.protocol !== 'http:') {
                refuse(`only http URLs are forwarded, not ${url.protocol}`);
                return;
            }
            const headers = head.slice(1).filter((h) => !/^(proxy-[a-z-]*|connection|keep-alive):/i.test(h));
            const request = [`${method} ${url.pathname}${url.search} ${version}`, ...headers, 'connection: close', '', ''].join('\r\n');
            void this.open(url.hostname, Number(url.port === '' ? 80 : url.port)).then((s) => {
                if (typeof s === 'string') {
                    refuse(s);
                    return;
                }
                s.write(request);
                Egress.join(client, s, rest);
            });
        };
        go();
    }
}
