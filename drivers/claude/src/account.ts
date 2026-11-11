// The account the driver runs Claude on: one directory per login under nexus-server's home,
// ~/.nexus/server/accounts/<name>/, named by the drivers file's --account <name>. Claude runs with that directory as
// CLAUDE_CONFIG_DIR and the account's token, from the file oauth-token there (one line, from `claude setup-token`, mode
// 0600), as CLAUDE_CODE_OAUTH_TOKEN, in Claude's environment only. Nothing else is a credential: not the Keychain, not
// the server's environment, not ~/.claude. The token is never logged, written to the server or put in an argument.

import { readFileSync, statSync } from 'node:fs';

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

function isDirectory(path: string): boolean {
    try {
        return statSync(path).isDirectory();
    } catch {
        return false;
    }
}

/** The file's mode bits and whether it is a regular file, or undefined if it is missing. */
function fileMode(path: string): { file: boolean; mode: number } | undefined {
    try {
        const s = statSync(path);
        return { file: s.isFile(), mode: s.mode & 0o777 };
    } catch {
        return undefined;
    }
}

/**
 * The account `name` under `home` (HOME): its directory must exist, and its token file must be a regular file no one
 * but its owner can read or write, holding one line. Otherwise why not, and how to fix it; the token never appears in
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
    if (!isDirectory(dir)) {
        return { cause: `the account ${name} has no directory: ${dir} is missing`,
            fix: `make it (\`mkdir -p -m 700 ${dir}\`), then ${makeToken(dir)}` };
    }
    const path = `${dir}/${kTokenFile}`;
    const st = fileMode(path);
    if (st === undefined) {
        return { cause: `the account ${name} is not logged in: ${path} is missing`, fix: makeToken(dir) };
    }
    if (!st.file) {
        return { cause: `the account ${name}'s token ${path} is not a regular file`, fix: makeToken(dir) };
    }
    if ((st.mode & 0o077) !== 0) {
        return { cause: `the account ${name}'s token ${path} has mode 0${st.mode.toString(8)}, so others can read it`,
            fix: `run \`chmod 600 ${path}\`, and make a new token if anyone else could have read it: ${makeToken(dir)}` };
    }
    let text: string;
    try {
        text = readFileSync(path, 'utf8');
    } catch (e) {
        const code = (e as { code?: unknown }).code;
        return { cause: `the account ${name}'s token ${path} cannot be read${typeof code === 'string' ? ` (${code})` : ''}`,
            fix: `make it readable by the user nexus-server runs as, mode 0600, or ${makeToken(dir)}` };
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
