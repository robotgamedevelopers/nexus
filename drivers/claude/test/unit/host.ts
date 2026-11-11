// A Host for the unit tests: what the driver writes, parsed; the harnesses it starts, as FakeHarness; its exit.

import { Driver, type Arguments, type HarnessStart, type Host } from '../../src/driver.js';
import type { DriverMessage } from '../../src/protocol.js';
import { FakeHarness } from '../fake-harness.js';

export class TestHost implements Host {
    readonly out: DriverMessage[] = [];
    readonly logs: string[] = [];
    readonly harnesses: FakeHarness[] = [];
    exitCode: number | undefined;
    oauthToken = false;
    private ids = 0;
    /** Set up each harness as it starts (account, scripted answers). */
    prepare: (h: FakeHarness) => void = () => undefined;

    write(line: string): void {
        if (!line.endsWith('\n') || line.indexOf('\n') !== line.length - 1) {
            throw new Error(`not one line: ${JSON.stringify(line)}`);
        }
        this.out.push(JSON.parse(line) as DriverMessage);
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
    const driver = new Driver(host, { cwd: '/work', settingSources: [], ...args });
    driver.start();
    await settle();
    host.take();
    return { host, driver };
}
