// The sandbox can listen as this friend. Only this host-side process can reach Redis.
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { createServer } from 'node:net';
import { homedir } from 'node:os';
import { join } from 'node:path';

export function busListener(friend: string, address: string) {
    if (process.platform !== 'darwin' || !/^[a-zA-Z0-9][a-zA-Z0-9._-]{0,63}$/u.test(friend) ||
        !/^127\.0\.0\.1:[1-9][0-9]{0,4}$/u.test(address) || Number(address.split(':')[1]) > 65535)
        throw new Error('Invalid message bus listener grant');
    const directory = mkdtempSync('/private/tmp/nexus-bus-');
    const path = join(directory, 'listen');
    let occupied = false;
    const listener = createServer(socket => {
        if (occupied) { socket.end('This friend already has a listener.\n'); return; }
        occupied = true;
        // No command or argument comes from this socket. The CLI supplies messages as text.
        const child = spawn(join(homedir(), '.local/bin/nova-secrets'), [
            'exec', '--only', 'NOVA_BUS_PASSWORD', '--', join(homedir(), '.local/bin/nova-bus'),
            'hook', '--as', friend, '--harness', 'claude',
        ], {
            cwd: homedir(), env: { HOME: homedir(), PATH: '/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin', NOVA_BUS_REDIS: address, NOVA_BUS_USER: 'bus' },
            stdio: ['ignore', 'pipe', 'pipe']
        });
        child.stdout.pipe(socket, {end: false});
        child.stderr.pipe(socket, {end: false});
        child.on('error', () => socket.end('Could not launch nova-bus.\n'));
        child.on('close', () => { occupied = false; socket.end(); });
        socket.on('error', () => child.kill());
        socket.on('close', () => child.kill());
    });
    listener.listen(path);
    listener.unref();
    process.once('exit', () => { listener.close(); rmSync(directory, {force: true, recursive: true}); });
    return path;
}
