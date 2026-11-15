// The programs Claude's sandbox needs on Linux and WSL2, bubblewrap (bwrap) and socat (docs/secrets.md, "The token
// wall"). Claude finds them on its PATH; none is installed on the machine for it. nexus's build makes both from pinned
// sources (cmake/sandbox.cmake) into one directory, which nexus-server names to its drivers in NEXUS_SANDBOX_BIN:
//
//   bwrap/bwrap          bubblewrap as nexus built it
//   socat/socat          socat as nexus built it
//
// Ubuntu 24.04 and later restrict unprivileged user namespaces with AppArmor (the sysctl
// kernel.apparmor_restrict_unprivileged_userns is 1): a program makes them only when an AppArmor profile attached to its
// path allows it. nexus's bwrap has none until the driver loads one, once per machine and build directory, through
// passwordless sudo, with no prompt (appArmorProfile: the profile, which allows user namespaces and nothing else, as the
// profiles Ubuntu ships for Chrome and Electron apps do). Without passwordless sudo the driver refuses to start and names
// what is missing; it never changes the sysctl, which would lift the restriction for every program. The system's
// /usr/bin/bwrap is no way round it: its profile confines what it runs so that the sandbox's own seccomp step, which
// needs a user namespace inside the sandbox, fails (measured on an Ubuntu 26.04 bench, 2026-11-15). WSL2's kernel has no
// AppArmor, and nexus's bwrap runs there as it is. macOS's sandbox is the system's, and needs neither program.
//
// On Linux the sandbox also needs ~/.bashrc and ~/.gitconfig to exist: Claude's sandbox keeps commands from writing
// them, and holds one that is missing in a way that every bash or git command then fails on ("Permission denied";
// measured in CI on Ubuntu and WSL2). sandboxHomeFiles makes each one that is missing, empty, which bash and git read as
// they read none.

import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { closeSync, constants, existsSync, lstatSync, openSync, readFileSync, realpathSync } from 'node:fs';

/** The variable nexus-server sets to its build's sandbox tools directory (src/server/main.cpp). */
export const kSandboxBinVariable = 'NEXUS_SANDBOX_BIN';
/** 1 when AppArmor lets only programs with a profile make user namespaces (Ubuntu 24.04 and later). */
export const kRestrictUserns = '/proc/sys/kernel/apparmor_restrict_unprivileged_userns';
/** Where the profiles AppArmor loads at boot live. */
export const kAppArmorDir = '/etc/apparmor.d';
/** What loads a profile into the kernel. */
export const kAppArmorParser = '/usr/sbin/apparmor_parser';
/**
 * sudo, and what writes the profile into place from standard input and sets its mode: by absolute path, never looked up
 * on a PATH the AI could write. tee reads its standard input as it is; `install /dev/stdin` fails where sudo runs the
 * command on a terminal of its own (use_pty, Ubuntu's default: "No such device or address", CI on Ubuntu, 2026-11-15).
 */
export const kSudo = '/usr/bin/sudo';
export const kTee = '/usr/bin/tee';
export const kChmod = '/usr/bin/chmod';

/** What the driver needs of the machine to set up the sandbox: a seam for the unit tests. */
export interface ToolsSystem {
    exists(path: string): boolean;
    /** The file's text, or undefined when it cannot be read. */
    read(path: string): string | undefined;
    /** The path with every link resolved, or the path as given when it cannot be. */
    realpath(path: string): string;
    /** The folder at `path` itself (a link is not followed): its owner and mode, or undefined when it is not a folder. */
    folder(path: string): { uid: number; mode: number } | undefined;
    /**
     * Runs kSudo -n with `args`, never prompting, with `input` on its standard input: undefined when it ran and
     * succeeded, else what went wrong.
     */
    sudo(args: readonly string[], input?: string): string | undefined;
}

export const realToolsSystem: ToolsSystem = {
    exists: (path) => existsSync(path),
    read: (path) => {
        try {
            return readFileSync(path, 'utf8');
        } catch {
            return undefined;
        }
    },
    realpath: (path) => {
        try {
            return realpathSync(path);
        } catch {
            return path;
        }
    },
    folder: (path) => {
        try {
            const st = lstatSync(path);
            return st.isDirectory() ? { uid: st.uid, mode: st.mode & 0o7777 } : undefined;
        } catch {
            return undefined;
        }
    },
    sudo: (args, input) => {
        try {
            execFileSync(kSudo, ['-n', ...args], { input: input ?? '', stdio: ['pipe', 'ignore', 'pipe'], timeout: 30_000 });
            return undefined;
        } catch (e) {
            const err = e as { stderr?: Buffer; message?: string };
            const line = (err.stderr?.toString('utf8') ?? '').split('\n').find((l) => l.trim() !== '');
            return (line ?? err.message ?? 'it failed').trim();
        }
    },
};

/** The AppArmor profile that lets the bwrap at `bwrap` (a resolved path) make user namespaces: its name and text. */
export function appArmorProfile(bwrap: string): { name: string; text: string } {
    const name = `nexus-bwrap-${createHash('sha256').update(bwrap).digest('hex').slice(0, 16)}`;
    const text = [
        '# nexus: lets nexus\'s own bubblewrap make the user namespaces its sandbox needs, and nothing else',
        '# (docs/secrets.md, "The token wall"). Loaded by the Claude driver through passwordless sudo.',
        'abi <abi/4.0>,',
        'include <tunables/global>',
        '',
        `profile ${name} "${bwrap}" flags=(unconfined) {`,
        '  userns,',
        '}',
        '',
    ].join('\n');
    return { name, text };
}

/** Characters AppArmor reads in a path as a pattern, or that end the quoted path. */
const kNotLiteral = /["\\*?[\]{}^\n\r\0]/;

export type SandboxTools =
    | { path: string | undefined; did?: string }
    | { cause: string; fix: string };

/**
 * The PATH Claude gets so its sandbox finds bwrap and socat: `path` (undefined: leave PATH as it is, off Linux), and in
 * `did` the one line saying so when the driver loaded the AppArmor profile; or why the sandbox cannot run here, which
 * refuses the start rather than run the AI unwalled.
 */
export function sandboxTools(platform: NodeJS.Platform, env: Readonly<Record<string, string | undefined>>,
    sys: ToolsSystem = realToolsSystem): SandboxTools {
    if (platform !== 'linux') {
        return { path: undefined };
    }
    const dir = env[kSandboxBinVariable];
    if (dir === undefined || dir === '') {
        return { cause: `${kSandboxBinVariable} is not set, so the sandbox's bwrap and socat cannot be found`,
            fix: 'start the driver from nexus-server, which sets it to its build\'s sandbox tools (cmake/sandbox.cmake)' };
    }
    const bwrapDir = `${dir}/bwrap`;
    const socatDir = `${dir}/socat`;
    for (const program of [`${bwrapDir}/bwrap`, `${socatDir}/socat`]) {
        if (!sys.exists(program)) {
            return { cause: `the sandbox's ${program} is missing`,
                fix: 'build nexus-server again (cmake --build), which builds the sandbox tools beside it' };
        }
    }
    const rest = env['PATH'];
    const path = [bwrapDir, socatDir, ...(rest === undefined || rest === '' ? [] : [rest])].join(':');
    if (sys.read(kRestrictUserns)?.trim() !== '1') {
        return { path };
    }
    const loaded = loadProfile(sys.realpath(`${bwrapDir}/bwrap`), sys);
    if (loaded !== undefined && 'cause' in loaded) {
        return loaded;
    }
    return loaded === undefined ? { path } : { path, did: loaded.did };
}

/** The profile for `bwrap` in place: undefined when it already was, `did` when the driver loaded it, or the refusal. */
function loadProfile(bwrap: string, sys: ToolsSystem): { did: string } | { cause: string; fix: string } | undefined {
    const restricted = 'this kernel\'s AppArmor lets a program make the user namespaces the sandbox needs only with a '
        + 'profile (kernel.apparmor_restrict_unprivileged_userns = 1)';
    if (kNotLiteral.test(bwrap)) {
        return { cause: `${restricted}, and nexus's bwrap is at ${JSON.stringify(bwrap)}, a path AppArmor would read as a pattern`,
            fix: 'build nexus in a directory whose path has none of " \\ * ? [ ] { } ^' };
    }
    const { name, text } = appArmorProfile(bwrap);
    const file = `${kAppArmorDir}/${name}`;
    if (sys.read(file) === text) {
        return undefined;
    }
    if (!sys.exists(kAppArmorParser)) {
        return { cause: `${restricted}, and ${kAppArmorParser}, which loads one, is missing`,
            fix: `install AppArmor's parser (apparmor), which loads nexus's profile for ${bwrap}` };
    }
    // What root writes into must be root's alone: a folder another user could write (or a link to one) would let them
    // swap the profile file between the write and the load at boot.
    const dir = sys.folder(kAppArmorDir);
    if (dir?.uid !== 0 || (dir.mode & 0o022) !== 0) {
        return { cause: `${restricted}, and ${kAppArmorDir}, where nexus would keep its profile, is `
            + (dir === undefined ? 'not a folder' : `owned by uid ${dir.uid} with mode 0${dir.mode.toString(8)}`)
            + ', not root\'s alone', fix: `make it root's, mode 0755: sudo chown root:root ${kAppArmorDir} && sudo chmod 0755 ${kAppArmorDir}` };
    }
    const noSudo = sys.sudo(['true']);
    if (noSudo !== undefined) {
        return { cause: `${restricted}; nexus loads its own for ${bwrap}, once, through passwordless sudo, and sudo -n `
            + `here says: ${noSudo}`, fix: 'passwordless sudo for this user is what is missing' };
    }
    // Loaded first, then kept where AppArmor loads it at boot: a profile file there is always one that loaded. The text
    // goes to root on standard input, never through a file: a file the driver wrote would be one the AI's sandbox can
    // write too (the temporary directories are writable in it), and a session already running could change it between
    // the two commands. Every path root is handed is a program's, by absolute path, or the profile's under kAppArmorDir.
    const failed = sys.sudo([kAppArmorParser, '-r'], text) ?? sys.sudo([kTee, file], text) ?? sys.sudo([kChmod, '0644', file])
        ?? (sys.read(file) === text ? undefined : `${file} does not hold the profile's text after writing it`);
    if (failed !== undefined) {
        return { cause: `${restricted}; loading nexus's profile for ${bwrap} through sudo failed: ${failed}`,
            fix: `the profile is ${file}, from drivers/claude/src/tools.ts appArmorProfile` };
    }
    return { did: `loaded the AppArmor profile ${file}, which lets ${bwrap} make user namespaces` };
}

/** What the driver needs to make a file: a seam for the unit tests. */
export interface HomeFs {
    exists(path: string): boolean;
    /** Makes `path` empty, mode 0644, only if nothing is there: false when something is, or it cannot. */
    createEmpty(path: string): boolean;
}

export const realHomeFs: HomeFs = {
    exists: (path) => existsSync(path),
    createEmpty: (path) => {
        try {
            closeSync(openSync(path, constants.O_CREAT | constants.O_EXCL | constants.O_WRONLY | constants.O_NOFOLLOW, 0o644));
            return true;
        } catch {
            return false;
        }
    },
};

/** The files under HOME that Claude's sandbox on Linux needs to exist, or every bash or git command in it fails. */
export const kSandboxHomeFiles = ['.bashrc', '.gitconfig'] as const;

/**
 * Makes each of kSandboxHomeFiles under `home` that is missing (Linux only): the ones it made, or, when one is missing
 * and cannot be made, the refusal that names it (the start is refused then, rather than every command failing later
 * with "Permission denied").
 */
export function sandboxHomeFiles(platform: NodeJS.Platform, home: string, fs: HomeFs = realHomeFs):
    { made: string[] } | { cause: string; fix: string } {
    if (platform !== 'linux' || home === '') {
        return { made: [] };
    }
    const made: string[] = [];
    for (const name of kSandboxHomeFiles) {
        const path = `${home}/${name}`;
        if (fs.exists(path)) {
            continue;
        }
        if (fs.createEmpty(path)) {
            made.push(path);
        } else if (!fs.exists(path)) {
            return { cause: `${path} is missing and the driver cannot make it; Claude's sandbox on Linux needs it to exist, `
                + 'or every bash and git command in it fails', fix: `make it, empty: touch ${path}` };
        }
    }
    return { made };
}
