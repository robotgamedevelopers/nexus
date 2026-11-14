#!/usr/bin/env node
// nexus-claude-driver: nexus-server's driver for Claude (docs/driver-protocol.md), on the Claude Agent SDK.
//
//   node dist/src/main.js --account <name> --cwd <dir> [--resume <session id>] [--model <model>] [--effort <level>]
//                         [--setting-sources <user,project,local>] [--claude <path>]
//
// The server passes --cwd and --resume; the others come from the drivers file. The account is the directory
// ~/.nexus/server/accounts/<name>/ and its token file (account.ts): Claude gets them as CLAUDE_CONFIG_DIR and
// CLAUDE_CODE_OAUTH_TOKEN, in its own environment only, and never the Keychain's login, the server's environment's or
// ~/.claude. Every API-key and login variable is removed first (env.ts), and the driver refuses any session not on a
// subscription: a nexus friend never bills the API.

import { query } from '@anthropic-ai/claude-agent-sdk';

import type { Account } from './account.js';
import type { Harness, HarnessStart } from './driver.js';
import { harnessEnv, scrub } from './env.js';
import { runProcess } from './run.js';

const { env, removed } = scrub(process.env);
// Claude's process gets only `env`; the removed names also leave this process's own environment.
for (const name of removed) {
    Reflect.deleteProperty(process.env, name);
}
env['CLAUDE_AGENT_SDK_CLIENT_APP'] = 'nexus-claude-driver/0.1.0';

function startHarness(start: HarnessStart, account: Account): Harness {
    return query({
        prompt: start.prompts,
        options: {
            cwd: start.cwd,
            // The whole environment the harness gets (it replaces, not merges): the scrubbed one, with the account's login.
            env: harnessEnv(env, account),
            ...(start.sessionId === undefined ? {} : { sessionId: start.sessionId }),
            ...(start.resume === undefined ? {} : { resume: start.resume }),
            ...(start.model === undefined ? {} : { model: start.model }),
            ...(start.effort === undefined ? {} : { effort: start.effort }),
            ...(start.claudePath === undefined ? {} : { pathToClaudeCodeExecutable: start.claudePath }),
            // No settings files, and so no hooks and no apiKeyHelper, unless the drivers file asks for them.
            settingSources: start.settingSources,
            // nexus is for people who trust their AIs: no permission prompts, and the calls that still ask are allowed.
            permissionMode: 'bypassPermissions',
            allowDangerouslySkipPermissions: true,
            canUseTool: (_name, input) => Promise.resolve({ behavior: 'allow', updatedInput: input }),
            includePartialMessages: true,
            stderr: (data: string) => process.stderr.write(data),
        },
    });
}

runProcess(startHarness, removed);
