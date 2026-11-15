// The token wall's pieces (src/wall.ts), and the account's Claude state kept apart from its login (src/account.ts
// openConfigDir): what the sandbox, the deny rules and the hook are built from, and what the hook refuses, over a fake
// file system. No process, no socket, no time; files only inside the test's working directory. The wall end to end,
// against the real Claude, is the functional test (test/functional/wall.test.ts).

import assert from 'node:assert/strict';
import { existsSync, lstatSync, mkdirSync, mkdtempSync, readFileSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { after, test } from 'node:test';

import { configDirOf, openConfigDir } from '../../src/account.js';
import {
    bothForms, configFiles, kClaudeSandboxProtects, kGitNameRules, kLoginVariables, kNoSettingsHooks, nexusOutsideState, programsOf, toolRefusal, wallDenyRules, wallHooks, wallOf, wallSandbox,
    type Wall, type WallFs,
} from '../../src/wall.js';

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

void test('a new Claude state directory takes the state an older driver left in the login, never a secret', () => {
    const home = join(scratch, 'home');
    const login = join(home, '.nexus/server/accounts/main');
    mkdirSync(join(login, 'projects', 'p'), { recursive: true, mode: 0o700 });
    writeFileSync(join(login, 'oauth-token'), 'secret-token\n', { mode: 0o600 });
    writeFileSync(join(login, 'api-key'), 'secret-key\n', { mode: 0o600 });
    writeFileSync(join(login, '.claude.json'), '{}');
    writeFileSync(join(login, 'projects', 'p', 'session.jsonl'), 'turns\n');
    symlinkSync('/nonexistent', join(login, 'projects', 'link'));
    const configDir = configDirOf(home, 'main');
    assert.equal(openConfigDir({ dir: login, configDir }), undefined);
    assert.equal(readFileSync(join(configDir, 'projects', 'p', 'session.jsonl'), 'utf8'), 'turns\n');
    assert.ok(existsSync(join(configDir, '.claude.json')));
    assert.ok(!existsSync(join(configDir, 'oauth-token')), 'the token was copied');
    assert.ok(!existsSync(join(configDir, 'api-key')), 'a secret was copied');
    assert.ok(lstatSync(join(configDir, 'projects', 'link')).isSymbolicLink(), 'a link was followed');
    assert.equal(lstatSync(configDir).mode & 0o777, 0o700);
    // Once: a later start leaves it as it is.
    writeFileSync(join(login, 'projects', 'p', 'later.jsonl'), 'x');
    assert.equal(openConfigDir({ dir: login, configDir }), undefined);
    assert.ok(!existsSync(join(configDir, 'projects', 'p', 'later.jsonl')));
    // A file where the directory should be is refused, with the fix.
    const other = configDirOf(home, 'other');
    writeFileSync(other, 'not a directory');
    assert.match(openConfigDir({ dir: login, configDir: other }) ?? '', /not a directory of this user's/);
    // A login with nothing in it makes an empty one.
    const fresh = join(home, '.nexus/server/accounts/fresh');
    mkdirSync(fresh, { mode: 0o700 });
    assert.equal(openConfigDir({ dir: fresh, configDir: configDirOf(home, 'fresh') }), undefined);
    assert.ok(lstatSync(configDirOf(home, 'fresh')).isDirectory());
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
