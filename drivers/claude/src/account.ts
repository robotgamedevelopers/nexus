// The server selects one account under ~/.nexus/server/accounts/<name>/. For an account with a configured
// email, the credential broker supplies the verified access token. Legacy accounts read their oauth-token file.
// Claude receives only the access token, on a descriptor (wall.ts). Its state lives separately under
// ~/.nexus/server/harness/claude/<name>/, outside the private credentials directory.

import {
    closeSync, constants, existsSync, fstatSync, futimesSync, lstatSync, mkdirSync, openSync, readdirSync, readFileSync,
    renameSync, rmSync, writeFileSync,
} from 'node:fs';
import { basename, dirname } from 'node:path';

/** A case-insensitive filesystem may have resolved a differently spelled account name. */
function spellingProblem(dir: string): string | undefined {
    try {
        if (readdirSync(dirname(dir)).includes(basename(dir))) {
            return undefined;
        }
    } catch {
        return `cannot read the parent of ${dir} to verify the account directory's spelling`;
    }
    return `${dir} does not match the spelling of the account directory; check the account name's capitalization`;
}

/** An account the driver can start Claude on. */
export interface Account {
    name: string;
    dir: string;        // the login's directory, accounts/<name>/: secrets only, which no tool reaches (wall.ts)
    configDir: string;  // CLAUDE_CONFIG_DIR, harness/claude/<name>/: Claude's own state (configDirOf)
    token: string;      // handed to Claude on a descriptor (wall.ts); never logged, never in an environment
}

/** Why there is no account to start on: a `no-credential` error's cause and fix. */
export interface AccountRefusal {
    cause: string;
    fix: string;
}

/** The name of the token file in an account's directory. */
export const kTokenFile = 'oauth-token';

/** An account's name: 1 to 64 letters, digits, '-', '_' or '.', not starting with '.' or '-' (it reaches an argv). */
export function isAccountName(name: string): boolean {
    return /^[A-Za-z0-9_][A-Za-z0-9._-]{0,63}$/.test(name);
}

/**
 * Where the logins are under HOME: the server's one path (src/server/home.h kAccountsUnderHome, which its sign-in check
 * reads too, whatever --dir says). The unit test holds the two equal.
 */
export const kAccountsUnderHome = '.nexus/server/accounts';

/** The accounts' directory under `home`. */
export function accountsDir(home: string): string {
    return `${home.replace(/\/+$/, '')}/${kAccountsUnderHome}`;
}

/** What the person runs to make the account's token, and where to save it. */
function makeToken(dir: string): string {
    return `run \`CLAUDE_CONFIG_DIR=${dir} claude setup-token\` with the subscription account, then save the token it `
        + `prints, alone on one line, to ${dir}/${kTokenFile} with mode 0600 (\`chmod 600 ${dir}/${kTokenFile}\`)`;
}

/** The error's code (ENOENT, ELOOP, EACCES...), or empty. */
function codeOf(e: unknown): string {
    const code = (e as { code?: unknown }).code;
    return typeof code === 'string' ? code : '';
}

/** This process's user id, or undefined where there is none (Windows). */
function uid(): number | undefined {
    return process.getuid?.();
}

/**
 * The account `name` under `home` (HOME): its directory must be a directory of this user's that no one else can reach
 * (mode 0700), and its token file a regular file, not a link, of this user's that no one else can read or write (mode
 * 0600), holding one line. Otherwise why not, and how to fix it; the token never appears in
 * either.
 */
export function readAccount(home: string | undefined, name: string, verified = false): Account | AccountRefusal {
    if (home === undefined || home === '') {
        return { cause: 'HOME is not set, so the driver cannot find its account under ~/.nexus/server/accounts',
            fix: 'start nexus-server with HOME set to your home directory' };
    }
    if (!isAccountName(name)) {
        return { cause: `--account ${name} is not an account name (1 to 64 letters, digits, '-', '_' or '.', not starting with '.')`,
            fix: 'fix the driver\'s --account in nexus-server\'s drivers file' };
    }
    const dir = `${accountsDir(home)}/${name}`;
    const loginFix = verified ? `sign in to ${name} in Nexus with its configured email` : makeToken(dir);
    let dirStat;
    try {
        dirStat = lstatSync(dir);
    } catch {
        return { cause: `the account ${name} has no directory: ${dir} is missing`,
            fix: `make it (\`mkdir -p -m 700 ${dir}\`), then ${loginFix}` };
    }
    const spelling = spellingProblem(dir);
    if (spelling !== undefined) {
        return { cause: spelling, fix: 'use the exact account name recorded on disk' };
    }
    // The directory itself, never a link to one elsewhere, this user's, and no one else's to change.
    if (!dirStat.isDirectory() || (uid() !== undefined && dirStat.uid !== uid())) {
        return { cause: `the account ${name}'s ${dir} is not a directory of this user's (a link, a file or another user's)`,
            fix: `move it aside and make it again (\`mkdir -p -m 700 ${dir}\`), then ${loginFix}` };
    }
    if ((dirStat.mode & 0o077) !== 0) {
        return { cause: `the account ${name}'s ${dir} has mode 0${(dirStat.mode & 0o777).toString(8)}, so others can reach its login`,
            fix: `run \`chmod 700 ${dir}\`` };
    }
    if (verified) return { name, dir, configDir: configDirOf(home, name), token: '' };
    const path = `${dir}/${kTokenFile}`;
    // Opened without following a link, then checked and read through the same descriptor: what is checked is what is read.
    let fd: number;
    try {
        fd = openSync(path, constants.O_RDONLY | constants.O_NOFOLLOW);
    } catch (e) {
        const code = codeOf(e);
        if (code === 'ENOENT') {
            return { cause: `the account ${name} is not logged in: ${path} is missing`, fix: makeToken(dir) };
        }
        if (code === 'ELOOP' || code === 'EMLINK') {
            return { cause: `the account ${name}'s token ${path} is a link; the token must be the file itself`,
                fix: `remove the link, then ${makeToken(dir)}` };
        }
        return { cause: `the account ${name}'s token ${path} cannot be read${code === '' ? '' : ` (${code})`}`,
            fix: `make it readable by the user nexus-server runs as, mode 0600, or ${makeToken(dir)}` };
    }
    let text: string;
    try {
        const st = fstatSync(fd);
        if (!st.isFile()) {
            return { cause: `the account ${name}'s token ${path} is not a regular file`, fix: makeToken(dir) };
        }
        if (uid() !== undefined && st.uid !== uid()) {
            return { cause: `the account ${name}'s token ${path} belongs to another user`, fix: `remove it, then ${makeToken(dir)}` };
        }
        if ((st.mode & 0o077) !== 0) {
            return { cause: `the account ${name}'s token ${path} has mode 0${(st.mode & 0o777).toString(8)}, so others can read it`,
                fix: `run \`chmod 600 ${path}\`, and make a new token if anyone else could have read it: ${makeToken(dir)}` };
        }
        text = readFileSync(fd, 'utf8');
    } catch (e) {
        const code = codeOf(e);
        return { cause: `the account ${name}'s token ${path} cannot be read${code === '' ? '' : ` (${code})`}`,
            fix: `make it readable by the user nexus-server runs as, mode 0600, or ${makeToken(dir)}` };
    } finally {
        closeSync(fd);
    }
    const lines = text.split(/\r?\n/).map((l) => l.trim()).filter((l) => l !== '');
    if (lines.length !== 1 || /\s/.test(lines[0] ?? '')) {
        return { cause: `the account ${name}'s token ${path} does not hold one token on one line`, fix: makeToken(dir) };
    }
    return { name, dir, configDir: configDirOf(home, name), token: lines[0] ?? '' };
}

/** Where Claude's own state is kept for an account, under HOME: never inside the accounts' directory. */
export const kConfigUnderHome = '.nexus/server/harness/claude';

/**
 * The account `name`'s CLAUDE_CONFIG_DIR under `home`: Claude's settings, sessions, memory and shell snapshots. It is
 * apart from the login's directory so that the wall can keep every tool out of the logins whole, while the AI still
 * reads and writes its own state.
 */
export function configDirOf(home: string, name: string): string {
    return `${home.replace(/\/+$/, '')}/${kConfigUnderHome}/${name}`;
}

/**
 * What a new Claude state directory takes from the login's directory, where an older driver kept Claude's state: the
 * person's own settings and instructions, by name, and nothing else. Never a directory (projects/ holds the old
 * transcripts and memory, shell-snapshots/ the old shells' environments: from before the wall, any of them may hold
 * the token, and the new directory is one the AI reads), never .claude.json (Claude's record of each project, the
 * prompts typed in it among them), and never a file the secret dialog stored (docs/secrets.md). What is not taken stays
 * where it was, under the wall. Of these, one that holds the token, has another name (a hard link) or is larger than
 * kMaxStateFileBytes is refused, and the directory is not made (stateFile). The cost: a session from before the wall
 * does not resume.
 */
export const kClaudeStateFiles = ['settings.json', 'settings.local.json', 'CLAUDE.md'] as const;

/** The most of one file openConfigDir copies: settings and instructions are small, a transcript is not. */
export const kMaxStateFileBytes = 1024 * 1024;

/**
 * One of kClaudeStateFiles from the login's directory, `path`, checked on the descriptor it is read from (so what is
 * checked is what is copied): the text to copy, undefined when there is none or it is a link (never followed, never
 * taken), or why it is refused: never a file with another name (a hard link: the same file may be one the AI must not read), never one larger than
 * kMaxStateFileBytes, never one holding the account's `token` (scanned, never logged).
 */
function stateFile(path: string, token: string): { text: Buffer; atime: Date; mtime: Date } | undefined | string {
    let fd: number;
    try {
        fd = openSync(path, constants.O_RDONLY | constants.O_NOFOLLOW);
    } catch (e) {
        // Not there, or a link (never followed, and not taken: what it names may be anything).
        if (codeOf(e) === 'ENOENT' || codeOf(e) === 'ELOOP') {
            return undefined;
        }
        return `${path} cannot be read (${codeOf(e)}); move it aside, and the driver makes the Claude state directory again`;
    }
    try {
        const st = fstatSync(fd);
        if (!st.isFile()) {
            return `${path} is not a regular file; move it aside, and the driver makes the Claude state directory again`;
        }
        if (st.nlink > 1) {
            return `${path} has ${st.nlink} names (a hard link); a new Claude state directory takes no file with another `
                + 'name: copy it to a file of its own, and the driver makes the directory again';
        }
        if (st.size > kMaxStateFileBytes) {
            return `${path} is larger than ${kMaxStateFileBytes} bytes, more than settings or instructions hold; move it `
                + 'aside, and the driver makes the Claude state directory again';
        }
        const text = readFileSync(fd);
        if (text.length > kMaxStateFileBytes) {
            return `${path} grew past ${kMaxStateFileBytes} bytes while it was read; move it aside, and the driver makes `
                + 'the Claude state directory again';
        }
        if (token !== '' && text.includes(token)) {
            return `${path} holds the account's login token, and the new Claude state directory is one the AI reads; take `
                + 'the token out of it (and make a new one: the AI may have read it before), and the driver makes the directory again';
        }
        return { text, atime: st.atime, mtime: st.mtime };
    } finally {
        closeSync(fd);
    }
}

/**
 * Makes the account's CLAUDE_CONFIG_DIR (mode 0700) if it is missing. A new one takes a copy of the person's settings and
 * instructions an older driver left in the login's directory (kClaudeStateFiles, each checked by stateFile, which
 * refuses one it will not copy, with why); the copy is made once, and the login's directory is left as it was.
 * Undefined, or why not.
 */
export function openConfigDir(account: Pick<Account, 'dir' | 'configDir' | 'token'>, migrateSettings = true): string | undefined {
    if (existsSync(account.configDir)) {
        const st = lstatSync(account.configDir);
        if (!st.isDirectory() || (uid() !== undefined && st.uid !== uid())) {
            return `${account.configDir} is not a directory of this user's; move it aside, and the driver makes it again`;
        }
        return spellingProblem(account.configDir);
    }
    const partial = `${account.configDir}.partial-${process.pid}`;
    try {
        mkdirSync(dirname(account.configDir), { recursive: true, mode: 0o700 });
        rmSync(partial, { recursive: true, force: true });
        mkdirSync(partial, { mode: 0o700 });
        for (const name of migrateSettings ? kClaudeStateFiles : []) {
            const got = stateFile(`${account.dir}/${name}`, account.token);
            if (typeof got === 'string') {
                rmSync(partial, { recursive: true, force: true });
                return got;
            }
            if (got !== undefined) {
                const out = `${partial}/${name}`;
                writeFileSync(out, got.text, { mode: 0o600, flag: 'wx' });
                const fd = openSync(out, constants.O_RDONLY);
                try {
                    futimesSync(fd, got.atime, got.mtime);
                } finally {
                    closeSync(fd);
                }
            }
        }
        // Whole or not at all: a driver that dies mid-copy leaves only a .partial, made again by the next.
        renameSync(partial, account.configDir);
    } catch (e) {
        rmSync(partial, { recursive: true, force: true });
        if (existsSync(account.configDir)) {
            return spellingProblem(account.configDir);   // Another driver may have made it first.
        }
        const code = codeOf(e);
        return `cannot make ${account.configDir}${code === '' ? '' : ` (${code})`}; make it with \`mkdir -p -m 700 ${account.configDir}\``;
    }
    return spellingProblem(account.configDir);
}

/** Whether `a` is an account rather than a refusal. */
export function isAccount(a: Account | AccountRefusal): a is Account {
    return 'token' in a;
}
