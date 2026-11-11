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

import { randomUUID } from 'node:crypto';

import type {
    AccountInfo,
    EffortLevel,
    Query,
    SDKAssistantMessage,
    SDKMessage,
    SDKResultMessage,
    SDKUserMessage,
    SettingSource,
} from '@anthropic-ai/claude-agent-sdk';

import {
    cut,
    decodeServerLine,
    encode,
    isSessionId,
    kProtocolVersion,
    LineReader,
    oneLine,
    whole,
    type Billing,
    type DriverMessage,
    type ErrorCode,
    type Settings,
    type Usage,
} from './protocol.js';

/** What the driver uses of the SDK's Query. */
export type Harness = Pick<Query, 'interrupt' | 'setModel' | 'applyFlagSettings' | 'accountInfo' | 'close'> &
    AsyncIterable<SDKMessage>;

/** How a harness is started: a new session (`sessionId`) or a resumed one (`resume`). */
export interface HarnessStart {
    prompts: AsyncIterable<SDKUserMessage>;
    cwd: string;
    sessionId?: string;
    resume?: string;
    model?: string;
    effort?: EffortLevel;
    settingSources: SettingSource[];
    claudePath?: string;
}

/** The world outside the driver. */
export interface Host {
    write(line: string): void;          // one protocol line to standard output
    log(text: string): void;            // a line to standard error
    exit(code: number): void;           // end the process (after what was written reaches the server)
    startHarness(start: HarnessStart): Harness;
    newSessionId(): string;
    oauthToken: boolean;                // the environment carries CLAUDE_CODE_OAUTH_TOKEN (an account's login)
}

export const kEffortLevels: readonly EffortLevel[] = ['low', 'medium', 'high', 'xhigh', 'max'];
const kSettingSources: readonly SettingSource[] = ['user', 'project', 'local'];

export function isEffortLevel(s: string): s is EffortLevel {
    return (kEffortLevels as readonly string[]).includes(s);
}

/** The driver's arguments, or why they cannot be used. */
export interface Arguments {
    cwd: string;
    resume?: string;
    model?: string;
    effort?: EffortLevel;
    settingSources: SettingSource[];
    claudePath?: string;
}

const kUsage = 'the arguments are --cwd <dir> and --resume <session id> (from the server), and from the drivers file '
    + '--model <model>, --effort <' + kEffortLevels.join('|') + '>, --setting-sources <user,project,local> and '
    + '--claude <path of a claude executable>';

export function parseArguments(argv: readonly string[]): Arguments | { error: string } {
    const a: Partial<Arguments> & { settingSources: SettingSource[] } = { settingSources: [] };
    for (let i = 0; i < argv.length; i++) {
        const name = argv[i];
        const value = argv[i + 1];
        if (value === undefined || !['--cwd', '--resume', '--model', '--effort', '--setting-sources', '--claude'].includes(name ?? '')) {
            return { error: `unknown or incomplete argument ${name ?? ''}; ${kUsage}` };
        }
        i++;
        switch (name) {
            case '--cwd': a.cwd = value; break;
            case '--resume': a.resume = value; break;
            case '--model': a.model = value; break;
            case '--claude': a.claudePath = value; break;
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
    if (a.cwd === undefined || a.cwd === '') {
        return { error: `--cwd <dir> is required; ${kUsage}` };
    }
    if (a.resume !== undefined && !isSessionId(a.resume)) {
        return { error: `--resume ${a.resume} is not a session id (1 to 256 printable ASCII characters, no spaces)` };
    }
    return { ...a, cwd: a.cwd };
}

/** The user messages pushed into the query, one per turn. */
class Prompts implements AsyncIterable<SDKUserMessage> {
    private pending: SDKUserMessage[] = [];
    private wake: (() => void) | undefined;
    private closed = false;

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
    message?: SDKUserMessage;   // its prompt as pushed, until the query reads it
    previousModel?: string;     // the model before this turn's change ('' Claude's default), if it changed one
    pushed: boolean;            // its prompt is in the query
    stopping: boolean;          // a stop was taken for it
    textOpen: boolean;          // a text block is open
    held: string;               // a high surrogate held back from the last delta, to go with the next
    streamed: Set<string>;      // ids of the API messages whose text arrived as stream events
    failure?: Failure;
}

interface Failure {
    cause: string;
    code: ErrorCode;
    fix: string;
    model?: boolean;            // the model cannot be used
}

/** The longest text delta written, in UTF-16 units: far inside the line limit whatever the characters. */
const kMaxDelta = 16384;

function isHighSurrogate(c: number): boolean {
    return c >= 0xd800 && c <= 0xdbff;
}

/** Model and effort changes waiting for the next turn; a model of '' is Claude's default. */
interface Changes {
    model?: string;
    effort?: EffortLevel;
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
    for (const key of ['command', 'file_path', 'notebook_path', 'path', 'pattern', 'url', 'query', 'description', 'prompt']) {
        const v = o[key];
        if (typeof v === 'string' && v !== '') {
            return cut(oneLine(v), 200);
        }
    }
    const json = JSON.stringify(input) as string | undefined;
    return cut(oneLine(json === undefined || json === '{}' ? name : json), 200);
}

export class Driver {
    private readonly host: Host;
    private readonly args: Arguments;
    private readonly reader = new LineReader();
    private harness: Harness | undefined;
    private prompts = new Prompts();
    private generation = 0;
    private ready = false;                       // the credential check passed
    private starting: Promise<void> = Promise.resolve();   // the current harness's credential check
    private ended = false;                       // exiting: nothing more is written
    private chain: Promise<void> = Promise.resolve();
    private lastTurn = 0;
    private open: Turn | undefined;
    private pending: Changes = {};
    private model: string;                       // the model in use as far as the driver set it ('' Claude's default)
    private session = '';
    private cost: number | undefined;
    private plan = '';

    constructor(host: Host, args: Arguments) {
        this.host = host;
        this.args = args;
        this.model = args.model ?? '';
    }

    // ---- The process's life ----

    /** Writes the hello and starts the harness; nothing reaches it before the credential check passes. */
    start(): void {
        this.write({ kind: 'hello', protocol: kProtocolVersion, harness: 'claude' });
        this.launch(this.args.resume);
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
    }

    private quit(code: number): void {
        this.ended = true;
        this.prompts.close();
        this.harness?.close();
        this.host.exit(code);
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

    private launch(resume: string | undefined): void {
        const generation = ++this.generation;
        this.ready = false;
        this.prompts = new Prompts();
        const start: HarnessStart = { prompts: this.prompts, cwd: this.args.cwd, settingSources: this.args.settingSources };
        if (resume === undefined) {
            start.sessionId = this.host.newSessionId();
        } else {
            start.resume = resume;
        }
        if (this.args.model !== undefined) {
            start.model = this.args.model;
        }
        if (this.args.effort !== undefined) {
            start.effort = this.args.effort;
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
            info = await harness.accountInfo();
        } catch (e) {
            if (generation !== this.generation || this.ended) {
                return;
            }
            const why = message(e);
            if (this.args.resume !== undefined && sessionId === this.args.resume && isSessionNotFound(why)) {
                this.notFound(harness);
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
        if (provider !== 'firstParty' || keySource !== 'none' || /api.?key|auth.?token|helper/i.test(token)) {
            billing = 'api-key';
        } else if (plan !== '') {
            billing = 'subscription';
        } else if (this.host.oauthToken && (token === '' || /oauth/i.test(token))) {
            // An account's token from `claude setup-token` is a subscription's; Claude may not name the plan for it.
            billing = 'subscription';
        } else {
            billing = 'none';
        }
        this.plan = plan;
        this.write(plan === '' ? { kind: 'credential', billing } : { kind: 'credential', billing, plan: whole(plan) });
        if (billing === 'api-key') {
            this.refuseCredential(`Claude would bill ${provider === 'firstParty' ? `an API key or auth token (apiKeySource ${keySource}${token === '' ? '' : `, tokenSource ${token}`})` : `the ${provider} provider`}, not a subscription login; nexus runs Claude only on a subscription`,
                'remove the API key or provider setting Claude finds (an apiKeyHelper, a key saved by `claude /login` with a Console account, or a provider in managed settings) and run `claude /login` with the subscription account, in the user session nexus-server runs in');
            return;
        }
        if (billing === 'none') {
            this.refuseCredential('Claude has no subscription login: the account reports no subscription',
                'run `claude /login` with the subscription account in the user session nexus-server runs in (a process started over ssh cannot read the login keychain), or give the driver CLAUDE_CODE_OAUTH_TOKEN from `claude setup-token`');
            return;
        }
        this.ready = true;
        this.reportSession(sessionId);
    }

    /**
     * The --resume id is not a session Claude knows, found at the start or once the first turn began: go on in a new
     * session (sequence rule 10). A prompt the old query took never ran; it goes into the new one.
     */
    private notFound(old: Harness): void {
        old.close();
        const id = this.args.resume ?? '';
        const t = this.open;
        this.error(`Claude does not know the session ${id} in ${this.args.cwd}; the conversation goes on in a new session, without the earlier history`,
            'nothing to do: the new session is reported next', 'session-not-found', t?.turn);
        delete this.args.resume;
        this.launch(undefined);
        if (t?.pushed === true) {
            t.pushed = false;
            t.held = '';
            t.textOpen = false;
            this.enqueue(async () => {
                await this.started();
                if (this.open === t && !t.stopping && !this.ended && this.ready) {
                    t.message = this.prompts.push(t.prompt);
                    t.pushed = true;
                }
            });
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
        const turn = this.open?.turn;
        if (this.open !== undefined) {
            this.closeText(this.open);
        }
        this.error(cause, fix, 'no-credential', turn);
        if (turn !== undefined) {
            this.write({ kind: 'done', turn, outcome: 'error' });
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
        if (generation === this.generation && !this.ended && !this.resumeLost(why)) {
            this.fail(why);
        }
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
                break;
            case 'settings':
                this.settings(m);
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

    private send(turn: number, prompt: string): void {
        this.lastTurn = turn;
        const t: Turn = { turn, prompt, pushed: false, stopping: false, textOpen: false, held: '', streamed: new Set() };
        this.open = t;
        const changes = this.pending;
        this.pending = {};
        // Whether the turn still waits for its prompt to go in (a stop or the end can come during each await).
        const waiting = (): boolean => this.open === t && !t.stopping && !this.ended;
        this.enqueue(async () => {
            await this.started();
            if (!waiting() || !this.ready) {
                this.restore(changes);
                return;
            }
            await this.apply(changes, t);
            if (!waiting()) {
                return;
            }
            t.message = this.prompts.push(prompt);
            t.pushed = true;
        });
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

    private stop(turn: number): void {
        const t = this.open;
        if (t?.turn !== turn || t.stopping) {
            return;
        }
        t.stopping = true;
        if (!t.pushed || (t.message !== undefined && this.prompts.withdraw(t.message))) {
            // The prompt never reached the query: nothing runs, nothing to interrupt.
            this.finish(t, 'stopped');
            return;
        }
        const harness = this.harness;
        if (harness === undefined) {
            return;
        }
        const answered = harness.interrupt();
        this.enqueue(async () => {
            try {
                await answered;
            } catch (e) {
                this.fail(`Claude did not stop the turn: ${message(e)}`);
            }
        });
    }

    private settings(m: Settings): void {
        if (m.model !== undefined) {
            this.pending.model = m.model;
        }
        if (m.effort !== undefined) {
            if (isEffortLevel(m.effort)) {
                this.pending.effort = m.effort;
            } else {
                this.error(`effort ${m.effort} is not a level Claude has; the effort stays as it was`,
                    `choose one of ${kEffortLevels.join(', ')}`, 'other');
            }
        }
    }

    /** Model and effort changes, before the prompt of the turn they apply from. */
    private async apply(changes: Changes, t: Turn): Promise<void> {
        const harness = this.harness;
        if (harness === undefined) {
            return;
        }
        if (changes.model !== undefined) {
            try {
                await harness.setModel(changes.model === '' ? undefined : changes.model);
                t.previousModel = this.model;
                this.model = changes.model;
            } catch (e) {
                this.error(`Claude did not take the model ${changes.model}: ${message(e)}; the model stays as it was`,
                    'choose a model Claude has', 'harness');
            }
        }
        if (changes.effort !== undefined) {
            try {
                await harness.applyFlagSettings({ effortLevel: changes.effort });
            } catch (e) {
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
            case 'system':
                if (m.subtype === 'init' && m.apiKeySource !== 'none') {
                    this.write({ kind: 'credential', billing: 'api-key', ...(this.plan === '' ? {} : { plan: this.plan }) });
                    this.refuseCredential(`Claude started a turn billing an API key (apiKeySource ${m.apiKeySource}), not the subscription login; nexus runs Claude only on a subscription`,
                        'remove the API key Claude finds (an apiKeyHelper or a key saved by `claude /login` with a Console account) and run `claude /login` with the subscription account');
                }
                return;
            case 'rate_limit_event': {
                const info = m.rate_limit_info;
                if (info.isUsingOverage === true || info.overageInUse === true) {
                    this.refuseCredential('Claude began drawing on paid extra usage (overage) beyond the subscription; nexus runs Claude only on the subscription\'s included usage',
                        'turn extra usage off for this account in its Claude settings, or wait until the subscription\'s limit resets');
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
            this.text(t, e.delta.text);
        } else if (e.type === 'content_block_stop' || e.type === 'message_stop') {
            this.closeText(t);
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
            t.failure = failure(m.error, said);
            return;
        }
        for (const b of m.message.content) {
            if (b.type === 'text' && !t.streamed.has(m.message.id)) {
                this.text(t, b.text);
                this.closeText(t);
            } else if (b.type === 'tool_use' || b.type === 'server_tool_use' || b.type === 'mcp_tool_use') {
                this.closeText(t);
                const name = cut(oneLine(b.name), 200) || 'tool';
                this.write({ kind: 'tool', turn: t.turn, name, summary: toolSummary(b.name, b.input) });
            }
        }
    }

    /** A piece of text: whole characters only, a high surrogate at its end held for the next piece. */
    private text(t: Turn, delta: string): void {
        let s = t.held + delta;
        t.held = '';
        if (isHighSurrogate(s.charCodeAt(s.length - 1))) {
            t.held = s.slice(-1);
            s = s.slice(0, -1);
        }
        while (s !== '') {
            let n = Math.min(s.length, kMaxDelta);
            if (n < s.length && isHighSurrogate(s.charCodeAt(n - 1))) {
                n--;
            }
            t.textOpen = true;
            this.write({ kind: 'text', turn: t.turn, delta: whole(s.slice(0, n)) });
            s = s.slice(n);
        }
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
        if (t.stopping) {
            this.finish(t, 'stopped', r);
            return;
        }
        if (r.subtype === 'success' && !r.is_error && t.failure === undefined) {
            this.finish(t, 'ok', r);
            return;
        }
        const said = (r.subtype === 'success' ? [r.result] : r.errors).join('; ');
        if (t.failure === undefined && this.resumeLost(said)) {
            return;
        }
        const f = t.failure ?? {
            cause: `Claude ended the turn with an error (${r.subtype}): ${said || 'no cause given'}`,
            code: 'harness' as const,
            fix: kFixAgain,
        };
        if (t.failure?.model === true && t.previousModel !== undefined) {
            // The model this turn changed to cannot run: the one before comes back from the next turn.
            this.restore({ model: t.previousModel });
            this.model = t.previousModel;
            f.cause += `; the model goes back to ${t.previousModel === '' ? 'Claude\'s default' : t.previousModel} from the next turn`;
        }
        this.closeText(t);
        if (f.code === 'no-credential') {
            this.refuseCredential(f.cause, f.fix);
            return;
        }
        this.error(f.cause, f.fix, f.code, t.turn);
        this.finish(t, 'error', r);
    }

    private finish(t: Turn, outcome: 'ok' | 'stopped' | 'error', r?: SDKResultMessage): void {
        this.closeText(t);
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

/** The failure an assistant message's error code names. */
function failure(code: NonNullable<SDKAssistantMessage['error']>, said: string): Failure {
    const detail = said === '' ? code : `${code}: ${said}`;
    switch (code) {
        case 'authentication_failed':
        case 'oauth_org_not_allowed':
        case 'account_on_hold':
        case 'verification_required':
        case 'billing_error':
            return { cause: `Claude's login cannot be used (${detail})`, code: 'no-credential',
                fix: 'run `claude /login` with the subscription account in the user session nexus-server runs in, or give the driver a fresh CLAUDE_CODE_OAUTH_TOKEN from `claude setup-token`' };
        case 'rate_limit':
            return { cause: `the subscription's usage limit is reached (${detail})`, code: 'harness',
                fix: 'wait until the limit resets (the cause says when), or move the session to another account' };
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
