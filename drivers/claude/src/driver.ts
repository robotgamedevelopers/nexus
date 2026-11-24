// The Claude driver's logic: the driver protocol (docs/driver-protocol.md) on one side, one Claude Agent SDK query in
// streaming input mode on the other. Everything outside the process (the SDK's query, standard output, the exit) comes
// in through a Host, so the unit tests run it against a scripted query with no process, clock or network.
//
// A turn: the server's `send` becomes one user message pushed into the query; the query's stream events become `text`
// and `text-end`, its tool_use blocks `tool`, and its `result` the turn's `done`. A `stop` interrupts the query, whose
// `result` then closes the turn as stopped; the next prompt is pushed only once the interrupt has answered, so an
// interrupt can never land in a later turn. Model and effort changes wait for the next `send` and are applied before
// its prompt is pushed, so a turn never changes model halfway.
//
// Subscription only. The harness runs with no API-key variables (env.ts), no settings files unless asked
// (--setting-sources), and is checked before any prompt reaches it: accountInfo() must name a first-party subscription
// and no API key; and every `init` must say apiKeySource "none". A turn that starts drawing on paid extra usage
// (overage) is stopped. Each refusal is a `no-credential` error with its cause and fix, then the driver exits.
//
// The turn's state machine (the credential check, the push, stop and interrupt, settings, a lost --resume id, the
// refusals) is models/ClaudeDriver.tla. Each step below that the model names is recorded through Host.step, and the
// model job checks the traces of the unit tests and of test/walk.ts against it (models/ClaudeDriverTrace.tla).

import type { VerifiedLogin } from './verified.js';
import { randomUUID } from 'node:crypto';
import { isAbsolute } from 'node:path';

import type {
    AccountInfo,
    EffortLevel,
    McpSdkServerConfigWithInstance,
    Query,
    SDKAssistantMessage,
    SDKMessage,
    SDKResultMessage,
    SDKUserMessage,
    SettingSource,
} from '@anthropic-ai/claude-agent-sdk';
import { createSdkMcpServer, tool } from '@anthropic-ai/claude-agent-sdk';
import { z } from 'zod';

import { isAccountName, kTokenFile } from './account.js';
import {
    cut,
    decodeServerLine,
    encode,
    isSecretWhy,
    isSessionId,
    kProtocolVersion,
    LineReader,
    oneLine,
    whole,
    type Billing,
    type DriverMessage,
    type ErrorCode,
    type SecretAnswer,
    type Settings,
    type Usage,
} from './protocol.js';

/** What the driver uses of the SDK's Query. */
export type Harness = Pick<Query, 'interrupt' | 'setModel' | 'applyFlagSettings' | 'accountInfo' | 'close'> &
    AsyncIterable<SDKMessage> & {catalogReady?: Promise<unknown>};

/** How a harness is started: a new session (`sessionId`) or a resumed one (`resume`). */
export interface HarnessStart {
    prompts: AsyncIterable<SDKUserMessage>;
    cwd: string;
    sessionId?: string;
    resume?: string;
    model?: string;
    effort?: EffortLevel;
    startupContext?: string;
    settingSources: SettingSource[];
    claudePath?: string;
    limited: boolean;                   // the friend is limited to `cwd` (--limit cwd; wall.ts limitOf)
    writeDirs?: string[];
    keepsOut: string[];                 // and kept out of these too (--keep-out <dir>; wall.ts limitOf)
    askSecret: (account: string, what: string, why: string) => Promise<string>;
}

/** Model and effort changes; an absent field is no change (in `inUse`, the value the driver was started with). */
export interface Changes {
    model?: string;                     // '' is Claude's default
    effort?: EffortLevel;
}

/** The driver's state as models/ClaudeDriver.tla names it, recorded after each of its steps for the trace check. */
export interface DriverState {
    phase: 'checking' | 'ready' | 'ended';
    resume: 'given' | 'none';           // the harness was started with a --resume id
    open: number;                       // the open turn, 0 for none
    stage: 'none' | 'waiting' | 'applying' | 'pushed' | 'taken';
    stopping: boolean;
    textOpen: boolean;
    failure: 'none' | 'harness' | 'no-credential' | 'rate-limit' | 'usage-limit' | 'model-limit';
    interrupting: number;               // the turn whose interrupt has not answered yet, 0 for none
    pending: Changes;                   // settings for the next send
    carry: Changes;                     // the open turn's settings, not yet applied
    inUse: Changes;                     // the settings the harness runs with
    last: number;                       // the last turn sent
    secrets: boolean;                   // the server offered "secrets" before the first send
    asking: number;                     // the open ask-secret number, 0 for none
    asks: number;                       // the last ask-secret number minted
}

/** What a recorded step was given: a turn, an ask, settings, or what kind of result or failure. */
export interface StepArgs {
    turn?: number;
    ask?: number;
    changes?: Changes;
    kind?: 'ok' | 'error' | 'lost';
    code?: DriverState['failure'];
    rejectedLimit?: Turn['rejectedLimit'];
}

/** The world outside the driver. */
export interface Host {
    write(line: string): void;          // one protocol line to standard output
    log(text: string): void;            // a line to standard error
    exit(code: number): void;           // end the process (after what was written reaches the server)
    startHarness(start: HarnessStart): Harness;
    newSessionId(): string;
    refreshCredential?(): Promise<'same' | 'changed' | 'sign-in' | 'unavailable'>;
    /** A step of the turn's state machine and the state after it (models/ClaudeDriver.tla); only tests listen. */
    step?(op: string, args: StepArgs, state: DriverState): void;
}

export const kEffortLevels: readonly EffortLevel[] = ['low', 'medium', 'high', 'xhigh', 'max'];
const kSettingSources: readonly SettingSource[] = ['user', 'project', 'local'];

export function isEffortLevel(s: string): s is EffortLevel {
    return (kEffortLevels as readonly string[]).includes(s);
}

/** The driver's arguments, or why they cannot be used. */
export interface Arguments {
    account: string;                    // the login: ~/.nexus/server/accounts/<account> (account.ts)
    cwd: string;
    resume?: string;
    model?: string;
    effort?: EffortLevel;
    verified?: VerifiedLogin;
    startupContextFd?: number;
    startupContext?: string;
    settingSources: SettingSource[];
    claudePath?: string;
    limit?: 'cwd';                      // from the server: the friend may read and write only in --cwd
    writeDirs?: string[];
    keepsOut?: string[];                // from the server, with --limit: more directories it is kept out of (--keep-out)
}

const kUsage = 'the arguments are --cwd <dir>, --resume <session id>, --limit cwd and --keep-out <dir>, with --account-email <email>, --verified-helper <path> and --auth-claude <path> for verified accounts (from the server), and from the drivers file '
    + '--account <name> (required), --model <model>, --effort <' + kEffortLevels.join('|') + '>, --setting-sources <user,project,local> and '
    + '--claude <path of a claude executable>';

export function parseArguments(argv: readonly string[]): Arguments | { error: string } {
    const a: Partial<Arguments> & { settingSources: SettingSource[]; keepsOut: string[] } = { settingSources: [], keepsOut: [] };
    let email, helper, claude;
    for (let i = 0; i < argv.length; i++) {
        const name = argv[i];
        const value = argv[i + 1];
        if (value === undefined || !['--account', '--cwd', '--resume', '--limit', '--write-dir', '--keep-out', '--model', '--effort', '--setting-sources', '--claude', '--startup-context-fd', '--account-email', '--verified-helper', '--auth-claude'].includes(name ?? '')) {
            return { error: `unknown or incomplete argument ${name ?? ''}; ${kUsage}` };
        }
        i++;
        switch (name) {
            case '--startup-context-fd': {
                const fd = Number(value);
                if (!/^\d+$/.test(value) || !Number.isSafeInteger(fd) || fd < 3 || a.startupContextFd !== undefined)
                    return { error: 'expected one inherited startup context descriptor (3 or greater)' };
                a.startupContextFd = fd;
                break;
            }
            case '--account-email':
                if (email !== undefined || !value || Buffer.byteLength(value) > 254 || /[\s\p{Cc}]/u.test(value))
                    return { error: 'expected one configured email for the selected account' };
                email = value;
                break;
            case '--verified-helper':
                if (helper !== undefined || !isAbsolute(value)) return { error: 'expected one absolute verified helper path' };
                helper = value;
                break;
            case '--auth-claude':
                if (claude !== undefined || !isAbsolute(value)) return { error: 'expected one absolute authentication CLI path' };
                claude = value;
                break;
            case '--account': a.account = value; break;
            case '--cwd': a.cwd = value; break;
            case '--resume': a.resume = value; break;
            case '--model': a.model = value; break;
            case '--claude': a.claudePath = value; break;
            case '--limit':
                // The one limit there is: a value this driver does not know is refused, never run unlimited.
                if (value !== 'cwd') {
                    return { error: `--limit ${value} is not a limit this driver knows; the one it knows is --limit cwd` };
                }
                a.limit = value;
                break;
            case '--write-dir':
                if (!isAbsolute(value) || value === '/' || Array.from(value).some(c => c.charCodeAt(0) < 32 || '*?[]'.includes(c))) {
                    return { error: '--write-dir requires an absolute directory without wildcard characters' };
                }
                (a.writeDirs ??= []).push(value);
                break;
            case '--keep-out':
                // One more directory a limited friend is kept out of: absolute and not the root, or refused.
                if (!isAbsolute(value) || value === '/') {
                    return { error: `--keep-out ${value} is not an absolute directory other than /` };
                }
                a.keepsOut.push(value);
                break;
            case '--effort':
                if (!isEffortLevel(value)) {
                    return { error: `--effort ${value} is not one of ${kEffortLevels.join(', ')}` };
                }
                a.effort = value;
                break;
            default: {
                const sources = value === '' ? [] : value.split(',');
                for (const s of sources) {
                    if (!(kSettingSources as readonly string[]).includes(s)) {
                        return { error: `--setting-sources names ${s}; it takes a comma-separated list of ${kSettingSources.join(', ')}` };
                    }
                }
                a.settingSources = sources as SettingSource[];
            }
        }
    }
    if (email !== undefined || helper !== undefined || claude !== undefined) {
        if (email === undefined || helper === undefined || claude === undefined)
            return { error: 'verified Claude accounts require --account-email, --verified-helper and --auth-claude together' };
        a.verified = { email, helper, claude };
    }
    if (a.cwd === undefined || a.cwd === '') {
        return { error: `--cwd <dir> is required; ${kUsage}` };
    }
    if (a.account === undefined || a.account === '') {
        return { error: `--account <name> is required: the account's login is ~/.nexus/server/accounts/<name>; ${kUsage}` };
    }
    if (a.writeDirs?.length && a.limit !== 'cwd') return { error: '--write-dir requires --limit cwd' };
    if (a.keepsOut.length > 0 && a.limit !== 'cwd') {
        return { error: `--keep-out is for a limited friend, and comes with --limit cwd; ${kUsage}` };
    }
    if (a.resume !== undefined && !isSessionId(a.resume)) {
        return { error: `--resume ${a.resume} is not a session id (1 to 256 printable ASCII characters, no spaces)` };
    }
    const { keepsOut, ...rest } = a;
    return { ...rest, cwd: a.cwd, account: a.account, ...(keepsOut.length === 0 ? {} : { keepsOut }) };
}

/** The user messages pushed into the query, one per turn. */
class Prompts implements AsyncIterable<SDKUserMessage> {
    private pending: SDKUserMessage[] = [];
    private wake: (() => void) | undefined;
    private closed = false;
    private readonly onTake: (m: SDKUserMessage) => void;

    /** `onTake` hears each prompt as the query reads it: from then on it cannot be withdrawn. */
    constructor(onTake: (m: SDKUserMessage) => void) {
        this.onTake = onTake;
    }

    /** Queues a prompt for the query; the message returned is the handle withdraw() takes. */
    push(text: string): SDKUserMessage {
        const m: SDKUserMessage = { type: 'user', message: { role: 'user', content: text }, parent_tool_use_id: null };
        this.pending.push(m);
        this.wake?.();
        return m;
    }

    /** Takes back a prompt the query has not read yet; false when it has (it is then on its way to Claude). */
    withdraw(m: SDKUserMessage): boolean {
        const i = this.pending.indexOf(m);
        if (i === -1) {
            return false;
        }
        this.pending.splice(i, 1);
        return true;
    }

    close(): void {
        this.closed = true;
        this.wake?.();
    }

    async *[Symbol.asyncIterator](): AsyncIterator<SDKUserMessage> {
        for (;;) {
            const next = this.pending.shift();
            if (next !== undefined) {
                this.onTake(next);
                yield next;
                continue;
            }
            if (this.closed) {
                return;
            }
            await new Promise<void>((resolve) => { this.wake = resolve; });
            this.wake = undefined;
        }
    }
}

interface Turn {
    turn: number;
    prompt: string;
    changes: Changes;           // the settings it starts with, until applied (back to pending if it never starts)
    message?: SDKUserMessage;   // its prompt as pushed, until the query reads it
    revertTo?: { model: string | undefined };   // the model before this turn changed it, if it did
    applying: boolean;          // its settings are going to the harness, its prompt next
    pushed: boolean;            // its prompt is in the query
    taken: boolean;             // the query has read its prompt
    stopping: boolean;          // a stop was taken for it
    textOpen: boolean;          // a text block is open
    held: string;               // a high surrogate held back from the last delta, to go with the next
    streamed: Set<string>;      // ids of the API messages whose text arrived as stream events
    rejectedLimit?: 'model-limit' | 'usage-limit' | 'harness';
    failure?: Failure;
}

interface Failure {
    cause: string;
    code: ErrorCode;
    fix: string;
    rateLimited?: boolean;      // the SDK tagged the assistant error rate_limit
    model?: boolean;            // the model cannot be used
}

/** The longest text delta written, in UTF-16 units: far inside the line limit whatever the characters. */
const kMaxDelta = 16384;

function isHighSurrogate(c: number): boolean {
    return c >= 0xd800 && c <= 0xdbff;
}

const kFixAgain = 'send the prompt again; if it keeps failing, the driver\'s log (nexus-server\'s standard error) has more';

/** Whether an SDK failure says that the --resume id is not a session it knows. */
export function isSessionNotFound(text: string): boolean {
    return text.includes('No conversation found with session ID');
}

function message(e: unknown): string {
    return e instanceof Error ? e.message : String(e);
}

/** A tool use in one line: what it acts on, from its input. */
export function toolSummary(name: string, input: unknown): string {
    const o = typeof input === 'object' && input !== null ? input as Record<string, unknown> : {};
    if ((name === kSecretToolName || name === kSecretMcpToolName)
        && typeof o['account'] === 'string' && o['account'] !== ''
        && typeof o['what'] === 'string' && o['what'] !== '') {
        const why = typeof o['why'] === 'string' && o['why'] !== '' ? `: ${o['why']}` : '';
        return cut(oneLine(`${o['account']} ${o['what']}${why}`), 200);
    }
    for (const key of ['command', 'file_path', 'notebook_path', 'path', 'pattern', 'url', 'query', 'description', 'prompt']) {
        const v = o[key];
        if (typeof v === 'string' && v !== '') {
            return cut(oneLine(v), 200);
        }
    }
    const json = JSON.stringify(input) as string | undefined;
    return cut(oneLine(json === undefined || json === '{}' ? name : json), 200);
}

export const kSecretToolName = 'nexus_ask_secret';
export const kSecretMcpToolName = `mcp__nexus__${kSecretToolName}`;
/** Past the server's 10-minute dialog deadline (docs/secrets.md), so the server's timeout answers first. */
export const kSecretToolTimeoutMs = 11 * 60 * 1000;

/** The in-process SDK MCP server that gives Claude the nexus_ask_secret(account, what, why) tool (docs/secrets.md). */
export function secretMcpServer(askSecret: HarnessStart['askSecret']): McpSdkServerConfigWithInstance {
    return createSdkMcpServer({
        name: 'nexus',
        version: '0.1.0',
        alwaysLoad: true,
        timeout: kSecretToolTimeoutMs,
        tools: [
            tool(
                kSecretToolName,
                'Ask the person, in the nexus window\'s secret dialog, to paste a secret for an account this friend is '
                    + 'linked to (for a claude account, what is "oauth-token"). Writes ask-secret and waits for the '
                    + 'person\'s answer, returning "stored" once the file is saved or "refused: <reason>" ("no-client", '
                    + '"busy", "cancelled", "timeout" or "malformed"); the secret value never crosses this tool.',
                {
                    account: z.string().describe('The account name in accounts.json (1 to 64 letters, digits, -, _ or .)'),
                    what: z.string().describe('The secret file the account\'s provider documents (for claude, "oauth-token")'),
                    why: z.string().describe('One short line shown in the dialog saying why the secret is needed (at most 200 UTF-8 bytes)'),
                },
                async ({ account, what, why }) => ({
                    content: [{ type: 'text' as const, text: await askSecret(account, what, why) }],
                }),
            ),
        ],
    });
}

export class Driver {
    private readonly children = new Map<string, string | undefined>();
    private readonly host: Host;
    private readonly args: Arguments;
    private readonly reader = new LineReader();
    private harness: Harness | undefined;
    private prompts = new Prompts(() => undefined);
    private generation = 0;
    private replaced: Promise<void> = Promise.resolve();   // settles when the current harness is replaced
    private replace: () => void = () => undefined;
    private ready = false;                       // the credential check passed
    private starting: Promise<void> = Promise.resolve();   // the current harness's credential check
    private ended = false;                       // exiting: nothing more is written
    private chain: Promise<void> = Promise.resolve();
    private lastTurn = 0;
    private open: Turn | undefined;
    private pending: Changes = {};
    private inUse: Changes = {};                 // the settings the harness runs with, beyond the start arguments
    private interrupting = 0;                    // the turn whose interrupt has not answered yet
    private session = '';
    private cost: number | undefined;
    private plan = '';
    private sawServer = false;                   // a server line arrived
    private secrets = false;                     // the server line named "secrets"
    private asks = 0;                            // the last ask-secret number minted
    private asking: { ask: number; resolve: (answer: string) => void } | undefined;

    constructor(host: Host, args: Arguments) {
        this.host = host;
        this.args = args;
    }

    // ---- The process's life ----

    /** Writes the hello and starts the harness; nothing reaches it before the credential check passes. */
    start(): void {
        this.write({ kind: 'hello', protocol: kProtocolVersion, harness: 'claude', features: ['limit'] });
        if (this.args.verified) {
            this.starting = this.refreshCredential().then(() => {
                if (!this.ended) this.launch(this.args.resume);
            });
        } else {
            this.launch(this.args.resume);
        }
        this.record('init');
    }

    /** Bytes from standard input. */
    feed(chunk: Uint8Array): void {
        for (const item of this.reader.feed(chunk)) {
            if (this.ended) {
                return;
            }
            if ('error' in item) {
                this.refuseLine(item.error);
            } else {
                this.line(item.line);
            }
        }
    }

    /** Standard input ended: stop the harness and exit, owing no `done`. */
    end(): void {
        if (this.ended) {
            return;
        }
        for (const item of this.reader.finish()) {
            this.host.log(`nexus-claude-driver: ${item.error}`);
        }
        this.quit(0);
        this.record('end');
    }

    /** One step of models/ClaudeDriver.tla, for the trace check. */
    private record(op: string, args: StepArgs = {}): void {
        if (this.host.step !== undefined) {
            this.host.step(op, args, this.state());
        }
    }

    private state(): DriverState {
        const t = this.open;
        let stage: DriverState['stage'] = 'none';
        if (t !== undefined) {
            stage = t.taken ? 'taken' : t.pushed ? 'pushed' : t.applying ? 'applying' : 'waiting';
        }
        return {
            phase: this.ended ? 'ended' : this.ready ? 'ready' : 'checking',
            resume: this.args.resume === undefined ? 'none' : 'given',
            open: t?.turn ?? 0,
            stage,
            stopping: t?.stopping ?? false,
            textOpen: t?.textOpen ?? false,
            failure: traceFailure(t?.failure),
            interrupting: this.interrupting,
            pending: { ...this.pending },
            carry: { ...(t?.changes ?? {}) },
            inUse: { ...this.inUse },
            last: this.lastTurn,
            secrets: this.secrets,
            asking: this.asking?.ask ?? 0,
            asks: this.asks,
        };
    }

    private quit(code: number): void {
        this.ended = true;
        this.cancelAsk();
        this.prompts.close();
        this.harness?.close();
        this.host.exit(code);
    }

    private cancelAsk(): void {
        const open = this.asking;
        if (open !== undefined) {
            this.asking = undefined;
            open.resolve('refused: cancelled');
        }
    }

    private write(m: DriverMessage): void {
        if (!this.ended) {
            this.host.write(encode(m));
        }
    }

    private error(cause: string, fix: string, code: ErrorCode, turn?: number): void {
        const m: DriverMessage = { kind: 'error', cause: cut(whole(cause), 4000), fix, code };
        this.write(turn === undefined ? m : { ...m, turn });
    }

    /** Runs `op` after every operation before it. */
    private enqueue(op: () => Promise<void>): void {
        this.chain = this.chain.then(op).catch((e: unknown) => {
            this.fail(`the driver failed: ${message(e)}`);
        });
    }

    // ---- The harness ----

    // Keep the durable refresh grant outside the SDK. A changed access token requires a new SDK process,
    // since the running one already consumed its credential descriptor.
    private async refreshCredential(): Promise<boolean> {
        const result = await this.host.refreshCredential?.().catch(() => 'unavailable' as const);
        if (this.ended) return false;
        if (result === 'changed' || result === 'same') return result === 'changed';
        if (result === 'sign-in') {
            this.refuseCredential('The selected Claude login is missing or its verified email does not match.', tokenFix(this.args));
        } else {
            this.error('The verified Claude login could not be checked or refreshed.', 'Check the credential helper and provider connection, then retry.', 'harness', this.open?.turn);
            if (this.open) this.write({ kind: 'done', turn: this.open.turn, outcome: 'error' });
            this.quit(1);
        }
        return false;
    }

    private launch(resume: string | undefined): void {
        if (this.children.size) {
            this.children.clear();
            this.write({kind: 'child-agents', count: 0});
        }
        const generation = ++this.generation;
        this.ready = false;
        this.prompts = new Prompts((m) => { this.took(m); });
        this.replaced = new Promise<void>((resolve) => { this.replace = resolve; });
        const start: HarnessStart = { prompts: this.prompts, cwd: this.args.cwd, settingSources: this.args.settingSources,
            limited: this.args.limit === 'cwd', ...(this.args.writeDirs ? {writeDirs: this.args.writeDirs} : {}), keepsOut: [...(this.args.keepsOut ?? [])],
            askSecret: (account, what, why) => this.askSecret(generation, account, what, why) };
        if (this.args.startupContext !== undefined) start.startupContext = this.args.startupContext;
        if (resume === undefined) {
            start.sessionId = this.host.newSessionId();
        } else {
            start.resume = resume;
        }
        // A harness started again (a --resume id Claude does not know) runs with the settings in use, not the start's.
        const model = this.inUse.model ?? this.args.model;
        if (model !== undefined && model !== '') {
            start.model = model;
        }
        const effort = this.inUse.effort ?? this.args.effort;
        if (effort !== undefined) {
            start.effort = effort;
        }
        if (this.args.claudePath !== undefined) {
            start.claudePath = this.args.claudePath;
        }
        let harness: Harness;
        try {
            harness = this.host.startHarness(start);
        } catch (e) {
            this.fail(`the Claude harness did not start: ${message(e)}`);
            return;
        }
        this.harness = harness;
        this.starting = this.check(harness, generation, start.sessionId ?? start.resume ?? '');
        void this.pump(harness, generation);
    }

    /** The credential check, before any prompt: a first-party subscription login and no API key, or a refusal. */
    private async check(harness: Harness, generation: number, sessionId: string): Promise<void> {
        let info: AccountInfo;
        try {
            if (harness.catalogReady) await harness.catalogReady;
            info = await harness.accountInfo();
        } catch (e) {
            if (generation !== this.generation || this.ended) {
                return;
            }
            const why = message(e);
            if (this.args.resume !== undefined && sessionId === this.args.resume && isSessionNotFound(why)) {
                this.notFound(harness);
                this.record('lost');
                return;
            }
            this.fail(`the Claude harness did not start: ${why}`);
            return;
        }
        if (generation !== this.generation || this.ended) {
            return;
        }
        const provider = info.apiProvider ?? 'firstParty';
        const keySource = info.apiKeySource ?? 'none';
        const plan = info.subscriptionType ?? '';
        const token = info.tokenSource ?? '';
        let billing: Billing;
        // An auth token (ANTHROPIC_AUTH_TOKEN) bills the API; the account's OAuth token (CLAUDE_CODE_OAUTH_TOKEN) does not.
        const apiToken = /api.?key|helper/i.test(token) || (/auth.?token/i.test(token) && !/oauth/i.test(token));
        if (provider !== 'firstParty' || keySource !== 'none' || apiToken) {
            billing = 'api-key';
        } else if (plan !== '') {
            billing = 'subscription';
        } else if (/oauth/i.test(token)) {
            // The account's token from `claude setup-token` (account.ts) is a subscription's, and Claude may not name
            // the plan for it; but Claude must say it is using that token, or the session is refused.
            billing = 'subscription';
        } else {
            billing = 'none';
        }
        if (this.args.verified && billing === 'subscription' && token !== 'CLAUDE_CODE_OAUTH_TOKEN_FILE_DESCRIPTOR') {
            this.refuseCredential('Claude did not use the verified account credential supplied on its descriptor.', tokenFix(this.args));
            return;
        }
        this.plan = plan;
        this.write(plan === '' ? { kind: 'credential', billing } : { kind: 'credential', billing, plan: whole(plan) });
        if (billing === 'api-key') {
            this.refuseCredential(`Claude would bill ${provider === 'firstParty' ? `an API key or auth token (apiKeySource ${keySource}${token === '' ? '' : `, tokenSource ${token}`})` : `the ${provider} provider`}, not a subscription login; nexus runs Claude only on a subscription`,
                `remove the API key or provider setting Claude finds (an apiKeyHelper in the account's settings, or a provider in managed settings), then ${tokenFix(this.args)}`);
            this.record('refused');
            return;
        }
        if (billing === 'none') {
            this.refuseCredential('Claude has no subscription login: the account reports no subscription',
                tokenFix(this.args));
            this.record('refused');
            return;
        }
        this.ready = true;
        this.reportSession(sessionId);
        this.record('checked');
    }

    /**
     * The --resume id is not a session Claude knows, found at the start or once the first turn began: go on in a new
     * session (sequence rule 10). A prompt the old query took never ran; it goes into the new one, after the new
     * session's credential check, with the settings in use. A turn being stopped ends: nothing runs any more. An
     * interrupt the old query had not answered goes with it.
     */
    private notFound(old: Harness): void {
        old.close();
        this.cancelAsk();
        const id = this.args.resume ?? '';
        const t = this.open;
        if (t !== undefined) {
            this.closeText(t);
        }
        this.error(`Claude does not know the session ${id} in ${this.args.cwd}; the conversation goes on in a new session, without the earlier history`,
            'nothing to do: the new session is reported next', 'session-not-found', t?.turn);
        delete this.args.resume;
        this.interrupting = 0;
        this.replace();
        this.launch(undefined);
        if (t === undefined) {
            return;
        }
        delete t.failure;
        if (t.stopping) {
            this.finish(t, 'stopped');
            return;
        }
        const begun = t.pushed;
        t.applying = false;
        t.pushed = false;
        t.taken = false;
        delete t.message;
        t.streamed = new Set();
        if (begun) {
            this.enqueue(() => this.begin(t));
        }
    }

    /** Whether `why` is Claude not knowing the --resume id, and if so, goes on in a new session. */
    private resumeLost(why: string): boolean {
        if (this.args.resume === undefined || !isSessionNotFound(why) || this.harness === undefined) {
            return false;
        }
        this.notFound(this.harness);
        return true;
    }

    private refuseCredential(cause: string, fix: string): void {
        this.cancelAsk();
        const turn = this.open?.turn;
        if (this.open !== undefined) {
            this.closeText(this.open);
        }
        this.error(cause, fix, 'no-credential', turn);
        if (turn !== undefined) {
            this.write({ kind: 'done', turn, outcome: 'error' });
            this.open = undefined;
        }
        this.quit(1);
    }

    /** The harness failed in a way the driver cannot go on from: the open turn is left for the server to cut. */
    private fail(cause: string): void {
        if (this.ended) {
            return;
        }
        this.error(cause, 'send the prompt again: nexus-server starts the driver again and resumes the session', 'harness', this.open?.turn);
        this.quit(1);
        this.record('failed');
    }

    private reportSession(id: string): void {
        if (id === '' || id === this.session || !isSessionId(id)) {
            return;
        }
        this.session = id;
        this.write({ kind: 'session', id });
    }

    private async pump(harness: Harness, generation: number): Promise<void> {
        let why = 'the Claude harness ended its stream';
        try {
            for await (const m of harness) {
                if (generation !== this.generation || this.ended) {
                    return;
                }
                this.take(m);
            }
        } catch (e) {
            why = `the Claude harness failed: ${message(e)}`;
        }
        // A failure during the credential check is that check's to report (a --resume id Claude does not know ends
        // the stream and fails accountInfo() alike); after it, the harness is gone.
        await this.starting;
        if (generation !== this.generation || this.ended) {
            return;
        }
        if (this.resumeLost(why)) {
            this.record('lost');
        } else {
            this.fail(why);
        }
    }

    /** The query read a prompt: the open turn's, unless that was withdrawn or belongs to a harness replaced since. */
    private took(m: SDKUserMessage): void {
        const t = this.open;
        if (t?.message !== m || this.ended) {
            return;
        }
        t.taken = true;
        delete t.message;
        this.record('take');
    }

    // ---- From the server ----

    private line(bytes: Uint8Array): void {
        const d = decodeServerLine(bytes);
        if ('error' in d) {
            this.refuseLine(d.error, d.turn);
            return;
        }
        const m = d.message;
        switch (m.kind) {
            case 'send':
                if (this.open !== undefined) {
                    this.refuse(`send of turn ${m.turn} arrived while turn ${this.open.turn} is open`, m.turn);
                } else if (m.turn <= this.lastTurn) {
                    this.refuse(`send of turn ${m.turn} does not follow turn ${this.lastTurn}`, m.turn);
                } else {
                    this.send(m.turn, m.prompt);
                }
                break;
            case 'stop':
                this.stop(m.turn);
                this.record('stop', { turn: m.turn });
                break;
            case 'settings':
                this.settings(m);
                break;
            case 'server':
                if (this.sawServer) {
                    this.refuse('a second server message arrived; the server writes its features once, before any send');
                } else if (this.lastTurn > 0) {
                    this.refuse(`a server message arrived after turn ${this.lastTurn}'s send; it comes before any send`);
                } else {
                    this.sawServer = true;
                    if (m.features.includes('secrets')) {
                        this.secrets = true;
                        this.record('server');
                    }
                }
                break;
            case 'secret':
                this.secret(m);
                break;
            case 'unknown':
                break;
        }
    }

    private refuseLine(why: string, turn?: number): void {
        this.refuse(`the server sent a line the driver refused: ${why}`, turn);
    }

    /** A line from the server the driver cannot accept: one protocol error, then the end (sequence rule 4). */
    private refuse(cause: string, turn?: number): void {
        this.error(cause, 'the server must keep docs/driver-protocol.md: one message per line, turns in order, each after the last one\'s done', 'protocol', turn);
        this.quit(3);
    }

    /** The settings received so far go with this turn: a stop before it starts gives them back (finish). */
    private send(turn: number, prompt: string): void {
        this.lastTurn = turn;
        const t: Turn = {
            turn, prompt, changes: this.pending, applying: false, pushed: false, taken: false, stopping: false,
            textOpen: false, held: '', streamed: new Set(),
        };
        this.open = t;
        this.pending = {};
        this.record('send', { turn });
        this.enqueue(() => this.begin(t));
    }

    /**
     * A turn's start, after every operation before it (an unanswered interrupt among them): its settings, then its
     * prompt. A stop or the end can come during each await; a --resume id Claude does not know, during the settings,
     * replaces the harness, and the start begins again on the new one once its credential check passes.
     */
    private async begin(t: Turn): Promise<void> {
        const waiting = (): boolean => this.open === t && !t.stopping && !t.pushed && !this.ended;
        if (this.args.verified) {
            await this.started();
            if (!waiting() || !this.ready) return;
            const changed = await this.refreshCredential();
            if (!waiting()) return;
            if (changed) {
                this.prompts.close();
                this.harness?.close();
                this.replace();
                this.args.resume = this.session;
                this.launch(this.session);
            }
        }
        for (;;) {
            await this.started();
            if (!waiting() || !this.ready) {
                return;
            }
            const generation = this.generation;
            const changes = t.changes;
            const before = { ...this.inUse };
            t.changes = {};
            t.applying = true;
            if (changes.model !== undefined) {
                this.inUse.model = changes.model;
            }
            if (changes.effort !== undefined) {
                this.inUse.effort = changes.effort;
            }
            this.record('apply');
            await this.apply(changes, before, t);
            if (!waiting()) {
                return;
            }
            if (this.replacedSince(t, generation)) {
                continue;
            }
            t.applying = false;
            t.message = this.prompts.push(t.prompt);
            t.pushed = true;
            this.record('push');
            return;
        }
    }

    /** The model in use goes back to `model` (absent: the start's). */
    private modelBack(model: string | undefined): void {
        if (model === undefined) {
            delete this.inUse.model;
        } else {
            this.inUse.model = model;
        }
    }

    /** Whether the harness was replaced (a --resume id lost) while turn `t`'s settings went to it. */
    private replacedSince(t: Turn, generation: number): boolean {
        return !t.applying || generation !== this.generation || !this.ready;
    }

    /** Changes a turn did not get to apply go back to wait for the next turn, under any newer ones. */
    private restore(changes: Changes): void {
        if (changes.model !== undefined && this.pending.model === undefined) {
            this.pending.model = changes.model;
        }
        if (changes.effort !== undefined && this.pending.effort === undefined) {
            this.pending.effort = changes.effort;
        }
    }

    /** Waits for the credential check, and for the next one when a --resume id Claude does not know starts another. */
    private async started(): Promise<void> {
        for (;;) {
            const s = this.starting;
            await s;
            if (this.starting === s) {
                return;
            }
        }
    }

    /** A stop ends what runs when it arrives, or nothing: it is never kept for a later turn. */
    private stop(turn: number): void {
        const t = this.open;
        if (t?.turn !== turn || t.stopping) {
            return;
        }
        t.stopping = true;
        this.cancelAsk();
        if (!t.pushed || (t.message !== undefined && this.prompts.withdraw(t.message))) {
            // The prompt never reached the query: nothing runs, nothing to interrupt.
            this.finish(t, 'stopped');
            return;
        }
        const harness = this.harness;
        if (harness === undefined) {
            return;
        }
        this.interrupting = turn;
        const generation = this.generation;
        const gone = this.replaced;
        const answered = harness.interrupt();
        // The next turn's start waits behind this, so the interrupt cannot land in it.
        this.enqueue(async () => {
            try {
                await Promise.race([answered, gone]);
            } catch (e) {
                if (generation === this.generation && this.interrupting === turn) {
                    this.fail(`Claude did not stop the turn: ${message(e)}`);
                }
                return;
            }
            if (generation === this.generation && this.interrupting === turn && !this.ended) {
                this.interrupting = 0;
                this.record('answered');
            }
        });
    }

    private settings(m: Settings): void {
        const changes: Changes = {};
        if (m.model !== undefined) {
            changes.model = m.model;
            this.pending.model = m.model;
        }
        if (m.effort !== undefined) {
            if (isEffortLevel(m.effort)) {
                changes.effort = m.effort;
                this.pending.effort = m.effort;
            } else {
                this.error(`effort ${m.effort} is not a level Claude has; the effort stays as it was`,
                    `choose one of ${kEffortLevels.join(', ')}`, 'other');
            }
        }
        this.record('settings', { changes });
    }

    /**
     * Model and effort changes, before the prompt of the turn they apply from; `inUse` already holds them, and a value
     * Claude refuses goes back to the one before.
     */
    private async apply(changes: Changes, before: Changes, t: Turn): Promise<void> {
        const harness = this.harness;
        if (harness === undefined) {
            return;
        }
        if (changes.model !== undefined) {
            try {
                await harness.setModel(changes.model === '' ? undefined : changes.model);
                t.revertTo = { model: before.model };
            } catch (e) {
                this.modelBack(before.model);
                this.error(`Claude did not take the model ${changes.model}: ${message(e)}; the model stays as it was`,
                    'choose a model Claude has', 'harness');
            }
        }
        if (changes.effort !== undefined) {
            try {
                await harness.applyFlagSettings({ effortLevel: changes.effort });
            } catch (e) {
                if (before.effort === undefined) {
                    delete this.inUse.effort;
                } else {
                    this.inUse.effort = before.effort;
                }
                this.error(`Claude did not take the effort ${changes.effort}: ${message(e)}; the effort stays as it was`,
                    `choose one of ${kEffortLevels.join(', ')}`, 'harness');
            }
        }
    }

    // ---- From the harness ----

    private take(m: SDKMessage): void {
        if (this.ready && 'session_id' in m && typeof m.session_id === 'string') {
            this.reportSession(m.session_id);
        }
        switch (m.type) {
            case 'system': {
                const before = this.children.size;
                if (m.subtype === 'task_started' && (m.task_type === 'local_agent' || m.subagent_type || m.spawn_depth)) {
                    this.children.set(m.task_id, m.run_id);
                } else if (m.subtype === 'task_notification' && (!m.run_id || this.children.get(m.task_id) === m.run_id)) {
                    this.children.delete(m.task_id);
                }
                if (this.children.size !== before) this.write({kind: 'child-agents', count: this.children.size});
                if (m.subtype === 'init' && m.apiKeySource !== 'none') {
                    this.write({ kind: 'credential', billing: 'api-key', ...(this.plan === '' ? {} : { plan: this.plan }) });
                    this.refuseCredential(`Claude started a turn billing an API key (apiKeySource ${m.apiKeySource}), not the subscription login; nexus runs Claude only on a subscription`,
                        `remove the API key Claude finds (an apiKeyHelper in the account's settings), then ${tokenFix(this.args)}`);
                    this.record('refused');
                }
                return;
            }
            case 'rate_limit_event': {
                const info = m.rate_limit_info;
                const turn = this.current();
                if (turn && info.status === 'rejected') {
                    // Only turn failures consume this observation. Service throttling and quota warnings do not move accounts.
                    const window: string = info.rateLimitType ?? '';
                    if (window === 'seven_day_fable') turn.rejectedLimit = 'model-limit';
                    else if (window === 'five_hour' || window === 'seven_day') turn.rejectedLimit = 'usage-limit';
                    else if (window) turn.rejectedLimit = 'harness';
                }
                if (info.isUsingOverage === true || info.overageInUse === true) {
                    this.refuseCredential('Claude began drawing on paid extra usage (overage) beyond the subscription; nexus runs Claude only on the subscription\'s included usage',
                        'turn extra usage off for this account in its Claude settings, or wait until the subscription\'s limit resets');
                    this.record('refused');
                }
                return;
            }
            case 'stream_event':
                if (m.parent_tool_use_id === null) {
                    this.streamEvent(m.event);
                }
                return;
            case 'assistant':
                if (m.parent_tool_use_id === null) {
                    this.assistant(m);
                }
                return;
            case 'result':
                this.result(m);
                return;
            default:
                return;
        }
    }

    /** The open turn, if its prompt is in the query; messages for no such turn (a turn's trailers) are dropped. */
    private current(): Turn | undefined {
        const t = this.open;
        return t?.pushed === true ? t : undefined;
    }

    private streamEvent(e: Extract<SDKMessage, { type: 'stream_event' }>['event']): void {
        const t = this.current();
        if (t === undefined) {
            return;
        }
        if (e.type === 'message_start') {
            this.messageId = e.message.id;
        } else if (e.type === 'content_block_delta' && e.delta.type === 'text_delta') {
            t.streamed.add(this.messageId);
            if (this.text(t, e.delta.text)) {
                this.record('delta');
            }
        } else if (e.type === 'content_block_stop' || e.type === 'message_stop') {
            this.closeText(t);
            this.record('block-end');
        }
    }

    private messageId = '';

    private assistant(m: SDKAssistantMessage): void {
        const t = this.current();
        if (t === undefined) {
            return;
        }
        if (m.error !== undefined) {
            const said = m.message.content.flatMap((b) => (b.type === 'text' ? [b.text] : [])).join(' ');
            t.failure = failure(m.error, said, this.args);
            this.record('assistant-error', { code: traceFailure(t.failure) });
            return;
        }
        for (const b of m.message.content) {
            if (b.type === 'text' && !t.streamed.has(m.message.id)) {
                this.text(t, b.text);
                this.closeText(t);
                this.record('block-text');
            } else if (b.type === 'tool_use' || b.type === 'server_tool_use' || b.type === 'mcp_tool_use') {
                this.closeText(t);
                const name = cut(oneLine(b.name), 200) || 'tool';
                this.write({ kind: 'tool', turn: t.turn, name, summary: toolSummary(b.name, b.input) });
                this.record('tool');
            }
        }
    }

    /**
     * A piece of text: whole characters only, a high surrogate at its end held for the next piece. Whether anything
     * was written.
     */
    private text(t: Turn, delta: string): boolean {
        let s = t.held + delta;
        t.held = '';
        if (isHighSurrogate(s.charCodeAt(s.length - 1))) {
            t.held = s.slice(-1);
            s = s.slice(0, -1);
        }
        const wrote = s !== '';
        while (s !== '') {
            let n = Math.min(s.length, kMaxDelta);
            if (n < s.length && isHighSurrogate(s.charCodeAt(n - 1))) {
                n--;
            }
            t.textOpen = true;
            this.write({ kind: 'text', turn: t.turn, delta: whole(s.slice(0, n)) });
            s = s.slice(n);
        }
        return wrote;
    }

    private closeText(t: Turn): void {
        if (t.held !== '') {
            const held = t.held;
            t.held = '';
            t.textOpen = true;
            this.write({ kind: 'text', turn: t.turn, delta: whole(held) });
        }
        if (t.textOpen) {
            t.textOpen = false;
            this.write({ kind: 'text-end', turn: t.turn });
        }
    }

    private result(r: SDKResultMessage): void {
        const t = this.current();
        if (t === undefined) {
            this.host.log(`nexus-claude-driver: a result (${r.subtype}) with no turn running, dropped`);
            return;
        }
        if (Number.isFinite(r.total_cost_usd) && r.total_cost_usd > 0) {
            this.cost = r.total_cost_usd;
        }
        const ok = r.subtype === 'success' && !r.is_error;
        const said = (r.subtype === 'success' ? [r.result] : r.errors).join('; ');
        this.resultOf(t, r, ok, said);
        this.record('result', { kind: ok ? 'ok' : isSessionNotFound(said) ? 'lost' : 'error',
            rejectedLimit: t.rejectedLimit });
    }

    private resultOf(t: Turn, r: SDKResultMessage, ok: boolean, said: string): void {
        if (t.stopping) {
            this.finish(t, 'stopped', r);
            return;
        }
        if (ok && t.failure === undefined) {
            this.finish(t, 'ok', r);
            return;
        }
        if (t.failure === undefined && this.resumeLost(said)) {
            return;
        }
        const f: Failure = t.failure ?? {
            cause: `Claude ended the turn with an error (${r.subtype}): ${said || 'no cause given'}`,
            code: 'harness' as const,
            fix: kFixAgain,
        };
        if (t.rejectedLimit && (!t.failure || f.rateLimited) && f.code !== 'model-limit') {
            f.code = t.rejectedLimit;
        }
        if (t.failure?.model === true && t.revertTo !== undefined) {
            // The model this turn changed to cannot run: the one before comes back from the next turn.
            const back = t.revertTo.model ?? this.args.model ?? '';
            this.restore({ model: back });
            this.modelBack(t.revertTo.model);
            f.cause += `; the model goes back to ${back === '' ? 'Claude\'s default' : back} from the next turn`;
        }
        this.closeText(t);
        if (f.code === 'no-credential') {
            this.refuseCredential(f.cause, f.fix);
            return;
        }
        this.error(f.cause, f.fix, f.code, t.turn);
        this.finish(t, 'error', r);
    }

    private secret(m: SecretAnswer): void {
        const open = this.asking;
        if (open?.ask === m.ask) {
            this.asking = undefined;
            open.resolve(m.outcome === 'stored' ? 'stored' : `refused: ${m.reason ?? 'unknown'}`);
        }
        this.record('secret', { ask: m.ask });
    }

    /**
     * The AI's nexus_ask_secret(account, what, why) tool (docs/secrets.md). Checked locally first so a malformed AI
     * argument never writes a line the server refuses; then gated on an active turn and a server offering "secrets".
     */
    private askSecret(generation: number, account: string, what: string, why: string): Promise<string> {
        if (!isAccountName(account)) {
            return Promise.resolve('refused: malformed (account must be 1 to 64 letters, digits, -, _ or ., not starting with . or -)');
        }
        if (!isAccountName(what)) {
            return Promise.resolve('refused: malformed (what must be a secret file name of 1 to 64 letters, digits, -, _ or ., not starting with . or -)');
        }
        if (!isSecretWhy(why)) {
            return Promise.resolve('refused: malformed (why must be one line of at most 200 UTF-8 bytes with no control, bidirectional or zero-width characters)');
        }
        const t = this.current();
        if (this.ended || !this.ready || generation !== this.generation || t === undefined) {
            return Promise.resolve('refused: cancelled');
        }
        if (t.stopping) {
            this.record('ask-refused');
            return Promise.resolve('refused: cancelled');
        }
        if (!this.secrets) {
            this.record('ask-refused');
            return Promise.resolve('refused: no-client');
        }
        if (this.asking !== undefined) {
            this.record('ask-refused');
            return Promise.resolve('refused: busy');
        }
        const ask = ++this.asks;
        return new Promise<string>((resolve) => {
            this.asking = { ask, resolve };
            this.write({ kind: 'ask-secret', ask, account, what, why });
            this.record('ask');
        });
    }

    private finish(t: Turn, outcome: 'ok' | 'stopped' | 'error', r?: SDKResultMessage): void {
        this.cancelAsk();
        this.closeText(t);
        this.restore(t.changes);
        t.changes = {};
        this.open = undefined;
        const done: DriverMessage = { kind: 'done', turn: t.turn, outcome };
        if (this.cost !== undefined) {
            done.session_cost_usd = this.cost;
        }
        if (r !== undefined) {
            const u = r.usage;
            const usage: Usage = {};
            const put = (key: keyof Usage, v: unknown): void => {
                if (typeof v === 'number' && Number.isSafeInteger(v) && v >= 0) {
                    usage[key] = v;
                }
            };
            put('input_tokens', u.input_tokens);
            put('output_tokens', u.output_tokens);
            put('cache_read_tokens', u.cache_read_input_tokens);
            put('cache_write_tokens', u.cache_creation_input_tokens);
            if (Object.keys(usage).length > 0) {
                done.usage = usage;
            }
        }
        this.write(done);
    }
}

/** How to give the account a new subscription login (account.ts). */
function tokenFix(args: Arguments): string {
    if (args.verified) return `Sign in again to ${args.account} in Nexus with ${args.verified.email}.`;
    const account = args.account;
    const dir = `~/.nexus/server/accounts/${account}`;
    return `make a new token with \`CLAUDE_CONFIG_DIR=${dir} claude setup-token\` on the subscription account, and save it, `
        + `alone on one line, to ${dir}/${kTokenFile} with mode 0600`;
}

function traceFailure(f: Failure | undefined): DriverState['failure'] {
    if (!f) return 'none';
    if (f.code === 'usage-limit' || f.code === 'model-limit' || f.code === 'no-credential') return f.code;
    return f.rateLimited ? 'rate-limit' : 'harness';
}

/** The failure an assistant message's error code names. */
function failure(code: NonNullable<SDKAssistantMessage['error']>, said: string, args: Arguments): Failure {
    const detail = said === '' ? code : `${code}: ${said}`;
    switch (code) {
        case 'authentication_failed':
        case 'oauth_org_not_allowed':
        case 'account_on_hold':
        case 'verification_required':
        case 'billing_error':
            return { cause: `Claude's login cannot be used (${detail})`, code: 'no-credential',
                fix: tokenFix(args) };
        case 'rate_limit': {
            // Older SDKs only give the scope in the provider's typed error message.
            const fable = /\bfable(?:['’]s)?(?: usage| weekly| credit)? limits?\b|\bout of fable\b|\blimit (?:reached )?for (?:claude[- ])?fable\b/i.test(said);
            const accountQuota = /\b(?:weekly|session|subscription|five[- ]hour|5[- ]hour|usage) limit\b|\bout of (?:subscription |usage )?credits\b/i.test(said);
            return { cause: `Claude reported a rate limit (${detail})`, rateLimited: true,
                code: fable ? 'model-limit' : accountQuota ? 'usage-limit' : 'harness',
                fix: 'check the reported limit and reset time' };
        }
        case 'model_not_found':
            return { cause: `Claude has no such model (${detail})`, code: 'harness', fix: 'choose a model Claude has', model: true };
        default:
            return { cause: `Claude failed the turn (${detail})`, code: 'harness', fix: kFixAgain };
    }
}

/** A fresh session id. */
export function newSessionId(): string {
    return randomUUID();
}
