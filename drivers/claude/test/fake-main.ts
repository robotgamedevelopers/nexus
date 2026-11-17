// The Claude driver as a process over a scripted query instead of the SDK: what the protocol conformance test
// (tests/driver/conformance_test.cpp, "prompted") starts. Never reaches Claude. A prompt "reply" is answered with a
// text block and a successful result; "wait" with a text block that stays open until the driver interrupts, then the
// result an interrupt gives (error_during_execution); "ask-secret" calls start.askSecret for work's oauth-token and
// streams the answer ("stored" or "refused: <reason>") as its text block; anything else with an error result. Its
// account is made up: no file is read, and no token reaches anything.

import { runProcess } from '../src/run.js';
import { blockStop, FakeHarness, init, openText, result, streamedText } from './fake-harness.js';

let replies = 0;
runProcess((start) => {
    const h = new FakeHarness(start);
    const session = start.sessionId ?? start.resume ?? 'fake';
    h.onPrompt = (text) => {
        replies++;
        const id = `m${replies}`;
        if (text === 'reply') {
            h.emit(init(session), ...streamedText(session, id, ['hello ', 'there']), result(session));
        } else if (text === 'wait') {
            h.emit(init(session), ...openText(session, id, 'partial'));
            h.onInterrupt = () => {
                h.emit(blockStop(session), result(session, { subtype: 'error_during_execution' }));
            };
        } else if (text === 'ask-secret') {
            void start.askSecret('work', 'oauth-token', 'the login expired').then((answer) => {
                h.emit(init(session), ...streamedText(session, id, [answer]), result(session));
            });
        } else {
            h.emit(init(session), result(session, { subtype: 'error_during_execution', errors: [`no script for ${text}`] }));
        }
    };
    return h;
}, [], (name) => ({ name, dir: '/nonexistent/accounts/' + name, configDir: '/nonexistent/harness/claude/' + name,
    token: 'conformance' }), () => undefined);
