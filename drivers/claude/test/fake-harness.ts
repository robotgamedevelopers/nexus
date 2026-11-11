// A stand-in for the Claude Agent SDK's query: the messages a test (or a script) gives it, the prompts the driver
// pushes into it, and the control calls it receives. No process, no clock, no network.

import type {
    AccountInfo,
    SDKControlInterruptResponse,
    SDKMessage,
    SDKUserMessage,
} from '@anthropic-ai/claude-agent-sdk';

import type { Harness, HarnessStart } from '../src/driver.js';

export const kSubscription: AccountInfo = { subscriptionType: 'Claude Ultra', apiProvider: 'firstParty' };

export class FakeHarness implements Harness {
    readonly start: HarnessStart;
    account: AccountInfo | Error = kSubscription;
    /** What the driver called, in order: `interrupt`, `model <m>`, `effort <e>`, `close`. */
    readonly calls: string[] = [];
    readonly prompts: string[] = [];
    onPrompt: (text: string) => void = () => undefined;
    onInterrupt: () => void = () => undefined;
    interruptFails: Error | undefined;
    setModelFails: Error | undefined;

    private queue: SDKMessage[] = [];
    private wake: (() => void) | undefined;
    private ended: Error | 'ended' | undefined;

    constructor(start: HarnessStart) {
        this.start = start;
        void this.consume();
    }

    private async consume(): Promise<void> {
        for await (const m of start(this.start.prompts)) {
            const content = m.message.content;
            const text = typeof content === 'string' ? content : '';
            this.prompts.push(text);
            this.onPrompt(text);
        }
    }

    /** The harness writes these messages. */
    emit(...messages: SDKMessage[]): void {
        this.queue.push(...messages);
        this.wake?.();
    }

    /** The harness's stream ends, or fails with `error`. */
    finish(error?: Error): void {
        this.ended = error ?? 'ended';
        this.wake?.();
    }

    async *[Symbol.asyncIterator](): AsyncIterator<SDKMessage> {
        for (;;) {
            const next = this.queue.shift();
            if (next !== undefined) {
                yield next;
                continue;
            }
            if (this.ended instanceof Error) {
                throw this.ended;
            }
            if (this.ended === 'ended') {
                return;
            }
            await new Promise<void>((resolve) => { this.wake = resolve; });
            this.wake = undefined;
        }
    }

    interrupt(): Promise<SDKControlInterruptResponse | undefined> {
        this.calls.push('interrupt');
        if (this.interruptFails !== undefined) {
            return Promise.reject(this.interruptFails);
        }
        this.onInterrupt();
        return Promise.resolve(undefined);
    }

    setModel(model?: string): Promise<void> {
        this.calls.push(`model ${model ?? ''}`);
        return this.setModelFails === undefined ? Promise.resolve() : Promise.reject(this.setModelFails);
    }

    applyFlagSettings(settings: Parameters<Harness['applyFlagSettings']>[0]): Promise<void> {
        this.calls.push(`effort ${String(settings.effortLevel)}`);
        return Promise.resolve();
    }

    accountInfo(): Promise<AccountInfo> {
        return this.account instanceof Error ? Promise.reject(this.account) : Promise.resolve(this.account);
    }

    close(): void {
        this.calls.push('close');
        this.finish();
    }
}

function start(prompts: AsyncIterable<SDKUserMessage>): AsyncIterable<SDKUserMessage> {
    return prompts;
}

// ---- The messages a query writes, with only the fields the driver reads ----

let uuid = 0;
function base(session: string): { uuid: `${string}-${string}-${string}-${string}-${string}`; session_id: string } {
    uuid++;
    return { uuid: `00000000-0000-0000-0000-${String(uuid).padStart(12, '0')}`, session_id: session };
}

export function init(session: string, apiKeySource = 'none'): SDKMessage {
    return { type: 'system', subtype: 'init', apiKeySource, model: 'claude-opus-5-5', ...base(session) } as SDKMessage;
}

/** A text block streamed in `pieces`, as API message `id`, and the assistant message that repeats it. */
export function streamedText(session: string, id: string, pieces: string[]): SDKMessage[] {
    const event = (e: object): SDKMessage => ({ type: 'stream_event', event: e, parent_tool_use_id: null, ...base(session) }) as SDKMessage;
    return [
        event({ type: 'message_start', message: { id } }),
        event({ type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } }),
        ...pieces.map((text) => event({ type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text } })),
        event({ type: 'content_block_stop', index: 0 }),
        assistant(session, id, [{ type: 'text', text: pieces.join('') }]),
    ];
}

/** Only the start of a streamed text block: its first piece, with the block still open. */
export function openText(session: string, id: string, piece: string): SDKMessage[] {
    const event = (e: object): SDKMessage => ({ type: 'stream_event', event: e, parent_tool_use_id: null, ...base(session) }) as SDKMessage;
    return [
        event({ type: 'message_start', message: { id } }),
        event({ type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } }),
        event({ type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: piece } }),
    ];
}

export function blockStop(session: string): SDKMessage {
    return { type: 'stream_event', event: { type: 'content_block_stop', index: 0 }, parent_tool_use_id: null, ...base(session) };
}

export function assistant(session: string, id: string, content: object[], error?: string, parent: string | null = null): SDKMessage {
    return {
        type: 'assistant',
        message: { id, content },
        parent_tool_use_id: parent,
        ...(error === undefined ? {} : { error }),
        ...base(session),
    } as SDKMessage;
}

export function toolUse(session: string, id: string, name: string, input: object): SDKMessage {
    return assistant(session, id, [{ type: 'tool_use', id: `tool-${id}`, name, input }]);
}

export interface ResultOptions {
    subtype?: 'success' | 'error_during_execution' | 'error_max_turns';
    isError?: boolean;
    cost?: number;
    result?: string;
    errors?: string[];
}

export function result(session: string, o: ResultOptions = {}): SDKMessage {
    const subtype = o.subtype ?? 'success';
    return {
        type: 'result',
        subtype,
        is_error: o.isError ?? subtype !== 'success',
        total_cost_usd: o.cost ?? 0.01,
        usage: { input_tokens: 100, output_tokens: 20, cache_read_input_tokens: 5, cache_creation_input_tokens: 0 },
        ...(subtype === 'success' ? { result: o.result ?? 'done' } : { errors: o.errors ?? [] }),
        ...base(session),
    } as SDKMessage;
}

export function rateLimit(session: string, info: object): SDKMessage {
    return { type: 'rate_limit_event', rate_limit_info: { status: 'allowed', ...info }, ...base(session) };
}
