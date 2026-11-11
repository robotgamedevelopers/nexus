// Drives the real Driver with a seeded random walk over the scripted query and writes its traces as a TLA+ module
// (ClaudeDriverTraceData.tla) for models/ClaudeDriverTrace.tla, under which TLC checks that each trace is a behaviour
// of models/ClaudeDriver.tla.
//
//   node dist/test/walk.js <seed> <steps> [--faulty-settings | --faulty-refusal | --faulty-latch] <out.tla>
//
// The walk plays the server (sends, each after the last turn's done; stops for any turn; settings; the end of input)
// and the query (the credential check's answer, reading prompts or holding them, settings and interrupts answered
// late, text, tools, an assistant's error, results, refusals, the stream failing, a --resume id it does not know).
// Each driver's life is one trace; when a driver exits, the next life starts, with or without a --resume id.
//
// It is a pure function of its arguments: its random numbers come from the seed (splitmix64), never from the clock
// or the OS, and it waits only on the event loop's queues (no timer, no process, no network).
//
// --faulty-settings walks a driver whose stop drops the settings of a turn stopped before it began;
// --faulty-refusal one whose refusal leaves an open text block unended before its done; --faulty-latch one that keeps
// a stop that found its turn not open and stops the next turn with it. TLC must reject each one's traces; that is the
// negative check that shows the binding can fail.

import type { AccountInfo, EffortLevel, SDKControlInterruptResponse } from '@anthropic-ai/claude-agent-sdk';

import { Driver, type DriverState, type HarnessStart, type Host, type StepArgs } from '../src/driver.js';
import type { DriverMessage } from '../src/protocol.js';
import { assistant, blockStop, init, kSubscription, openText, rateLimit, result, toolUse, FakeHarness } from './fake-harness.js';
import { Trace, writeTraceModule } from './trace.js';

class Rng {
    private state: bigint;

    constructor(seed: number) {
        this.state = BigInt(seed);
    }

    private next(): bigint {
        const m = (1n << 64n) - 1n;
        this.state = (this.state + 0x9E3779B97F4A7C15n) & m;
        let z = this.state;
        z = ((z ^ (z >> 30n)) * 0xBF58476D1CE4E5B9n) & m;
        z = ((z ^ (z >> 27n)) * 0x94D049BB133111EBn) & m;
        return z ^ (z >> 31n);
    }

    below(n: number): number {
        return Number(this.next() % BigInt(n));
    }

    /** True `percent` times in a hundred. */
    chance(percent: number): boolean {
        return this.below(100) < percent;
    }

    pick<T>(items: readonly T[]): T {
        const item = items[this.below(items.length)];
        if (item === undefined) {
            throw new Error('pick from nothing');
        }
        return item;
    }
}

interface Deferred<T> {
    resolve: (v: T) => void;
    reject: (e: Error) => void;
}

function deferred<T>(into: Deferred<T>[]): Promise<T> {
    return new Promise<T>((resolve, reject) => { into.push({ resolve, reject }); });
}

const kLost = 'No conversation found with session ID';

/** A query whose every answer the walk gives when it chooses. */
class Query extends FakeHarness {
    readonly checks: Deferred<AccountInfo>[] = [];
    readonly interrupts: Deferred<SDKControlInterruptResponse | undefined>[] = [];
    readonly applies: Deferred<undefined>[] = [];
    readonly session: string;

    constructor(start: HarnessStart) {
        super(start);
        this.session = start.sessionId ?? start.resume ?? '';
        this.accountInfo = () => deferred(this.checks);
        this.interrupt = () => { this.calls.push('interrupt'); return deferred(this.interrupts); };
        this.setModel = (model) => { this.calls.push(`model ${model ?? ''}`); return deferred(this.applies); };
        this.applyFlagSettings = (s) => { this.calls.push(`effort ${String(s.effortLevel)}`); return deferred(this.applies); };
    }
}

class WalkHost implements Host {
    readonly trace = new Trace();
    readonly queries: Query[] = [];
    exited = false;
    oauthToken = false;
    /** The turns the server has sent and has not yet seen the done of: at most one. */
    awaiting = 0;
    private ids = 0;
    hold = false;

    write(line: string): void {
        const m = JSON.parse(line) as DriverMessage;
        if (m.kind === 'done' && m.turn === this.awaiting) {
            this.awaiting = 0;
        }
        this.trace.wrote(m);
    }

    log(): void {
        // the walk does not read the driver's log
    }

    exit(): void {
        this.exited = true;
    }

    startHarness(start: HarnessStart): Query {
        const q = new Query(start);
        q.holdPrompts = this.hold;
        this.queries.push(q);
        return q;
    }

    newSessionId(): string {
        this.ids++;
        return `walk-${String(this.ids)}`;
    }

    step(op: string, args: StepArgs, state: DriverState): void {
        this.trace.step(op, args, state);
    }

    get query(): Query {
        const q = this.queries[this.queries.length - 1];
        if (q === undefined) {
            throw new Error('no query started');
        }
        return q;
    }
}

/** Lets every promise and pending callback run: no clock, only the event loop's queues. */
async function settle(): Promise<void> {
    for (let i = 0; i < 8; i++) {
        await new Promise<void>((resolve) => { setImmediate(resolve); });
    }
}

function line(m: object): Uint8Array {
    return Buffer.from(JSON.stringify(m) + '\n');
}

// ---- Faults: the driver's own methods replaced, as a bug in them would be ----

type Method = (this: unknown, ...args: unknown[]) => unknown;

function method(name: string): Method {
    const m = (Driver.prototype as unknown as Record<string, unknown>)[name];
    if (typeof m !== 'function') {
        throw new Error(`Driver has no method ${name}`);
    }
    return m as Method;
}

function replace(name: string, m: Method): void {
    (Driver.prototype as unknown as Record<string, unknown>)[name] = m;
}

interface OpenTurn {
    open?: { turn: number; textOpen: boolean; held: string };
}

function fault(name: string): void {
    switch (name) {
        case 'settings':
            replace('restore', () => undefined);
            return;
        case 'refusal': {
            const refuse = method('refuseCredential');
            replace('refuseCredential', function (this: unknown, ...args: unknown[]) {
                const t = (this as OpenTurn).open;
                if (t !== undefined) {
                    t.textOpen = false;
                    t.held = '';
                }
                return refuse.apply(this, args);
            });
            return;
        }
        case 'latch': {
            const stop = method('stop');
            const send = method('send');
            let latched = 0;
            replace('stop', function (this: unknown, ...args: unknown[]) {
                if ((this as OpenTurn).open?.turn !== args[0]) {
                    latched = args[0] as number;
                }
                return stop.apply(this, args);
            });
            replace('send', function (this: unknown, ...args: unknown[]) {
                const r = send.apply(this, args);
                if (latched !== 0) {
                    latched = 0;
                    stop.call(this, args[0]);
                }
                return r;
            });
            return;
        }
        default:
            throw new Error(`unknown fault ${name}: one of settings, refusal, latch`);
    }
}

// ---- The walk ----

const kModels = ['opus', 'sonnet'];
const kEfforts: EffortLevel[] = ['low', 'high'];

/** One event of the walk, chosen from those that can happen now. */
async function event(rng: Rng, host: WalkHost, driver: Driver, last: { turn: number }, ids: { n: number }): Promise<void> {
    const q = host.query;
    const s = q.session;
    const choices: (() => void)[] = [];
    const add = (weight: number, f: () => void): void => {
        for (let i = 0; i < weight; i++) {
            choices.push(f);
        }
    };
    // the server
    if (host.awaiting === 0) {
        add(6, () => {
            const turn = last.turn + 1 + rng.below(2);
            last.turn = turn;
            host.awaiting = turn;
            driver.feed(line({ kind: 'send', turn, prompt: `prompt ${String(turn)}` }));
        });
    }
    add(3, () => {
        const turn = host.awaiting !== 0 && rng.chance(70) ? host.awaiting : 1 + rng.below(last.turn + 1);
        driver.feed(line({ kind: 'stop', turn }));
    });
    add(2, () => {
        const m: { kind: string; model?: string; effort?: string } = { kind: 'settings' };
        if (rng.chance(60)) {
            m.model = rng.pick(kModels);
        }
        if (m.model === undefined || rng.chance(40)) {
            m.effort = rng.pick(kEfforts);
        }
        driver.feed(line(m));
    });
    add(rng.chance(15) ? 1 : 0, () => { driver.end(); });
    // the query's answers
    if (q.checks.length > 0) {
        add(6, () => {
            const c = q.checks.shift();
            const r = rng.below(100);
            if (q.start.resume !== undefined && r < 20) {
                c?.reject(new Error(`Claude Code returned an error result: ${kLost}: ${q.start.resume}`));
            } else if (r < 24) {
                c?.resolve({ ...kSubscription, apiKeySource: 'ANTHROPIC_API_KEY' });
            } else if (r < 27) {
                c?.reject(new Error('the CLI did not start'));
            } else {
                c?.resolve(kSubscription);
            }
        });
    }
    if (q.holdPrompts) {
        add(4, () => { q.takePrompts(); });
    } else {
        add(1, () => { q.holdPrompts = true; });
    }
    if (q.interrupts.length > 0) {
        add(4, () => {
            const i = q.interrupts.shift();
            if (rng.chance(4)) {
                i?.reject(new Error('the interrupt was refused'));
            } else {
                i?.resolve(undefined);
            }
        });
    }
    if (q.applies.length > 0) {
        add(5, () => { q.applies.shift()?.resolve(undefined); });
    }
    // what the query writes
    const running = host.awaiting !== 0 ? 5 : 1;
    add(3 * running, () => {
        ids.n++;
        q.emit(...openText(s, `m${String(ids.n)}`, 'some text '));
    });
    add(2 * running, () => { q.emit(blockStop(s)); });
    add(running, () => {
        ids.n++;
        q.emit(assistant(s, `a${String(ids.n)}`, [{ type: 'text', text: 'whole' }]));
    });
    add(running, () => {
        ids.n++;
        q.emit(toolUse(s, `t${String(ids.n)}`, 'Bash', { command: 'ls' }));
    });
    add(rng.chance(30) ? running : 0, () => {
        ids.n++;
        const code = rng.chance(30) ? 'authentication_failed' : 'rate_limit';
        q.emit(assistant(s, `e${String(ids.n)}`, [{ type: 'text', text: 'failed' }], code));
    });
    add(3 * running, () => {
        const r = rng.below(100);
        if (r < 60) {
            q.emit(init(s), result(s));
        } else if (r < 75 || q.start.resume === undefined) {
            q.emit(result(s, { subtype: 'error_during_execution', errors: ['it failed'] }));
        } else {
            q.emit(result(s, { subtype: 'error_during_execution', errors: [`${kLost}: ${q.start.resume}`] }));
        }
    });
    add(rng.chance(20) ? 1 : 0, () => {
        q.emit(rng.chance(50) ? rateLimit(s, { isUsingOverage: true }) : init(s, 'apiKeyHelper'));
    });
    add(rng.chance(15) ? 1 : 0, () => {
        if (q.start.resume !== undefined && rng.chance(60)) {
            q.finish(new Error(`Claude Code returned an error result: ${kLost}: ${q.start.resume}`));
        } else {
            q.finish(new Error('the CLI crashed'));
        }
    });
    rng.pick(choices)();
    await settle();
}

async function main(argv: readonly string[]): Promise<number> {
    const args = argv.filter((a) => !a.startsWith('--faulty-'));
    const faults = argv.filter((a) => a.startsWith('--faulty-')).map((a) => a.slice('--faulty-'.length));
    const [seedArg, stepsArg, out] = args;
    const seed = Number(seedArg);
    const steps = Number(stepsArg);
    if (args.length !== 3 || out === undefined || !Number.isSafeInteger(seed) || !Number.isSafeInteger(steps) || steps < 1) {
        process.stderr.write('usage: walk <seed> <steps> [--faulty-settings | --faulty-refusal | --faulty-latch] <out.tla>\n');
        return 2;
    }
    for (const f of faults) {
        fault(f);
    }
    const rng = new Rng(seed);
    const traces: Trace[] = [];
    const ids = { n: 0 };
    let step = 0;
    let lives = 0;
    while (step < steps) {
        lives++;
        const host = new WalkHost();
        host.hold = rng.chance(30);
        const resume = rng.chance(40) ? `resumed-${String(lives)}` : undefined;
        const driver = new Driver(host, {
            account: 'walk', cwd: '/walk', settingSources: [], ...(resume === undefined ? {} : { resume }),
            ...(rng.chance(30) ? { model: 'opus' } : {}),
        });
        driver.start();
        await settle();
        const last = { turn: rng.below(3) };
        while (step < steps && !host.exited) {
            step++;
            await event(rng, host, driver, last, ids);
        }
        traces.push(host.trace);
    }
    const records = traces.reduce((n, t) => n + t.records.length, 0);
    const count = (op: string): number => traces.reduce((n, t) => n + t.records.filter((r) => r['op'] === op).length, 0);
    writeTraceModule(out, traces, `walk seed ${String(seed)}, ${String(steps)} events${faults.map((f) => `, faulty ${f}`).join('')}`);
    process.stdout.write(`walk seed ${String(seed)}: ${String(lives)} lives, ${String(records)} steps; `
        + ['send', 'stop', 'settings', 'apply', 'push', 'take', 'answered', 'result', 'refused', 'failed', 'lost', 'end']
            .map((op) => `${op} ${String(count(op))}`).join(', ') + '\n');
    return 0;
}

main(process.argv.slice(2)).then((code) => { process.exitCode = code; }, (e: unknown) => {
    process.stderr.write(`walk failed: ${e instanceof Error ? e.stack ?? e.message : String(e)}\n`);
    process.exitCode = 1;
});
