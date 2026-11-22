const { randomBytes, randomUUID, timingSafeEqual } = require('node:crypto');
const { mkdir, readFile, rename, unlink, writeFile } = require('node:fs/promises');
const { dirname, isAbsolute, join } = require('node:path');
const { setTimeout: delay } = require('node:timers/promises');

exports.name = 'fatfishbox-desktop-connection';
exports.inject = ['connection', 'webServer'];

async function legacyCookie(connection, origin, signal) {
    const url = new URL(connection.authenticatedUrl(origin + '/connect'));
    if (url.origin !== origin || url.pathname !== '/connect')
        throw new Error('Unexpected DSH API connection address');
    url.searchParams.set('interactions', 'none');
    const response = await fetch(url, {
        redirect: 'manual',
        signal: AbortSignal.any([signal, AbortSignal.timeout(5000)]),
    });
    const cookie = response.headers.getSetCookie()[0]?.split(';', 1)[0];
    await response.body?.cancel();
    if (response.status !== 302 || !cookie) throw new Error('DSH did not issue an API cookie');
    return cookie;
}

exports.apply = (ctx) => {
    const local = process.env.LOCALAPPDATA;
    if (process.platform !== 'win32' || !local || !isAbsolute(local)) return;
    const file = join(local, 'FatFishBox', 'dsh-desktop', 'auth-private.json');
    const connectionId = randomUUID();
    const temporary = file + '.' + connectionId + '.tmp';
    const lifetime = new AbortController();
    let release;
    const publish = async () => {
        // The profile can load before the Desktop listener receives its assigned port.
        for (let attempt = 0; !ctx.webServer.port && attempt < 150; ++attempt)
            await delay(200, undefined, { signal: lifetime.signal, ref: false });
        const { host, protocol, port } = ctx.webServer;
        if (!Number.isInteger(port) || !['http:', 'https:'].includes(protocol) ||
            (host !== undefined && host !== '127.0.0.1'))
            throw new Error('Expected a loopback DSH Desktop listener');
        const origin = protocol + '//127.0.0.1:' + port;
        const connection = ctx.connection;
        let cookie;
        if (connection.authentication) {
            const name = 'fatfishbox-dsh-session=';
            const secret = randomBytes(32).toString('base64url');
            const expected = Buffer.from(secret);
            const peer = connection.createPeer({ principal: { kind: 'owner' }, interactions: 'none' });
            const unregister = connection.authentication.register({
                authenticate(request) {
                    const header = request.headers instanceof Headers
                        ? request.headers.get('cookie') : request.headers.cookie;
                    if (typeof header !== 'string') return;
                    const matches = header.split(';').map(part => part.trim()).filter(part => part.startsWith(name));
                    if (matches.length !== 1) return;
                    const actual = Buffer.from(matches[0].slice(name.length));
                    if (actual.length === expected.length && timingSafeEqual(actual, expected)) return peer;
                },
            });
            release = async () => {
                await unregister();
                await peer.dispose();
            };
            cookie = name + secret;
        } else {
            cookie = await legacyCookie(connection, origin, lifetime.signal);
        }
        lifetime.signal.throwIfAborted();
        await mkdir(dirname(file), { recursive: true });
        try {
            await writeFile(temporary, JSON.stringify({
                origin, cookie, connectionId, desktopHostPid: process.pid,
            }), { flag: 'wx', mode: 0o600 });
            lifetime.signal.throwIfAborted();
            await rename(temporary, file);
        } finally {
            await unlink(temporary).catch(() => {});
        }
    };
    ctx.effect(() => {
        const published = publish().catch(() => {
            if (!lifetime.signal.aborted) console.warn('[FatFishBox] Could not publish the local DSH connection');
        });
        return async () => {
            lifetime.abort();
            await published;
            await release?.();
            const current = await readFile(file, 'utf8').then(JSON.parse).catch(() => null);
            if (current?.connectionId === connectionId) await unlink(file).catch(() => {});
        };
    }, 'FatFishBox local Desktop connection');
};
