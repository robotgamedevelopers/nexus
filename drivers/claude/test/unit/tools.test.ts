// Which bwrap and socat Claude's sandbox finds on Linux, and whether the driver loads nexus's AppArmor profile for its
// bwrap (tools.ts), on a made-up machine: no real file is read or made, and no process runs.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
    appArmorProfile, kAppArmorDir, kAppArmorParser, kInstall, kRestrictUserns, sandboxHomeFiles, sandboxTools, type HomeFs,
    type ToolsSystem,
} from '../../src/tools.js';

/** A machine of `files`, where sudo -n works when `sudo` is true; every sudo call is recorded with what it was given. */
function fakeSystem(files: Record<string, string>, sudo = true, failing?: string) {
    const calls: string[][] = [];
    const inputs: (string | undefined)[] = [];
    const sys: ToolsSystem = {
        exists: (path) => path in files,
        read: (path) => files[path],
        realpath: (path) => path.replace('/link/', '/real/'),
        sudo: (args, input) => {
            calls.push([...args]);
            inputs.push(input);
            if (!sudo) {
                return 'sudo: a password is required';
            }
            if (failing !== undefined && args[0] === failing) {
                return `${failing}: it failed`;
            }
            if (args[0] === kInstall && args[3] === '/dev/stdin') {
                files[args[4] ?? ''] = input ?? '';
            }
            return undefined;
        },
    };
    return { sys, calls, inputs, files };
}

/**
 * Every path handed to a command run as root is a program at a fixed absolute path, /dev/stdin, or a profile under
 * kAppArmorDir: never a file the AI's sandbox could write (its temporary directories, HOME, the working directory).
 */
function onlyRootOwnedPaths(calls: readonly string[][]): void {
    for (const call of calls) {
        for (const arg of call) {
            if (arg.startsWith('-') || /^[0-7]{4}$/.test(arg) || arg === 'true') {
                continue;
            }
            assert.ok(arg === kAppArmorParser || arg === kInstall || arg === '/dev/stdin' || arg.startsWith(`${kAppArmorDir}/`),
                `a root command was handed ${arg}`);
        }
    }
}

const kBin = '/build/sandbox';
const kBwrap = `${kBin}/bwrap/bwrap`;
const kBuilt = { [kBwrap]: '', [`${kBin}/socat/socat`]: '', [kAppArmorParser]: '' };
const kRestricted = { ...kBuilt, [kRestrictUserns]: '1\n' };
const kPath = `${kBin}/bwrap:${kBin}/socat:/usr/bin`;

void test('off Linux the PATH is left as it is, and nothing runs', () => {
    const m = fakeSystem({});
    assert.deepEqual(sandboxTools('darwin', { PATH: '/usr/bin' }, m.sys), { path: undefined });
    assert.deepEqual(sandboxTools('win32', {}, m.sys), { path: undefined });
    assert.deepEqual(m.calls, []);
});

void test('a kernel that does not restrict user namespaces gets nexus\'s own bwrap first, with no sudo', () => {
    const m = fakeSystem({ ...kBuilt, [kRestrictUserns]: '0\n' });
    assert.deepEqual(sandboxTools('linux', { NEXUS_SANDBOX_BIN: kBin, PATH: '/usr/bin:/bin' }, m.sys),
        { path: `${kBin}/bwrap:${kBin}/socat:/usr/bin:/bin` });
    assert.deepEqual(m.calls, []);
});

void test('no AppArmor at all (WSL2) is the same as no restriction', () => {
    const m = fakeSystem({ ...kBuilt });
    assert.deepEqual(sandboxTools('linux', { NEXUS_SANDBOX_BIN: kBin, PATH: '/usr/bin' }, m.sys), { path: kPath });
    assert.deepEqual(m.calls, []);
});

void test('a restricting kernel with the profile in place runs nothing as root', () => {
    const { name, text } = appArmorProfile(kBwrap);
    const m = fakeSystem({ ...kRestricted, [`${kAppArmorDir}/${name}`]: text });
    assert.deepEqual(sandboxTools('linux', { NEXUS_SANDBOX_BIN: kBin, PATH: '/usr/bin' }, m.sys), { path: kPath });
    assert.deepEqual(m.calls, []);
});

void test('a restricting kernel with no profile loads it through sudo -n, then keeps it, and says so', () => {
    const m = fakeSystem({ ...kRestricted });
    const { name, text } = appArmorProfile(kBwrap);
    const got = sandboxTools('linux', { NEXUS_SANDBOX_BIN: kBin, PATH: '/usr/bin' }, m.sys);
    assert.deepEqual(got, { path: kPath, did: `loaded the AppArmor profile ${kAppArmorDir}/${name}, which lets ${kBwrap} make user namespaces` });
    // The text goes to root on standard input, to both commands: no file the sandbox could change between them.
    assert.deepEqual(m.calls, [['true'], [kAppArmorParser, '-r'], [kInstall, '-m', '0644', '/dev/stdin', `${kAppArmorDir}/${name}`]]);
    assert.deepEqual(m.inputs, [undefined, text, text]);
    onlyRootOwnedPaths(m.calls);
    assert.equal(m.files[`${kAppArmorDir}/${name}`], text);
    // The next start finds it in place and runs nothing.
    m.calls.length = 0;
    assert.deepEqual(sandboxTools('linux', { NEXUS_SANDBOX_BIN: kBin, PATH: '/usr/bin' }, m.sys), { path: kPath });
    assert.deepEqual(m.calls, []);
});

void test('a profile file that differs (another build, an older text) is loaded again', () => {
    const { name } = appArmorProfile(kBwrap);
    const m = fakeSystem({ ...kRestricted, [`${kAppArmorDir}/${name}`]: 'profile old {}\n' });
    const got = sandboxTools('linux', { NEXUS_SANDBOX_BIN: kBin, PATH: '/usr/bin' }, m.sys);
    assert.ok('did' in got);
    assert.equal(m.calls.length, 3);
});

void test('no passwordless sudo refuses in one plain line naming what is missing, and runs nothing else', () => {
    const m = fakeSystem({ ...kRestricted }, false);
    const got = sandboxTools('linux', { NEXUS_SANDBOX_BIN: kBin, PATH: '/usr/bin' }, m.sys);
    assert.ok('cause' in got);
    assert.match(got.cause, /apparmor_restrict_unprivileged_userns = 1/);
    assert.match(got.cause, /a password is required/);
    assert.equal(got.fix, 'passwordless sudo for this user is what is missing');
    assert.doesNotMatch(`${got.cause} ${got.fix}`, /sysctl -w|\n/);
    assert.deepEqual(m.calls, [['true']]);
});

void test('a load that fails refuses and keeps no profile file', () => {
    const m = fakeSystem({ ...kRestricted }, true, kAppArmorParser);
    const { name } = appArmorProfile(kBwrap);
    const got = sandboxTools('linux', { NEXUS_SANDBOX_BIN: kBin, PATH: '/usr/bin' }, m.sys);
    assert.ok('cause' in got);
    assert.match(got.cause, /apparmor_parser: it failed/);
    assert.equal(m.files[`${kAppArmorDir}/${name}`], undefined);
    assert.deepEqual(m.calls.map((c) => c[0]), ['true', kAppArmorParser]);
    onlyRootOwnedPaths(m.calls);
});

void test('no AppArmor parser on a restricting kernel refuses before sudo', () => {
    const m = fakeSystem({ [kBwrap]: '', [`${kBin}/socat/socat`]: '', [kRestrictUserns]: '1' });
    const got = sandboxTools('linux', { NEXUS_SANDBOX_BIN: kBin }, m.sys);
    assert.ok('cause' in got);
    assert.match(got.cause, /apparmor_parser, which loads one, is missing/);
    assert.deepEqual(m.calls, []);
});

void test('the profile names the resolved bwrap, and a path AppArmor would read as a pattern is refused', () => {
    const m = fakeSystem({ ['/link/sandbox/bwrap/bwrap']: '', ['/link/sandbox/socat/socat']: '', [kAppArmorParser]: '',
        [kRestrictUserns]: '1' });
    sandboxTools('linux', { NEXUS_SANDBOX_BIN: '/link/sandbox' }, m.sys);
    assert.match(m.inputs[1] ?? '', /"\/real\/sandbox\/bwrap\/bwrap" flags=\(unconfined\)/);
    const glob = fakeSystem({ ['/b*/bwrap/bwrap']: '', ['/b*/socat/socat']: '', [kAppArmorParser]: '', [kRestrictUserns]: '1' });
    const got = sandboxTools('linux', { NEXUS_SANDBOX_BIN: '/b*' }, glob.sys);
    assert.ok('cause' in got);
    assert.match(got.cause, /read as a pattern/);
    assert.deepEqual(glob.calls, []);
});

void test('the profile allows user namespaces and nothing else, under a name of its path', () => {
    const a = appArmorProfile('/a/bwrap');
    const b = appArmorProfile('/b/bwrap');
    assert.notEqual(a.name, b.name);
    assert.match(a.name, /^nexus-bwrap-[0-9a-f]{16}$/);
    const rules = a.text.split('\n').filter((l) => l.startsWith('  '));
    assert.deepEqual(rules, ['  userns,']);
    assert.match(a.text, /^abi <abi\/4\.0>,$/m);
});

void test('no NEXUS_SANDBOX_BIN, or a missing program, refuses', () => {
    assert.ok('cause' in sandboxTools('linux', { PATH: '/usr/bin' }, fakeSystem(kBuilt).sys));
    assert.ok('cause' in sandboxTools('linux', { NEXUS_SANDBOX_BIN: '', PATH: '/usr/bin' }, fakeSystem(kBuilt).sys));
    const got = sandboxTools('linux', { NEXUS_SANDBOX_BIN: kBin }, fakeSystem({ [kBwrap]: '' }).sys);
    assert.ok('cause' in got);
    assert.match(got.cause, /socat\/socat is missing/);
});

void test('an empty PATH adds nothing after the tools', () => {
    assert.deepEqual(sandboxTools('linux', { NEXUS_SANDBOX_BIN: kBin }, fakeSystem(kBuilt).sys),
        { path: `${kBin}/bwrap:${kBin}/socat` });
});

function fakeHome(present: string[], canMake = true): HomeFs & { made: string[] } {
    const made: string[] = [];
    return {
        made,
        exists: (path) => present.includes(path) || made.includes(path),
        createEmpty: (path) => {
            if (!canMake) {
                return false;
            }
            made.push(path);
            return true;
        },
    };
}

void test('on Linux the driver makes the missing ~/.bashrc and ~/.gitconfig, and never touches one there', () => {
    const none = fakeHome([]);
    assert.deepEqual(sandboxHomeFiles('linux', '/h', none), { made: ['/h/.bashrc', '/h/.gitconfig'] });
    const one = fakeHome(['/h/.bashrc']);
    assert.deepEqual(sandboxHomeFiles('linux', '/h', one), { made: ['/h/.gitconfig'] });
    assert.deepEqual(one.made, ['/h/.gitconfig']);
    const off = fakeHome([]);
    assert.deepEqual(sandboxHomeFiles('darwin', '/h', off), { made: [] });
    assert.deepEqual(sandboxHomeFiles('linux', '', off), { made: [] });
    assert.deepEqual(off.made, []);
});

void test('the check on root\'s paths catches a file the sandbox could write (the check\'s own witness)', () => {
    assert.throws(() => { onlyRootOwnedPaths([[kAppArmorParser, '-r', '/tmp/nexus-apparmor-x/profile']]); }, /handed \/tmp\//);
    assert.throws(() => { onlyRootOwnedPaths([['sudo', kInstall]]); }, /handed sudo/);
});

void test('a home file the sandbox needs that cannot be made refuses the start in one plain line', () => {
    const got = sandboxHomeFiles('linux', '/h', fakeHome(['/h/.bashrc'], false));
    assert.ok('cause' in got);
    assert.match(got.cause, /^\/h\/\.gitconfig is missing and the driver cannot make it/);
    assert.equal(got.fix, 'make it, empty: touch /h/.gitconfig');
    assert.doesNotMatch(`${got.cause} ${got.fix}`, /\n/);
    // One made by another process in the meantime is no refusal.
    const looked = new Set<string>();
    const raced: HomeFs = {
        exists: (p) => { const seen = looked.has(p); looked.add(p); return seen; },
        createEmpty: () => false,
    };
    assert.deepEqual(sandboxHomeFiles('linux', '/h', raced), { made: [] });
});
