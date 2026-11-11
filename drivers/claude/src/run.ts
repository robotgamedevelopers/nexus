// The driver as a process: its arguments, standard input and output, and its exit, around a way to start the harness.
// main.ts gives it the Claude Agent SDK; the conformance test's fake-main.ts a scripted query.

import { statSync } from 'node:fs';

import { Driver, newSessionId, parseArguments, type Harness, type HarnessStart, type Host } from './driver.js';
import { encode, kProtocolVersion } from './protocol.js';

function isDirectory(path: string): boolean {
    try {
        return statSync(path).isDirectory();
    } catch {
        return false;
    }
}

/** Runs the driver over this process's standard input and output until its input ends or it refuses to go on. */
export function runProcess(startHarness: (start: HarnessStart) => Harness, oauthToken: boolean, removed: readonly string[]): void {
    let exiting = false;
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
        startHarness,
        newSessionId,
        oauthToken,
    };
    if (removed.length > 0) {
        host.log(`nexus-claude-driver: removed from Claude's environment: ${removed.join(' ')}`);
    }

    const args = parseArguments(process.argv.slice(2));
    if ('error' in args || !isDirectory(args.cwd)) {
        const [cause, fix] = 'error' in args
            ? [args.error, 'fix the driver\'s line in nexus-server\'s drivers file']
            : [`cannot work in ${args.cwd}: it is not a directory`, 'give the friend a working directory that exists, in nexus-server\'s drivers file'];
        host.write(encode({ kind: 'hello', protocol: kProtocolVersion, harness: 'claude' }));
        host.write(encode({ kind: 'error', cause, fix, code: 'other' }));
        host.exit(2);
        return;
    }
    const driver = new Driver(host, args);
    process.stdout.on('error', () => { host.exit(1); });
    driver.start();
    process.stdin.on('data', (chunk: Buffer) => { driver.feed(chunk); });
    process.stdin.on('end', () => { driver.end(); });
    process.stdin.on('error', () => { driver.end(); });
}
