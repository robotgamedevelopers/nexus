// Unit tests of the driver's logic against a scripted query (FakeHarness): no process, no clock, no network.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';

import {
    Driver, kSecretMcpToolName, kSecretToolName, parseArguments, secretMcpServer, toolSummary,
} from '../../src/driver.js';
import type { DriverMessage } from '../../src/protocol.js';
import {
    assistant,
    blockStop,
    init,
    kSubscription,
    openText,
    rateLimit,
    result,
    streamedText,
    toolUse,
} from '../fake-harness.js';
import { LeakScan } from '../leak-scan.js';
import { line, settle, started, TestHost } from './host.js';

const S = 'session-1';

void test('start: hello first, then the credential and the session, before any prompt', async () => {
    const host = new TestHost();
    const driver = new Driver(host, { account: 'main', cwd: '/work', settingSources: [] });
    driver.start();
    assert.deepEqual(host.out, [{ kind: 'hello', protocol: 1, harness: 'claude', features: ['limit'] }]);
    await settle();
    assert.deepEqual(host.take().slice(1), [
        { kind: 'credential', billing: 'subscription', plan: 'Claude Ultra' },
        { kind: 'session', id: S },
    ]);
    const start = host.harness.start;
    assert.equal(start.sessionId, S);
    assert.equal(start.resume, undefined);
    assert.equal(start.cwd, '/work');
    assert.deepEqual(start.settingSources, []);
    assert.equal(host.exitCode, undefined);
});

void test('a turn: streamed text, a tool, more text, done with the session cost and usage', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'send', turn: 7, prompt: 'What is in src?' }));
    await settle();
    assert.deepEqual(host.harness.prompts, ['What is in src?']);
    host.harness.emit(
        init(S),
        ...streamedText(S, 'm1', ['Let me ', 'look.']),
        toolUse(S, 'm2', 'Bash', { command: 'ls src', description: 'list' }),
        ...streamedText(S, 'm3', ['It holds the server.']),
        result(S, { cost: 0.021 }),
    );
    await settle();
    assert.deepEqual(host.take(), [
        { kind: 'text', turn: 7, delta: 'Let me ' },
        { kind: 'text', turn: 7, delta: 'look.' },
        { kind: 'text-end', turn: 7 },
        { kind: 'tool', turn: 7, name: 'Bash', summary: 'ls src' },
        { kind: 'text', turn: 7, delta: 'It holds the server.' },
        { kind: 'text-end', turn: 7 },
        { kind: 'done', turn: 7, outcome: 'ok', session_cost_usd: 0.021,
            usage: { input_tokens: 100, output_tokens: 20, cache_read_tokens: 5, cache_write_tokens: 0 } },
    ]);
});

void test('a stop mid-turn interrupts the query; its result closes the turn as stopped', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'send', turn: 2, prompt: 'Now read every file.' }));
    await settle();
    host.harness.emit(...openText(S, 'm1', 'Reading the first'));
    await settle();
    driver.feed(line({ kind: 'stop', turn: 1 }));       // not the open turn: nothing
    driver.feed(line({ kind: 'stop', turn: 2 }));
    driver.feed(line({ kind: 'stop', turn: 2 }));       // a second stop of the same turn: nothing more
    await settle();
    assert.deepEqual(host.harness.calls, ['interrupt']);
    host.harness.emit(...openText(S, 'm1', ' one').slice(2), blockStop(S), result(S, { subtype: 'error_during_execution', cost: 0.034 }));
    await settle();
    assert.deepEqual(host.take(), [
        { kind: 'text', turn: 2, delta: 'Reading the first' },
        { kind: 'text', turn: 2, delta: ' one' },
        { kind: 'text-end', turn: 2 },
        { kind: 'done', turn: 2, outcome: 'stopped', session_cost_usd: 0.034,
            usage: { input_tokens: 100, output_tokens: 20, cache_read_tokens: 5, cache_write_tokens: 0 } },
    ]);
    driver.feed(line({ kind: 'stop', turn: 2 }));       // for a turn already ended: no answer
    await settle();
    assert.deepEqual(host.take(), []);
    assert.deepEqual(host.harness.calls, ['interrupt']);
});

void test('the next prompt waits for the interrupt to answer, so it never lands in the next turn', async () => {
    const { host, driver } = await started();
    let answer: () => void = () => undefined;
    const answered = new Promise<undefined>((resolve) => { answer = () => { resolve(undefined); }; });
    host.harness.interrupt = () => { host.harness.calls.push('interrupt'); return answered; };
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'one' }));
    await settle();
    driver.feed(line({ kind: 'stop', turn: 1 }));
    host.harness.emit(result(S, { subtype: 'error_during_execution' }));
    await settle();
    driver.feed(line({ kind: 'send', turn: 2, prompt: 'two' }));
    await settle();
    assert.deepEqual(host.harness.prompts, ['one']);
    answer();
    await settle();
    assert.deepEqual(host.harness.prompts, ['one', 'two']);
});

void test('a stop before the prompt reached the query ends the turn at once; the prompt never goes', async () => {
    let release: () => void = () => undefined;
    const host = new TestHost();
    host.prepare = (h) => {
        h.accountInfo = () => new Promise((resolve) => { release = () => { resolve(kSubscription); }; });
    };
    const driver = new Driver(host, { account: 'main', cwd: '/work', settingSources: [] });
    driver.start();
    driver.feed(line({ kind: 'send', turn: 4, prompt: 'never' }));
    driver.feed(line({ kind: 'stop', turn: 4 }));
    await settle();
    assert.deepEqual(host.take().slice(1), [{ kind: 'done', turn: 4, outcome: 'stopped' }]);
    release();
    await settle();
    assert.deepEqual(host.harness.prompts, []);
    assert.deepEqual(host.harness.calls, []);
});

async function refusedAtStart(account: object): Promise<TestHost> {
    const host = new TestHost();
    host.prepare = (h) => { h.account = account; };
    const driver = new Driver(host, { account: 'main', cwd: '/work', settingSources: [] });
    driver.start();
    driver.feed(line({ kind: 'send', turn: 3, prompt: 'hello' }));
    await settle();
    return host;
}

function errorOf(messages: DriverMessage[]): Extract<DriverMessage, { kind: 'error' }> {
    const e = messages.find((m): m is Extract<DriverMessage, { kind: 'error' }> => m.kind === 'error');
    assert.ok(e !== undefined, `no error in ${JSON.stringify(messages)}`);
    return e;
}

void test('an API key is refused before any prompt reaches Claude', async () => {
    const host = await refusedAtStart({ ...kSubscription, apiKeySource: 'ANTHROPIC_API_KEY' });
    const out = host.take();
    assert.deepEqual(out[1], { kind: 'credential', billing: 'api-key', plan: 'Claude Ultra' });
    const e = errorOf(out);
    assert.equal(e.code, 'no-credential');
    assert.equal(e.turn, 3);
    assert.match(e.cause, /API key/);
    assert.match(e.fix, /CLAUDE_CONFIG_DIR=~\/\.nexus\/server\/accounts\/main claude setup-token/);
    assert.match(e.fix, /accounts\/main\/oauth-token with mode 0600/);
    assert.deepEqual(out[out.length - 1], { kind: 'done', turn: 3, outcome: 'error' });
    assert.equal(host.exitCode, 1);
    assert.deepEqual(host.harness.prompts, []);
    assert.ok(host.harness.calls.includes('close'));
});

void test('another provider is refused', async () => {
    const host = await refusedAtStart({ apiProvider: 'bedrock' });
    const out = host.take();
    assert.deepEqual(out[1], { kind: 'credential', billing: 'api-key' });
    assert.match(errorOf(out).cause, /bedrock/);
    assert.equal(host.exitCode, 1);
    assert.deepEqual(host.harness.prompts, []);
});

void test('no subscription is refused; the account token counts as one only when Claude says it uses it', async () => {
    const none = await refusedAtStart({ apiProvider: 'firstParty' });
    const out = none.take();
    assert.deepEqual(out[1], { kind: 'credential', billing: 'none' });
    assert.equal(errorOf(out).code, 'no-credential');
    assert.equal(none.exitCode, 1);
    assert.deepEqual(none.harness.prompts, []);

    const token = await refusedAtStart({ apiProvider: 'firstParty', tokenSource: 'CLAUDE_CODE_OAUTH_TOKEN' });
    assert.deepEqual(token.take()[1], { kind: 'credential', billing: 'subscription' });
    assert.equal(token.exitCode, undefined);
    assert.deepEqual(token.harness.prompts, ['hello']);

    // The token as the driver hands it over, on a descriptor (wall.ts), is the same subscription login.
    const fd = await refusedAtStart({ apiProvider: 'firstParty', tokenSource: 'CLAUDE_CODE_OAUTH_TOKEN_FILE_DESCRIPTOR' });
    assert.deepEqual(fd.take()[1], { kind: 'credential', billing: 'subscription' });
    assert.equal(fd.exitCode, undefined);
    assert.deepEqual(fd.harness.prompts, ['hello']);
});

void test('an init that names an API key ends the turn and the driver', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'hi' }));
    await settle();
    host.harness.emit(init(S, 'apiKeyHelper'), ...streamedText(S, 'm1', ['never shown']));
    await settle();
    const out = host.take();
    assert.deepEqual(out[0], { kind: 'credential', billing: 'api-key', plan: 'Claude Ultra' });
    assert.equal(errorOf(out).code, 'no-credential');
    assert.equal(errorOf(out).turn, 1);
    assert.deepEqual(out[out.length - 1], { kind: 'done', turn: 1, outcome: 'error' });
    assert.equal(out.filter((m) => m.kind === 'text').length, 0);
    assert.equal(host.exitCode, 1);
    assert.ok(host.harness.calls.includes('close'));
});

void test('paid extra usage (overage) ends the turn and the driver', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'hi' }));
    await settle();
    host.harness.emit(rateLimit(S, { isUsingOverage: false }));
    await settle();
    assert.equal(host.exitCode, undefined);
    host.harness.emit(rateLimit(S, { isUsingOverage: true }));
    await settle();
    const out = host.take();
    assert.match(errorOf(out).cause, /extra usage/);
    assert.equal(errorOf(out).code, 'no-credential');
    assert.deepEqual(out[out.length - 1], { kind: 'done', turn: 1, outcome: 'error' });
    assert.equal(host.exitCode, 1);
});

void test('--resume resumes that session and reports it', async () => {
    const host = new TestHost();
    const driver = new Driver(host, { account: 'main', cwd: '/work', resume: 'abc-123', settingSources: [] });
    driver.start();
    await settle();
    assert.equal(host.harness.start.resume, 'abc-123');
    assert.equal(host.harness.start.sessionId, undefined);
    assert.deepEqual(host.take().slice(1), [
        { kind: 'credential', billing: 'subscription', plan: 'Claude Ultra' },
        { kind: 'session', id: 'abc-123' },
    ]);
});

void test('fresh and resumed harnesses each receive their standing instructions', async () => {
    for (const resume of [undefined, 'existing']) {
        const host = new TestHost();
        const driver = new Driver(host, { account: 'main', cwd: '/work', settingSources: [],
            startupContext: 'standing instructions', ...(resume === undefined ? {} : { resume }) });
        driver.start();
        await settle();
        assert.equal(host.harness.start.startupContext, 'standing instructions');
    }
});

void test('a --resume id Claude does not know: a notice, a new session, and the turn goes on', async () => {
    const host = new TestHost();
    host.prepare = (h) => {
        if (h.start.resume !== undefined) {
            h.account = new Error('Claude Code returned an error result: No conversation found with session ID: gone-1');
            h.finish(new Error('Claude Code returned an error result: No conversation found with session ID: gone-1'));
        }
    };
    const driver = new Driver(host, { account: 'main', cwd: '/work', resume: 'gone-1', settingSources: [] });
    driver.start();
    driver.feed(line({ kind: 'send', turn: 5, prompt: 'go on' }));
    await settle();
    assert.equal(host.harnesses.length, 2);
    assert.equal(host.harness.start.sessionId, 'session-1');
    assert.equal(host.harness.start.resume, undefined);
    const out = host.take();
    const e = errorOf(out);
    assert.equal(e.code, 'session-not-found');
    assert.equal(e.turn, 5);
    assert.deepEqual(out.slice(out.indexOf(e) + 1), [
        { kind: 'credential', billing: 'subscription', plan: 'Claude Ultra' },
        { kind: 'session', id: 'session-1' },
    ]);
    assert.deepEqual(host.harness.prompts, ['go on']);
    host.harness.emit(...streamedText('session-1', 'm1', ['fresh']), result('session-1'));
    await settle();
    assert.deepEqual(host.take().map((m) => m.kind), ['text', 'text-end', 'done']);
    assert.equal(host.exitCode, undefined);
});

void test('model and effort apply from the next send, before its prompt; never inside an open turn', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'settings', model: 'claude-sonnet-5-5', effort: 'high' }));
    await settle();
    assert.deepEqual(host.harness.calls, []);
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'one' }));
    await settle();
    assert.deepEqual(host.harness.calls, ['model claude-sonnet-5-5', 'effort high']);
    assert.deepEqual(host.harness.prompts, ['one']);
    driver.feed(line({ kind: 'settings', effort: 'low' }));
    await settle();
    assert.deepEqual(host.harness.calls, ['model claude-sonnet-5-5', 'effort high']);
    host.harness.emit(result(S));
    await settle();
    driver.feed(line({ kind: 'send', turn: 2, prompt: 'two' }));
    await settle();
    assert.deepEqual(host.harness.calls, ['model claude-sonnet-5-5', 'effort high', 'effort low']);
    assert.deepEqual(host.harness.prompts, ['one', 'two']);
});

void test('an effort Claude does not have is refused and the rest stays; a model Claude refuses is said', async () => {
    const { host, driver } = await started();
    host.untraced = 'settings Claude refuses';
    host.harness.setModelFails = new Error('unknown model');
    driver.feed(line({ kind: 'settings', model: 'nope', effort: 'extreme' }));
    const out = host.take();
    assert.equal(errorOf(out).code, 'other');
    assert.match(errorOf(out).cause, /extreme/);
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'one' }));
    await settle();
    const e = errorOf(host.take());
    assert.equal(e.code, 'harness');
    assert.equal(e.turn, undefined);
    assert.deepEqual(host.harness.calls, ['model nope']);
    assert.deepEqual(host.harness.prompts, ['one']);
});

void test('start values from the drivers file go to the query', async () => {
    const { host } = await started({ model: 'opus', effort: 'xhigh', settingSources: ['user', 'project'], claudePath: '/bin/claude' });
    const s = host.harness.start;
    assert.equal(s.model, 'opus');
    assert.equal(s.effort, 'xhigh');
    assert.deepEqual(s.settingSources, ['user', 'project']);
    assert.equal(s.claudePath, '/bin/claude');
    assert.equal(s.limited, false);
    assert.deepEqual(s.keepsOut, []);
    const limited = await started({ limit: 'cwd', keepsOut: ['/data'] });
    assert.equal(limited.host.harness.start.limited, true);
    assert.deepEqual(limited.host.harness.start.keepsOut, ['/data']);
});

async function refusedLine(bytes: Uint8Array, opened = false): Promise<TestHost> {
    const { host, driver } = await started();
    host.untraced = 'a line the driver refuses';
    if (opened) {
        driver.feed(line({ kind: 'send', turn: 3, prompt: 'a' }));
        await settle();
    }
    driver.feed(bytes);
    driver.feed(line({ kind: 'send', turn: 50, prompt: 'never read' }));
    await settle();
    return host;
}

void test('a line the driver cannot accept: one protocol error, then exit 3', async () => {
    const cases: [Uint8Array, number | undefined, RegExp, boolean][] = [
        [line({ kind: 'send', turn: 7 }), 7, /prompt/, false],
        [Buffer.from('not json\n'), undefined, /not JSON/, false],
        [line({ kind: 'send', turn: 4, prompt: 'b' }), 4, /while turn 3 is open/, true],
        [line({ kind: 'stop', turn: 1.5 }), undefined, /turn/, false],
    ];
    for (const [bytes, turn, cause, opened] of cases) {
        const host = await refusedLine(bytes, opened);
        const out = host.take();
        const errors = out.filter((m) => m.kind === 'error');
        assert.equal(errors.length, 1, JSON.stringify(out));
        const e = errorOf(out);
        assert.equal(e.code, 'protocol');
        assert.equal(e.turn, turn);
        assert.match(e.cause, cause);
        assert.equal(host.exitCode, 3);
        assert.equal(out.filter((m) => m.kind === 'done').length, 0);
    }
});

void test('a send whose turn does not follow the last one is refused', async () => {
    const { host, driver } = await started();
    host.untraced = 'a line the driver refuses';
    driver.feed(line({ kind: 'send', turn: 5, prompt: 'a' }));
    await settle();
    host.harness.emit(result(S));
    await settle();
    host.take();
    driver.feed(line({ kind: 'send', turn: 5, prompt: 'again' }));
    await settle();
    const e = errorOf(host.take());
    assert.equal(e.code, 'protocol');
    assert.equal(e.turn, 5);
    assert.match(e.cause, /does not follow turn 5/);
    assert.equal(host.exitCode, 3);
});

void test('an unknown kind is skipped; the end of input exits 0 owing no done', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'pause', turn: 1 }));
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'a' }));
    await settle();
    driver.end();
    await settle();
    assert.deepEqual(host.take(), []);
    assert.equal(host.exitCode, 0);
    assert.ok(host.harness.calls.includes('close'));
});

void test('the usage limit fails the turn with its cause; the driver goes on', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'a' }));
    await settle();
    const said = 'You\'ve hit your weekly limit · resets Nov 14 at 4am';
    host.harness.emit(init(S), assistant(S, 'm1', [{ type: 'text', text: said }], 'rate_limit'),
        result(S, { isError: true, result: said, cost: 0 }));
    await settle();
    const out = host.take();
    assert.deepEqual(out.map((m) => m.kind), ['error', 'done']);
    const e = errorOf(out);
    assert.equal(e.code, 'harness');
    assert.equal(e.turn, 1);
    assert.ok(e.cause.includes(said));
    assert.deepEqual(out[1], { kind: 'done', turn: 1, outcome: 'error',
        usage: { input_tokens: 100, output_tokens: 20, cache_read_tokens: 5, cache_write_tokens: 0 } });
    assert.equal(host.exitCode, undefined);
});

void test('a login that fails ends the turn and the driver with no-credential', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'a' }));
    await settle();
    host.harness.emit(assistant(S, 'm1', [{ type: 'text', text: 'Not logged in' }], 'authentication_failed'),
        result(S, { isError: true, result: 'Not logged in' }));
    await settle();
    const out = host.take();
    assert.deepEqual(out.map((m) => m.kind), ['error', 'done']);
    assert.equal(errorOf(out).code, 'no-credential');
    assert.equal(host.exitCode, 1);
});

void test('an error result: its cause, then done error; the session cost stays at its last known value', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'a' }));
    await settle();
    host.harness.emit(result(S, { cost: 0.5 }));
    await settle();
    driver.feed(line({ kind: 'send', turn: 2, prompt: 'b' }));
    await settle();
    host.harness.emit(result(S, { subtype: 'error_max_turns', errors: ['too many turns'], cost: 0 }));
    await settle();
    const out = host.take();
    assert.match(errorOf(out).cause, /too many turns/);
    const done = out[out.length - 1];
    assert.ok(done?.kind === 'done');
    assert.equal(done.outcome, 'error');
    assert.equal(done.session_cost_usd, 0.5);
});

void test('the harness failing after the start: the cause with the open turn, then exit 1 owing no done', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'send', turn: 9, prompt: 'a' }));
    await settle();
    host.harness.finish(new Error('the CLI crashed'));
    await settle();
    const out = host.take();
    assert.deepEqual(out.map((m) => m.kind), ['error']);
    assert.equal(errorOf(out).turn, 9);
    assert.match(errorOf(out).cause, /crashed/);
    assert.equal(host.exitCode, 1);
});

void test('an interrupt Claude refuses: the cause, then exit 1', async () => {
    const { host, driver } = await started();
    host.harness.interruptFails = new Error('no');
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'a' }));
    await settle();
    driver.feed(line({ kind: 'stop', turn: 1 }));
    await settle();
    assert.match(errorOf(host.take()).cause, /did not stop/);
    assert.equal(host.exitCode, 1);
});

void test('whole characters: a pair split across deltas is written together', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'a' }));
    await settle();
    const face = '\u{1F600}';
    host.harness.emit(...streamedText(S, 'm1', ['hi ' + face.slice(0, 1), face.slice(1) + '!']), result(S));
    await settle();
    const texts = host.take().flatMap((m) => (m.kind === 'text' ? [m.delta] : []));
    assert.deepEqual(texts, ['hi ', face + '!']);
    assert.ok(texts.every((t) => t.isWellFormed()));
});

void test('messages after a turn\'s result, and a subagent\'s stream, are dropped', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'a' }));
    await settle();
    const sub = streamedText(S, 'sub', ['inside a subagent']).map((m) => ({ ...m, parent_tool_use_id: 'tool-1' }));
    host.harness.emit(...sub, result(S), ...streamedText(S, 'm9', ['after the result']), toolUse(S, 'm10', 'Read', { file_path: '/x' }));
    await settle();
    assert.deepEqual(host.take().map((m) => m.kind), ['done']);
});

void test('text that arrived only in an assistant message is still written', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'a' }));
    await settle();
    host.harness.emit(assistant(S, 'm1', [{ type: 'text', text: 'whole' }]), result(S));
    await settle();
    assert.deepEqual(host.take().slice(0, 2), [{ kind: 'text', turn: 1, delta: 'whole' }, { kind: 'text-end', turn: 1 }]);
});

void test('settings sent with a turn stopped before its prompt went in carry to the next turn', async () => {
    let release: () => void = () => undefined;
    const host = new TestHost();
    host.prepare = (h) => {
        h.accountInfo = () => new Promise((resolve) => { release = () => { resolve(kSubscription); }; });
    };
    const driver = new Driver(host, { account: 'main', cwd: '/work', settingSources: [] });
    driver.start();
    driver.feed(line({ kind: 'settings', model: 'opus', effort: 'low' }));
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'a' }));
    driver.feed(line({ kind: 'stop', turn: 1 }));
    release();
    await settle();
    driver.feed(line({ kind: 'settings', effort: 'max' }));
    driver.feed(line({ kind: 'send', turn: 2, prompt: 'b' }));
    await settle();
    assert.deepEqual(host.harness.calls, ['model opus', 'effort max']);
    assert.deepEqual(host.harness.prompts, ['b']);
});

void test('a refusal mid-turn closes the open text block before its done', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'a' }));
    await settle();
    host.harness.emit(...openText(S, 'm1', 'partial'), rateLimit(S, { isUsingOverage: true }));
    await settle();
    assert.deepEqual(host.take().map((m) => m.kind), ['text', 'text-end', 'error', 'done']);
    assert.equal(host.exitCode, 1);
});

void test('a stop before the query took the prompt withdraws it: no interrupt, nothing runs', async () => {
    const { host, driver } = await started({}, (h) => { h.holdPrompts = true; });
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'a' }));
    await settle();
    driver.feed(line({ kind: 'stop', turn: 1 }));
    await settle();
    assert.deepEqual(host.take(), [{ kind: 'done', turn: 1, outcome: 'stopped' }]);
    host.harness.takePrompts();
    driver.feed(line({ kind: 'send', turn: 2, prompt: 'b' }));
    await settle();
    assert.deepEqual(host.harness.calls, []);
    assert.deepEqual(host.harness.prompts, ['b']);
});

void test('a --resume id Claude finds unknown only when the turn starts: a new session, and the prompt goes there', async () => {
    const host = new TestHost();
    const driver = new Driver(host, { account: 'main', cwd: '/work', resume: 'gone-2', settingSources: [] });
    driver.start();
    await settle();
    host.take();
    driver.feed(line({ kind: 'send', turn: 3, prompt: 'go on' }));
    await settle();
    const first = host.harness;
    assert.deepEqual(first.prompts, ['go on']);
    first.emit(result('gone-2', { subtype: 'error_during_execution', errors: ['No conversation found with session ID: gone-2'] }));
    first.finish(new Error('Claude Code returned an error result: No conversation found with session ID: gone-2'));
    await settle();
    assert.equal(host.harnesses.length, 2);
    assert.deepEqual(host.harness.prompts, ['go on']);
    const out = host.take();
    assert.equal(errorOf(out).code, 'session-not-found');
    assert.equal(errorOf(out).turn, 3);
    assert.ok(out.some((m) => m.kind === 'session' && m.id === 'session-1'));
    assert.ok(!out.some((m) => m.kind === 'done'));
    host.harness.emit(result('session-1'));
    await settle();
    assert.deepEqual(host.take().map((m) => m.kind), ['done']);
    assert.equal(host.exitCode, undefined);
});

void test('a model Claude cannot run goes back to the one before from the next turn', async () => {
    const { host, driver } = await started({ model: 'opus' });
    host.untraced = 'a model Claude cannot run';
    driver.feed(line({ kind: 'settings', model: 'no-such-model' }));
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'a' }));
    await settle();
    host.harness.emit(assistant(S, 'm1', [{ type: 'text', text: 'model not found' }], 'model_not_found'), result(S, { isError: true }));
    await settle();
    assert.match(errorOf(host.take()).cause, /goes back to opus/);
    driver.feed(line({ kind: 'send', turn: 2, prompt: 'b' }));
    await settle();
    assert.deepEqual(host.harness.calls, ['model no-such-model', 'model opus']);
});

void test('a token source that is an API key or auth token is refused', async () => {
    for (const tokenSource of ['ANTHROPIC_AUTH_TOKEN', 'apiKeyHelper']) {
        const host = await refusedAtStart({ ...kSubscription, tokenSource });
        assert.deepEqual(host.take()[1], { kind: 'credential', billing: 'api-key', plan: 'Claude Ultra' });
        assert.equal(host.exitCode, 1);
        assert.deepEqual(host.harness.prompts, []);
    }
    const odd = await refusedAtStart({ apiProvider: 'firstParty', tokenSource: 'something-else' });
    assert.deepEqual(odd.take()[1], { kind: 'credential', billing: 'none' });
    assert.equal(odd.exitCode, 1);
});

// ---- Found by the model (models/ClaudeDriver.tla) ----

void test('settings carried by a turn stopped before it began reach the very next turn, even one sent at once', async () => {
    let release: () => void = () => undefined;
    const host = new TestHost();
    host.prepare = (h) => {
        h.accountInfo = () => new Promise((resolve) => { release = () => { resolve(kSubscription); }; });
    };
    const driver = new Driver(host, { account: 'main', cwd: '/work', settingSources: [] });
    driver.start();
    driver.feed(line({ kind: 'settings', model: 'opus' }));
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'a' }));
    driver.feed(line({ kind: 'stop', turn: 1 }));
    driver.feed(line({ kind: 'send', turn: 2, prompt: 'b' }));
    release();
    await settle();
    assert.deepEqual(host.harness.calls, ['model opus']);
    assert.deepEqual(host.harness.prompts, ['b']);
});

void test('a --resume id lost once the turn began: the new session runs with the settings in use', async () => {
    const host = new TestHost();
    const driver = new Driver(host, { account: 'main', cwd: '/work', resume: 'gone-3', model: 'opus', effort: 'low', settingSources: [] });
    driver.start();
    await settle();
    driver.feed(line({ kind: 'settings', model: 'sonnet', effort: 'max' }));
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'a' }));
    await settle();
    assert.deepEqual(host.harness.calls, ['model sonnet', 'effort max']);
    host.harness.emit(result('gone-3', { subtype: 'error_during_execution', errors: ['No conversation found with session ID: gone-3'] }));
    await settle();
    assert.equal(host.harnesses.length, 2);
    assert.equal(host.harness.start.model, 'sonnet');
    assert.equal(host.harness.start.effort, 'max');
    assert.deepEqual(host.harness.prompts, ['a']);
});

void test('a stopped turn whose --resume id is lost before its result still gets its done', async () => {
    const host = new TestHost();
    const driver = new Driver(host, { account: 'main', cwd: '/work', resume: 'gone-4', settingSources: [] });
    driver.start();
    await settle();
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'a' }));
    await settle();
    driver.feed(line({ kind: 'stop', turn: 1 }));
    await settle();
    host.take();
    host.harness.finish(new Error('Claude Code returned an error result: No conversation found with session ID: gone-4'));
    await settle();
    const out = host.take();
    assert.equal(errorOf(out).code, 'session-not-found');
    assert.deepEqual(out.filter((m) => m.kind === 'done'), [{ kind: 'done', turn: 1, outcome: 'stopped' }]);
    assert.deepEqual(host.harness.prompts, []);
    driver.feed(line({ kind: 'send', turn: 2, prompt: 'b' }));
    await settle();
    assert.deepEqual(host.harness.prompts, ['b']);
});

void test('a --resume id lost with a text block open: the block is ended before the notice', async () => {
    const host = new TestHost();
    const driver = new Driver(host, { account: 'main', cwd: '/work', resume: 'gone-5', settingSources: [] });
    driver.start();
    await settle();
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'a' }));
    await settle();
    host.take();
    host.harness.emit(...openText('gone-5', 'm1', 'part'));
    await settle();
    host.harness.finish(new Error('Claude Code returned an error result: No conversation found with session ID: gone-5'));
    await settle();
    assert.deepEqual(host.take().map((m) => m.kind).slice(0, 3), ['text', 'text-end', 'error']);
});

void test('a --resume id lost while settings are applied: the prompt waits for the new session\'s credential check', async () => {
    let applied: () => void = () => undefined;
    let release: () => void = () => undefined;
    const host = new TestHost();
    host.prepare = (h) => {
        if (h.start.resume === undefined) {
            h.accountInfo = () => new Promise((resolve) => { release = () => { resolve(kSubscription); }; });
        } else {
            h.setModel = (m) => { h.calls.push(`model ${m ?? ''}`); return new Promise((resolve) => { applied = resolve; }); };
        }
    };
    const driver = new Driver(host, { account: 'main', cwd: '/work', resume: 'gone-6', settingSources: [] });
    driver.start();
    await settle();
    driver.feed(line({ kind: 'settings', model: 'sonnet' }));
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'a' }));
    await settle();
    const first = host.harness;
    first.finish(new Error('Claude Code returned an error result: No conversation found with session ID: gone-6'));
    await settle();
    applied();
    await settle();
    assert.equal(host.harnesses.length, 2);
    assert.deepEqual(host.harness.prompts, []);
    assert.deepEqual(first.prompts, []);
    release();
    await settle();
    assert.deepEqual(host.harness.prompts, ['a']);
    assert.equal(host.harness.start.model, 'sonnet');
});

void test('parseArguments', () => {
    assert.deepEqual(parseArguments(['--account', 'a', '--cwd', '/w']), { account: 'a', cwd: '/w', settingSources: [] });
    assert.deepEqual(parseArguments(['--cwd', '/w', '--account', 'a', '--resume', 'abc', '--effort', 'max', '--setting-sources', 'user,local']),
        { account: 'a', cwd: '/w', resume: 'abc', effort: 'max', settingSources: ['user', 'local'] });
    // The server's --limit cwd: the one limit there is.
    assert.deepEqual(parseArguments(['--account', 'a', '--cwd', '/w', '--limit', 'cwd']),
        { account: 'a', cwd: '/w', limit: 'cwd', settingSources: [] });
    // The server's --keep-out, one per directory of friends.json's "limit_keeps_out": only with --limit cwd, each
    // absolute and not the root.
    assert.deepEqual(parseArguments(['--account', 'a', '--cwd', '/w', '--limit', 'cwd', '--keep-out', '/data', '--keep-out', '/net']),
        { account: 'a', cwd: '/w', limit: 'cwd', settingSources: [], keepsOut: ['/data', '/net'] });
    const keepAlone = parseArguments(['--account', 'a', '--cwd', '/w', '--keep-out', '/data']);
    assert.ok('error' in keepAlone && keepAlone.error.includes('comes with --limit cwd'));
    for (const dir of ['data', '/']) {
        const r = parseArguments(['--account', 'a', '--cwd', '/w', '--limit', 'cwd', '--keep-out', dir]);
        assert.ok('error' in r && r.error.includes('not an absolute directory'), dir);
    }
    const unknownLimit = parseArguments(['--account', 'a', '--cwd', '/w', '--limit', 'home']);
    assert.ok('error' in unknownLimit && unknownLimit.error.includes('the one it knows is --limit cwd'));
    const noAccount = parseArguments(['--cwd', '/w']);
    assert.ok('error' in noAccount && noAccount.error.includes('--account <name> is required'));
    for (const bad of [[], ['--cwd'], ['--account', 'a'], ['--account', '', '--cwd', '/w'], ['--cwd', '/w', '--x', 'y'],
        ['--account', 'a', '--cwd', '/w', '--effort', 'huge'], ['--account', 'a', '--cwd', '/w', '--resume', 'has space'],
        ['--account', 'a', '--cwd', '/w', '--setting-sources', 'policy'], ['--account', 'a', '--cwd', '/w', '--limit', 'home'],
        ['--account', 'a', '--cwd', '/w', '--limit']]) {
        assert.ok('error' in parseArguments(bad), bad.join(' '));
    }
});

void test('toolSummary is one line of what the tool acts on', () => {
    assert.equal(toolSummary('Bash', { command: 'ls\nsrc' }), 'ls src');
    assert.equal(toolSummary('Read', { file_path: '/a/b' }), '/a/b');
    assert.equal(toolSummary('X', {}), 'X');
    assert.equal(toolSummary('X', { n: 1 }), '{"n":1}');
    assert.equal(toolSummary('Bash', { command: 'x'.repeat(500) }).length, 200);
    assert.equal(toolSummary(kSecretToolName, { account: 'work', what: 'oauth-token', why: 'login expired' }),
        'work oauth-token: login expired');
    assert.equal(toolSummary(kSecretMcpToolName, { account: 'work', what: 'oauth-token', why: '' }),
        'work oauth-token');
});

void test('nexus_ask_secret over the SDK MCP server: writes ask-secret after server secrets, returns stored or refused', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'server', features: ['secrets'] }));
    const mcp = secretMcpServer(host.harness.start.askSecret);
    const [clientSide, serverSide] = InMemoryTransport.createLinkedPair();
    await mcp.instance.connect(serverSide);
    const client = new Client({ name: 'test', version: '1.0.0' });
    await client.connect(clientSide);
    try {
        const listed = await client.listTools();
        assert.deepEqual(listed.tools.map((t) => t.name), [kSecretToolName]);
        const spec = listed.tools[0];
        assert.ok(spec !== undefined);
        assert.deepEqual(spec.inputSchema.required, ['account', 'what', 'why']);
        assert.equal(spec._meta?.['anthropic/alwaysLoad'], true);

        driver.feed(line({ kind: 'send', turn: 1, prompt: 'sign in' }));
        await settle();
        const call1 = client.callTool({
            name: kSecretToolName,
            arguments: { account: 'work', what: 'oauth-token', why: 'the login expired' },
        });
        await settle();
        assert.deepEqual(host.take(), [
            { kind: 'ask-secret', ask: 1, account: 'work', what: 'oauth-token', why: 'the login expired' },
        ]);
        driver.feed(line({ kind: 'secret', ask: 1, outcome: 'stored', reason: 'unconfirmed' }));
        const got1 = await call1;
        assert.deepEqual(got1.content, [{ type: 'text', text: 'stored' }]);
        assert.equal(got1.isError, undefined);
        host.harness.emit(result(S));
        await settle();
        assert.deepEqual(host.take().map((m) => m.kind), ['done']);

        driver.feed(line({ kind: 'send', turn: 2, prompt: 'again' }));
        await settle();
        const call2 = client.callTool({
            name: kSecretToolName,
            arguments: { account: 'work', what: 'oauth-token', why: '' },
        });
        await settle();
        assert.deepEqual(host.take(), [
            { kind: 'ask-secret', ask: 2, account: 'work', what: 'oauth-token', why: '' },
        ]);
        driver.feed(line({ kind: 'secret', ask: 2, outcome: 'refused', reason: 'timeout' }));
        const got2 = await call2;
        assert.deepEqual(got2.content, [{ type: 'text', text: 'refused: timeout' }]);
        host.harness.emit(result(S));
        await settle();

        const scan = new LeakScan('ZxCvBnMqWeRtYuIoPaSdFgHjKl', 'work');
        for (const m of host.out) {
            assert.equal(scan.find(JSON.stringify(m)), '');
        }
        for (const l of host.logs) {
            assert.equal(scan.find(l), '');
        }
    } finally {
        await client.close();
        await mcp.instance.close();
    }
});

void test('nexus_ask_secret without a server line naming secrets answers refused: no-client without writing ask-secret', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'ask' }));
    await settle();
    assert.equal(await host.harness.start.askSecret('work', 'oauth-token', 'expired'), 'refused: no-client');
    assert.deepEqual(host.take(), []);
    host.harness.emit(result(S));
    await settle();

    // A server line that does not name "secrets" leaves asks refused locally too.
    const other = await started();
    other.driver.feed(line({ kind: 'server', features: ['later'] }));
    other.driver.feed(line({ kind: 'send', turn: 1, prompt: 'ask' }));
    await settle();
    assert.equal(await other.host.harness.start.askSecret('work', 'oauth-token', 'expired'), 'refused: no-client');
    assert.deepEqual(other.host.take(), []);
    other.host.harness.emit(result(S));
    await settle();
});

void test('a second concurrent nexus_ask_secret is refused busy locally while the first stays open', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'server', features: ['secrets'] }));
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'ask' }));
    await settle();
    const first = host.harness.start.askSecret('work', 'oauth-token', 'first');
    await settle();
    assert.equal(await host.harness.start.askSecret('work', 'oauth-token', 'second'), 'refused: busy');
    assert.deepEqual(host.take(), [
        { kind: 'ask-secret', ask: 1, account: 'work', what: 'oauth-token', why: 'first' },
    ]);
    driver.feed(line({ kind: 'secret', ask: 1, outcome: 'stored' }));
    assert.equal(await first, 'stored');
    host.harness.emit(result(S));
    await settle();
});

void test('stopping a turn cancels its open ask at once, and a late secret answer from that ask is skipped', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'server', features: ['secrets'] }));
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'one' }));
    await settle();
    const ask1 = host.harness.start.askSecret('work', 'oauth-token', 'expired');
    await settle();
    assert.deepEqual(host.take(), [
        { kind: 'ask-secret', ask: 1, account: 'work', what: 'oauth-token', why: 'expired' },
    ]);
    driver.feed(line({ kind: 'stop', turn: 1 }));
    assert.equal(await ask1, 'refused: cancelled');
    assert.equal(await host.harness.start.askSecret('work', 'oauth-token', 'while stopping'), 'refused: cancelled');
    host.harness.emit(result(S, { subtype: 'error_during_execution' }));
    await settle();
    assert.deepEqual(host.take().map((m) => m.kind), ['done']);

    // Turn 2 asks; the server's late answer to ask 1 arrives first and must not settle ask 2.
    driver.feed(line({ kind: 'send', turn: 2, prompt: 'two' }));
    await settle();
    let settled2: string | undefined;
    const ask2 = host.harness.start.askSecret('work', 'oauth-token', 'again').then((v) => { settled2 = v; return v; });
    await settle();
    assert.deepEqual(host.take(), [
        { kind: 'ask-secret', ask: 2, account: 'work', what: 'oauth-token', why: 'again' },
    ]);
    driver.feed(line({ kind: 'secret', ask: 1, outcome: 'stored' }));
    driver.feed(line({ kind: 'secret', ask: 99, outcome: 'stored' }));
    await settle();
    assert.equal(settled2, undefined);
    driver.feed(line({ kind: 'secret', ask: 2, outcome: 'refused', reason: 'no-client' }));
    assert.equal(await ask2, 'refused: no-client');
    host.harness.emit(result(S));
    await settle();
});

void test('a lost --resume id, a credential refusal, a crash or EOF cancels an open secret ask', async () => {
    // Lost --resume id while an ask is open: cancelled on the old harness, and the new harness can ask cleanly.
    const lostHost = new TestHost();
    const lostDriver = new Driver(lostHost, { account: 'main', cwd: '/work', resume: 'gone-ask', settingSources: [] });
    lostDriver.start();
    await settle();
    lostHost.take();
    lostDriver.feed(line({ kind: 'server', features: ['secrets'] }));
    lostDriver.feed(line({ kind: 'send', turn: 1, prompt: 'ask' }));
    await settle();
    const oldHarness = lostHost.harness;
    const lostAsk = oldHarness.start.askSecret('work', 'oauth-token', 'expired');
    await settle();
    oldHarness.finish(new Error('Claude Code returned an error result: No conversation found with session ID: gone-ask'));
    assert.equal(await lostAsk, 'refused: cancelled');
    await settle();
    // Stale call on the replaced harness does nothing.
    assert.equal(await oldHarness.start.askSecret('work', 'oauth-token', 'stale'), 'refused: cancelled');
    const freshAsk = lostHost.harness.start.askSecret('work', 'oauth-token', 'fresh');
    await settle();
    lostDriver.feed(line({ kind: 'secret', ask: 2, outcome: 'stored' }));
    assert.equal(await freshAsk, 'stored');
    lostHost.harness.emit(result('session-1'));
    await settle();

    // Overage refusal mid-ask.
    const over = await started();
    over.driver.feed(line({ kind: 'server', features: ['secrets'] }));
    over.driver.feed(line({ kind: 'send', turn: 1, prompt: 'ask' }));
    await settle();
    const overAsk = over.host.harness.start.askSecret('work', 'oauth-token', 'expired');
    over.host.harness.emit(rateLimit(S, { isUsingOverage: true }));
    assert.equal(await overAsk, 'refused: cancelled');

    // EOF mid-ask.
    const eof = await started();
    eof.driver.feed(line({ kind: 'server', features: ['secrets'] }));
    eof.driver.feed(line({ kind: 'send', turn: 1, prompt: 'ask' }));
    await settle();
    const eofAsk = eof.host.harness.start.askSecret('work', 'oauth-token', 'expired');
    eof.driver.end();
    assert.equal(await eofAsk, 'refused: cancelled');
});

void test('invalid nexus_ask_secret arguments are refused locally without writing a malformed ask-secret line', async () => {
    const { host, driver } = await started();
    driver.feed(line({ kind: 'server', features: ['secrets'] }));
    // Outside a turn: cancelled locally.
    assert.equal(await host.harness.start.askSecret('work', 'oauth-token', 'before send'), 'refused: cancelled');
    driver.feed(line({ kind: 'send', turn: 1, prompt: 'ask' }));
    await settle();
    const badCalls: [string, string, string][] = [
        ['', 'oauth-token', 'ok'],
        ['-flag', 'oauth-token', 'ok'],
        ['.dot', 'oauth-token', 'ok'],
        ['a/b', 'oauth-token', 'ok'],
        ['x'.repeat(65), 'oauth-token', 'ok'],
        ['work', '', 'ok'],
        ['work', '../oauth-token', 'ok'],
        ['work', 'oauth-token', 'two\nlines'],
        ['work', 'oauth-token', '\u202ereversed'],
        ['work', 'oauth-token', 'a'.repeat(201)],
    ];
    for (const [account, what, why] of badCalls) {
        const answer = await host.harness.start.askSecret(account, what, why);
        assert.match(answer, /^refused: malformed/, `${account} ${what} ${why}`);
    }
    assert.deepEqual(host.take(), []);
    assert.equal(host.exitCode, undefined);
    host.harness.emit(result(S));
    await settle();
});

void test('a malformed, duplicate or late server line, or a malformed secret line, is refused with exit 3', async () => {
    for (const bytes of [
        line({ kind: 'server' }),
        line({ kind: 'secret', ask: 1, outcome: 'refused' }),
    ]) {
        const host = await refusedLine(bytes);
        assert.equal(errorOf(host.take()).code, 'protocol');
        assert.equal(host.exitCode, 3);
    }
    const dup = await started();
    dup.host.untraced = 'a line the driver refuses';
    dup.driver.feed(line({ kind: 'server', features: ['secrets'] }));
    dup.driver.feed(line({ kind: 'server', features: ['secrets'] }));
    await settle();
    assert.equal(errorOf(dup.host.take()).code, 'protocol');
    assert.equal(dup.host.exitCode, 3);

    const late = await started();
    late.host.untraced = 'a line the driver refuses';
    late.driver.feed(line({ kind: 'send', turn: 1, prompt: 'a' }));
    late.driver.feed(line({ kind: 'server', features: ['secrets'] }));
    await settle();
    assert.equal(errorOf(late.host.take()).code, 'protocol');
    assert.equal(late.host.exitCode, 3);
});
