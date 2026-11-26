// The registry fixes the endpoint and sending identity. Only the host process can reach Redis.
import {spawn} from 'node:child_process';
import {homedir} from 'node:os';
import {join} from 'node:path';
import {tool} from '@anthropic-ai/claude-agent-sdk';
import {z} from 'zod';

const name = /^[a-zA-Z0-9][a-zA-Z0-9._-]{0,63}$/u;
export const sendName = 'nexus_bus_send';

export function busSendTool(grant: string) {
    const parts = grant.split('@');
    const [ai, endpoint] = parts;
    if (parts.length !== 2 || !ai || !name.test(ai) || !endpoint ||
        !/^127\.0\.0\.1:[1-9]\d{0,4}$/u.test(endpoint) || Number(endpoint.split(':')[1]) > 65535)
        throw new Error('Invalid message bus send grant.');
    return tool(sendName, 'Send a message to a peer from your own registered bus name.',
        {to: z.string().regex(name), subject: z.string().min(1).max(4096), body: z.string().max(1024 * 1024),
            reply_to: z.string().regex(/^\d+-\d+$/u).optional()},
        async ({to, subject, body, reply_to}) => {
            const home = homedir();
            const result = await new Promise<string>((resolve, reject) => {
                const child = spawn(join(home, '.local/bin/nova-secrets'), ['exec', '--only', 'NOVA_BUS_PASSWORD', '--',
                    join(home, '.local/bin/nova-bus'), 'send', '--as', ai, '--to', to,
                    ...(reply_to ? ['--re', reply_to] : []), '--', subject, '-'], {
                    cwd: home, env: {HOME: home, PATH: '/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin',
                        NOVA_BUS_USER: 'bus', NOVA_BUS_REDIS: endpoint}, timeout: 15000,
                    stdio: ['pipe', 'pipe', 'ignore']});
                let output = '';
                child.stdout.on('data', (chunk: Buffer) => { output = (output + chunk.toString()).slice(-4096); });
                child.once('error', reject);
                child.once('close', code => {
                    if (code === 0) resolve(output.trim());
                    else reject(new Error('Send returned no receipt. Check bus presence for the sender and recipient.'));
                });
                child.stdin.on('error', () => { /* close supplies the authoritative command result */ });
                child.stdin.end(body + '\n');
            });
            return {content: [{type: 'text' as const, text: result}]};
        });
}
