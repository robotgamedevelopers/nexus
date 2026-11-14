// The live functional test: the real driver (dist/src/main.js) on the real SDK and the subscription login of an
// account under nexus-server's home. It spends the subscription, so it is opt-in (`npm run test:live`) and never runs in CI.
//
// It checks: hello, then credential subscription and the session id before any turn, with an API key planted in the
// environment removed; a turn answered (text, done ok); a turn stopped mid-reply (done stopped); a model and effort
// change taking effect from the next turn; the driver killed and started again with --resume (the same session id,
// and the conversation remembered); and a --resume id Claude does not know (a notice, then a new session).
//
// When the subscription's usage limit is reached, Claude answers every turn with that error; the test then checks what
// needs no reply (start, credential, key removal, resume) and exits 2 saying so.
//
// The account is named by NEXUS_LIVE_ACCOUNT: the driver's --account, a login at ~/.nexus/server/accounts/<name>/
// under HOME, as for nexus-server (`NEXUS_LIVE_ACCOUNT=<name> npm run test:live`).

import { spawn, type ChildProcessWithoutNullStreams } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createInterface } from 'node:readline';

import type { DriverMessage } from '../../src/protocol.js';

const main = join(import.meta.dirname, '..', '..', 'src', 'main.js');
const account = process.env['NEXUS_LIVE_ACCOUNT'] ?? '';
if (account === '') {
    process.stderr.write('live: NEXUS_LIVE_ACCOUNT is not set; set it to the account under ~/.nexus/server/accounts/ to run on '
        + '(`NEXUS_LIVE_ACCOUNT=<name> npm run test:live`)\n');
    process.exit(1);
}
const work = mkdtempSync(join(tmpdir(), 'nexus-claude-live-'));
let failures = 0;
const state = { limited: false };

function check(ok: boolean, what: string): void {
    process.stdout.write(`${ok ? 'ok  ' : 'FAIL'} ${what}\n`);
    if (!ok) {
        failures++;
    }
}

class Driver {
    readonly out: DriverMessage[] = [];
    readonly stderr: string[] = [];
    private readonly child: ChildProcessWithoutNullStreams;
    private waiters: (() => void)[] = [];
    private exited: number | null | undefined;

    constructor(args: string[]) {
        const env = { ...process.env, ANTHROPIC_API_KEY: 'sk-ant-api03-planted-by-the-live-test-never-valid' };
        this.child = spawn(process.execPath, [main, '--account', account, '--cwd', work, ...args], { env, stdio: ['pipe', 'pipe', 'pipe'] });
        createInterface({ input: this.child.stdout }).on('line', (line) => {
            this.out.push(JSON.parse(line) as DriverMessage);
            this.poke();
        });
        createInterface({ input: this.child.stderr }).on('line', (line) => { this.stderr.push(line); });
        this.child.on('exit', (code) => { this.exited = code; this.poke(); });
    }

    private poke(): void {
        const w = this.waiters;
        this.waiters = [];
        for (const f of w) {
            f();
        }
    }

    send(m: object): void {
        this.child.stdin.write(JSON.stringify(m) + '\n');
    }

    /** Waits until `pred` holds over what was written, or `seconds` pass. */
    async until(pred: (out: DriverMessage[]) => boolean, seconds: number): Promise<boolean> {
        const deadline = Date.now() + seconds * 1000;
        while (!pred(this.out)) {
            if (this.exited !== undefined || Date.now() > deadline) {
                return pred(this.out);
            }
            await new Promise<void>((resolve) => {
                this.waiters.push(resolve);
                setTimeout(resolve, 250);
            });
        }
        return true;
    }

    done(turn: number): Extract<DriverMessage, { kind: 'done' }> | undefined {
        return this.out.find((m): m is Extract<DriverMessage, { kind: 'done' }> => m.kind === 'done' && m.turn === turn);
    }

    text(turn: number): string {
        return this.out.flatMap((m) => (m.kind === 'text' && m.turn === turn ? [m.delta] : [])).join('');
    }

    errors(turn?: number): Extract<DriverMessage, { kind: 'error' }>[] {
        return this.out.filter((m): m is Extract<DriverMessage, { kind: 'error' }> => m.kind === 'error' && m.turn === turn);
    }

    kill(): void {
        this.child.kill('SIGKILL');
    }

    async close(): Promise<number | null | undefined> {
        this.child.stdin.end();
        await this.until(() => this.exited !== undefined, 15);
        return this.exited;
    }
}

async function turn(d: Driver, n: number, prompt: string, seconds = 120): Promise<void> {
    const t0 = Date.now();
    d.send({ kind: 'send', turn: n, prompt });
    await d.until(() => d.done(n) !== undefined, seconds);
    const done = d.done(n);
    process.stdout.write(`     turn ${n}: ${done?.outcome ?? 'no done'} in ${Date.now() - t0} ms; text ${JSON.stringify(d.text(n).slice(0, 120))}\n`);
    for (const e of d.errors(n)) {
        process.stdout.write(`     turn ${n} error (${e.code}): ${e.cause}\n`);
        if (e.cause.includes('usage limit')) {
            state.limited = true;
        }
    }
}

const first = new Driver(['--model', 'claude-haiku-5-5', '--effort', 'low']);
check(await first.until((o) => o.some((m) => m.kind === 'session'), 30), 'session reported before any turn');
check(first.out[0]?.kind === 'hello', 'hello first');
const credential = first.out.find((m) => m.kind === 'credential');
check(credential?.kind === 'credential' && credential.billing === 'subscription', `credential subscription (${JSON.stringify(credential)})`);
check(first.stderr.some((l) => l.includes('removed from Claude\'s environment') && l.includes('ANTHROPIC_API_KEY')), 'the planted ANTHROPIC_API_KEY was removed');
const session = first.out.find((m) => m.kind === 'session');
const id = session?.kind === 'session' ? session.id : '';

await turn(first, 1, 'Remember the word "heron". Reply with just the word ok.');
if (!state.limited) {
    check(first.done(1)?.outcome === 'ok', 'turn 1 answered');
    check(first.text(1).length > 0, 'turn 1 has text');

    first.send({ kind: 'send', turn: 2, prompt: 'Count from 1 to 400, one number per line, nothing else.' });
    const t0 = Date.now();
    await first.until(() => first.text(2).length > 0, 60);
    first.send({ kind: 'stop', turn: 2 });
    await first.until(() => first.done(2) !== undefined, 30);
    process.stdout.write(`     stop answered in ${Date.now() - t0} ms after the first text\n`);
    check(first.done(2)?.outcome === 'stopped', 'turn 2 stopped mid-reply');

    first.send({ kind: 'settings', model: 'claude-sonnet-5-5', effort: 'medium' });
    await turn(first, 3, 'Reply with just the word fine.');
    check(first.done(3)?.outcome === 'ok', 'turn 3 answered after a model and effort change');
}

first.kill();
await new Promise<void>((resolve) => { setTimeout(resolve, 500); });
const second = new Driver(['--resume', id]);
check(await second.until((o) => o.some((m) => m.kind === 'session'), 30), 'resumed driver reports its session');
const again = second.out.find((m) => m.kind === 'session');
check(again?.kind === 'session' && again.id === id, `same session id after a kill and --resume (${id})`);
check(second.out.every((m) => !(m.kind === 'error' && m.code === 'session-not-found')), 'the session was found');
if (!state.limited) {
    await turn(second, 4, 'What word did I ask you to remember? Reply with just that word.');
    check(second.done(4)?.outcome === 'ok' && /heron/i.test(second.text(4)), 'the resumed session remembers');
}
check(await second.close() === 0, 'the end of input exits 0');

const unknown = '00000000-0000-4000-8000-000000000000';
const third = new Driver(['--resume', unknown]);
check(await third.until((o) => o.some((m) => m.kind === 'session'), 30), 'an unknown --resume id: a session is reported');
check(third.out.some((m) => m.kind === 'error' && m.code === 'session-not-found'), 'an unknown --resume id: a session-not-found notice');
const fresh = third.out.find((m) => m.kind === 'session');
check(fresh?.kind === 'session' && fresh.id !== unknown, 'an unknown --resume id: the session is a new one');
check(await third.close() === 0, 'the end of input exits 0 after a new session');

if (state.limited) {
    process.stdout.write(`BLOCKED: the subscription's usage limit is reached; the turns could not be answered (${failures} other failure(s))\n`);
    process.exit(2);
}
process.stdout.write(failures === 0 ? 'live: all passed\n' : `live: ${failures} failure(s)\n`);
process.exit(failures === 0 ? 0 : 1);
