// The host fixes each destination. Only arguments for the granted operation cross the socket.
import {execFile} from 'node:child_process';
import {mkdtempSync, rmSync} from 'node:fs';
import {createServer} from 'node:net';
import {homedir} from 'node:os';
import {join} from 'node:path';
import {gitGrant} from './git-grant.js';

interface Routes { repository: string; githubAlias: string; ghConfig: string; bench: string; benchUser: string; benchKey: string }

export function routeCommand(routes: Routes, args: string[], endpoint?: string) {
    const [command, ...rest] = args;
    const home = homedir();
    const env: Record<string, string> = {HOME: home, PATH: '/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin', GH_PROMPT_DISABLED: '1'};
    if (command === 'nova-bus' && rest.length === 1 && rest[0] === 'who' && endpoint)
        return {program: join(home, '.local/bin/nova-secrets'), args: ['exec', '--only', 'NOVA_BUS_PASSWORD', '--',
            join(home, '.local/bin/nova-bus'), 'who'], env: {...env, NOVA_BUS_REDIS: endpoint, NOVA_BUS_USER: 'bus'}};
    if (command === 'ssh' && rest[0] === routes.bench && rest.length >= 2)
        return {program: '/usr/bin/ssh', args: ['-T', '-oBatchMode=yes', '-oClearAllForwardings=yes',
            '-oForwardAgent=no', '-oForwardX11=no', '-oControlMaster=no', '-oControlPath=none',
            '-oStrictHostKeyChecking=yes', '-oIdentitiesOnly=yes', '-i', routes.benchKey, '-l', routes.benchUser, '--', routes.bench, ...rest.slice(1)], env};
    if (command !== 'gh' || rest.shift() !== 'api') throw Error('Use gh api, ssh to the configured bench, or nova-bus who.');
    const path = rest.shift() ?? '';
    const root = 'repos/' + routes.repository;
    if ((path !== 'user' && path !== root && !path.startsWith(root + '/')) ||
        !path.split('/').every(part => /^[a-zA-Z0-9_.-]+$/.test(part) && part !== '.' && part !== '..'))
        throw Error('API path outside the configured repository.');
    const options: string[] = [];
    while (rest.length) {
        const flag = rest.shift();
        const value = rest.shift();
        if (!flag || !value || value.startsWith('-') ||
            !((flag === '--jq' && /^\.[a-zA-Z0-9_.]+$/.test(value)) ||
              ((flag === '-X' || flag === '--method') && ['GET','POST','PATCH','PUT','DELETE'].includes(value)) ||
              ((flag === '-f' || flag === '--raw-field') && /^[a-zA-Z_][a-zA-Z0-9_]*=/.test(value))))
            throw Error('Unsupported gh api option. Use -X, -f, or a simple --jq field.');
        options.push(flag, value);
    }
    if (path === 'user' && options.some(arg => ['-f','--raw-field','POST','PATCH','PUT','DELETE'].includes(arg)))
        throw Error('Only reading the authenticated user is allowed.');
    return {program: 'gh', args: ['api', path, ...options], env: {...env, GH_CONFIG_DIR: routes.ghConfig}};
}

export function workRoutes(configuration: string, env: Record<string,string>, sender: string | undefined, driver: string) {
    const routes = JSON.parse(configuration) as Routes;
    if (!/^[\w-]+\/[\w.-]+$/.test(routes.repository) || routes.repository.includes('..') ||
        !/^[a-z0-9][a-z0-9-]*$/.test(routes.githubAlias) || !/^[a-z0-9][a-z0-9.-]*$/.test(routes.bench) ||
        !/^[a-z_][a-z0-9_-]*$/.test(routes.benchUser) || !routes.benchKey.startsWith('/') || !routes.ghConfig.startsWith('/')) throw Error('Invalid work routes in the server profile.');
    const git = gitGrant('git@' + routes.githubAlias + ':' + routes.repository + '.git', env,
        'git@github.com:' + routes.repository + '.git', true);
    const directory = mkdtempSync('/private/tmp/nexus-work-');
    const path = join(directory, 'commands');
    const bin = join(driver, 'bin/work');
    const listener = createServer({allowHalfOpen: true}, socket => {
        const chunks: Buffer[] = [];
        let size = 0;
        socket.setTimeout(15000, () => socket.destroy());
        socket.on('error', () => socket.destroy());
        socket.on('data', (part: Buffer) => {
            size += part.length;
            if (size > 65536) socket.destroy();
            else chunks.push(part);
        });
        socket.on('end', () => {
            try {
                if (socket.destroyed) return;
                const args: unknown = JSON.parse(Buffer.concat(chunks).toString());
                if (!Array.isArray(args) || !args.every((s: unknown): s is string => typeof s === 'string' && !s.includes('\0')))
                    throw Error('Invalid command arguments.');
                const cmd = routeCommand(routes, args, sender?.split('@')[1]);
                socket.setTimeout(0);
                const child = execFile(cmd.program, cmd.args, {cwd: homedir(), env: cmd.env, timeout: 600000,
                    maxBuffer: 8 * 1024 * 1024}, (err, stdout, stderr) => {
                    if (!socket.destroyed) socket.end(JSON.stringify({code: err ? 1 : 0, stdout, stderr}));
                });
                child.stdin?.end();
                socket.once('close', () => child.kill());
            } catch (err) {
                socket.end(JSON.stringify({code: 1, stderr: String(err), stdout: ''}));
            }
        });
    });
    listener.listen(path);
    listener.unref();
    process.once('exit', () => {listener.close(); rmSync(directory, {recursive: true, force: true});});
    return {sockets: [...git.sockets, path], bin, env: {...git.env, NEXUS_WORK_SOCKET: path, PATH: bin + ':' + (env['PATH'] ?? '/usr/bin:/bin')}};
}
