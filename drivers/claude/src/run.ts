// The driver as a process: its arguments, standard input and output, and its exit, around a way to start the harness.
// main.ts gives it the Claude Agent SDK; the conformance test's fake-main.ts a scripted query.

import { statSync } from 'node:fs';

import { isAccount, openConfigDir, readAccount, type Account, type AccountRefusal } from './account.js';
import { Driver, newSessionId, parseArguments, type Arguments, type Harness, type HarnessStart, type Host } from './driver.js';
import { encode, kProtocolVersion, type DriverMessage } from './protocol.js';

function isDirectory(path: string): boolean {
    try {
        return statSync(path).isDirectory();
    } catch {
        return false;
    }
}

/**
 * What the driver starts with: its arguments and its account, or the lines that refuse the start (hello, then an
 * error: `other` for arguments or a directory that cannot be used, or no sandbox for the AI on this machine (`tools`,
 * tools.ts), `no-credential` for an account with no login). No
 * line ever holds the account's token.
 */
export function prepare(argv: readonly string[], isDir: (path: string) => boolean,
    findAccount: (name: string) => Account | AccountRefusal,
    openConfig: (account: Account) => string | undefined = () => undefined,
    tools?: { cause: string; fix: string }):
    { args: Arguments; account: Account } | { refusal: DriverMessage[] } {
    const hello: DriverMessage = { kind: 'hello', protocol: kProtocolVersion, harness: 'claude' };
    const args = parseArguments(argv);
    if ('error' in args) {
        return { refusal: [hello, { kind: 'error', cause: args.error, fix: 'fix the driver\'s line in nexus-server\'s drivers file', code: 'other' }] };
    }
    if (!isDir(args.cwd)) {
        return { refusal: [hello, { kind: 'error', cause: `cannot work in ${args.cwd}: it is not a directory`,
            fix: 'give the friend a working directory that exists, in nexus-server\'s drivers file', code: 'other' }] };
    }
    if (tools !== undefined) {
        return { refusal: [hello, { kind: 'error', cause: `no sandbox for the AI: ${tools.cause}`, fix: tools.fix,
            code: 'other' }] };
    }
    const found = findAccount(args.account);
    if (!isAccount(found)) {
        return { refusal: [hello, { kind: 'error', cause: found.cause, fix: found.fix, code: 'no-credential' }] };
    }
    const config = openConfig(found);
    if (config !== undefined) {
        return { refusal: [hello, { kind: 'error', cause: `the account ${found.name}'s Claude state: ${config}`,
            fix: 'fix the directory named, then start the session again', code: 'other' }] };
    }
    return { args, account: found };
}

/**
 * Runs the driver over this process's standard input and output until its input ends or it refuses to go on. Before
 * anything else it refuses to start when the AI's sandbox cannot run here (`tools`), finds the --account's login with
 * `findAccount` (account.ts, under HOME), and refuses to start without one, then makes the account's Claude state
 * directory with `openConfig`; every harness then starts on that account.
 */
export function runProcess(startHarness: (start: HarnessStart, account: Account) => Harness, removed: readonly string[],
    findAccount: (name: string) => Account | AccountRefusal = (name) => readAccount(process.env['HOME'], name),
    openConfig: (account: Account) => string | undefined = openConfigDir,
    tools?: { cause: string; fix: string }): void {
    let exiting = false;
    const ready = prepare(process.argv.slice(2), isDirectory, findAccount, openConfig, tools);
    const account = 'refusal' in ready ? undefined : ready.account;
    const host: Host = {
        write(line) {
            process.stdout.write(line);
        },
        log(text) {
            process.stderr.write(text + '\n');
        },
        exit(code) {
            if (exiting) {
                return;
            }
            exiting = true;
            // Standard output to a pipe is asynchronous on macOS: exit once what was written has gone.
            process.stdout.write('', () => process.exit(code));
            process.stdout.once('error', () => process.exit(code));
        },
        startHarness(start) {
            if (account === undefined) {
                throw new Error('the harness started before the account was read');
            }
            return startHarness(start, account);
        },
        newSessionId,
    };
    if (removed.length > 0) {
        host.log(`nexus-claude-driver: removed from Claude's environment: ${removed.join(' ')}`);
    }

    if ('refusal' in ready) {
        for (const m of ready.refusal) {
            host.write(encode(m));
        }
        host.exit(2);
        return;
    }
    const driver = new Driver(host, ready.args);
    process.stdout.on('error', () => { host.exit(1); });
    driver.start();
    process.stdin.on('data', (chunk: Buffer) => { driver.feed(chunk); });
    process.stdin.on('end', () => { driver.end(); });
    process.stdin.on('error', () => { driver.end(); });
}
