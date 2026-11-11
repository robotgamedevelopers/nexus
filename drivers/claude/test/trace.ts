// Traces of the driver for models/ClaudeDriverTrace.tla: each step the driver records through Host.step, with its
// state after the step and what the step wrote, written as a TLA+ module (ClaudeDriverTraceData.tla) that TLC checks
// against models/ClaudeDriver.tla.

import { writeFileSync } from 'node:fs';

import type { Changes, DriverState, StepArgs } from '../src/driver.js';
import type { DriverMessage } from '../src/protocol.js';

/** What a step wrote, as the model names it: a kind and the turn (0 for none). The hello, credential and session
 * lines are not the model's; a run of text pieces is one `text`. */
interface Wire {
    k: string;
    t: number;
}

const kFields = ['model', 'effort'] as const;

type Value = string | number | boolean | Value[] | { [key: string]: Value };

export type TraceRecord = Record<string, Value>;

/** One driver's life: its steps, in order. */
export class Trace {
    readonly records: TraceRecord[] = [];
    private wire: Wire[] = [];

    /** A line the driver wrote. */
    wrote(m: DriverMessage): void {
        let w: Wire | undefined;
        switch (m.kind) {
            case 'text': {
                const prev = this.wire[this.wire.length - 1];
                if (prev?.k === 'text' && prev.t === m.turn) {
                    return;
                }
                w = { k: 'text', t: m.turn };
                break;
            }
            case 'text-end':
                w = { k: 'text-end', t: m.turn };
                break;
            case 'tool':
                w = { k: 'tool', t: m.turn };
                break;
            case 'done':
                w = { k: `done-${m.outcome}`, t: m.turn };
                break;
            case 'error':
                w = { k: `error-${m.code}`, t: m.turn ?? 0 };
                break;
            default:
                return;
        }
        this.wire.push(w);
    }

    /** A step the driver recorded. */
    step(op: string, args: StepArgs, state: DriverState): void {
        const wrote = this.wire.map((w): Value => ({ k: w.k, t: w.t }));
        this.wire = [];
        this.records.push({
            op,
            turn: args.turn ?? 0,
            changes: settings(args.changes ?? {}, 'none'),
            kind: args.kind ?? 'none',
            code: args.code ?? 'none',
            phase: state.phase,
            resume: state.resume,
            open: state.open,
            stage: state.stage,
            stopping: state.stopping,
            textOpen: state.textOpen,
            failure: state.failure,
            interrupting: state.interrupting,
            pending: settings(state.pending, 'none'),
            carry: settings(state.carry, 'none'),
            inUse: settings(state.inUse, 'start'),
            last: state.last,
            wrote,
        });
    }
}

function settings(c: Changes, absent: string): Record<string, Value> {
    const out: Record<string, Value> = {};
    for (const f of kFields) {
        out[f] = c[f] ?? absent;
    }
    return out;
}

function tla(v: Value): string {
    if (typeof v === 'string') {
        return JSON.stringify(v);
    }
    if (typeof v === 'number') {
        return String(v);
    }
    if (typeof v === 'boolean') {
        return v ? 'TRUE' : 'FALSE';
    }
    if (Array.isArray(v)) {
        return `<<${v.map(tla).join(', ')}>>`;
    }
    return `[${Object.entries(v).map(([key, x]) => `${key} |-> ${tla(x)}`).join(', ')}]`;
}

/** The module ClaudeDriverTraceData: the instance and the traces. */
export function traceModule(traces: readonly Trace[], comment: string): string {
    let maxTurn = 1;
    const values = new Set<string>();
    for (const t of traces) {
        for (const r of t.records) {
            for (const key of ['turn', 'open', 'last', 'interrupting']) {
                const n = r[key];
                if (typeof n === 'number' && n > maxTurn) {
                    maxTurn = n;
                }
            }
            for (const key of ['changes', 'pending', 'carry', 'inUse']) {
                for (const v of Object.values(r[key] as Record<string, Value>)) {
                    if (typeof v === 'string' && v !== 'none' && v !== 'start') {
                        values.add(v);
                    }
                }
            }
        }
    }
    const lines = [
        '---- MODULE ClaudeDriverTraceData ----',
        `\\* ${comment}`,
        `TraceMaxTurn == ${String(maxTurn)}`,
        `TraceFields == {${kFields.map((f) => JSON.stringify(f)).join(', ')}}`,
        `TraceValues == {${[...values].sort().map((v) => JSON.stringify(v)).join(', ')}}`,
        'Traces == <<',
        traces.map((t) => `  <<\n${t.records.map((r) => `    ${tla(r)}`).join(',\n')}\n  >>`).join(',\n'),
        '>>',
        '====',
        '',
    ];
    return lines.join('\n');
}

export function writeTraceModule(path: string, traces: readonly Trace[], comment: string): void {
    writeFileSync(path, traceModule(traces, comment));
}
