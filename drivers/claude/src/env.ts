// The environment the harness runs in: the server's, without anything that would make Claude bill an API key or
// another provider instead of the subscription login. Pure.

/**
 * Whether `name` is a variable that can put Claude on an API key, an API gateway or another provider, or that
 * belongs to a Claude Code session the driver was started from:
 *   ANTHROPIC_*            API key, auth token, base URL, custom headers, and every provider's own settings
 *   CLAUDE_CODE_USE_*      the switch to Bedrock, Vertex, Foundry and the other providers
 *   AWS_BEARER_TOKEN_BEDROCK, *_API_KEY of a provider (Bedrock, Vertex, Foundry)
 *   CLAUDECODE, CLAUDE_CODE_* other than the two account variables below: a parent Claude Code session's plumbing
 * Kept: CLAUDE_CODE_OAUTH_TOKEN (a subscription login's token, how an account is chosen) and CLAUDE_CONFIG_DIR.
 */
export function isApiKeyVariable(name: string): boolean {
    if (name === 'CLAUDE_CODE_OAUTH_TOKEN' || name === 'CLAUDE_CONFIG_DIR') {
        return false;
    }
    return name.startsWith('ANTHROPIC_')
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
