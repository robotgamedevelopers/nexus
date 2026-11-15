// The driver's leak scan (test/leak-scan.ts) finds what tests/leak_scan.h finds: the value, a run of 8 of its bytes,
// its hex in either case, its base64 standard and URL-safe at each alignment, and its length beside the account's name.

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { LeakScan } from '../leak-scan.js';

void test('the scan finds the value in every form the header names, and nothing else', () => {
    const value = 'QwErTyUiOpAsDfGhJkLzXcVbNm';
    const scan = new LeakScan(value, 'main');
    const bytes = Buffer.from(value);
    assert.match(scan.find(`x ${value} y`), /a run of its bytes/);
    assert.match(scan.find(`..${value.slice(5, 13)}..`), /a run of its bytes/);
    assert.match(scan.find(bytes.toString('hex')), /its hex/);
    assert.match(scan.find(bytes.toString('hex').toUpperCase()), /its hex/);
    for (const shift of [0, 1, 2]) {
        assert.match(scan.find(`p${bytes.subarray(shift).toString('base64')}`), /its base64/, `shift ${shift}`);
        assert.match(scan.find(bytes.subarray(shift).toString('base64url')), /its base64/, `url shift ${shift}`);
    }
    assert.match(scan.find('accounts/main/oauth-token is 26 bytes'), /its length \(26\) beside the account main/);
    assert.equal(scan.find('accounts/main is 260 bytes'), '', 'a longer number is not the length');
    assert.equal(scan.find(`accounts/main${'.'.repeat(80)} 26 bytes`), '', 'too far from the account');
    assert.equal(scan.find(value.slice(0, 7)), '');
    assert.equal(new LeakScan('abc').find('xxabcxx'), 'the value at byte 2');
});
