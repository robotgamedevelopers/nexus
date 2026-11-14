// A Host for the unit tests: what the driver writes, parsed; the harnesses it starts, as FakeHarness; its exit; and
// the trace of its steps. With NEXUS_DRIVER_TRACE set to a file's path, the traces of every test that keeps to
// models/ClaudeDriver.tla are written there when the tests end, for TLC (models/ClaudeDriverTrace.tla).

import { Driver, type Arguments, type DriverState, type HarnessStart, type Host, type StepArgs } from '../../src/driver.js';
import type { DriverMessage } from '../../src/protocol.js';
import { FakeHarness } from '../fake-harness.js';
import { Trace, writeTraceModule } from '../trace.js';

const hosts: TestHost[] = [];

const tracePath = process.env['NEXUS_DRIVER_TRACE'];
if (tracePath !== undefined && tracePath !== '') {
    process.on('exit', () => {
        const traces = hosts.filter((h) => h.untraced === undefined && h.trace.records.length > 0).map((h) => h.trace);
        writeTraceModule(tracePath, traces, `${String(traces.length)} traces of the driver's unit tests (test/unit/driver.test.ts)`);
    });
}

export class TestHost implements Host {
    readonly out: DriverMessage[] = [];
    readonly logs: string[] = [];
    readonly harnesses: FakeHarness[] = [];
    exitCode: number | undefined;
    readonly trace = new Trace();
    /** Why this test's trace is not checked against the model: it goes where the model does not (a line the driver
     * refuses, a setting Claude refuses, a model Claude cannot run). */
    untraced: string | undefined;
    private ids = 0;

    constructor() {
        hosts.push(this);
    }
    /** Set up each harness as it starts (account, scripted answers). */
    prepare: (h: FakeHarness) => void = () => undefined;

    write(line: string): void {
        if (!line.endsWith('\n') || line.indexOf('\n') !== line.length - 1) {
            throw new Error(`not one line: ${JSON.stringify(line)}`);
        }
        const m = JSON.parse(line) as DriverMessage;
        this.out.push(m);
        this.trace.wrote(m);
    }

    step(op: string, args: StepArgs, state: DriverState): void {
        this.trace.step(op, args, state);
    }

    log(text: string): void {
        this.logs.push(text);
    }

    exit(code: number): void {
        if (this.exitCode !== undefined) {
            throw new Error('exit twice');
        }
        this.exitCode = code;
    }

    startHarness(start: HarnessStart): FakeHarness {
        const h = new FakeHarness(start);
        this.harnesses.push(h);
        this.prepare(h);
        return h;
    }

    newSessionId(): string {
        this.ids++;
        return `session-${this.ids}`;
    }

    /** The last harness started. */
    get harness(): FakeHarness {
        const h = this.harnesses[this.harnesses.length - 1];
        if (h === undefined) {
            throw new Error('no harness started');
        }
        return h;
    }

    /** What was written since the last call. */
    take(): DriverMessage[] {
        return this.out.splice(0, this.out.length);
    }
}

/** Lets every promise and pending callback run: no clock, only the event loop's queues. */
export async function settle(): Promise<void> {
    for (let i = 0; i < 20; i++) {
        await new Promise<void>((resolve) => { setImmediate(resolve); });
    }
}

export function line(m: object): Uint8Array {
    return Buffer.from(JSON.stringify(m) + '\n');
}

/** A driver started on a TestHost, its credential check done and its start lines taken. */
export async function started(args: Partial<Arguments> = {}, prepare?: (h: FakeHarness) => void): Promise<{ host: TestHost; driver: Driver }> {
    const host = new TestHost();
    if (prepare !== undefined) {
        host.prepare = prepare;
    }
    const driver = new Driver(host, { account: 'main', cwd: '/work', settingSources: [], ...args });
    driver.start();
    await settle();
    host.take();
    return { host, driver };
}
