// The account (src/account.ts): an account's directory and token file under a HOME made inside the test's working
// directory, each refusal with the file it names and the fix, the start refused before any harness (src/run.ts
// prepare), and Claude's environment: the account's login and no inherited one (src/env.ts).

import assert from 'node:assert/strict';
import { chmodSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { after, test } from 'node:test';

import {
    accountsDir, isAccount, kAccountsUnderHome, openConfigDir, readAccount, type Account, type AccountRefusal,
} from '../../src/account.js';
import { harnessEnv, scrub } from '../../src/env.js';
import { prepare } from '../../src/run.js';

const home = mkdtempSync(join(process.cwd(), '.account-test-'));
after(() => { rmSync(home, { recursive: true, force: true }); });

function refusal(a: Account | AccountRefusal): AccountRefusal {
    assert.ok(!isAccount(a), 'an account where a refusal was expected');
    return a;
}

/** Makes the account `name`, with `token` in its token file at `mode` (no file if undefined). */
function makeAccount(name: string, token?: string, mode = 0o600): string {
    const dir = join(accountsDir(home), name);
    mkdirSync(dir, { recursive: true, mode: 0o700 });
    if (token !== undefined) {
        writeFileSync(join(dir, 'oauth-token'), token, { mode });
        chmodSync(join(dir, 'oauth-token'), mode);
    }
    return dir;
}

void test('a differently cased account cannot share a Claude token or state directory', () => {
    const dir = makeAccount('CaseExample', 'synthetic-token');
    const alias = join(accountsDir(home), 'caseexample');
    if (existsSync(alias)) {
        assert.match(refusal(readAccount(home, 'caseexample')).cause, /spelling/);
    } else {
        assert.match(refusal(readAccount(home, 'caseexample')).cause, /missing/);
    }
    const state = join(home, 'config', 'CaseExample');
    mkdirSync(state, { recursive: true, mode: 0o700 });
    const stateAlias = join(home, 'config', 'caseexample');
    if (existsSync(stateAlias)) {
        assert.match(openConfigDir({ dir, token: '', configDir: stateAlias }) ?? '', /spelling/);
    }
});

void test('an account is its directory under ~/.nexus/server/accounts and the one token in its file', () => {
    const dir = makeAccount('main', 'sk-ant-oat01-secret\n');
    assert.equal(dir, `${home}/.nexus/server/accounts/main`);
    const a = readAccount(home, 'main');
    assert.deepEqual(a, { name: 'main', dir, configDir: `${home}/.nexus/server/harness/claude/main`, token: 'sk-ant-oat01-secret' });
    makeAccount('crlf', '  tok-2\r\n\r\n');
    assert.deepEqual(readAccount(`${home}/`, 'crlf'), { name: 'crlf', dir: `${home}/.nexus/server/accounts/crlf`,
        configDir: `${home}/.nexus/server/harness/claude/crlf`, token: 'tok-2' });
});

void test('the logins are where the server looks for them: the one path of src/server/home.h', () => {
    // dist/test/unit -> the repository's root.
    const header = readFileSync(join(import.meta.dirname, '..', '..', '..', '..', '..', 'src', 'server', 'home.h'),
        'utf8');
    assert.ok(header.includes(`kAccountsUnderHome = "${kAccountsUnderHome}";`), 'home.h names another accounts path');
    assert.equal(accountsDir('/h/'), `/h/${kAccountsUnderHome}`);
});

void test('each missing or unsafe login is refused, naming the file and the fix, never the token', () => {
    const noHome = refusal(readAccount(undefined, 'main'));
    assert.match(noHome.cause, /HOME is not set/);
    assert.match(refusal(readAccount('', 'main')).cause, /HOME is not set/);

    for (const bad of ['', '.hidden', '../up', 'a/b', 'x'.repeat(65)]) {
        assert.match(refusal(readAccount(home, bad)).cause, /is not an account name/, bad);
    }

    const missing = refusal(readAccount(home, 'nobody'));
    const nobody = `${home}/.nexus/server/accounts/nobody`;
    assert.match(missing.cause, /has no directory/);
    assert.ok(missing.cause.includes(nobody));
    assert.ok(missing.fix.includes(`CLAUDE_CONFIG_DIR=${nobody} claude setup-token`));
    assert.ok(missing.fix.includes(`${nobody}/oauth-token with mode 0600`));

    const loggedOut = makeAccount('out');
    const out = refusal(readAccount(home, 'out'));
    assert.match(out.cause, /is not logged in/);
    assert.ok(out.cause.includes(`${loggedOut}/oauth-token is missing`));
    assert.ok(out.fix.includes(`CLAUDE_CONFIG_DIR=${loggedOut} claude setup-token`));

    makeAccount('open', 'sk-open-secret\n', 0o644);
    const open = refusal(readAccount(home, 'open'));
    assert.match(open.cause, /mode 0644, so others can read it/);
    assert.match(open.fix, /chmod 600/);
    makeAccount('group', 'sk-group-secret\n', 0o640);
    assert.match(refusal(readAccount(home, 'group')).cause, /mode 0640/);

    makeAccount('two', 'one\ntwo\n');
    assert.match(refusal(readAccount(home, 'two')).cause, /one token on one line/);
    makeAccount('empty', '\n');
    assert.match(refusal(readAccount(home, 'empty')).cause, /one token on one line/);

    const dirAccount = makeAccount('dir');
    mkdirSync(join(dirAccount, 'oauth-token'));
    assert.match(refusal(readAccount(home, 'dir')).cause, /not a regular file/);

    if (process.getuid?.() !== 0) {
        makeAccount('locked', 'sk-locked-secret\n', 0o000);
        assert.match(refusal(readAccount(home, 'locked')).cause, /cannot be read/);
    }

    // A token that is a link (to a file elsewhere, say another tool's login) is refused, whatever the target's mode.
    const elsewhere = join(home, 'elsewhere-token');
    writeFileSync(elsewhere, 'sk-elsewhere-secret\n', { mode: 0o600 });
    const linked = makeAccount('linked');
    symlinkSync(elsewhere, join(linked, 'oauth-token'));
    assert.match(refusal(readAccount(home, 'linked')).cause, /is a link/);

    // The account's directory must be the user's own, mode 0700, and not a link.
    const loose = makeAccount('loose', 'sk-loose-secret\n');
    chmodSync(loose, 0o755);
    const looseRefusal = refusal(readAccount(home, 'loose'));
    assert.match(looseRefusal.cause, /mode 0755/);
    assert.match(looseRefusal.fix, /chmod 700/);
    symlinkSync(makeAccount('real', 'sk-real-secret\n'), join(accountsDir(home), 'alias'));
    assert.match(refusal(readAccount(home, 'alias')).cause, /not a directory of this user's/);

    for (const name of ['nobody', 'out', 'open', 'group', 'two', 'empty', 'dir', 'locked', 'linked', 'loose', 'alias']) {
        const r = readAccount(home, name);
        assert.ok(!JSON.stringify(r).includes('secret'), name);
    }
});

void test('the start is refused with no-credential before any harness when the account has no login', () => {
    const findHere = (name: string): Account | AccountRefusal => readAccount(home, name);
    const r = prepare(['--account', 'nobody', '--cwd', '/w'], () => true, findHere);
    assert.ok('refusal' in r);
    assert.equal(r.refusal.length, 2);
    assert.equal(r.refusal[0]?.kind, 'hello');
    const e = r.refusal[1];
    assert.ok(e?.kind === 'error');
    assert.equal(e.code, 'no-credential');
    assert.ok(e.cause.includes(`${home}/.nexus/server/accounts/nobody`));
    assert.match(e.fix, /claude setup-token/);

    const noAccount = prepare(['--cwd', '/w'], () => true, findHere);
    assert.ok('refusal' in noAccount && noAccount.refusal[1]?.kind === 'error' && noAccount.refusal[1].code === 'other');

    makeAccount('ready', 'sk-ready-secret\n');
    const ok = prepare(['--account', 'ready', '--cwd', '/w'], () => true, findHere);
    assert.ok('account' in ok);
    assert.equal(ok.account.token, 'sk-ready-secret');
    assert.equal(ok.args.account, 'ready');
});

void test('the start is refused before any account is read when the AI\'s sandbox cannot be set up', () => {
    let looked = false;
    const findAny = (name: string): Account | AccountRefusal => {
        looked = true;
        return { name, dir: '/h/a', configDir: '/h/c', token: 't' };
    };
    const setup = { cause: 'the sandbox\'s /b/bwrap/bwrap is missing', fix: 'build nexus-server again' };
    const r = prepare(['--account', 'ready', '--cwd', '/w'], () => true, findAny, () => undefined, setup);
    assert.ok('refusal' in r);
    assert.deepEqual(r.refusal.map((m) => m.kind), ['hello', 'error']);
    const e = r.refusal[1];
    assert.ok(e?.kind === 'error');
    assert.equal(e.cause, 'no sandbox for the AI: the sandbox\'s /b/bwrap/bwrap is missing');
    assert.equal(e.fix, 'build nexus-server again');
    assert.equal(looked, false, 'no account is read for a session that cannot start');
    // Without a refusal the same start goes on.
    assert.ok('account' in prepare(['--account', 'ready', '--cwd', '/w'], () => true, findAny, () => undefined, undefined));
});

void test('a limited friend starts only where the limit holds, else it is refused before any account is read', () => {
    let looked = false;
    const findAny = (name: string): Account | AccountRefusal => {
        looked = true;
        return { name, dir: '/h/a', configDir: '/h/c', token: 't' };
    };
    const linux = { home: '/h', platform: 'linux', realpath: (p: string): string => p.replace(/^\/link/, '/h') };
    const start = (cwd: string, place = linux, more: string[] = []): ReturnType<typeof prepare> =>
        prepare(['--account', 'ready', '--cwd', cwd, '--limit', 'cwd', ...more], () => true, findAny, () => undefined,
            undefined, place);
    const refused = (r: ReturnType<typeof prepare>): string => {
        assert.ok('refusal' in r);
        const e = r.refusal[1];
        assert.ok(e?.kind === 'error' && e.code === 'other');
        assert.match(e.fix, /"limit"/);
        return e.cause;
    };
    assert.match(refused(start('/h')), /is the home directory/);
    assert.match(refused(start('/link')), /is the home directory/, 'a link to HOME is HOME');
    assert.match(refused(start('/')), /root of the file system/);
    assert.match(refused(start('/h/.nexus/server')), /inside ~\/.nexus/);
    assert.match(refused(start('/h/.ssh')), /inside ~\/.ssh/);
    assert.match(refused(start('/h/.nexus')), /holds ~\/.nexus/);
    assert.match(refused(start('/h/w', { ...linux, platform: 'win32' })), /cannot limit it on win32/);
    assert.match(refused(start('/h/w', linux, ['--claude', '/bin/claude'])), /--claude \/bin\/claude/);
    assert.match(refused(start('/h/w', { ...linux, home: '' })), /HOME is not set/);
    assert.equal(looked, false, 'no account is read for a limited session that cannot start');
    // Its own directory, anywhere else: under HOME, or beside it.
    assert.ok('account' in start('/h/w'));
    assert.ok('account' in start('/srv/proj'));
    assert.ok('account' in start('/h/w', { ...linux, platform: 'darwin' }));
    // Unlimited, the same places start as before.
    assert.ok('account' in prepare(['--account', 'ready', '--cwd', '/h'], () => true, findAny, () => undefined, undefined, linux));
    // The directory is resolved once: the path checked is the path the session works in (and limitOf is given). A
    // link that a second resolve would see moved (to HOME) changes nothing: the first answer is the one used.
    let calls = 0;
    const moving = { ...linux, realpath: (p: string): string => {
        if (p !== '/link/w') {
            return p;
        }
        calls += 1;
        return calls === 1 ? '/srv/w' : '/h';
    } };
    const once = start('/link/w', moving);
    assert.ok('args' in once && once.args.cwd === '/srv/w' && calls === 1, JSON.stringify(once));
    assert.match(refused(start('/gone', { ...linux, realpath: () => { throw new Error('ENOENT'); } })), /does not resolve/);
    const plain = prepare(['--account', 'ready', '--cwd', '/link/w'], () => true, findAny, () => undefined, undefined, linux);
    assert.ok('args' in plain && plain.args.cwd === '/link/w', 'unlimited: the directory as given');
});

void test('Claude gets the account\'s directory, none it inherited, and its token in no environment', () => {
    const { env } = scrub({
        CLAUDE_CONFIG_DIR: '/Users/me/.claude', CLAUDE_CODE_OAUTH_TOKEN: 'inherited', ANTHROPIC_API_KEY: 'k',
        PATH: '/bin', HOME: '/h',
    });
    const account: Account = { name: 'main', dir: '/h/.nexus/server/accounts/main',
        configDir: '/h/.nexus/server/harness/claude/main', token: 'own' };
    const harness = harnessEnv(env, account);
    assert.deepEqual(harness, { PATH: '/bin', HOME: '/h', CLAUDE_CONFIG_DIR: '/h/.nexus/server/harness/claude/main' });
    assert.ok(!Object.values(harness).includes('own'), 'the token is in Claude\'s environment');
});
