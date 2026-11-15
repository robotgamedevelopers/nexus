// tests/leak_scan.h in TypeScript, for the driver's tests: how a test finds a fake secret value in what something
// wrote. The value, any run of 8 or more of its bytes, its hex (either case, any run of 8 or more of its bytes), its
// base64 (standard and URL-safe, at each of the three alignments, any run of 9 or more of its bytes), and its length
// written as a number within 64 bytes of the account's name. A test makes its value up, of letters found in nothing
// else it writes, so a match is a leak and never chance. Kept to the header's rules; the two change together.

export class LeakScan {
    private readonly needles: { needle: string; kind: string }[] = [];

    /** `account`: the account the value is for, whose name a printed length would sit beside; empty for none. */
    constructor(readonly value: string, private readonly account = '') {
        const kRun = 8;
        const bytes = Buffer.from(value, 'utf8');
        const latin = bytes.toString('latin1');
        for (let i = 0; i + kRun <= latin.length; i++) {
            this.needles.push({ needle: latin.slice(i, i + kRun), kind: 'a run of its bytes' });
        }
        if (latin.length < kRun && latin.length > 0) {
            this.needles.push({ needle: latin, kind: 'the value' });
        }
        for (const upper of [false, true]) {
            const h = upper ? bytes.toString('hex').toUpperCase() : bytes.toString('hex');
            for (let i = 0; i + 2 * kRun <= h.length; i += 2) {
                this.needles.push({ needle: h.slice(i, i + 2 * kRun), kind: 'its hex' });
            }
        }
        for (let shift = 0; shift < 3 && shift < bytes.length; shift++) {
            for (const url of [false, true]) {
                const b = bytes.subarray(shift).toString(url ? 'base64url' : 'base64');
                // The quads before the last, whose characters each depend on the value's bytes alone.
                const body = b.length >= 4 ? b.length - (b.length % 4 === 0 ? 4 : b.length % 4) : 0;
                for (let i = 0; i + 12 <= body; i += 4) {
                    this.needles.push({ needle: b.slice(i, i + 12), kind: 'its base64' });
                }
            }
        }
    }

    /** The needles by kind, in the order they were made, then by length: each looked up by window, not by search. */
    private groups(): { kind: string; length: number; set: Set<string> }[] {
        if (this.grouped === undefined) {
            const out: { kind: string; length: number; set: Set<string> }[] = [];
            for (const { needle, kind } of this.needles) {
                let g = out.find((x) => x.kind === kind && x.length === needle.length);
                if (g === undefined) {
                    g = { kind, length: needle.length, set: new Set<string>() };
                    out.push(g);
                }
                g.set.add(needle);
            }
            this.grouped = out;
        }
        return this.grouped;
    }

    private grouped: { kind: string; length: number; set: Set<string> }[] | undefined;

    /** What in `text` gives the value away ("its hex at byte 120"), or empty when nothing does. */
    find(text: string): string {
        const t = Buffer.from(text, 'utf8').toString('latin1');
        // Every window of each needle's length, against that length's needles: the same matches as searching for each
        // needle, in time linear in the text (a long value has thousands of needles).
        for (const { kind, length, set } of this.groups()) {
            for (let at = 0; at + length <= t.length; at++) {
                if (set.has(t.slice(at, at + length))) {
                    return `${kind} at byte ${at}`;
                }
            }
        }
        if (this.account !== '') {
            const length = String(Buffer.byteLength(this.value, 'utf8'));
            const digit = (c: string | undefined): boolean => c !== undefined && c >= '0' && c <= '9';
            for (let at = t.indexOf(length); at !== -1; at = t.indexOf(length, at + 1)) {
                if (digit(t[at - 1]) || digit(t[at + length.length])) {
                    continue;
                }
                const from = Math.max(0, at - 64 - this.account.length);
                const near = t.slice(from, at + length.length + 64 + this.account.length);
                if (near.includes(this.account)) {
                    return `its length (${length}) beside the account ${this.account} at byte ${at}`;
                }
            }
        }
        return '';
    }

    in(text: string): boolean {
        return this.find(text) !== '';
    }
}
