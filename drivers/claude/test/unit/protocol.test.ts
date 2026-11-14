// Unit tests of the driver's protocol code: the server's lines refused by the reference reader's rules, the line
// reader over every chunking, and the encoder's lines.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { isApiKeyVariable, scrub } from '../../src/env.js';
import { cut, decodeServerLine, encode, isSessionId, kMaxLineBytes, LineReader, oneLine } from '../../src/protocol.js';

function decode(text: string): ReturnType<typeof decodeServerLine> {
    return decodeServerLine(Buffer.from(text));
}

/** Why `bytes` is refused (empty if it is not). */
function refusal(bytes: Buffer): string {
    const d = decodeServerLine(bytes);
    return 'error' in d ? d.error : '';
}

void test('the server\'s messages', () => {
    assert.deepEqual(decode('{"kind":"send","turn":1,"prompt":"hi"}'), { message: { kind: 'send', turn: 1, prompt: 'hi' } });
    assert.deepEqual(decode('{"kind":"send","turn":1.0e0,"prompt":"","extra":[1]}'), { message: { kind: 'send', turn: 1, prompt: '' } });
    assert.deepEqual(decode('{"kind":"stop","turn":9007199254740991}'), { message: { kind: 'stop', turn: 9007199254740991 } });
    assert.deepEqual(decode('{"kind":"settings","model":"opus"}'), { message: { kind: 'settings', model: 'opus' } });
    assert.deepEqual(decode('{"kind":"settings","model":null,"effort":"low"}'), { message: { kind: 'settings', effort: 'low' } });
    assert.deepEqual(decode('{"kind":"pause"}'), { message: { kind: 'unknown', name: 'pause' } });
    assert.deepEqual(decode('{"kind":"stop","turn":1,"turn":2}'), { message: { kind: 'stop', turn: 2 } });
});

void test('refusals, each with its cause and the turn when it is readable', () => {
    const refused: [string, RegExp, number?][] = [
        ['{"kind":"send","turn":7}', /no "prompt"/, 7],
        ['{"kind":"send","turn":0,"prompt":"x"}', /"turn"/],
        ['{"kind":"send","turn":1.5,"prompt":"x"}', /"turn"/],
        ['{"kind":"send","turn":9007199254740992,"prompt":"x"}', /"turn"/],
        ['{"kind":"send","turn":"1","prompt":"x"}', /"turn"/],
        ['{"kind":"send","turn":2,"prompt":5}', /not a string/, 2],
        ['{"kind":"stop"}', /no "turn"/],
        ['{"kind":"settings"}', /neither/],
        ['{"kind":"settings","model":""}', /empty "model"/],
        ['{"kind":"settings","effort":3}', /not a string/],
        ['{"turn":3}', /no "kind"/, 3],
        ['{"kind":""}', /no "kind"/],
        ['[1]', /not an object/],
        ['nope', /not JSON/],
        ['{"kind":"send","turn":4,"prompt":"\\ud83d"}', /lone surrogate/, 4],
        ['[' .repeat(1025) + ']'.repeat(1025), /nested too deeply/],
    ];
    for (const [text, cause, turn] of refused) {
        const d = decode(text);
        assert.ok('error' in d, text);
        assert.match(d.error, cause, text);
        assert.equal(d.turn, turn, text);
    }
    const atLimit = decode('{"kind":"x","a":' + '['.repeat(1023) + ']'.repeat(1023) + '}');
    assert.deepEqual(atLimit, { message: { kind: 'unknown', name: 'x' } });
    assert.match(refusal(Buffer.from([0xef, 0xbb, 0xbf, 0x7b, 0x7d])), /byte-order mark/);
    assert.match(refusal(Buffer.from([0x22, 0xc0, 0xaf, 0x22])), /not UTF-8/);
    assert.match(refusal(Buffer.from([0x22, 0xed, 0xa0, 0x80, 0x22])), /not UTF-8/);
});

void test('the line reader, over every chunking', () => {
    const stream = Buffer.from('{"a":1}\r\n\n{"b":"é"}\nlast');
    for (let size = 1; size <= stream.length; size++) {
        const r = new LineReader();
        const lines: string[] = [];
        for (let i = 0; i < stream.length; i += size) {
            for (const item of r.feed(stream.subarray(i, i + size))) {
                lines.push('line' in item ? Buffer.from(item.line).toString() : `!${item.error}`);
            }
        }
        for (const item of r.finish()) {
            lines.push(`!${item.error}`);
        }
        assert.equal(lines.length, 4, `size ${size}: ${JSON.stringify(lines)}`);
        assert.deepEqual(lines.slice(0, 3), ['{"a":1}', '', '{"b":"é"}']);
        assert.match(lines[3] ?? '', /^!the stream ended inside a line/);
    }
});

void test('the line limit: refused once when passed, dropped to its newline, the next line read', () => {
    const at = Buffer.alloc(kMaxLineBytes, 0x61);
    const r = new LineReader();
    assert.deepEqual(r.feed(Buffer.concat([at, Buffer.from('\n')])).map((i) => 'line' in i), [true]);
    const over = r.feed(Buffer.concat([at, Buffer.from('b')]));
    assert.equal(over.length, 1);
    assert.ok('error' in (over[0] ?? {}));
    assert.deepEqual(r.feed(Buffer.from('more\nnext\n')).map((i) => ('line' in i ? Buffer.from(i.line).toString() : 'error')), ['next']);
    assert.deepEqual(r.finish(), []);
});

void test('the lines the driver writes', () => {
    assert.equal(encode({ kind: 'hello', protocol: 1, harness: 'claude' }), '{"kind":"hello","protocol":1,"harness":"claude"}\n');
    assert.equal(encode({ kind: 'error', cause: 'c', fix: 'f', code: 'harness', turn: 2 }), '{"kind":"error","cause":"c","fix":"f","code":"harness","turn":2}\n');
    assert.equal(encode({ kind: 'text', turn: 1, delta: 'a b' }).split('\n').length, 2);
    assert.equal(oneLine(' a\tb\r\nc d\u0085 '), 'a b c d');
    assert.equal(oneLine('\ud83d'), '�');
    assert.equal(cut('abcdef', 4), 'abc…');
    assert.equal(cut('ab\u{1F600}c', 4), 'ab…');
    assert.ok(isSessionId('5f0c2a8e-1d34-4b7e-9a51-3c6f2b0d9e77'));
    for (const bad of ['', '-x', 'a b', 'x'.repeat(257), 'é']) {
        assert.ok(!isSessionId(bad), bad);
    }
});

void test('the environment loses every API-key and inherited login variable', () => {
    const { env, removed } = scrub({
        ANTHROPIC_API_KEY: 'k', ANTHROPIC_AUTH_TOKEN: 't', ANTHROPIC_BASE_URL: 'u', CLAUDE_CODE_USE_BEDROCK: '1',
        AWS_BEARER_TOKEN_BEDROCK: 'b', CLAUDECODE: '1', CLAUDE_CODE_ENTRYPOINT: 'cli', VERTEX_API_KEY: 'v',
        CLAUDE_CODE_OAUTH_TOKEN: 'o', CLAUDE_CONFIG_DIR: '/c', PATH: '/bin', HOME: '/h', SSH_AUTH_SOCK: '/s',
    });
    assert.deepEqual(env, { PATH: '/bin', HOME: '/h', SSH_AUTH_SOCK: '/s' });
    assert.deepEqual(removed, ['ANTHROPIC_API_KEY', 'ANTHROPIC_AUTH_TOKEN', 'ANTHROPIC_BASE_URL', 'AWS_BEARER_TOKEN_BEDROCK',
        'CLAUDECODE', 'CLAUDE_CODE_ENTRYPOINT', 'CLAUDE_CODE_OAUTH_TOKEN', 'CLAUDE_CODE_USE_BEDROCK', 'CLAUDE_CONFIG_DIR',
        'VERTEX_API_KEY']);
    assert.ok(isApiKeyVariable('ANTHROPIC_FOUNDRY_API_KEY'));
    assert.ok(!isApiKeyVariable('GITHUB_TOKEN'));
});
