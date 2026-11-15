// The token wall's pieces (src/wall.ts), and the account's Claude state kept apart from its login (src/account.ts
// openConfigDir): what the sandbox, the deny rules and the hook are built from, and what the hook refuses, over a fake
// file system. No process, no socket, no time; files only inside the test's working directory. The wall end to end,
// against the real Claude, is the functional test (test/functional/wall.test.ts).

import assert from 'node:assert/strict';
import { existsSync, linkSync, lstatSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { after, test } from 'node:test';

import { configDirOf, kClaudeStateFiles, kMaxStateFileBytes, openConfigDir } from '../../src/account.js';
import {
    bothForms, configFiles, folded, kClaudeSandboxProtects, kCredentialNames, kDataRoots, kGitNameRules, kLimitedReadsUnderHome, kLoginVariables, kNoSettingsHooks, limitOf, nexusOutsideState, programsOf, projectStateOf, realOf, toolRefusal, wallDenyRules, wallHooks, wallOf, wallSandbox,
    type Wall, type WallFs,
} from '../../src/wall.js';
import { LeakScan } from '../leak-scan.js';

const scratch = mkdtempSync(join(process.cwd(), '.wall-unit-'));
after(() => { rmSync(scratch, { recursive: true, force: true }); });

// macOS's /var is /private/var: the fake resolves /var/home the same way.
const realpath = (p: string): string => p.replace(/^\/var\//, '/private/var/');

void test('the wall names the accounts\' directory and the config files in both spellings', () => {
    assert.deepEqual(bothForms('/h/x/', realpath), ['/h/x']);
    assert.deepEqual(bothForms('/var/h', realpath), ['/var/h', '/private/var/h']);
    assert.deepEqual(bothForms('/gone', () => { throw new Error('ENOENT'); }), ['/gone']);
    const wall = wallOf('/var/home', '/w', '/var/home/.nexus/server/harness/claude/main', realpath);
    assert.deepEqual(wall.secrets, ['/var/home/.nexus/server/accounts', '/private/var/home/.nexus/server/accounts']);
    for (const f of configFiles('/w', '/var/home/.nexus/server/harness/claude/main', '/var/home')) {
        assert.ok(wall.config.includes(f), f);
    }
    assert.ok(wall.config.includes('/private/var/home/.nexus/server/harness/claude/main/settings.json'));
    assert.ok(wall.config.includes('/w/.claude/settings.local.json'));
    assert.ok(wall.config.includes('/w/.mcp.json'));
    // Claude's state is never inside the logins' directory, so that the directory can be walled whole.
    assert.ok(!configDirOf('/h', 'main').startsWith('/h/.nexus/server/accounts'));
});

const wall: Wall = {
    secrets: ['/h/.nexus/server/accounts'],
    config: configFiles('/w', '/h/.nexus/server/harness/claude/main', '/h'),
};

void test('the sandbox is on with no way out, the directory unreadable and unwritable, the login variables unset', () => {
    const s = wallSandbox(wall, ['/h', '/w'], 4321);
    assert.deepEqual(s.network, { httpProxyPort: 4321, socksProxyPort: 4321 });
    assert.equal(s.enabled, true);
    assert.equal(s.failIfUnavailable, true);
    assert.equal(s.allowUnsandboxedCommands, false);
    assert.equal(s.autoAllowBashIfSandboxed, true);
    assert.deepEqual(s.filesystem.denyRead, ['/h/.nexus/server/accounts']);
    // The startup files Claude's sandbox protects itself stay out of its denyWrite (kClaudeSandboxProtects).
    assert.ok(wall.config.includes('/h/.bashrc') && !s.filesystem.denyWrite.includes('/h/.bashrc'));
    assert.deepEqual(s.filesystem.denyWrite, ['/h/.nexus/server/accounts',
        ...wall.config.filter((p) => !kClaudeSandboxProtects.has(p.slice(p.lastIndexOf('/') + 1)))]);
    assert.deepEqual(s.filesystem.allowWrite, ['/h', '/w']);
    assert.deepEqual(s.credentials.files, [{ path: '/h/.nexus/server/accounts', mode: 'deny' }]);
    const denied: string[] = s.credentials.envVars.map((v) => v.name);
    for (const name of ['CLAUDE_CODE_OAUTH_TOKEN', 'CLAUDE_CODE_OAUTH_TOKEN_FILE_DESCRIPTOR', 'ANTHROPIC_API_KEY']) {
        assert.ok(denied.includes(name), name);
    }
    assert.deepEqual(denied, [...kLoginVariables]);
    assert.deepEqual(s.credentials.envVars.map((v) => v.mode as string), kLoginVariables.map(() => 'deny'));
});

void test('the deny rules keep the file tools out of the directory and off the config files', () => {
    const rules = wallDenyRules(wall);
    for (const r of ['Read(//h/.nexus/server/accounts)', 'Read(//h/.nexus/server/accounts/**)',
        'Edit(//h/.nexus/server/accounts)', 'Edit(//h/.nexus/server/accounts/**)', 'Edit(//w/.claude/settings.json)',
        'Edit(//w/.mcp.json)', 'Edit(//h/.nexus/server/harness/claude/main/.claude.json)', 'Edit(//h/.ssh/**)']) {
        assert.ok(rules.includes(r), r);
    }
    // Reading a config file is fine: only writing one would let a command run outside the sandbox.
    assert.ok(!rules.some((r) => r.startsWith('Read(//w/')));
    assert.deepEqual(kNoSettingsHooks, { disableAllHooks: true });
});

void test('the file tools may not write what makes a git directory, by name, anywhere', () => {
    const rules = wallDenyRules(wall);
    for (const r of ['Edit(//**/.git/config)', 'Edit(//**/.git/hooks)', 'Edit(//**/.git/hooks/**)', 'Edit(//**/commondir)']) {
        assert.ok(rules.includes(r), r);
    }
    assert.deepEqual([...kGitNameRules].sort(), rules.filter((r) => r.startsWith('Edit(//**/')).sort());
    // They hold for every wall, whatever its paths: an empty one too.
    assert.deepEqual(wallDenyRules({ secrets: [], config: [] }), [...kGitNameRules]);
    // Never a rule on .git itself or on HEAD: Claude merges Edit rules into the macOS sandbox, where they stop commits.
    assert.ok(!rules.some((r) => /\.git\)|\.git\/\*\*\)|HEAD/.test(r)));
});

void test('the hook refuses the file tools a .git, anything in one, a HEAD or a commondir, and lets reading them be', () => {
    const fs = fakeFs({});
    const refuse = (tool: string, input: Record<string, unknown>): string | undefined =>
        toolRefusal(tool, input, wall, '/w', '/h', fs);
    for (const path of ['/w/elsewhere/.git', '.git', 'repo/.git/config', '/h/gitdir/HEAD', '~/gitdir/commondir']) {
        assert.match(refuse('Write', { file_path: path, content: 'x' }) ?? '', /what makes a git directory/, path);
        assert.match(refuse('Edit', { file_path: path, old_string: 'a', new_string: 'b' }) ?? '', /git directory/, path);
    }
    assert.equal(refuse('Read', { file_path: 'repo/.git/config' }), undefined);
    assert.equal(refuse('Write', { file_path: '/w/src/.gitignore', content: 'x' }), undefined);
    assert.equal(refuse('Write', { file_path: '/w/HEAD.md', content: 'x' }), undefined);
});

/** A file system of paths to identities, with links resolved by `links`. */
function fakeFs(files: Record<string, string>, links: Record<string, string> = {}): WallFs {
    const resolveLinks = (p: string): string => {
        for (const [from, to] of Object.entries(links)) {
            if (p === from || p.startsWith(`${from}/`)) {
                return to + p.slice(from.length);
            }
        }
        return p;
    };
    return {
        realpath(p) {
            const r = resolveLinks(p);
            if (!(r in files) && !Object.keys(files).some((f) => f.startsWith(`${r}/`))) {
                throw new Error('ENOENT');
            }
            return r;
        },
        identity: (p) => files[resolveLinks(p)],
        files: (root) => Object.keys(files).filter((f) => f.startsWith(`${root}/`)),
    };
}

const token = '/h/.nexus/server/accounts/main/oauth-token';
const fs = fakeFs({ [token]: '1:10', '/w/notes.md': '1:11', '/w/hard-link': '1:10' }, { '/w/soft': '/h/.nexus/server/accounts/main' });
const refuse = (tool: string, input: Record<string, unknown>): string | undefined => toolRefusal(tool, input, wall, '/w', '/h', fs);

void test('the hook refuses a call that names the directory in a command, pattern, URL or path', () => {
    for (const command of ['cat ~/.nexus/server/accounts/main/oauth-token', 'ls $HOME/.nexus/server/accounts',
        'ls ${HOME}/.nexus/server/accounts', `cat ${token}`]) {
        assert.match(refuse('Bash', { command }) ?? '', /accounts' directory/, command);
    }
    assert.ok(refuse('Grep', { pattern: 'x', path: '/h/.nexus/server/accounts' }) !== undefined);
    assert.ok(refuse('Glob', { pattern: '/h/.nexus/server/accounts/**' }) !== undefined);
    assert.ok(refuse('WebFetch', { url: `http://localhost:8000${token}` }) !== undefined);
    // Words that only mention it, in what a tool writes, are not a way in: editing these docs stays possible.
    assert.equal(refuse('Write', { file_path: '/w/notes.md', content: 'the logins are in ~/.nexus/server/accounts' }), undefined);
    assert.equal(refuse('Bash', { command: 'grep -rn ".nexus/server/accounts" docs/' }), undefined);
});

void test('the hook refuses a path that resolves into the directory, or a file of it under another name', () => {
    assert.ok(refuse('Read', { file_path: token }) !== undefined);
    assert.ok(refuse('Read', { file_path: '~/.nexus/server/accounts/main/oauth-token' }) !== undefined);
    assert.ok(refuse('Read', { file_path: '../h/.nexus/server/accounts/main/oauth-token' }) !== undefined);
    assert.ok(refuse('Read', { file_path: 'soft/oauth-token' }) !== undefined, 'a symbolic link');
    assert.ok(refuse('Read', { file_path: 'hard-link' }) !== undefined, 'a hard link');
    assert.ok(refuse('Write', { file_path: '/h/.nexus/server/accounts/main/api-key', content: 'x' }) !== undefined, 'a new file');
    assert.equal(refuse('Read', { file_path: 'notes.md' }), undefined);
    assert.equal(refuse('Read', { file_path: '/w/missing.md' }), undefined);
    // A search that only passes over the directory is the deny rules' to keep out of it, not the hook's to refuse.
    assert.equal(refuse('Grep', { pattern: 'x', path: '/h' }), undefined);
});

void test('the hook refuses a write to a config file, and a URL that is not http or https', () => {
    assert.match(refuse('Write', { file_path: '.claude/settings.local.json', content: '{}' }) ?? '', /outside the sandbox/);
    assert.ok(refuse('Edit', { file_path: '/w/.mcp.json', old_string: 'a', new_string: 'b' }) !== undefined);
    assert.ok(refuse('Write', { file_path: '/h/.nexus/server/harness/claude/main/settings.json', content: '{}' }) !== undefined);
    assert.ok(refuse('Write', { file_path: '~/.gitconfig', content: '[core]' }) !== undefined, 'the person\'s git config');
    assert.ok(refuse('Write', { file_path: '/h/Library/LaunchAgents/x.plist', content: '' }) !== undefined, 'a login agent');
    assert.equal(refuse('Read', { file_path: '.claude/settings.json' }), undefined, 'reading one is fine');
    assert.equal(refuse('Write', { file_path: '/h/.nexus/server/harness/claude/main/projects/m/memory/a.md', content: 'x' }),
        undefined, 'memory is writable');
    assert.match(refuse('WebFetch', { url: 'file:///etc/hosts', prompt: 'p' }) ?? '', /http and https/);
    assert.equal(refuse('WebFetch', { url: 'https://example.com/', prompt: 'p' }), undefined);
});

void test('the hook answers deny with the reason, and nothing for a call it lets go', async () => {
    const matcher = wallHooks(wall, '/w', '/h', fs).PreToolUse?.[0];
    const hook = matcher?.hooks[0];
    assert.ok(hook !== undefined);
    const base = { session_id: 's', transcript_path: '/t', cwd: '/w', hook_event_name: 'PreToolUse' as const, tool_use_id: 'u' };
    const signal = new AbortController().signal;
    const denied = await hook({ ...base, tool_name: 'Read', tool_input: { file_path: token } }, 'u', { signal });
    assert.deepEqual((denied as { hookSpecificOutput?: { permissionDecision?: string } }).hookSpecificOutput?.permissionDecision, 'deny');
    assert.deepEqual(await hook({ ...base, tool_name: 'Read', tool_input: { file_path: 'notes.md' } }, 'u', { signal }), {});
});

void test('a new Claude state directory takes only the person\'s settings and instructions from the login, never a transcript', () => {
    const home = join(scratch, 'home');
    const login = join(home, '.nexus/server/accounts/main');
    // A made-up token, of letters found in nothing else here, in every place an older driver's Claude may have put it.
    const token = 'nexuswallMIGRATEqzxjkvwpTOKENbcdfghmnrst';
    mkdirSync(join(login, 'projects', 'p', 'memory'), { recursive: true, mode: 0o700 });
    mkdirSync(join(login, 'shell-snapshots'), { mode: 0o700 });
    writeFileSync(join(login, 'oauth-token'), `${token}\n`, { mode: 0o600 });
    writeFileSync(join(login, 'api-key'), 'secret-key\n', { mode: 0o600 });
    writeFileSync(join(login, '.claude.json'), `{"projects":{"/w":{"history":[{"display":"use ${token}"}]}}}`);
    writeFileSync(join(login, 'projects', 'p', 'session.jsonl'), `{"tool_result":"${token}"}\n`);
    writeFileSync(join(login, 'projects', 'p', 'memory', 'notes.md'), `the token is ${token}\n`);
    writeFileSync(join(login, 'shell-snapshots', 'snapshot-bash.sh'), `export CLAUDE_CODE_OAUTH_TOKEN=${token}\n`);
    writeFileSync(join(login, 'history.jsonl'), `{"display":"${token}"}\n`);
    writeFileSync(join(login, 'settings.json'), '{"model":"opus"}');
    writeFileSync(join(login, 'CLAUDE.md'), 'be kind\n');
    // A link named as a file that is taken, to the token: never followed.
    symlinkSync(join(login, 'oauth-token'), join(login, 'settings.local.json'));
    const configDir = configDirOf(home, 'main');
    assert.equal(openConfigDir({ dir: login, configDir, token }), undefined);
    assert.deepEqual(readdirSync(configDir).sort(), ['CLAUDE.md', 'settings.json']);
    assert.equal(readFileSync(join(configDir, 'settings.json'), 'utf8'), '{"model":"opus"}');
    assert.equal(readFileSync(join(configDir, 'CLAUDE.md'), 'utf8'), 'be kind\n');
    const scan = new LeakScan(token, 'main');
    for (const name of readdirSync(configDir, { recursive: true, encoding: 'utf8' })) {
        const p = join(configDir, name);
        if (lstatSync(p).isFile()) {
            assert.equal(scan.find(readFileSync(p, 'utf8')), '', `${name} holds the token`);
        }
    }
    // What is not taken stays where it was.
    assert.ok(existsSync(join(login, 'projects', 'p', 'session.jsonl')));
    assert.equal(lstatSync(configDir).mode & 0o777, 0o700);
    // Once: a later start leaves it as it is.
    writeFileSync(join(login, 'CLAUDE.md'), 'changed\n');
    assert.equal(openConfigDir({ dir: login, configDir, token }), undefined);
    assert.equal(readFileSync(join(configDir, 'CLAUDE.md'), 'utf8'), 'be kind\n');
    // A file where the directory should be is refused, with the fix.
    const other = configDirOf(home, 'other');
    writeFileSync(other, 'not a directory');
    assert.match(openConfigDir({ dir: login, configDir: other, token }) ?? '', /not a directory of this user's/);
    // A login with nothing in it makes an empty one.
    const fresh = join(home, '.nexus/server/accounts/fresh');
    mkdirSync(fresh, { mode: 0o700 });
    assert.equal(openConfigDir({ dir: fresh, configDir: configDirOf(home, 'fresh'), token }), undefined);
    assert.ok(lstatSync(configDirOf(home, 'fresh')).isDirectory());
});

void test('a new Claude state directory refuses a taken file that holds the token, has another name, or is too large', () => {
    const home = join(scratch, 'home-refuse');
    const token = 'nexuswallREFUSEqzxjkvwpTOKENbcdfghmnrst';
    const cases: [string, (login: string) => void, RegExp][] = [
        ...kClaudeStateFiles.map((name): [string, (login: string) => void, RegExp] =>
            [`the token in ${name}`, (login) => {
                writeFileSync(join(login, name), `{"env":{"X":"${token}"}}\n`);
            }, /holds the account's login token/]),
        ...kClaudeStateFiles.map((name): [string, (login: string) => void, RegExp] =>
            [`${name} a hard link to the token`, (login) => {
                linkSync(join(login, 'oauth-token'), join(login, name));
            }, /has 2 names \(a hard link\)/]),
        ...kClaudeStateFiles.map((name): [string, (login: string) => void, RegExp] =>
            [`${name} a hard link to a file elsewhere`, (login) => {
                writeFileSync(join(login, '..', `elsewhere-${name}`), 'x');
                linkSync(join(login, '..', `elsewhere-${name}`), join(login, name));
            }, /has 2 names/]),
        ['a CLAUDE.md larger than the cap', (login) => {
            writeFileSync(join(login, 'CLAUDE.md'), 'x'.repeat(kMaxStateFileBytes + 1));
        }, /larger than/],
    ];
    for (const [i, [what, plant, refusal]] of cases.entries()) {
        const login = join(home, `.nexus/server/accounts/a${i}`);
        mkdirSync(login, { recursive: true, mode: 0o700 });
        writeFileSync(join(login, 'oauth-token'), `${token}\n`, { mode: 0o600 });
        plant(login);
        const configDir = configDirOf(home, `a${i}`);
        const got = openConfigDir({ dir: login, configDir, token }) ?? '';
        assert.match(got, refusal, what);
        assert.ok(got.includes(login), `${what}: the refusal names the file`);
        assert.ok(!got.includes(token), `${what}: the refusal holds the token`);
        assert.ok(!existsSync(configDir), `${what}: the directory was made`);
        assert.deepEqual(readdirSync(dirname(configDir)).filter((n) => n.includes('partial')), [], `${what}: a partial was left`);
    }
    // The witness: the same login with nothing planted is taken.
    const login = join(home, '.nexus/server/accounts/plain');
    mkdirSync(login, { recursive: true, mode: 0o700 });
    writeFileSync(join(login, 'oauth-token'), `${token}\n`, { mode: 0o600 });
    writeFileSync(join(login, 'CLAUDE.md'), 'x'.repeat(kMaxStateFileBytes));
    assert.equal(openConfigDir({ dir: login, configDir: configDirOf(home, 'plain'), token }), undefined);
    assert.deepEqual(readdirSync(configDirOf(home, 'plain')), ['CLAUDE.md']);
});

void test('the migration test\'s scan finds a token planted in the new directory (its witness)', () => {
    const token = 'nexuswallWITNESSqzxjkvwpTOKENbcdfghmnrst';
    assert.notEqual(new LeakScan(token, 'main').find(`{"tool_result":"${token}"}`), '');
});

void test('nothing under ~/.nexus is writable but the session\'s own Claude state', () => {
    const dirs: Record<string, string[]> = {
        '/h/.nexus': ['server', 'other'],
        '/h/.nexus/server': ['drivers', 'store', 'harness', 'accounts'],
        '/h/.nexus/server/harness': ['claude'],
        '/h/.nexus/server/harness/claude': ['main', 'work'],
    };
    const got = nexusOutsideState('/h', '/h/.nexus/server/harness/claude/main', (d) => dirs[d] ?? []);
    for (const p of ['/h/.nexus/other', '/h/.nexus/server/drivers', '/h/.nexus/server/config', '/h/.nexus/server/nexus-config',
        '/h/.nexus/server/store', '/h/.nexus/server/accounts', '/h/.nexus/server/harness/claude/work']) {
        assert.ok(got.includes(p), p);
    }
    for (const p of ['/h/.nexus', '/h/.nexus/server', '/h/.nexus/server/harness', '/h/.nexus/server/harness/claude',
        '/h/.nexus/server/harness/claude/main']) {
        assert.ok(!got.includes(p), p);
    }
    // State kept anywhere else leaves all of ~/.nexus unwritable.
    assert.deepEqual(nexusOutsideState('/h', '/elsewhere/main', () => []), ['/h/.nexus']);
});

void test('the programs nexus runs: node, the driver, the sandbox tools, and the PATH under home', () => {
    const got = programsOf('/h', '/h/sdk/bin/node', '/h/nexus/drivers/claude',
        { NEXUS_SANDBOX_BIN: '/h/nexus/build/sandbox/', PATH: '/h/.local/bin:/usr/bin:/h:relative:/opt/homebrew/bin:/h/sdk/bin' });
    assert.deepEqual(got, ['/h/sdk/bin', '/h/nexus/drivers/claude', '/h/nexus/build/sandbox', '/h/.local/bin']);
    const files = configFiles('/w', '/h/.nexus/server/harness/claude/main', '/h', got, () => []);
    for (const p of [...got, '/h/.config/fish', '/h/.ssh', '/h/.config/systemd', '/h/.claude', '/h/.nexus/server/drivers']) {
        assert.ok(files.includes(p), p);
    }
});

/**
 * A name as a case-insensitive macOS volume compares it, written out by hand and not with the hook's `folded`: case
 * folded, and the two compatibility letters it treats as their plain one (the long s, the Kelvin sign). A fullwidth
 * letter and a Cyrillic look-alike are other names to it.
 */
function volumeName(name: string): string {
    return name.replace(/ſ/g, 's').replace(/K/g, 'k').toLowerCase();
}

/**
 * A disk of folders and files, each with an identity (`dev:ino`), links resolved component by component, and on a
 * case-insensitive volume (`caseless`) any spelling of a name finding it: a stand-in for realpath and stat.
 */
function diskFs(entries: Record<string, string>, links: Record<string, string>, caseless: boolean): WallFs {
    const same = (a: string, b: string): boolean => (caseless ? volumeName(a) === volumeName(b) : a === b);
    const canon = (p: string): string | undefined => {
        const parts = p.split('/').filter((s) => s !== '');
        let at = '';
        for (const part of parts) {
            const next = `${at}/${part}`;
            const link = Object.keys(links).find((l) => same(l, next));
            if (link !== undefined) {
                at = links[link] ?? '';
                continue;
            }
            const hit = Object.keys(entries).find((e) => same(e, next));
            if (hit === undefined) {
                return undefined;
            }
            at = hit;
        }
        return at === '' ? '/' : at;
    };
    return {
        realpath(p) {
            const c = canon(p);
            if (c === undefined) {
                throw new Error('ENOENT');
            }
            return c;
        },
        identity: (p) => {
            const c = canon(p);
            return c === undefined ? undefined : entries[c] ?? (c === '/' ? '0:1' : undefined);
        },
        files: (root) => Object.keys(entries).filter((f) => f.startsWith(`${root}/`) && entries[f]?.startsWith('f') === true),
    };
}

const kDisk: Record<string, string> = {
    '/h': 'd:2', '/h/.nexus': 'd:3', '/h/.nexus/server': 'd:4', '/h/.nexus/server/accounts': 'd:5',
    '/h/.nexus/server/accounts/main': 'd:6', '/h/.nexus/server/accounts/main/oauth-token': 'f:7',
    '/h/.claude': 'd:8', '/h/.ssh': 'd:9', '/w': 'd:10', '/w/sub': 'd:11', '/w/notes.md': 'f:12',
};
const kDiskLinks = { '/w/lnk2': '/h/.nexus/server/accounts', '/w/lnk': '/h/.claude' };
const diskWall: Wall = { secrets: ['/h/.nexus/server/accounts'], config: configFiles('/w', '/h/.nexus/server/harness/claude/main', '/h') };

void test('the hook judges a path by every spelling of it, where it resolves, and folder identity', () => {
    for (const caseless of [false, true]) {
        const d = diskFs(kDisk, kDiskLinks, caseless);
        const refuse = (tool: string, input: Record<string, unknown>): string | undefined => toolRefusal(tool, input, diskWall, '/w', '/h', d);
        const why = `(${caseless ? 'case-insensitive' : 'case-sensitive'})`;
        assert.match(refuse('Write', { file_path: '/h/.nexus//server/accounts/main/new-file', content: 'x' }) ?? '', /accounts' directory/, `doubled slash ${why}`);
        assert.match(refuse('Write', { file_path: '/w/lnk2/main/new-file', content: 'x' }) ?? '', /accounts' directory/, `a link to the accounts, a new name ${why}`);
        assert.match(refuse('Read', { file_path: 'lnk2/main/oauth-token' }) ?? '', /accounts' directory/, `a link, relative ${why}`);
        assert.match(refuse('Write', { file_path: '/w/lnk/settings.json', content: '{}' }) ?? '', /outside the sandbox/, `a link to ~/.claude ${why}`);
        assert.match(refuse('Write', { file_path: '/h//.ssh/config', content: '' }) ?? '', /outside the sandbox/, `doubled slash to ~/.ssh ${why}`);
        assert.match(refuse('Write', { file_path: '/w/sub/../.mcp.json', content: '{}' }) ?? '', /outside the sandbox/, `.. to the MCP file ${why}`);
        assert.match(refuse('Glob', { pattern: '.nexus/server/accounts/**', path: '/h' }) ?? '', /accounts' directory/, `Glob under HOME ${why}`);
        assert.match(refuse('Grep', { pattern: 'x', path: '/w', glob: '../h/.nexus/server/accounts/**' }) ?? '', /accounts' directory/, `Grep's file glob ${why}`);
        // What is not protected stays open: the working directory, a new file in it, a near name.
        assert.equal(refuse('Write', { file_path: '/w/sub/new.md', content: 'x' }), undefined, why);
        assert.equal(refuse('Read', { file_path: '/w//notes.md' }), undefined, why);
        assert.equal(refuse('Glob', { pattern: '**/*.md', path: '/w' }), undefined, why);
        assert.equal(refuse('Write', { file_path: '/h/.nexusx/new', content: 'x' }), undefined, why);
    }
    // Another case of the accounts' directory: the same folder on a case-insensitive volume, refused by its identity.
    const caseless = diskFs(kDisk, kDiskLinks, true);
    assert.match(toolRefusal('Grep', { pattern: 'x', path: '/h/.NEXUS/server/accounts' }, diskWall, '/w', '/h', caseless) ?? '',
        /accounts' directory/);
    assert.match(toolRefusal('Write', { file_path: '/h/.CLAUDE/settings.json', content: '{}' }, diskWall, '/w', '/h', caseless) ?? '',
        /outside the sandbox/);
    // On a case-sensitive one it is another folder, refused all the same (deny-leaning: the hook cannot tell the
    // volumes apart); a look-alike folder is still another name and may be read.
    const other = diskFs({ ...kDisk, '/h/.NEXUS': 'd:20', '/h/.NEXUS/server': 'd:21', '/h/.NEXUS/server/accounts': 'd:22' }, kDiskLinks, false);
    assert.match(toolRefusal('Grep', { pattern: 'x', path: '/h/.NEXUS/server/accounts' }, diskWall, '/w', '/h', other) ?? '',
        /accounts' directory/);
    assert.equal(toolRefusal('Grep', { pattern: 'x', path: '/h/.nеxus/server/accounts' }, diskWall, '/w', '/h', other), undefined);
});

/** `name` with its letters spelled other ways: the long s, the Kelvin sign, a fullwidth first letter, mixed case. */
function compatibilitySpellings(name: string): string[] {
    const out: string[] = [];
    if (name.includes('s')) {
        out.push(name.replace(/s/g, 'ſ'));
    }
    if (name.includes('k')) {
        out.push(name.replace(/k/g, 'K'));
    }
    const first = name.search(/[a-z]/);
    if (first >= 0) {
        out.push(name.slice(0, first) + String.fromCharCode(name.charCodeAt(first) - 0x61 + 0xff41) + name.slice(first + 1));
    }
    out.push(name.replace(/[a-z]/gi, (c: string, i: number) => (i % 2 === 0 ? c.toUpperCase() : c.toLowerCase())));
    return out;
}

/** `name` with its first look-alike letter swapped for the Cyrillic one: another name on every volume, or undefined. */
function lookAlike(name: string): string | undefined {
    const cyrillic: Record<string, string> = { a: 'а', c: 'с', e: 'е', o: 'о', p: 'р', H: 'Н', i: 'і' };
    const at = name.search(/[aceopHi]/);
    return at < 0 ? undefined : name.slice(0, at) + (cyrillic[name[at] ?? ''] ?? '') + name.slice(at + 1);
}

void test('the hook refuses a protected name by every spelling the volume takes for it, and allows look-alikes', () => {
    for (const caseless of [true, false]) {
        const d = diskFs(kDisk, kDiskLinks, caseless);
        const why = caseless ? 'case-insensitive' : 'case-sensitive';
        for (const p of diskWall.config) {
            const dir = dirname(p);
            const name = p.slice(dir.length + 1);
            for (const s of compatibilitySpellings(name)) {
                assert.match(toolRefusal('Write', { file_path: `${dir}/${s}`, content: 'x' }, diskWall, '/w', '/h', d) ?? '',
                    /outside the sandbox|accounts' directory/, `${dir}/${s} (${why})`);
            }
            const other = lookAlike(name);
            if (other !== undefined && !diskWall.config.some((c) => `${dir}/${other}`.startsWith(`${c}/`))) {
                assert.equal(toolRefusal('Write', { file_path: `${dir}/${other}`, content: 'x' }, diskWall, '/w', '/h', d),
                    undefined, `${dir}/${other} (${why})`);
            }
        }
        for (const p of ['/w/repo/.git/config', '/w/repo/.git', '/w/repo/HEAD', '/w/repo/commondir']) {
            const dir = dirname(p);
            const name = p.slice(dir.length + 1);
            for (const s of compatibilitySpellings(name)) {
                assert.match(toolRefusal('Write', { file_path: `${dir}/${s}`, content: 'x' }, diskWall, '/w', '/h', d) ?? '',
                    /git directory/, `${dir}/${s} (${why})`);
            }
            const other = lookAlike(name);
            if (other !== undefined && !dir.endsWith("/.git")) {
                assert.equal(toolRefusal('Write', { file_path: `${dir}/${other}`, content: 'x' }, diskWall, '/w', '/h', d),
                    undefined, `${dir}/${other} (${why})`);
            }
        }
    }
    // The protected names, plainly, to be sure the spellings above were of names the hook refuses.
    assert.ok(diskWall.config.includes('/h/.bashrc') && diskWall.config.includes('/h/.claude'));
    assert.equal(folded('ſettings.json'), 'settings.json');
    assert.equal(folded('.Kube'), '.kube');
});

void test('a file not made yet resolves through its nearest folder that is', () => {
    const d = diskFs(kDisk, kDiskLinks, false);
    assert.equal(realOf('/w/lnk2/main/new-file', d), '/h/.nexus/server/accounts/main/new-file');
    assert.equal(realOf('/w/lnk2/new/deeper/file', d), '/h/.nexus/server/accounts/new/deeper/file');
    assert.equal(realOf('/w/notes.md', d), '/w/notes.md');
    assert.equal(realOf('/nowhere/at/all', { realpath: () => { throw new Error('ENOENT'); } }), '/nowhere/at/all');
});

void test('a limited friend\'s sandbox reads none of HOME and the data roots but its directory, its state and its programs', () => {
    const exists = (p: string): boolean => p !== '/srv';
    const env = { PATH: '/var/h/.local/bin:/usr/bin:/home/linuxbrew/.linuxbrew/bin:/var/h/.ssh/bin', TMPDIR: '/var/t/' };
    // The working directory as prepare resolved it, once: the one path checked, allowed and given to Claude.
    const l = limitOf('/var/h', '/private/var/h/w', '/var/h/.nexus/server/harness/claude/main', ['/d/driver'], 'linux', env,
        '/opt/node/bin/node', realpath, exists);
    // HOME and every data root that exists, in both forms.
    assert.deepEqual(l.denyRead, ['/home', '/root', '/mnt', '/media', '/run/media', '/var/h', '/private/var/h']);
    for (const p of ['/private/var/h/w', '/d/driver', '/opt/node', '/var/h/.local/bin', '/home/linuxbrew/.linuxbrew/bin',
        '/home/linuxbrew/.linuxbrew', '/var/h/.gitconfig', '/var/h/.config/git/config', '/var/h/.config/git/ignore',
        '/var/h/.config/git/attributes', '/var/h/.bashrc',
        '/var/h/.nexus/server/harness/claude/main/shell-snapshots', '/var/h/.nexus/server/harness/claude/main/session-env',
        '/var/h/.nexus/server/harness/claude/main/projects/-private-var-h-w']) {
        assert.ok(l.allowRead.includes(p), p);
    }
    // Never ~/.ssh, ~/.nexus or a folder above either; never a token file a program keeps under HOME; not HOME's parent
    // bin; not ~/.config/git whole (git's credential store is in it); the directory in no second, unresolved form.
    for (const p of ['/var/h/.ssh/bin', '/var/h', '/var/h/.nexus', '/var/h/.npmrc', '/var/h/.local', '/var/h/.nexus/server/accounts',
        '/var/h/.config/git', '/private/var/h/.config/git', '/var/h/w']) {
        assert.ok(!l.allowRead.includes(p), p);
    }
    assert.ok(!l.allowRead.some((p) => p.startsWith('/var/h/.ssh') || p.startsWith('/private/var/h/.ssh')));
    // Writes: its directory and the temporary ones only.
    assert.deepEqual(l.allowWrite, ['/private/var/h/w', '/var/t', '/private/var/t', '/tmp', '/private/tmp']);
    assert.deepEqual(l.roots, ['/private/var/h/w']);
    // The project's folder, named by the resolved directory, under the state directory in both forms.
    assert.deepEqual(l.reads, ['/var/h/.nexus/server/harness/claude/main/projects/-private-var-h-w',
        '/private/var/h/.nexus/server/harness/claude/main/projects/-private-var-h-w']);
    assert.deepEqual(l.ssh, ['/var/h/.ssh', '/private/var/h/.ssh']);
    assert.equal(projectStateOf('/c', '/Users/me/my proj.x'), '/c/projects/-Users-me-my-proj-x');
    // A temporary directory that holds HOME is not written (a test's HOME in /tmp).
    const inTmp = limitOf('/tmp/run/h', '/tmp/run/h/w', '/tmp/run/h/.nexus/c', [], 'darwin', { TMPDIR: '/tmp/run/t' }, '/n/node', (p) => p, () => true);
    assert.deepEqual(inTmp.allowWrite, ['/tmp/run/h/w', '/tmp/run/t', '/private/tmp']);
    assert.deepEqual(inTmp.denyRead, [...kDataRoots['darwin'] ?? [], '/tmp/run/h']);
});

void test('a PATH folder under a denied root is allowed as that folder only; its parent only for a named Linux prefix', () => {
    const id = (p: string): string => p;
    const yes = (): boolean => true;
    // macOS: a PATH folder on another volume, in another user's home, inside HOME or inside another project: the folder
    // only, never its parent (which would open the volume, the other home, HOME or the project).
    const mac = limitOf('/Users/me', '/Users/me/w', '/Users/me/.nexus/c', [], 'darwin',
        { PATH: '/Volumes/disk/bin:/Users/other/bin:/Users/me/.local/bin:/Users/me/other-project/node_modules/.bin:/usr/bin' },
        '/Volumes/disk/bin/node', id, yes);
    for (const p of ['/Volumes/disk/bin', '/Users/other/bin', '/Users/me/.local/bin', '/Users/me/other-project/node_modules/.bin']) {
        assert.ok(mac.allowRead.includes(p), p);
    }
    for (const p of ['/Volumes/disk', '/Users/other', '/Users/me/.local', '/Users/me/other-project/node_modules',
        '/Users/me/other-project', '/Volumes', '/Users']) {
        assert.ok(!mac.allowRead.includes(p), p);
    }
    // Linux: the same, but for Linuxbrew's prefix, the one named install prefix; a sbin or bin elsewhere stays alone.
    const lin = limitOf('/home/me', '/home/me/w', '/home/me/.nexus/c', [], 'linux',
        { PATH: '/home/linuxbrew/.linuxbrew/bin:/home/linuxbrew/.linuxbrew/sbin:/mnt/data/bin:/home/other/sbin:/srv/x/bin' },
        '/usr/bin/node', id, yes);
    for (const p of ['/home/linuxbrew/.linuxbrew', '/home/linuxbrew/.linuxbrew/bin', '/home/linuxbrew/.linuxbrew/sbin',
        '/mnt/data/bin', '/home/other/sbin', '/srv/x/bin']) {
        assert.ok(lin.allowRead.includes(p), p);
    }
    for (const p of ['/mnt/data', '/home/other', '/srv/x', '/home/linuxbrew']) {
        assert.ok(!lin.allowRead.includes(p), p);
    }
    // Node's prefix: allowed deep under a root (a tool cache), never a root or a folder directly in one.
    const deep = limitOf('/Users/me', '/Users/me/w', '/Users/me/.nexus/c', [], 'darwin', {},
        '/Users/me/.nvm/versions/node/v22/bin/node', id, yes);
    assert.ok(deep.allowRead.includes('/Users/me/.nvm/versions/node/v22'));
    assert.ok(!mac.allowRead.includes('/Volumes/disk'));
    const homeBin = limitOf('/Users/me', '/Users/me/w', '/Users/me/.nexus/c', [], 'darwin', {}, '/Users/me/bin/node', id, yes);
    assert.ok(!homeBin.allowRead.includes('/Users/me'));
    // More denied roots from the server (--keep-out): only absolute ones, never the root; a PATH folder on one is that
    // folder. The environment names none: a variable of the old name changes nothing.
    const more = limitOf('/home/me', '/home/me/w', '/home/me/.nexus/c', [], 'linux',
        { PATH: '/data/x/bin' }, '/usr/bin/node', id, yes, ['/data/x', 'relative', '/']);
    assert.ok(more.denyRead.includes('/data/x') && !more.denyRead.includes('relative') && !more.denyRead.includes('/'));
    assert.ok(more.allowRead.includes('/data/x/bin') && !more.allowRead.includes('/data/x'));
    const fromEnv = limitOf('/home/me', '/home/me/w', '/home/me/.nexus/c', [], 'linux',
        { NEXUS_LIMIT_DENY_ROOTS: '/data/x', PATH: '/data/x/bin' }, '/usr/bin/node', id, yes);
    assert.ok(!fromEnv.denyRead.includes('/data/x'));
});

void test('a limited friend reads single named start-up files, never a folder and never a credential store', () => {
    const l = limitOf('/home/me', '/home/me/w', '/home/me/.nexus/c', ['/home/me/tools/credentials', '/opt/x/.git-credentials'],
        'linux', { PATH: '/home/me/.netrc' }, '/usr/bin/node', (p) => p, () => true);
    for (const f of kLimitedReadsUnderHome as readonly string[]) {
        assert.ok(!f.endsWith('/') && f !== '.config/git' && f !== '.config', f);
    }
    for (const p of l.allowRead) {
        const name = p.slice(p.lastIndexOf('/') + 1);
        assert.ok(!(kCredentialNames as readonly string[]).includes(name), p);
    }
    assert.ok(!l.allowRead.includes('/home/me/.config/git') && !l.allowRead.includes('/home/me/.config'));
    assert.ok(l.allowRead.includes('/home/me/.config/git/config'));
});

void test('a limited friend\'s sandbox, deny rules and settings', () => {
    const l = limitOf('/h', '/h/w', '/h/.nexus/server/harness/claude/main', [], 'linux', {}, '/n/node', (p) => p, () => true);
    const s = wallSandbox(wall, ['/h', '/w'], 4321, l);
    assert.deepEqual(s.filesystem.allowWrite, l.allowWrite);
    assert.deepEqual(s.filesystem.allowRead, l.allowRead);
    assert.deepEqual(s.filesystem.denyRead, ['/h/.nexus/server/accounts', ...l.denyRead]);
    // Unlimited: no allowRead, HOME writable as before.
    assert.equal('allowRead' in wallSandbox(wall, ['/h', '/w'], 4321).filesystem, false);
    const rules = wallDenyRules(wall, l);
    for (const r of ['Read(//h/.ssh)', 'Read(//h/.ssh/**)', 'Edit(//h/.ssh/**)']) {
        assert.ok(rules.includes(r), r);
    }
    assert.ok(!wallDenyRules(wall).includes('Read(//h/.ssh/**)'));
});

void test('the hook refuses a limited friend\'s file tools every path outside its directory, by where it resolves', () => {
    const d = fakeFs({ '/w/a.c': '1:1', '/w/sub/b.c': '1:2', '/h/notes': '1:3', '/h/.ssh/id': '1:4', '/c/projects/-w/m.md': '1:5' },
        { '/w/out': '/h' });
    const limit = { roots: ['/w'], reads: ['/c/projects/-w'] };
    const refuseL = (tool: string, input: Record<string, unknown>): string | undefined =>
        toolRefusal(tool, input, wall, '/w', '/h', d, limit);
    for (const [tool, input] of [['Read', { file_path: '/h/notes' }], ['Read', { file_path: '~/.ssh/id' }],
        ['Read', { file_path: 'out/notes' }], ['Read', { file_path: '/w/../h/notes' }], ['Read', { file_path: '/w//../h/notes' }],
        ['Grep', { pattern: 'x', path: '/h' }], ['Grep', { pattern: 'x' , glob: '/h/**' }], ['Glob', { pattern: '/h/**/*.c' }],
        ['Glob', { pattern: '**/*', path: '..' }], ['Write', { file_path: '/h/new', content: 'x' }],
        ['Write', { file_path: 'out/new', content: 'x' }], ['Edit', { file_path: '/c/projects/-w/m.md', old_string: 'a', new_string: 'b' }],
        ['NotebookEdit', { notebook_path: '/tmp/x.ipynb', new_source: 'x' }]] as const) {
        assert.match(refuseL(tool, input) ?? '', /limits this friend to its directory, \/w/, `${tool} ${JSON.stringify(input)}`);
    }
    for (const [tool, input] of [['Read', { file_path: 'a.c' }], ['Read', { file_path: '/w/sub/b.c' }], ['Grep', { pattern: 'x' }],
        ['Grep', { pattern: 'x', path: 'sub' }], ['Glob', { pattern: '**/*.c' }], ['Write', { file_path: 'new.c', content: 'x' }],
        ['Edit', { file_path: 'sub/b.c', old_string: 'a', new_string: 'b' }], ['Read', { file_path: '/c/projects/-w/m.md' }],
        ['Bash', { command: 'cat /h/notes' }]] as const) {
        assert.equal(refuseL(tool, input), undefined, `${tool} ${JSON.stringify(input)}`);
    }
    // Unlimited, the same reads outside go on.
    assert.equal(toolRefusal('Read', { file_path: '/h/notes' }, wall, '/w', '/h', d), undefined);
});
