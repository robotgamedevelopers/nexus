// The driver as a process: its arguments, standard input and output, and its exit, around a way to start the harness.
// main.ts gives it the Claude Agent SDK; the conformance test's fake-main.ts a scripted query.

import { closeSync, fstatSync, readSync, realpathSync, statSync } from 'node:fs';

import { isAccount, openConfigDir, readAccount, type Account, type AccountRefusal } from './account.js';
import { Driver, newSessionId, parseArguments, type Arguments, type Harness, type HarnessStart, type Host } from './driver.js';
import { encode, kProtocolVersion, type DriverMessage } from './protocol.js';
import { verifiedAccess } from './verified.js';
import { kDataRoots, limitRefusal } from './wall.js';

/** Where the driver runs, for a limited AI's checks: HOME, the platform, and how a path resolves. */
export interface Place {
    home: string;
    platform: string;
    realpath: (p: string) => string;
}

const kHere: Place = { home: process.env['HOME'] ?? '', platform: process.platform, realpath: (p) => realpathSync(p) };

/**
 * Why a limited AI (--limit cwd) cannot start here, or undefined: the limit is enforced or the session does not
 * start (fail closed). Only on the platforms whose sandbox nexus has measured it on (kDataRoots), only on the Claude
 * executable the driver ships (another's settings are not known to hold it), and never in a directory that would
 * hold what the limit keeps out (limitRefusal).
 */
export function limitedStartRefusal(args: Arguments, place: Place): string | undefined {
    if (args.limit === undefined) {
        return undefined;
    }
    if (kDataRoots[place.platform] === undefined) {
        return `this AI is limited to its directory, and nexus cannot limit it on ${place.platform}`;
    }
    if (args.claudePath !== undefined) {
        return 'this AI is limited to its directory, which holds only on the Claude the driver ships, and the drivers '
            + `file names another (--claude ${args.claudePath})`;
    }
    if (place.home === '') {
        return 'this AI is limited to its directory, and HOME is not set, so the driver cannot tell what to keep it from';
    }
    const why = limitRefusal(args.cwd, place.home, place.realpath);
    return why === undefined ? undefined : `this AI is limited to its directory, and ${why}`;
}

/**
 * The working directory a limited AI works in: resolved once, here, and that one path is the one checked
 * (limitRefusal), given to the sandbox and the file tools (limitOf) and given to Claude. Undefined when it does not
 * resolve (a limited session then does not start).
 */
export function resolvedCwd(cwd: string, place: Place): string | undefined {
    try {
        return place.realpath(cwd);
    } catch {
        return undefined;
    }
}

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
    findAccount: (name: string, verified: boolean) => Account | AccountRefusal,
    openConfig: (account: Account, migrateSettings: boolean) => string | undefined = () => undefined,
    tools?: { cause: string; fix: string }, place: Place = kHere):
    { args: Arguments; account: Account } | { refusal: DriverMessage[] } {
    const hello: DriverMessage = { kind: 'hello', protocol: kProtocolVersion, harness: 'claude', features: ['limit'] };
    const args = parseArguments(argv);
    if ('error' in args) {
        return { refusal: [hello, { kind: 'error', cause: args.error, fix: 'fix the driver\'s line in nexus-server\'s drivers file', code: 'other' }] };
    }
    if (args.startupContextFd !== undefined) {
        const fd = args.startupContextFd;
        try {
            try {
                const file = fstatSync(fd);
                if (!file.isFile() || file.size < 1 || file.size > 256 * 1024) throw new Error();
                const bytes = Buffer.alloc(file.size + 1);
                let length = 0;
                while (length < bytes.length) {
                    const n = readSync(fd, bytes, length, bytes.length - length, null);
                    if (n === 0) break;
                    length += n;
                }
                if (length > file.size) throw new Error();
                args.startupContext = new TextDecoder('utf-8', { fatal: true }).decode(bytes.subarray(0, length));
                if (!args.startupContext.trim()) throw new Error();
            } finally {
                closeSync(fd);
            }
        } catch {
            return { refusal: [hello, { kind: 'error', code: 'other',
                cause: args.primarySession ? 'Could not read the initial session context' : 'Could not read the AI startup context',
                fix: args.primarySession ? 'Check the initial session launcher on the Nexus server.'
                    : 'Check the AI definition and its configured home files on the Nexus server.' }] };
        }
    }
    if (!isDir(args.cwd)) {
        return { refusal: [hello, { kind: 'error', cause: `cannot work in ${args.cwd}: it is not a directory`,
            fix: 'give the AI a working directory that exists, in nexus-server\'s drivers file', code: 'other' }] };
    }
    if (args.limit !== undefined) {
        const resolved = resolvedCwd(args.cwd, place);
        if (resolved === undefined) {
            return { refusal: [hello, { kind: 'error', cause: `this AI is limited to its directory, and ${args.cwd} `
                + 'does not resolve', fix: 'give this instance a working directory that exists, in ais.json (docs/accounts.md, "limit")', code: 'other' }] };
        }
        args.cwd = resolved;
    }
    const unlimitable = limitedStartRefusal(args, place);
    if (unlimitable !== undefined) {
        return { refusal: [hello, { kind: 'error', cause: unlimitable,
            fix: 'give this instance a directory of its own in ais.json, or, for an AI on no API-billed account, '
                + 'set its "limit" to false there (docs/accounts.md, "limit")', code: 'other' }] };
    }
    if (tools !== undefined) {
        return { refusal: [hello, { kind: 'error', cause: `no sandbox for the AI: ${tools.cause}`, fix: tools.fix,
            code: 'other' }] };
    }
    const found = findAccount(args.account, args.verified !== undefined);
    if (!isAccount(found)) {
        return { refusal: [hello, { kind: 'error', cause: found.cause, fix: found.fix, code: 'no-credential' }] };
    }
    // Only legacy accounts migrate old settings from their secret directory. A verified token cannot
    // be used to detect a legacy credential accidentally embedded in those files.
    const config = openConfig(found, args.verified === undefined);
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
    findAccount: (name: string, verified: boolean) => Account | AccountRefusal = (name, verified) => readAccount(process.env['HOME'], name, verified),
    openConfig: (account: Account, migrateSettings: boolean) => string | undefined = openConfigDir,
    tools?: { cause: string; fix: string }): void {
    let exiting = false;
    const ready = prepare(process.argv.slice(2), isDirectory, findAccount, openConfig, tools);
    const account = 'refusal' in ready ? undefined : ready.account;
    const verified = 'refusal' in ready ? undefined : ready.args.verified;
    let launchedToken: string | undefined;
    const host: Host = {
        ...(verified && account ? {
            async refreshCredential() {
                const access = await verifiedAccess(verified, account.dir);
                if (typeof access === 'string') return access;
                account.token = access.token;
                return access.token === launchedToken ? 'same' as const : 'changed' as const;
            },
        } : {}),
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
            launchedToken = account.token;
            return startHarness(start, { ...account });
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
