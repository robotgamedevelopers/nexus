import assert from 'node:assert/strict';
import { chmodSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';

import { readAccount } from '../../src/account.js';
import { Driver, parseArguments, type Host } from '../../src/driver.js';
import { verifiedAccess } from '../../src/verified.js';
import { kSubscription, result } from '../fake-harness.js';
import { TestHost, line, settle } from './host.js';

const verified = { email: 'personal@example.test', helper: '/test/helper', claude: '/test/claude' };

void test('verified flags are inseparable; a verified account never reads its old setup token', () => {
    const args = ['--cwd', '/work', '--account', 'personal'];
    for (const extra of [['--account-email', verified.email], ['--verified-helper', verified.helper],
        ['--auth-claude', verified.claude], ['--account-email', verified.email, '--verified-helper', verified.helper]]) {
        assert.ok('error' in parseArguments([...args, ...extra]));
    }
    assert.deepEqual(parseArguments([...args, '--verified-helper', verified.helper, '--auth-claude', verified.claude,
        '--account-email', verified.email]), { account: 'personal', cwd: '/work', settingSources: [], verified });
    const home = mkdtempSync(join(tmpdir(), 'nexus-verified-home-'));
    try {
        const dir = join(home, '.nexus/server/accounts/personal');
        mkdirSync(dir, { recursive: true, mode: 0o700 });
        writeFileSync(join(dir, 'oauth-token'), 'legacy-other-account\n', { mode: 0o600 });
        assert.equal((readAccount(home, 'personal', true) as { token?: string }).token, '');
    } finally {
        rmSync(home, { recursive: true, force: true });
    }
});

void test('access helper uses exact account and email; all bad output is withheld', async () => {
    const temp = mkdtempSync(join(tmpdir(), 'nexus-verified-access-'));
    const helper = join(temp, 'helper');
    const account = join(temp, 'personal');
    const settings = { ...verified, helper };
    const run = async (script: string) => {
        writeFileSync(helper, '#!/bin/sh\n' + script);
        chmodSync(helper, 0o700);
        return verifiedAccess(settings, account);
    };
    try {
        const body = `test "$#" = 7 && test "$1" = access && test "$2" = --account-dir && test "$3" = "${account}" && `
            + `test "$4" = --email && test "$5" = ${verified.email} && test "$6" = --claude && test "$7" = ${verified.claude} || exit 13\n`;
        assert.deepEqual(await run(body + "printf 'fresh-token\\n'"), { token: 'fresh-token' });
        for (const code of [11, 12, 13, 1]) {
            assert.equal(await run(`echo never-disclose >&2; echo never-disclose; exit ${code}`), code === 11 || code === 12 ? 'sign-in' : 'unavailable');
        }
        assert.equal(await run('echo bogus; echo token'), 'unavailable');
    } finally {
        rmSync(temp, { recursive: true, force: true });
    }
});

void test('every turn checks its verified grant; rotation resumes, refusal sends no prompt', async () => {
    const host = new TestHost();
    host.untraced = 'The model treats the credential as constant within one harness.';
    host.prepare = (h) => { h.account = { ...kSubscription, tokenSource: 'CLAUDE_CODE_OAUTH_TOKEN_FILE_DESCRIPTOR' }; };
    const checks = ['changed', 'same', 'changed', 'sign-in'] as const;
    let checked = 0;
    const credential: NonNullable<Host['refreshCredential']> = () => Promise.resolve(checks[checked++] ?? 'unavailable');
    const driver = new Driver(Object.assign(host, { refreshCredential: credential }), {
        account: 'personal', verified, cwd: '/work', settingSources: [],
    });
    driver.start();
    await settle();
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'first' }));
    await settle();
    assert.deepEqual(host.harness.prompts, ['first']);
    host.harness.emit(result('session-1'));
    await settle();
    driver.feed(line({ kind: 'send', turn: 2, prompt: 'second' }));
    await settle();
    assert.equal(host.harnesses.length, 2);
    assert.equal(host.harness.start.resume, 'session-1');
    assert.deepEqual(host.harness.prompts, ['second']);
    host.harness.emit(result('session-1'));
    await settle();
    driver.feed(line({ kind: 'send', turn: 3, prompt: 'do not send' }));
    await settle();
    assert.equal(checked, 4);
    assert.ok(host.out.some((m) => m.kind === 'error' && m.code === 'no-credential'));
    assert.deepEqual(host.harness.prompts, ['second']);
    assert.equal(host.exitCode, 1);
});

void test('a verified account refuses an unconfirmed credential source before any prompt', async () => {
    const host = new TestHost();
    host.untraced = 'The model distinguishes billing methods but not the source of a subscription.';
    host.prepare = (h) => { h.account = { ...kSubscription, tokenSource: 'keychain' }; };
    const driver = new Driver(Object.assign(host, { refreshCredential: () => Promise.resolve('same' as const) }), {
        account: 'personal', verified, cwd: '/work', settingSources: [],
    });
    driver.start();
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'private' }));
    await settle();
    assert.deepEqual(host.harness.prompts, []);
    assert.ok(host.out.some((m) => m.kind === 'error' && m.code === 'no-credential'));
});

void test('a temporary verification outage keeps the credential and launches no harness', async () => {
    const host = new TestHost();
    host.untraced = 'No SDK process is started when the credential broker is unavailable.';
    const driver = new Driver(Object.assign(host, { refreshCredential: () => Promise.resolve('unavailable' as const) }), {
        account: 'personal', verified, cwd: '/work', settingSources: [],
    });
    driver.start();
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'private' }));
    await settle();
    assert.equal(host.harnesses.length, 0);
    assert.ok(host.out.some((m) => m.kind === 'error' && m.code === 'harness'));
    assert.ok(!host.out.some((m) => m.kind === 'error' && m.code === 'no-credential'));
});
