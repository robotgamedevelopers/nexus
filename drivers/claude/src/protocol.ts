// The driver protocol (docs/driver-protocol.md), the driver's side: the lines it reads from nexus-server, refused by
// the same rules as the reference reader (src/driver/protocol.cpp), and the lines it writes. Pure: no OS, no time.

/** The version this driver states in its hello. */
export const kProtocolVersion = 1;

/** The longest line, in bytes, not counting its newline (a CR before the newline counts). */
export const kMaxLineBytes = 1 << 20;

/** The deepest nesting of arrays and objects a line may have. */
export const kMaxDepth = 1024;

// ---- Server to driver ----

export interface Send {
    kind: 'send';
    turn: number;
    prompt: string;
}

export interface Stop {
    kind: 'stop';
    turn: number;
}

/** Model and effort for the turns that follow; a field that is absent stays as it is. */
export interface Settings {
    kind: 'settings';
    model?: string;
    effort?: string;
}

export interface ServerInfo {
    kind: 'server';
    features: string[];
}

export interface SecretAnswer {
    kind: 'secret';
    ask: number;
    outcome: 'stored' | 'refused';
    reason?: string;
}

/** A kind this version does not define: skipped. */
export interface Unknown {
    kind: 'unknown';
    name: string;
}

export type ServerMessage = Send | Stop | Settings | ServerInfo | SecretAnswer | Unknown;

/** A line read: the message, or why it was refused and the turn it named if that was readable. */
export type Decoded = { message: ServerMessage } | { error: string; turn?: number };

// ---- Driver to server ----

export type Billing = 'subscription' | 'api-key' | 'none';
export type Outcome = 'ok' | 'stopped' | 'error';
export type ErrorCode = 'protocol' | 'no-credential' | 'session-not-found' | 'harness' | 'other';

export interface Usage {
    input_tokens?: number;
    output_tokens?: number;
    cache_read_tokens?: number;
    cache_write_tokens?: number;
}

export type DriverMessage =
    | { kind: 'hello'; protocol: number; harness: string; features?: string[] }
    | { kind: 'credential'; billing: Billing; plan?: string }
    | { kind: 'session'; id: string }
    | { kind: 'text'; turn: number; delta: string }
    | { kind: 'text-end'; turn: number }
    | { kind: 'tool'; turn: number; name: string; summary: string }
    | { kind: 'done'; turn: number; outcome: Outcome; session_cost_usd?: number; usage?: Usage }
    | { kind: 'error'; cause: string; fix: string; turn?: number; code: ErrorCode }
    | { kind: 'ask-secret'; ask: number; account: string; what: string; why: string };

// ---- Reading ----

const utf8 = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true });

/** A turn: a whole number from 1 to 2^53 - 1. */
export function isTurn(value: unknown): value is number {
    return typeof value === 'number' && Number.isSafeInteger(value) && value >= 1;
}

/** Whether `text` nests arrays and objects deeper than kMaxDepth, counting only brackets outside strings. */
function nestedTooDeeply(text: string): boolean {
    let depth = 0;
    let inString = false;
    for (let i = 0; i < text.length; i++) {
        const c = text.charCodeAt(i);
        if (inString) {
            if (c === 0x5c) {
                i++;
            } else if (c === 0x22) {
                inString = false;
            }
        } else if (c === 0x22) {
            inString = true;
        } else if (c === 0x5b || c === 0x7b) {
            if (++depth > kMaxDepth) {
                return true;
            }
        } else if (c === 0x5d || c === 0x7d) {
            depth--;
        }
    }
    return false;
}

/** Whether every string in `value` (keys included) is whole Unicode characters: no lone surrogate. */
function wellFormed(value: unknown): boolean {
    if (typeof value === 'string') {
        return value.isWellFormed();
    }
    if (Array.isArray(value)) {
        return value.every(wellFormed);
    }
    if (typeof value === 'object' && value !== null) {
        return Object.entries(value).every(([k, v]) => k.isWellFormed() && wellFormed(v));
    }
    return true;
}

function isObject(value: unknown): value is Record<string, unknown> {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/** The member `key`, with null read as absent (an optional field may be written as null). */
function member(o: Record<string, unknown>, key: string): unknown {
    const v = Object.hasOwn(o, key) ? o[key] : undefined;
    return v ?? undefined;
}

/** The turn a line names, if it is JSON with a readable turn: for the error that refuses it. */
function readableTurn(text: string): number | undefined {
    try {
        const v: unknown = JSON.parse(text);
        if (isObject(v)) {
            const t = member(v, 'turn');
            if (isTurn(t)) {
                return t;
            }
        }
    } catch {
        // not JSON: no turn
    }
    return undefined;
}

/** One line from the server, without its newline (and without the CR of a CR LF), as a message. */
export function decodeServerLine(bytes: Uint8Array): Decoded {
    if (bytes.length >= 3 && bytes[0] === 0xef && bytes[1] === 0xbb && bytes[2] === 0xbf) {
        return { error: 'the line starts with a byte-order mark; the writer must send each line without one' };
    }
    let text: string;
    try {
        text = utf8.decode(bytes);
    } catch {
        return { error: 'the line is not UTF-8; the writer must send UTF-8' };
    }
    if (nestedTooDeeply(text)) {
        return { error: `the line's JSON is nested too deeply (more than ${kMaxDepth} arrays and objects inside each other); the writer must nest less` };
    }
    let value: unknown;
    try {
        value = JSON.parse(text);
    } catch {
        return { error: 'the line is not JSON; the writer must send one JSON object per line' };
    }
    const turn = readableTurn(text);
    const refuse = (why: string): Decoded => (turn === undefined ? { error: why } : { error: why, turn });
    if (!wellFormed(value)) {
        return refuse('the line is not UTF-8: an escaped lone surrogate (a \\u escape of U+D800 to U+DFFF that is not half of a pair); the writer must send whole characters');
    }
    if (!isObject(value)) {
        return { error: 'the line is JSON but not an object; each line is one object with a "kind"' };
    }
    const kind = member(value, 'kind');
    if (typeof kind !== 'string' || kind === '') {
        return refuse('the object has no "kind" string; each line names its kind, as {"kind":"send",...}');
    }
    const field = (why: string): Decoded => refuse(`a "${kind}" message ${why}; see docs/driver-protocol.md for its fields`);
    switch (kind) {
        case 'send': {
            const t = member(value, 'turn');
            const prompt = member(value, 'prompt');
            if (!isTurn(t)) {
                return field(t === undefined ? 'has no "turn"' : 'has "turn" that is not a whole number from 1 to 9007199254740991');
            }
            if (typeof prompt !== 'string') {
                return field(prompt === undefined ? 'has no "prompt"' : 'has "prompt" that is not a string');
            }
            return { message: { kind: 'send', turn: t, prompt } };
        }
        case 'stop': {
            const t = member(value, 'turn');
            if (!isTurn(t)) {
                return field(t === undefined ? 'has no "turn"' : 'has "turn" that is not a whole number from 1 to 9007199254740991');
            }
            return { message: { kind: 'stop', turn: t } };
        }
        case 'settings': {
            const message: Settings = { kind: 'settings' };
            for (const key of ['model', 'effort'] as const) {
                const v = member(value, key);
                if (v === undefined) {
                    continue;
                }
                if (typeof v !== 'string') {
                    return field(`has "${key}" that is not a string`);
                }
                if (v === '') {
                    return field(`has an empty "${key}"`);
                }
                message[key] = v;
            }
            if (message.model === undefined && message.effort === undefined) {
                return field('has neither "model" nor "effort"');
            }
            return { message };
        }
        case 'server': {
            const features = member(value, 'features');
            if (features === undefined) {
                return field('has no "features"');
            }
            if (!Array.isArray(features) || !features.every((e): e is string => typeof e === 'string')) {
                return field('has "features" that is not an array of strings');
            }
            return { message: { kind: 'server', features } };
        }
        case 'secret': {
            const ask = member(value, 'ask');
            if (!isTurn(ask)) {
                return field(ask === undefined ? 'has no "ask"' : 'has "ask" that is not a whole number from 1 to 9007199254740991');
            }
            const outcome = member(value, 'outcome');
            if (typeof outcome !== 'string') {
                return field(outcome === undefined ? 'has no "outcome"' : 'has "outcome" that is not a string');
            }
            const reason = member(value, 'reason');
            if (outcome === 'stored') {
                if (reason === undefined) {
                    return { message: { kind: 'secret', ask, outcome } };
                }
                if (typeof reason !== 'string') {
                    return field('has "reason" that is not a string');
                }
                if (reason === '') {
                    return field('has an empty "reason"');
                }
                return { message: { kind: 'secret', ask, outcome, reason } };
            }
            if (outcome === 'refused') {
                if (reason === undefined) {
                    return field('has no "reason"');
                }
                if (typeof reason !== 'string') {
                    return field('has "reason" that is not a string');
                }
                if (reason === '') {
                    return field('has an empty "reason"');
                }
                return { message: { kind: 'secret', ask, outcome, reason } };
            }
            return field(`has "outcome" "${outcome}"; it is one of stored, refused`);
        }
        default:
            return { message: { kind: 'unknown', name: kind } };
    }
}

/**
 * Cuts the bytes of a stream into lines. A line longer than kMaxLineBytes is refused as soon as it passes the limit
 * and dropped up to its newline; a line left without its newline at the end is refused.
 */
export class LineReader {
    private parts: Uint8Array[] = [];
    private size = 0;
    private dropping = false;

    /** The lines `chunk` completes: each its bytes (without CR LF or LF), or why it was refused. */
    feed(chunk: Uint8Array): ({ line: Uint8Array } | { error: string })[] {
        const out: ({ line: Uint8Array } | { error: string })[] = [];
        let start = 0;
        while (start <= chunk.length) {
            const nl = chunk.indexOf(0x0a, start);
            const end = nl === -1 ? chunk.length : nl;
            if (!this.dropping) {
                const piece = chunk.subarray(start, end);
                if (this.size + piece.length > kMaxLineBytes) {
                    out.push({ error: `a line is longer than the protocol's limit of ${kMaxLineBytes} bytes; the writer must refuse a message that long` });
                    this.parts = [];
                    this.size = 0;
                    this.dropping = true;
                } else if (piece.length > 0) {
                    this.parts.push(piece.slice());
                    this.size += piece.length;
                }
            }
            if (nl === -1) {
                break;
            }
            if (this.dropping) {
                this.dropping = false;
            } else {
                let line = Buffer.concat(this.parts, this.size);
                if (line.length > 0 && line[line.length - 1] === 0x0d) {
                    line = line.subarray(0, line.length - 1);
                }
                out.push({ line });
            }
            this.parts = [];
            this.size = 0;
            start = nl + 1;
        }
        return out;
    }

    /** The stream ended: a line left without its newline is refused. */
    finish(): { error: string }[] {
        const cut = this.size > 0 || this.dropping;
        this.parts = [];
        this.size = 0;
        this.dropping = false;
        return cut ? [{ error: 'the stream ended inside a line; the writer must end every line with a newline' }] : [];
    }
}

// ---- Writing ----

/** `s` with any lone surrogate replaced, so that it encodes as UTF-8 the reader takes. */
export function whole(s: string): string {
    return s.toWellFormed();
}

/** A session id the protocol carries: 1 to 256 printable ASCII characters, no spaces, not starting with `-`. */
export function isSessionId(id: string): boolean {
    return /^[!-~]{1,256}$/.test(id) && !id.startsWith('-');
}

/** Characters a tool's name or summary may not hold: controls, and line and paragraph separators. */
// eslint-disable-next-line no-control-regex
const lineBreakers = /[\u0000-\u001f\u007f-\u009f\u2028\u2029]+/g;

/** `s` as one line: each run of control characters and line breaks becomes one space, ends trimmed. */
export function oneLine(s: string): string {
    return whole(s).replace(lineBreakers, ' ').trim();
}

/** `s` cut to at most `max` UTF-16 units without splitting a character, with an ellipsis when cut. */
export function cut(s: string, max: number): string {
    if (s.length <= max) {
        return s;
    }
    let end = max - 1;
    const c = s.charCodeAt(end - 1);
    if (c >= 0xd800 && c <= 0xdbff) {
        end--;
    }
    return s.slice(0, end) + '…';
}

/** The longest reason an ask-secret may carry, in UTF-8 bytes (docs/secrets.md). */
export const kMaxSecretWhyBytes = 200;

/**
 * Whether `why` is a valid ask-secret reason (src/sync/secrets.cpp isSecretWhy): whole UTF-8 characters, at most 200
 * bytes, with no control (C0, DEL or C1), bidirectional, zero-width or line/paragraph-separator characters.
 */
export function isSecretWhy(why: string): boolean {
    if (!why.isWellFormed() || Buffer.byteLength(why, 'utf8') > kMaxSecretWhyBytes) {
        return false;
    }
    for (const ch of why) {
        const cp = ch.codePointAt(0) ?? 0;
        const control = cp < 0x20 || (cp >= 0x7f && cp <= 0x9f);
        const bidi = cp === 0x061c || cp === 0x200e || cp === 0x200f || (cp >= 0x202a && cp <= 0x202e)
            || (cp >= 0x2066 && cp <= 0x2069);
        const zeroWidth = (cp >= 0x200b && cp <= 0x200d) || cp === 0x2060 || cp === 0xfeff;
        const separator = cp === 0x2028 || cp === 0x2029;
        if (control || bidi || zeroWidth || separator) {
            return false;
        }
    }
    return true;
}

/** A message as one line ending in a newline. */
export function encode(message: DriverMessage): string {
    return JSON.stringify(message) + '\n';
}
