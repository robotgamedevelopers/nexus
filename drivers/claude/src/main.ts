#!/usr/bin/env node
// nexus-claude-driver: nexus-server's driver for Claude (docs/driver-protocol.md), on the Claude Agent SDK.
//
//   node dist/src/main.js --cwd <dir> [--resume <session id>] [--model <model>] [--effort <level>]
//                         [--setting-sources <user,project,local>] [--claude <path>]
//
// The server passes --cwd and --resume; the others come from the drivers file. The account is the environment's: the
// login of the user session the server runs in, or CLAUDE_CODE_OAUTH_TOKEN (and CLAUDE_CONFIG_DIR) when the server
// gives one. Every API-key variable is removed first (env.ts), and the driver refuses any session not on a
// subscription: a nexus friend never bills the API.

import { query } from '@anthropic-ai/claude-agent-sdk';

import type { Harness, HarnessStart } from './driver.js';
import { scrub } from './env.js';
import { runProcess } from './run.js';

const { env, removed } = scrub(process.env);
// Nothing in this process, the SDK included, sees what was removed.
for (const name of removed) {
    Reflect.deleteProperty(process.env, name);
}
env['CLAUDE_AGENT_SDK_CLIENT_APP'] = 'nexus-claude-driver/0.1.0';

function startHarness(start: HarnessStart): Harness {
    return query({
        prompt: start.prompts,
        options: {
            cwd: start.cwd,
            // The whole environment the harness gets (it replaces, not merges): the scrubbed one.
            env,
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

runProcess(startHarness, (env['CLAUDE_CODE_OAUTH_TOKEN'] ?? '') !== '', removed);
