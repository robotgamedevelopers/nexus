#!/usr/bin/env node
// nexus-claude-driver: nexus-server's driver for Claude (docs/driver-protocol.md), on the Claude Agent SDK.
//
//   node dist/src/main.js --account <name> --cwd <dir> [--resume <session id>] [--limit cwd [--keep-out <dir>]...]
//                         [--model <model>] [--effort <level>] [--setting-sources <user,project,local>] [--claude <path>]
//
// The server passes --cwd, --resume, --limit (a friend limited to its directory) and --keep-out (more directories it
// is kept out of, friends.json "limit_keeps_out"); the others come from the drivers
// file. The account is the directory
// ~/.nexus/server/accounts/<name>/ and its token file (account.ts): Claude gets the directory as CLAUDE_CONFIG_DIR and
// the token on a descriptor, never in an environment, and never the Keychain's login, the server's environment's or
// ~/.claude. Every API-key and login variable is removed first (env.ts), and the driver refuses any session not on a
// subscription: a nexus friend never bills the API. The AI Claude runs never reads the token (wall.ts).

import { query } from '@anthropic-ai/claude-agent-sdk';

import type { Account } from './account.js';
import { kSecretMcpToolName, kSecretToolName, secretMcpServer, type Harness, type HarnessStart } from './driver.js';
import { harnessEnv, scrub } from './env.js';
import { runProcess } from './run.js';
import { sandboxHomeFiles, sandboxTools } from './tools.js';
import { kDriverDir, wallOptions } from './wall.js';
import { Egress } from './egress.js';

const { env, removed } = scrub(process.env);
// Claude's process gets only `env`; the removed names also leave this process's own environment.
for (const name of removed) {
    Reflect.deleteProperty(process.env, name);
}
env['CLAUDE_AGENT_SDK_CLIENT_APP'] = 'nexus-claude-driver/0.1.0';
// On Linux and WSL2 the sandbox's bwrap and socat are nexus's own, put first on Claude's PATH (tools.ts), with the
// AppArmor profile nexus's bwrap needs on Ubuntu 24.04 and later loaded once through passwordless sudo; when they
// cannot run here the driver refuses to start rather than run the AI without its sandbox.
const tools = sandboxTools(process.platform, env);
if ('path' in tools && tools.path !== undefined) {
    env['PATH'] = tools.path;
}
if ('did' in tools) {
    process.stderr.write(`nexus-claude-driver: ${tools.did}\n`);
}
// Why the AI's sandbox cannot be set up here, if it cannot: the start is refused with it (run.ts prepare), in the
// protocol's lines, never with a stack trace and never with the AI run unwalled.
let setup: { cause: string; fix: string } | undefined = 'cause' in tools ? tools : undefined;
// On Linux every bash and git command in the sandbox fails without ~/.bashrc and ~/.gitconfig: made empty if missing.
const home = sandboxHomeFiles(process.platform, process.env['HOME'] ?? '');
if ('cause' in home) {
    setup ??= home;
} else {
    for (const made of home.made) {
        process.stderr.write(`nexus-claude-driver: made ${made}, empty: the sandbox's shell and git need it to exist\n`);
    }
}
// ssh on Claude's PATH goes through the sandbox's proxy, so `ssh host` reaches the fleet (bin/ssh).
env['PATH'] = `${kDriverDir}/bin:${env['PATH'] ?? '/usr/bin:/bin'}`;
// The AI's commands reach the network only through this proxy, which refuses this machine's own addresses (egress.ts).
let egressPort = 0;
try {
    egressPort = await new Egress().listen();
} catch (e) {
    setup ??= { cause: `the proxy for the AI's commands cannot listen on 127.0.0.1 (${e instanceof Error ? e.message : String(e)})`,
        fix: 'free a port on 127.0.0.1 for this user (a firewall or a sandbox around nexus-server may forbid listening)' };
}

function startHarness(start: HarnessStart, account: Account): Harness {
    return query({
        prompt: start.prompts,
        options: {
            cwd: start.cwd,
            ...(start.startupContext === undefined ? {} : {
                systemPrompt: { type: 'preset', preset: 'claude_code', append: start.startupContext },
            }),
            // The whole environment the harness gets (it replaces, not merges): the scrubbed one, with the account's
            // directory. The token is not in it: the wall's spawn hands it over on a descriptor.
            env: harnessEnv(env, account),
            ...(start.sessionId === undefined ? {} : { sessionId: start.sessionId }),
            ...(start.resume === undefined ? {} : { resume: start.resume }),
            ...(start.model === undefined ? {} : { model: start.model }),
            ...(start.effort === undefined ? {} : { effort: start.effort }),
            ...(start.claudePath === undefined ? {} : { pathToClaudeCodeExecutable: start.claudePath }),
            // No settings files, and so no hooks and no apiKeyHelper, unless the drivers file asks for them.
            settingSources: start.settingSources,
            // nexus is for people who trust their AIs: no permission prompts, and the calls that still ask are allowed.
            // The guard is the wall: the sandbox every shell command runs in, the deny rules and the hook (wall.ts).
            permissionMode: 'bypassPermissions',
            allowDangerouslySkipPermissions: true,
            canUseTool: (_name, input) => Promise.resolve({ behavior: 'allow', updatedInput: input }),
            includePartialMessages: true,
            mcpServers: { nexus: secretMcpServer(start.askSecret) },
            toolAliases: { [kSecretToolName]: kSecretMcpToolName },
            // A limited friend (--limit cwd) reads and writes only its directory (wall.ts limitOf).
            ...wallOptions(account, process.env['HOME'] ?? '', start.cwd, (data) => process.stderr.write(data), egressPort,
                env, { hook: true }, start.limited, start.keepsOut),
        },
    });
}

runProcess(startHarness, removed, undefined, undefined, setup);
