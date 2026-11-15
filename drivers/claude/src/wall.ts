// The token wall, best effort: the AI a driver runs is kept from reading its own login. The account's token (account.ts) reaches Claude's
// process on a socket, never in an environment or an argument, and every tool Claude runs is kept out of the accounts'
// directory, which holds secrets only (Claude's own state is in harness/claude/<name>/). The layers (docs/secrets.md,
// "The token wall", which also lists what stays open; test/functional/wall.test.ts tries routes against them):
//
//   1. The token is in no environment. Claude reads it from descriptor 3, a socket the driver writes it to and closes
//      (CLAUDE_CODE_OAUTH_TOKEN_FILE_DESCRIPTOR names the number only); Claude reads to the newline, closes the
//      descriptor and unsets the name, so no tool child inherits either. The wall for the variable.
//   2. Every shell command runs in the OS sandbox (Seatbelt on macOS, bubblewrap on Linux) with no way out of it
//      (allowUnsandboxedCommands false, failIfUnavailable true): the accounts' directory can be neither read nor
//      written, and the login variables are unset. The wall for the file, against the shell and all it runs.
//   3. The sandbox's only way out to the network is the driver's proxy (egress.ts), which refuses this machine's own
//      addresses: a command does not reach its sshd, or another service of it, directly (another machine can still
//      reach it).
//   4. Read and Edit deny rules on the accounts' directory: Claude's own file tools (Read, Write, Edit, Glob, Grep,
//      NotebookEdit) refuse it, in bypassPermissions mode too. The wall for the file, against the file tools.
//   5. Nothing runs outside the sandbox but Claude itself: settings hooks are off, and no tool may write what runs
//      outside it later (configFiles: the settings and MCP files Claude loads, nexus's own files and programs, and
//      what the person's programs run on their own). The wall around 2.
//   6. A PreToolUse hook (toolRefusal) refuses a call whose command, pattern, URL or path names the directory, a path
//      that resolves into it (spellings, links and case resolved first), a file of it under another name (a hard link), a write to a
//      config file, or a URL that is not http or https. A backstop only: it reads text, and text can be spelled around.
//
// Permissions stay bypassed: the sandbox allows every shell command in it without a prompt, and nothing asks.

import { spawn } from 'node:child_process';
import { lstatSync, readdirSync, realpathSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
import { dirname, isAbsolute, resolve } from 'node:path';

import type {
    HookCallbackMatcher, HookEvent, Options, SandboxSettings, SpawnedProcess, SpawnOptions,
} from '@anthropic-ai/claude-agent-sdk';

import { accountsDir, type Account } from './account.js';
import { kSandboxBinVariable } from './tools.js';

/** The descriptor Claude reads the token from. */
export const kTokenDescriptor = 3;

/** The variables Claude's shell commands must never see, whatever put them in its environment. */
export const kLoginVariables = [
    'CLAUDE_CODE_OAUTH_TOKEN', 'CLAUDE_CODE_OAUTH_TOKEN_FILE_DESCRIPTOR', 'CLAUDE_CODE_OAUTH_REFRESH_TOKEN',
    'CLAUDE_CODE_SESSION_ACCESS_TOKEN', 'ANTHROPIC_API_KEY', 'ANTHROPIC_AUTH_TOKEN',
] as const;

/** A path as given and as the file system resolves it (macOS has /var -> /private/var): both, when they differ. */
export function bothForms(path: string, realpath: (p: string) => string = realpathSync): string[] {
    const given = path.replace(/\/+$/, '');
    let real = given;
    try {
        real = realpath(given);
    } catch {
        // Not there yet: the given path is the one to deny, and it will be the one made.
    }
    return real === given ? [given] : [given, real];
}

/** What the wall keeps every tool from: the logins, and the files that would run a command outside the sandbox. */
export interface Wall {
    secrets: string[];  // the accounts' directory, both forms: never read, never written
    config: string[];   // the settings and MCP files Claude loads at its start: never written by a tool
}

/**
 * Files Claude's sandbox keeps every command from writing by itself, wherever they are. They stay out of the sandbox's
 * denyWrite: the sandbox needs no second rule for them, and on Linux a denied path that does not exist yet is held
 * open inside the sandbox in a way a shell reading it fails on. The file tools' deny rules and the hook still name them.
 */
export const kClaudeSandboxProtects = new Set(['.gitconfig', '.profile', '.bash_profile', '.bash_login', '.bashrc',
    '.bash_logout', '.zshrc', '.zshenv', '.zprofile', '.zlogin', '.zlogout', '.npmrc', '.envrc']);

/**
 * Under `home`, the files the person's own programs run or read commands from on their own, outside the sandbox: the
 * shells' startup files, login and session agents, and the config of git, ssh, tmux,
 * editors and the tools whose config names a program to run (a credential helper, an alias, an exec plugin). The
 * person edits them. The first line is protected in the sandbox by Claude's sandbox itself
 * (kClaudeSandboxProtects), and only from the file tools here.
 */
export const kRunsLaterUnderHome = [
    '.gitconfig', '.profile', '.bash_profile', '.bash_login', '.bashrc', '.bash_logout', '.zshrc', '.zshenv',
    '.zprofile', '.zlogin', '.zlogout', '.npmrc', '.envrc',
    '.config/git', '.ssh', 'Library/LaunchAgents', '.config/fish', '.config/environment.d', '.pam_environment',
    '.config/systemd', '.config/autostart', '.local/share/systemd',
    '.tmux.conf', '.config/tmux', '.vimrc', '.vim', '.config/nvim', '.emacs', '.emacs.d',
    '.claude', '.claude.json', '.config/gh', '.docker', '.kube', '.aws', '.config/go', '.cargo/config.toml',
    '.cargo/config', '.config/direnv', '.direnvrc',
] as const;

/** The names nexus-server keeps in its directory, ~/.nexus/server (src/server/home.h), whether they exist yet or not. */
const kServerNames = ['config', 'drivers', 'nexus-config', 'store', 'server.sock', 'server.log', 'accounts', 'harness'];

/**
 * Everything under ~/.nexus but the session's own Claude state `configDir`: at each level from ~/.nexus down to the
 * state directory, every other entry, those on disk and those nexus-server makes (`list` reads a directory; one that
 * is not there lists nothing). The server's drivers file, config and store, and the other accounts' state, are what
 * nexus-server and the other sessions read or run.
 */
export function nexusOutsideState(home: string, configDir: string, list: (dir: string) => string[]): string[] {
    const top = `${home}/.nexus`;
    if (!configDir.startsWith(`${top}/`)) {
        return [top];
    }
    const steps = configDir.slice(top.length + 1).split('/');
    const out: string[] = [];
    let dir = top;
    for (const step of steps) {
        const names = new Set(list(dir));
        if (dir === `${top}/server`) {
            for (const n of kServerNames) {
                names.add(n);
            }
        }
        if (dir === top) {
            names.add('server');
        }
        for (const n of names) {
            if (n !== step) {
                out.push(`${dir}/${n}`);
            }
        }
        dir = `${dir}/${step}`;
    }
    return out;
}

/**
 * The files no tool may write, because what they name runs outside the sandbox later: the settings and MCP files of a
 * session in `cwd` with Claude's state in `configDir` (a helper or an MCP server there runs when Claude next starts;
 * settings hooks are off, kNoSettingsHooks); everything nexus-server and the other sessions read under ~/.nexus
 * (nexusOutsideState); the `programs` nexus runs outside the sandbox (node, the driver, the sandbox's own bwrap and
 * socat, and every directory on the PATH under `home`); and under `home` what the person's own programs run on their
 * own (kRunsLaterUnderHome). (Claude's sandbox also keeps commands off every .git/config and .git/hooks.) The person
 * edits them.
 */
export function configFiles(cwd: string, configDir: string, home: string, programs: readonly string[] = [],
    list: (dir: string) => string[] = listDir): string[] {
    return [
        `${cwd}/.claude/settings.json`, `${cwd}/.claude/settings.local.json`, `${cwd}/.mcp.json`,
        `${configDir}/settings.json`, `${configDir}/settings.local.json`, `${configDir}/.claude.json`,
        ...nexusOutsideState(home, configDir, list),
        ...programs,
        ...kRunsLaterUnderHome.map((f) => `${home}/${f}`),
    ];
}

function listDir(dir: string): string[] {
    try {
        return readdirSync(dir);
    } catch {
        return [];
    }
}

/**
 * What nexus runs outside the sandbox that lives where the AI could otherwise write: the node running the driver, the
 * driver's own directory (its code and the Claude executable under node_modules), the sandbox's bwrap and socat
 * (NEXUS_SANDBOX_BIN, tools.ts), and each directory on the PATH under `home` (a program there runs in the person's
 * shell, and in nexus-server's when it looks one up).
 */
export function programsOf(home: string, execPath: string, driverDir: string,
    env: Readonly<Record<string, string | undefined>>): string[] {
    const under = (p: string): boolean => p === home || p.startsWith(`${home}/`);
    const out = [dirname(execPath), driverDir];
    const sandboxBin = env[kSandboxBinVariable];
    if (sandboxBin !== undefined && sandboxBin !== '') {
        out.push(sandboxBin);
    }
    for (const d of (env['PATH'] ?? '').split(':')) {
        if (isAbsolute(d) && under(d) && d !== home) {
            out.push(d);
        }
    }
    return [...new Set(out.map((p) => p.replace(/\/+$/, '')))];
}

/** The wall for a session: the accounts' directory under `home`, and the config files of `cwd` and `configDir`. */
export function wallOf(home: string, cwd: string, configDir: string, realpath: (p: string) => string = realpathSync,
    programs: readonly string[] = []): Wall {
    const resolveDir = (f: string): string[] => {
        const i = f.lastIndexOf('/');
        return bothForms(f.slice(0, i), realpath).map((d) => `${d}${f.slice(i)}`);
    };
    return {
        secrets: bothForms(accountsDir(home), realpath),
        config: [...new Set(configFiles(cwd, configDir, home, programs).flatMap(resolveDir))],
    };
}

/**
 * The sandbox every shell command runs in: on, with no way to run a command outside it, the accounts' directory
 * neither readable nor writable, the config files not writable, and the login variables unset. Writing stays open
 * wherever it was (`writable`: the person's home, the working directory and the temporary directories), but for the
 * config files (`wall.config`: the settings Claude loads, nexus's own files and programs, and what the person's own
 * unsandboxed programs would run later). A command's only way out to the network is a proxy, and the proxy is the
 * driver's (`egressPort`, egress.ts): it refuses this machine's own addresses, so ssh and git reach the fleet while a
 * local daemon (sshd above all, which would log the AI in as the person, outside the sandbox) is not reached directly by
 * the spellings of this machine the test tries; by way of another machine it is (docs/secrets.md, what stays open).
 * The local sockets tried are not reachable either. Claude's own sandbox also never lets a command write
 * .git/config or .git/hooks (what the person's git would run): git init, clone and config fail in it.
 */
export function wallSandbox(wall: Wall, writable: readonly string[], egressPort: number) {
    return {
        enabled: true,
        failIfUnavailable: true,
        autoAllowBashIfSandboxed: true,
        allowUnsandboxedCommands: false,
        network: { httpProxyPort: egressPort, socksProxyPort: egressPort },
        filesystem: {
            allowWrite: [...writable],
            denyRead: [...wall.secrets],
            denyWrite: [...wall.secrets,
                ...wall.config.filter((p) => !kClaudeSandboxProtects.has(p.slice(p.lastIndexOf('/') + 1)))],
        },
        credentials: {
            files: wall.secrets.map((path) => ({ path, mode: 'deny' as const })),
            envVars: kLoginVariables.map((name) => ({ name, mode: 'deny' as const })),
        },
    } satisfies SandboxSettings;
}

/**
 * What the person's git runs, by name, anywhere: a repository's config and hooks, and `commondir`, which makes a
 * directory a git directory under another name. Deny rules: the file tools may not create or change them, and Claude
 * also denies them to every shell command on macOS (it merges Edit deny rules into the sandbox's denyWrite), which
 * costs the AI's shell only `git worktree add` (it writes commondir). Best effort (docs/secrets.md, "The token wall").
 * A `.git` file or directory and a `HEAD` are refused to the file tools by the hook instead (kGitNames, toolRefusal):
 * as deny rules they reach the macOS sandbox too, and there they stop every commit (measured, 2026-11-15: git writes
 * .git/index.lock and .git/logs/HEAD).
 */
export const kGitNameRules = [
    'Edit(//**/.git/config)', 'Edit(//**/.git/hooks)', 'Edit(//**/.git/hooks/**)', 'Edit(//**/commondir)',
] as const;

/** The names the hook keeps the file tools from writing: a `.git` anywhere in the path, or a git directory's files. */
const kGitNames = new Set(['HEAD', 'commondir']);

/** Whether `abs` is what makes a git directory, by name: in or at a `.git`, or a git directory's HEAD or commondir. */
export function isGitName(abs: string): boolean {
    const parts = abs.split('/');
    return parts.includes('.git') || kGitNames.has(parts[parts.length - 1] ?? '');
}

/**
 * The permission rules that keep Claude's file tools out of the accounts' directory, and from writing the config
 * files (`//` starts an absolute path; Edit rules cover Write and NotebookEdit too). Deny rules hold in
 * bypassPermissions mode.
 */
export function wallDenyRules(wall: Wall): string[] {
    const rules: string[] = [...kGitNameRules];
    for (const p of wall.secrets) {
        for (const tool of ['Read', 'Edit']) {
            rules.push(`${tool}(/${p})`, `${tool}(/${p}/**)`);
        }
    }
    for (const f of wall.config) {
        rules.push(`Edit(/${f})`, `Edit(/${f}/**)`);
    }
    return rules;
}

/** Settings hooks run outside the sandbox: off, flag settings (the SDK's own hooks, the wall's among them, still run). */
export const kNoSettingsHooks = { disableAllHooks: true } as const;

/** What the hook needs to look at the file system: a seam for the unit tests. */
export interface WallFs {
    realpath(p: string): string;
    /** The device and inode of `p`, following links, or undefined when there is none. */
    identity(p: string): string | undefined;
    /** The files in the accounts' directory, recursively, as paths. */
    files(root: string): string[];
}

export const realFs: WallFs = {
    realpath: (p) => realpathSync(p),
    identity(p) {
        try {
            const st = statSync(p);
            return `${st.dev}:${st.ino}`;
        } catch {
            return undefined;
        }
    },
    files(root) {
        const out: string[] = [];
        const walk = (dir: string, depth: number): void => {
            if (depth > 4) {
                return;
            }
            let names: string[];
            try {
                names = readdirSync(dir);
            } catch {
                return;
            }
            for (const name of names) {
                const p = `${dir}/${name}`;
                try {
                    const st = lstatSync(p);
                    if (st.isDirectory()) {
                        walk(p, depth + 1);
                    } else if (st.isFile()) {
                        out.push(p);
                    }
                } catch {
                    // Gone since the listing: nothing to guard.
                }
            }
        };
        walk(root, 0);
        return out;
    },
};

/** The path arguments a tool call names, by the tool's input fields. */
function pathsOf(input: Record<string, unknown>): string[] {
    const out: string[] = [];
    for (const key of ['file_path', 'path', 'notebook_path']) {
        const v = input[key];
        if (typeof v === 'string' && v !== '') {
            out.push(v);
        }
    }
    return out;
}

/** The input fields that are commands, patterns or addresses: what a call acts on, never what it writes. */
function targetsOf(input: Record<string, unknown>): string[] {
    const out: string[] = [];
    for (const key of ['command', 'url', 'pattern', 'glob', 'file_path', 'path', 'notebook_path']) {
        const v = input[key];
        if (typeof v === 'string') {
            out.push(v);
        }
    }
    return out;
}

/** Whether `p` is `root` or inside it. */
function within(p: string, root: string): boolean {
    return p === root || p.startsWith(`${root}/`);
}

/** `p` joined to the directory `dir` as it is written, nothing normalised (the file system resolves `..` after a link). */
function joined(dir: string, p: string): string {
    return isAbsolute(p) ? p : `${dir}/${p}`;
}

/**
 * Where `p` is on disk: its realpath, or for a path that does not exist yet (a file to write), the realpath of its
 * nearest folder that does, with the rest of the path after it. A link to a protected folder with a new file name
 * resolves into the folder. `p` as given when no part of it resolves.
 */
export function realOf(p: string, fs: Pick<WallFs, 'realpath'>): string {
    let dir = p;
    let rest = '';
    for (;;) {
        try {
            const r = fs.realpath(dir);
            return rest === '' ? r : resolve(r, rest);
        } catch {
            // Not there: one folder up.
        }
        const parent = dirname(dir);
        if (parent === dir) {
            return p;
        }
        const name = dir.slice(parent.length).replace(/^\/+/, '');
        rest = rest === '' ? name : `${name}/${rest}`;
        dir = parent;
    }
}

/**
 * Whether the real path `real` is one of `roots` or inside one, by folder identity (device and inode): on a
 * case-insensitive volume one folder has many spellings, and a root that does not exist yet is matched by its nearest
 * folder that does and the rest of its path, the rest compared without case (deny-leaning on a case-sensitive one).
 */
function insideByIdentity(real: string, roots: readonly string[], fs: Pick<WallFs, 'identity'>): boolean {
    const index: { id: string; tail: string }[] = [];
    for (const root of roots) {
        let q = root;
        let tail = '';
        for (;;) {
            const id = fs.identity(q);
            if (id !== undefined) {
                index.push({ id, tail: tail.toLowerCase() });
                break;
            }
            const parent = dirname(q);
            if (parent === q) {
                break;
            }
            const name = q.slice(parent.length).replace(/^\/+/, '');
            tail = tail === '' ? name : `${name}/${tail}`;
            q = parent;
        }
    }
    let a = real;
    let rest = '';
    for (;;) {
        const id = fs.identity(a);
        if (id !== undefined) {
            const r = rest.toLowerCase();
            if (index.some((e) => e.id === id && (e.tail === '' || r === e.tail || r.startsWith(`${e.tail}/`)))) {
                return true;
            }
        }
        const parent = dirname(a);
        if (parent === a) {
            return false;
        }
        const name = a.slice(parent.length).replace(/^\/+/, '');
        rest = rest === '' ? name : `${name}/${rest}`;
        a = parent;
    }
}

/** The literal folder a glob pattern starts in (`base` for a relative pattern), up to its first segment with a wildcard. */
function globBase(pattern: string, base: string): string {
    const out: string[] = [];
    for (const seg of joined(base, pattern).split('/')) {
        if (/[*?[{]/.test(seg)) {
            break;
        }
        out.push(seg);
    }
    return out.join('/') || '/';
}

/** The paths a call opens or searches: its path fields, and the folder each Glob or Grep file pattern starts in. */
function openedOf(tool: string, input: Record<string, unknown>, cwd: string): string[] {
    const out = pathsOf(input);
    const base = typeof input['path'] === 'string' && input['path'] !== '' ? input['path'] : cwd;
    if (/^Glob$/.test(tool) && typeof input['pattern'] === 'string') {
        out.push(globBase(input['pattern'], base));
    }
    if (typeof input['glob'] === 'string') {
        out.push(globBase(input['glob'], base));
    }
    return out;
}

/** Every form of the path argument `raw` the wall judges: as written, normalised, and where each is on disk. */
function formsOf(raw: string, cwd: string, home: string, fs: WallFs): { forms: string[]; reals: string[] } {
    const given = raw === '~' ? home : raw.startsWith('~/') ? `${home}/${raw.slice(2)}` : raw;
    const written = joined(cwd, given);
    const normal = resolve(cwd, given);
    const reals = [...new Set([realOf(written, fs), realOf(normal, fs)])];
    return { forms: [...new Set([written, normal, ...reals])], reals };
}

/** The tools that write a file at their path. */
const kWriters = /^(Write|Edit|MultiEdit|NotebookEdit)$/;

/**
 * Why the hook refuses the tool call, or undefined to let it go on. `wall` from wallOf, `cwd` the call's working
 * directory, `home` HOME (a `~` in a path is it).
 */
export function toolRefusal(tool: string, input: Record<string, unknown>, wall: Wall, cwd: string, home: string,
    fs: WallFs): string | undefined {
    const refused = 'nexus keeps every tool out of the accounts\' directory, where the logins are; a secret is asked for '
        + 'with the in-app secret dialog, never read';
    // A command, pattern, URL or path that names the directory by an absolute spelling. (Text that only mentions it,
    // such as a document being written, is not looked at.)
    const spellings = [...wall.secrets, '~/.nexus/server/accounts', '$HOME/.nexus/server/accounts',
        '${HOME}/.nexus/server/accounts', `${home}/.nexus/server/accounts`];
    for (const t of targetsOf(input)) {
        if (spellings.some((p) => t.includes(p))) {
            return refused;
        }
    }
    // A URL a fetch tool would follow: only http and https, never a file.
    if (/fetch/i.test(tool) && typeof input['url'] === 'string' && !/^https?:\/\//i.test(input['url'])) {
        return `${tool} takes only http and https URLs here`;
    }
    // A path a file tool would open or search: by every spelling of it (doubled slashes, `..`, `~`), by where it
    // resolves (a link, a file not made yet under a linked folder), by folder identity (another case on a
    // case-insensitive volume), and by what file it is (a hard link has another name).
    let guarded: Set<string> | undefined;
    const inside = (forms: readonly string[], reals: readonly string[], roots: readonly string[]): boolean =>
        forms.some((f) => roots.some((r) => within(f, r))) || reals.some((r) => insideByIdentity(r, roots, fs));
    for (const raw of openedOf(tool, input, cwd)) {
        const { forms, reals } = formsOf(raw, cwd, home, fs);
        if (inside(forms, reals, wall.secrets)) {
            return refused;
        }
        if (kWriters.test(tool) && inside(forms, reals, wall.config)) {
            return `nexus keeps tools from writing the settings and MCP files Claude loads (${raw}): what they run would `
                + 'run outside the sandbox; the person edits them';
        }
        if (kWriters.test(tool) && forms.some(isGitName)) {
            return `nexus keeps the file tools from writing what makes a git directory (${raw}): the person's git runs `
                + 'what its config names; use git itself in the shell';
        }
        const id = fs.identity(reals[0] ?? raw);
        if (id === undefined) {
            continue;
        }
        if (guarded === undefined) {
            guarded = new Set<string>();
            for (const p of wall.secrets) {
                for (const f of fs.files(p)) {
                    const fid = fs.identity(f);
                    if (fid !== undefined) {
                        guarded.add(fid);
                    }
                }
            }
        }
        if (guarded.has(id)) {
            return refused;
        }
    }
    return undefined;
}

/** The PreToolUse hook: every tool, every call, refused with the reason when toolRefusal says so. */
export function wallHooks(wall: Wall, cwd: string, home: string, fs: WallFs = realFs):
Partial<Record<HookEvent, HookCallbackMatcher[]>> {
    return {
        PreToolUse: [{
            hooks: [(input) => {
                if (input.hook_event_name !== 'PreToolUse') {
                    return Promise.resolve({});
                }
                const toolInput = typeof input.tool_input === 'object' && input.tool_input !== null
                    ? input.tool_input as Record<string, unknown> : {};
                const why = toolRefusal(input.tool_name, toolInput, wall, input.cwd === '' ? cwd : input.cwd, home, fs);
                if (why === undefined) {
                    return Promise.resolve({});
                }
                return Promise.resolve({
                    hookSpecificOutput: { hookEventName: 'PreToolUse' as const, permissionDecision: 'deny' as const,
                        permissionDecisionReason: why },
                });
            }],
        }],
    };
}

/** The options of query() that build the wall (the rest are the driver's own: main.ts). */
export type WallOptions = Required<Pick<Options, 'spawnClaudeCodeProcess' | 'sandbox' | 'settings' | 'hooks'>>;

/** The driver's own directory (its package: dist/, node_modules/ and the Claude executable in it). */
export const kDriverDir = resolve(dirname(fileURLToPath(import.meta.url)), '../..');

/**
 * The wall for `account`, for a session working in `cwd`, with HOME `home`, its commands' network through the driver's
 * proxy on `egressPort` (egress.ts), and `env` the driver's (its PATH and NEXUS_SANDBOX_BIN name the programs it
 * protects). `layers` leaves a layer out, for the functional test that shows the walls under it hold alone
 * (test/functional/wall.test.ts); the driver always builds all of them.
 */
export function wallOptions(account: Account, home: string, cwd: string, stderr: (data: string) => void,
    egressPort: number, env: Readonly<Record<string, string | undefined>>,
    layers: { hook: boolean } = { hook: true }): WallOptions {
    const programs = programsOf(home, process.execPath, kDriverDir, env);
    const wall = wallOf(home, cwd, account.configDir, realpathSync, programs);
    const writable = [...new Set([home, cwd, tmpdir(), '/tmp', '/private/tmp', '/var/folders'])];
    return {
        spawnClaudeCodeProcess: spawnWithToken(account.token, stderr),
        sandbox: wallSandbox(wall, writable, egressPort),
        settings: { permissions: { deny: wallDenyRules(wall) }, ...kNoSettingsHooks },
        hooks: layers.hook ? wallHooks(wall, cwd, home) : {},
    };
}

/**
 * Starts Claude's process with the token on descriptor 3, a socket: written with its newline and closed, so Claude
 * reads it once and the descriptor is gone before any tool runs. The token is in no environment and no argument.
 * Claude's standard error goes to `stderr`.
 */
export function spawnWithToken(token: string, stderr: (data: string) => void): (options: SpawnOptions) => SpawnedProcess {
    return (options) => {
        const env: Record<string, string> = {};
        for (const [name, value] of Object.entries(options.env)) {
            if (value !== undefined && !(kLoginVariables as readonly string[]).includes(name)) {
                env[name] = value;
            }
        }
        env['CLAUDE_CODE_OAUTH_TOKEN_FILE_DESCRIPTOR'] = String(kTokenDescriptor);
        const child = spawn(options.command, options.args, {
            ...(options.cwd === undefined ? {} : { cwd: options.cwd }),
            env,
            signal: options.signal,
            stdio: ['pipe', 'pipe', 'pipe', 'pipe'],
            windowsHide: true,
        });
        const tokenPipe = child.stdio[kTokenDescriptor];
        if (tokenPipe !== null && tokenPipe !== undefined && 'end' in tokenPipe) {
            tokenPipe.on('error', () => { /* Claude gone before it read: its exit says why. */ });
            tokenPipe.end(`${token}\n`);
        }
        child.stderr.setEncoding('utf8');
        child.stderr.on('data', (data: string) => { stderr(data); });
        return child;
    };
}
