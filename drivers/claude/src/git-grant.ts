// Two repository-scoped Git protocol streams. Only the driver starts ssh and selects its destination.
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { createServer } from 'node:net';

const remotePattern = /^git@([a-z0-9][a-z0-9-]*):([a-zA-Z0-9_-]+\/[a-zA-Z0-9_.-]+\.git)$/u;

export function gitGrant(remote: string, env: Record<string, string>) {
    const match = remotePattern.exec(remote);
    const alias = match?.[1];
    const repo = match?.[2];
    if (process.platform !== 'darwin' || !alias || !repo || repo.includes('..')) throw new Error('Invalid GitHub repository grant for this platform');
    const directory = mkdtempSync('/private/tmp/nexus-git-');
    const sockets: string[] = [];
    const listeners = ['git-upload-pack', 'git-receive-pack'].map(service => {
        const path = directory + '/' + service;
        sockets.push(path);
        const listener = createServer({allowHalfOpen: true}, socket => {
            const child = spawn('/usr/bin/ssh', ['-T', '-oBatchMode=yes', '-oStrictHostKeyChecking=yes',
                '-oClearAllForwardings=yes', '-oForwardAgent=no', '-oHostname=github.com', '-p', '22',
                'git@' + alias, service + ' ' + repo], {stdio: ['pipe', 'pipe', 'inherit']});
            child.stdin.on('error', () => socket.destroy());
            socket.on('error', () => child.kill());
            child.on('error', () => socket.destroy());
            socket.on('close', () => child.kill());
            socket.pipe(child.stdin);
            child.stdout.pipe(socket);
        });
        listener.listen(path);
        listener.unref();
        return listener;
    });
    process.once('exit', () => {
        for (const listener of listeners) listener.close();
        rmSync(directory, {recursive: true, force: true});
    });
    const i = Number(env['GIT_CONFIG_COUNT'] ?? 0);
    if (!Number.isSafeInteger(i) || i < 0) throw new Error('Invalid Git config count');
    return {sockets, env: {...env, GIT_CONFIG_COUNT: String(i + 1),
        ['GIT_CONFIG_KEY_' + String(i)]: 'url.nexusgit::home.insteadOf', ['GIT_CONFIG_VALUE_' + String(i)]: remote,
        NEXUS_GIT_SOCKET_DIR: directory}};
}
