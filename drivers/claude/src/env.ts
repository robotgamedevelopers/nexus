// The environment the harness runs in: the server's, without anything that would make Claude bill an API key or
// another provider, or use a login other than the account's (account.ts), which is then added. Pure.

import type { Account } from './account.js';

/**
 * Whether `name` is a variable that can put Claude on an API key, an API gateway or another provider, or that
 * belongs to a Claude Code session the driver was started from:
 *   ANTHROPIC_*            API key, auth token, base URL, custom headers, and every provider's own settings
 *   CLAUDE_CODE_USE_*      the switch to Bedrock, Vertex, Foundry and the other providers
 *   AWS_BEARER_TOKEN_BEDROCK, *_API_KEY of a provider (Bedrock, Vertex, Foundry)
 *   CLAUDECODE, CLAUDE_CODE_*  a parent Claude Code session's plumbing, and CLAUDE_CODE_OAUTH_TOKEN, a login's token
 *   CLAUDE_CONFIG_DIR      another login's directory
 * The account's own CLAUDE_CONFIG_DIR comes only from its directory under nexus's home; its token is in no environment
 * at all (wall.ts: Claude reads it from a descriptor).
 */
export function isApiKeyVariable(name: string): boolean {
    return name === 'CLAUDE_CONFIG_DIR'
        || name.startsWith('ANTHROPIC_')
        || name.startsWith('CLAUDE_CODE_')
        || name === 'CLAUDECODE'
        || name === 'AWS_BEARER_TOKEN_BEDROCK'
        || /^(VERTEX|BEDROCK|FOUNDRY|AZURE_FOUNDRY)_.*(KEY|TOKEN)/.test(name);
}

/** `env` without the API-key variables, and the names it removed (never their values). */
export function scrub(env: Readonly<Record<string, string | undefined>>): { env: Record<string, string>; removed: string[] } {
    const kept: Record<string, string> = {};
    const removed: string[] = [];
    for (const [name, value] of Object.entries(env)) {
        if (value === undefined) {
            continue;
        }
        if (isApiKeyVariable(name)) {
            removed.push(name);
        } else {
            kept[name] = value;
        }
    }
    return { env: kept, removed: removed.sort() };
}

/**
 * Claude's environment: `scrubbed` (from scrub) with the account's Claude state directory. Never the token: it goes to Claude on a
 * descriptor (wall.ts spawnWithToken), so no tool Claude runs inherits it.
 */
export function harnessEnv(scrubbed: Readonly<Record<string, string>>, account: Account): Record<string, string> {
    return { ...scrubbed, CLAUDE_CONFIG_DIR: account.configDir };
}
