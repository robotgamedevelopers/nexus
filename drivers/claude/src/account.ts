// The account the driver runs Claude on: one directory per login under nexus-server's home,
// ~/.nexus/server/accounts/<name>/, named by the drivers file's --account <name>. Claude runs with that directory as
// CLAUDE_CONFIG_DIR and the account's token, from the file oauth-token there (one line, from `claude setup-token`, mode
// 0600), as CLAUDE_CODE_OAUTH_TOKEN, in Claude's environment only. Nothing else is a credential: not the Keychain, not
// the server's environment, not ~/.claude. The token is never logged, written to the server or put in an argument.

import { closeSync, constants, fstatSync, lstatSync, openSync, readFileSync } from 'node:fs';

/** An account the driver can start Claude on. */
export interface Account {
    name: string;
    dir: string;        // CLAUDE_CONFIG_DIR
    token: string;      // CLAUDE_CODE_OAUTH_TOKEN; never logged
}

/** Why there is no account to start on: a `no-credential` error's cause and fix. */
export interface AccountRefusal {
    cause: string;
    fix: string;
}

/** The name of the token file in an account's directory. */
export const kTokenFile = 'oauth-token';

/** An account's name: 1 to 64 letters, digits, '-', '_' or '.', not starting with '.'. */
export function isAccountName(name: string): boolean {
    return /^[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}$/.test(name);
}

/** The accounts' directory under `home`. */
export function accountsDir(home: string): string {
    return `${home.replace(/\/+$/, '')}/.nexus/server/accounts`;
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
export function readAccount(home: string | undefined, name: string): Account | AccountRefusal {
    if (home === undefined || home === '') {
        return { cause: 'HOME is not set, so the driver cannot find its account under ~/.nexus/server/accounts',
            fix: 'start nexus-server with HOME set to your home directory' };
    }
    if (!isAccountName(name)) {
        return { cause: `--account ${name} is not an account name (1 to 64 letters, digits, '-', '_' or '.', not starting with '.')`,
            fix: 'fix the driver\'s --account in nexus-server\'s drivers file' };
    }
    const dir = `${accountsDir(home)}/${name}`;
    let dirStat;
    try {
        dirStat = lstatSync(dir);
    } catch {
        return { cause: `the account ${name} has no directory: ${dir} is missing`,
            fix: `make it (\`mkdir -p -m 700 ${dir}\`), then ${makeToken(dir)}` };
    }
    // The directory itself, never a link to one elsewhere, this user's, and no one else's to change.
    if (!dirStat.isDirectory() || (uid() !== undefined && dirStat.uid !== uid())) {
        return { cause: `the account ${name}'s ${dir} is not a directory of this user's (a link, a file or another user's)`,
            fix: `move it aside and make it again (\`mkdir -p -m 700 ${dir}\`), then ${makeToken(dir)}` };
    }
    if ((dirStat.mode & 0o077) !== 0) {
        return { cause: `the account ${name}'s ${dir} has mode 0${(dirStat.mode & 0o777).toString(8)}, so others can reach its login`,
            fix: `run \`chmod 700 ${dir}\`` };
    }
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
    return { name, dir, token: lines[0] ?? '' };
}

/** Whether `a` is an account rather than a refusal. */
export function isAccount(a: Account | AccountRefusal): a is Account {
    return 'token' in a;
}
