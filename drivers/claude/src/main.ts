#!/usr/bin/env node
// nexus-claude-driver: nexus-server's driver for Claude (docs/driver-protocol.md), on the Claude Agent SDK.
//
//   node dist/src/main.js --account <name> --cwd <dir> [--resume <session id>] [--limit cwd [--keep-out <dir>]...]
//                         [--model <model>] [--effort <level>] [--setting-sources <user,project,local>] [--claude <path>]
//
//                         [--account-email <email> --verified-helper <path> --auth-claude <path>]
//
// The server supplies the expected email and credential broker for verified accounts. The broker verifies
// or refreshes the private grant before a prompt. Unconfigured legacy accounts use their setup token.
// The SDK receives only the access token via a descriptor and its own isolated state as CLAUDE_CONFIG_DIR.
// The driver checks the SDK's billing and credential source before handing it a prompt.

import { randomUUID } from 'node:crypto';
import { mkdir, rename, rm, writeFile } from 'node:fs/promises';
import { homedir } from 'node:os';
import { join } from 'node:path';
import { query } from '@anthropic-ai/claude-agent-sdk';

import type { Account } from './account.js';
import { parseArguments, kSecretMcpToolName, kSecretToolName, secretMcpServer, type Harness, type HarnessStart } from './driver.js';
import { harnessEnv, scrub } from './env.js';
import { runProcess } from './run.js';
import { sandboxHomeFiles, sandboxTools } from './tools.js';
import { kDriverDir, wallOptions, spawnWithToken } from './wall.js';
import { sendName } from './bus-sender.js';
import { busListener } from './bus-listener.js';
import { workRoutes } from './work-routes.js';
import { gitGrant } from './git-grant.js';
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
let egressPort = 0;
let setup: {cause: string; fix: string} | undefined;
const startupArgs = parseArguments(process.argv.slice(2));
if (!('noSandbox' in startupArgs && startupArgs.noSandbox)) {
    const tools = sandboxTools(process.platform, env);
    if ('path' in tools && tools.path !== undefined) {
        env['PATH'] = tools.path;
    }
    if ('did' in tools) {
        process.stderr.write(`nexus-claude-driver: ${tools.did}\n`);
    }
    // Why the AI's sandbox cannot be set up here, if it cannot: the start is refused with it (run.ts prepare), in the
    // protocol's lines, never with a stack trace and never with the AI run unwalled.
    setup = 'cause' in tools ? tools : undefined;
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

    try {
        egressPort = await new Egress().listen();
    } catch (e) {
        setup ??= { cause: `the proxy for the AI's commands cannot listen on 127.0.0.1 (${e instanceof Error ? e.message : String(e)})`,
            fix: 'free a port on 127.0.0.1 for this user (a firewall or a sandbox around nexus-server may forbid listening)' };
    }
}

let grant: ReturnType<typeof gitGrant> | undefined;
const configuredRoutes = env['NEXUS_WORK_ROUTES'];
delete env['NEXUS_WORK_ROUTES'];
let work: ReturnType<typeof workRoutes> | undefined;
let busSocket: string | undefined;
function startHarness(start: HarnessStart, account: Account): Harness {
    if (start.gitRemote) grant ??= gitGrant(start.gitRemote, env);
    const wall = start.noSandbox ? undefined : wallOptions(account, process.env['HOME'] ?? '', start.cwd, (data) => process.stderr.write(data),
        egressPort, env, {hook: true}, start.limited, start.keepsOut, start.writeDirs, start.readDirs);
    if (grant && wall?.sandbox.network) {
        Object.assign(wall.sandbox.network, {allowUnixSockets: grant.sockets});
        wall.sandbox.filesystem?.allowRead?.push(join(kDriverDir, 'bin/git-remote-nexusgit'));
    }
    let childEnv = {...(grant?.env ?? env)};
    if (configuredRoutes && start.limited && wall?.sandbox.network) {
        work ??= workRoutes(configuredRoutes, childEnv, start.busSender, kDriverDir);
        childEnv = {...work.env};
        wall.sandbox.network.allowUnixSockets = [...(wall.sandbox.network.allowUnixSockets ?? []), ...work.sockets];
        wall.sandbox.filesystem?.allowRead?.push(work.bin, join(kDriverDir, 'bin/git-remote-nexusgit'));
    }
    if (start.noSandbox && configuredRoutes) {
        const routes = JSON.parse(configuredRoutes) as {ghConfig: string; githubAlias: string; repository: string};
        childEnv['GH_CONFIG_DIR'] = routes.ghConfig;
        childEnv['GIT_CONFIG_COUNT'] = '1';
        childEnv['GIT_CONFIG_KEY_0'] = 'url.git@' + routes.githubAlias + ':' + routes.repository + '.git.insteadOf';
        childEnv['GIT_CONFIG_VALUE_0'] = 'git@github.com:' + routes.repository + '.git';
    }
    const instructions = [start.startupContext];
    if (start.busListener && wall?.sandbox.network) {
        const [ai, address] = start.busListener.split('@');
        if (!ai || !address) throw new Error('Missing message bus grant fields');
        busSocket ??= busListener(ai, address);
        const sockets = wall.sandbox.network.allowUnixSockets ?? [];
        wall.sandbox.network.allowUnixSockets = [...sockets, busSocket];
        const bin = join(kDriverDir, 'bin/bus');
        wall.sandbox.filesystem?.allowRead?.push(bin);
        Object.assign(childEnv, {PATH: bin + ':' + (childEnv['PATH'] ?? '/usr/bin:/bin'), NEXUS_BUS_AI: ai,
            NEXUS_BUS_LISTENER: busSocket});
        instructions.push('To listen on the message bus use Monitor with "' + join(bin, 'nova-bus') +
            '" hook --as ' + ai + '. Keep the full output for delivery.');
    }
    if (start.busSender) instructions.push('Peer messages arrive in chat. Use nexus_bus_send to send or reply as yourself. Keep routine sends and their receipts out of the conversation unless the person asks.');
    const startupContext = instructions.filter(Boolean).join('\n');
    const stream = query({
        prompt: start.prompts,
        options: {
            cwd: start.cwd,
            ...(!startupContext ? {} : {
                systemPrompt: { type: 'preset', preset: 'claude_code', append: startupContext },
            }),
            // The whole environment the harness gets (it replaces, not merges): the scrubbed one, with the account's
            // directory. The token is not in it: the wall's spawn hands it over on a descriptor.
            env: harnessEnv(childEnv, account),
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
            mcpServers: { nexus: secretMcpServer(start.askSecret, start.busSender) },
            toolAliases: { [kSecretToolName]: kSecretMcpToolName, ...(start.busSender ? {[sendName]: `mcp__nexus__${sendName}`} : {}) },
            // A limited AI (--limit cwd) reads and writes only its directory (wall.ts limitOf).
            ...(wall ?? {sandbox: {enabled: false}, spawnClaudeCodeProcess: spawnWithToken(account.token, data => process.stderr.write(data))}),
        },
    });
    const catalogReady = stream.supportedModels().then(async (rows) => {
        const valid = /^[A-Za-z0-9][A-Za-z0-9._:/-]{0,127}$/u;
        const models = Object.fromEntries(rows.filter(row => row.value !== "default" && valid.test(row.value))
            .map(row => [row.value, row.supportedEffortLevels ?? []]));
        if (Object.keys(models).length === 0) return;
        const resolved = Object.fromEntries(rows.flatMap(row => row.resolvedModel && valid.test(row.resolvedModel) && row.value in models
            ? [[row.value, row.resolvedModel]] : []));
        const order = Object.keys(models);
        if (!start.model || start.model === 'default') await stream.setModel(order[0]);
        const data = JSON.stringify({ order, models, resolved });
        if (data.length > 16384) return;
        const directory = join(homedir(), '.nexus', 'server', 'catalogs');
        await mkdir(directory, { recursive: true, mode: 0o700 });
        const temp = join(directory, randomUUID());
        try {
            await writeFile(temp, data, { mode: 0o600, flag: 'wx' });
            await rename(temp, join(directory, 'claude.json'));
        } finally {
            await rm(temp, { force: true });
        }
    }).catch(() => process.stderr.write('Claude model discovery did not update the Nexus catalog.\n'));
    return Object.assign(stream, {catalogReady});
}

runProcess(startHarness, removed, undefined, undefined, setup);
