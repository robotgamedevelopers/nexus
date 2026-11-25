import {strict as assert} from 'node:assert';
import {test} from 'node:test';
import {routeCommand} from '../../src/work-routes.js';

const routes = {repository: 'org/project', githubAlias: 'github-example', ghConfig: '/private/gh',
    bench: 'vision', benchUser: 'worker', benchKey: '/private/key'};

void test('work commands stay within their configured accounts and destinations', () => {
    for (const path of ['user', 'repos/org/project', 'repos/org/project/pulls/42'])
        assert.equal(routeCommand(routes, ['gh', 'api', path]).env['GH_CONFIG_DIR'], '/private/gh');
    for (const args of [['gh','api','user','-X','PATCH'], ['gh','api','repos/org/project/../other'],
        ['gh','api','repos/org/project','--hostname','other'], ['gh','api','repos/org/other'],
        ['ssh','-oProxyCommand=test','hostname'], ['nova-bus','send']])
        assert.throws(() => routeCommand(routes, args, '127.0.0.1:6379'));
    assert.deepEqual(routeCommand(routes, ['ssh','vision','hostname']).args.slice(-5), ['-l','worker','--','vision','hostname']);
    assert.throws(() => routeCommand(routes, ['nova-bus','who']));
});
