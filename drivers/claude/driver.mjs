// TRACER (not the product): nexus-server's shim to Claude through the Claude Agent SDK. One process per tab, started by
// the server. Line protocol, one JSON object per line.
//
//   stdin:  {"op":"send","text":"..."}   a prompt: one turn
//           {"op":"stop"}                interrupt the turn running now
//   stdout: {"ev":"ready","pid":N}
//           {"ev":"credential",...}      what the SDK says the credential is (once per process, before any reply)
//           {"ev":"session","id":"..."}  the session id, when known
//           {"ev":"text","text":"..."}   a whole assistant text block
//           {"ev":"tool","name":"...","summary":"..."}
//           {"ev":"done",...}            a turn's result: subtype, cost and usage
//           {"ev":"error","cause":"..."}
//
//   node driver.mjs --cwd <dir> [--resume <session id>] [--partial]
//
// Subscription only: every API-key variable is removed from the SDK's environment, and a session whose credential is an
// API key (apiKeySource other than "none", or a provider other than first party) is an error, not a fallback.

import { query } from '@anthropic-ai/claude-agent-sdk';
import { createInterface } from 'node:readline';

function out(obj) {
    process.stdout.write(JSON.stringify(obj) + '\n');
}

const args = process.argv.slice(2);
let cwd = process.cwd();
let resume;
let partial = false;
for (let i = 0; i < args.length; i++) {
    if (args[i] === '--cwd') cwd = args[++i];
    else if (args[i] === '--resume') resume = args[++i];
    else if (args[i] === '--partial') partial = true;
    else {
        out({ ev: 'error', cause: `unknown argument ${args[i]}; the arguments are --cwd <dir>, --resume <id>, --partial` });
        process.exit(2);
    }
}

// The SDK's environment is a short allow-list, not the server's: no ANTHROPIC_* (API key, auth token, base URL), no
// provider switch (CLAUDE_CODE_USE_BEDROCK/VERTEX/FOUNDRY, AWS_*), and none of a host Claude session's own CLAUDE_CODE_*
// variables (a driver started from inside a Claude Code session inherits that session's auth plumbing otherwise). The
// two account variables pass: CLAUDE_CODE_OAUTH_TOKEN (a subscription token from `claude setup-token`) and
// CLAUDE_CONFIG_DIR.
const KEEP = new Set(['HOME', 'PATH', 'USER', 'LOGNAME', 'SHELL', 'TMPDIR', 'LANG', 'TERM', 'TZ',
    'CLAUDE_CODE_OAUTH_TOKEN', 'CLAUDE_CONFIG_DIR']);
const env = {};
const removed = [];
for (const [k, v] of Object.entries(process.env)) {
    if (KEEP.has(k) || k.startsWith('LC_')) env[k] = v;
    else if (/^(ANTHROPIC_|CLAUDE|AWS_|USE_.*OAUTH)/.test(k)) removed.push(k);
}
env.CLAUDE_AGENT_SDK_CLIENT_APP = 'nexus-claude-driver/0.0.0-tracer';

// Streaming input: one async queue of user messages, so one process serves many turns and interrupt() works.
const pending = [];
let wake = null;
let closed = false;
async function* prompts() {
    for (;;) {
        while (pending.length > 0) yield pending.shift();
        if (closed) return;
        await new Promise((r) => { wake = r; });
        wake = null;
    }
}
function push(text) {
    pending.push({ type: 'user', message: { role: 'user', content: text }, parent_tool_use_id: null });
    if (wake) wake();
}

const q = query({
    prompt: prompts(),
    options: {
        cwd,
        resume,
        env,
        permissionMode: 'bypassPermissions',
        allowDangerouslySkipPermissions: true,
        includePartialMessages: partial,
        stderr: (data) => process.stderr.write(data),
    },
});

out({ ev: 'ready', pid: process.pid, removed_env: removed, oauth_token_env: 'CLAUDE_CODE_OAUTH_TOKEN' in env });

let credentialChecked = false;
let refused = false;
async function checkCredential(init) {
    let account = {};
    try {
        account = await q.accountInfo();
    } catch (e) {
        account = { error: String(e) };
    }
    // Never the email or organization: only what says which kind of credential it is.
    const cred = {
        ev: 'credential',
        apiKeySource: init.apiKeySource,
        tokenSource: account.tokenSource,
        subscriptionType: account.subscriptionType,
        apiProvider: account.apiProvider,
    };
    out(cred);
    if (account.subscriptionType === undefined) {
        // Not a refusal by itself (the turn's own result says what failed), but the likely cause, said first.
        out({ ev: 'error', cause: 'the SDK reports no subscription login (no subscriptionType, tokenSource ' +
            `${account.tokenSource}); log in with \`claude /login\` as the subscription account, or give the ` +
            'driver CLAUDE_CODE_OAUTH_TOKEN from `claude setup-token`' });
    }
    const apiProviderOk = account.apiProvider === undefined || account.apiProvider === 'firstParty';
    if (init.apiKeySource !== 'none' || !apiProviderOk) {
        refused = true;
        out({ ev: 'error', cause: `the session would bill an API key or another provider (apiKeySource ` +
            `${init.apiKeySource}, apiProvider ${account.apiProvider}), not the subscription login; ` +
            `remove the key (or apiKeyHelper setting) and run \`claude /login\` with the subscription account` });
        closed = true;
        if (wake) wake();
        try { await q.interrupt(); } catch { /* the turn may not have started */ }
        process.exit(3);
    }
}

let sentAt = Date.now();
const rl = createInterface({ input: process.stdin });
rl.on('line', async (line) => {
    let msg;
    try {
        msg = JSON.parse(line);
    } catch {
        out({ ev: 'error', cause: `stdin line is not JSON: ${line.slice(0, 80)}` });
        return;
    }
    if (msg.op === 'send' && typeof msg.text === 'string') {
        sentAt = Date.now();
        if (!refused) push(msg.text);
    } else if (msg.op === 'stop') {
        const t0 = Date.now();
        try {
            const r = await q.interrupt();
            out({ ev: 'stopped', ms: Date.now() - t0, response: r ?? null });
        } catch (e) {
            out({ ev: 'error', cause: `interrupt failed: ${e}` });
        }
    } else {
        out({ ev: 'error', cause: `unknown op in ${line.slice(0, 80)}; the ops are send and stop` });
    }
});
rl.on('close', () => {
    closed = true;
    if (wake) wake();
});

let sessionSent = '';
let streamed = false;
try {
    for await (const m of q) {
        if (m.session_id && m.session_id !== sessionSent) {
            sessionSent = m.session_id;
            out({ ev: 'session', id: m.session_id });
        }
        if (m.type === 'system' && m.subtype === 'init') {
            if (!credentialChecked) {
                credentialChecked = true;
                await checkCredential(m);
            }
        } else if (m.type === 'assistant') {
            if (m.error) out({ ev: 'error', cause: `assistant message error: ${JSON.stringify(m.error)}` });
            for (const b of m.message.content ?? []) {
                if (b.type === 'text') out({ ev: 'text', text: b.text });
                else if (b.type === 'tool_use') {
                    const input = JSON.stringify(b.input ?? {});
                    out({ ev: 'tool', name: b.name, summary: input.length > 120 ? input.slice(0, 117) + '...' : input });
                }
            }
        } else if (m.type === 'stream_event') {
            // Only the first delta of each turn, with its shape, so a stop can be timed against a running turn.
            const e = m.event;
            if (e.type === 'content_block_delta' && !streamed) {
                streamed = true;
                out({ ev: 'streaming', ms: Date.now() - sentAt, delta_type: e.delta?.type });
            }
        } else if (m.type === 'tool_use_summary') {
            out({ ev: 'tool_summary', summary: m.summary });
        } else if (m.type === 'result') {
            streamed = false;
            out({ ev: 'done', ms: Date.now() - sentAt, subtype: m.subtype, is_error: m.is_error, stop_reason: m.stop_reason,
                num_turns: m.num_turns, total_cost_usd: m.total_cost_usd, usage: m.usage, modelUsage: m.modelUsage,
                errors: m.errors });
        } else if (m.type === 'auth_status' && m.error) {
            out({ ev: 'error', cause: `auth: ${m.error}` });
        } else {
            // Recorded for the tracer: which other message kinds arrive.
            out({ ev: 'other', type: m.type, subtype: m.subtype });
        }
    }
    out({ ev: 'end' });
} catch (e) {
    out({ ev: 'error', cause: `the SDK's stream ended with: ${e && e.stack ? e.stack : e}` });
    process.exit(1);
}
