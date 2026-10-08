// Protocol test for the relay. Start it first:  npx wrangler dev --port 8787   then:  node test.mjs [http://localhost:8787]
// Needs Node 22+ (built-in WebSocket client).
const BASE = process.argv[2] || 'http://localhost:8787';
const WS = BASE.replace(/^http/, 'ws');
let failed = 0;
const ok = (cond, what) => { console.log((cond ? 'ok   ' : 'FAIL ') + what); if (!cond) failed++; };
const sleep = ms => new Promise(r => setTimeout(r, ms));
const post = (path, body) => fetch(BASE + path, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) }).then(async r => ({ status: r.status, body: await r.json() }));

function client(room, id) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`${WS}/room/${room}?id=${id}`);
    const c = { ws, msgs: [], closed: null };
    ws.onmessage = e => { if (e.data === 'pong') return c.msgs.push('pong'); const m = JSON.parse(e.data); c.msgs.push(m); if (m.t === 'welcome') resolve(c); };
    ws.onclose = e => { c.closed = e.code; resolve(c); };
    ws.onerror = () => resolve(c);
    c.send = o => ws.send(typeof o === 'string' ? o : JSON.stringify(o));
    c.find = t => c.msgs.find(m => m && m.t === t);
  });
}

const room = 'T' + Math.random().toString(36).slice(2, 7).toUpperCase();

// --- signalling ---
const a = await client(room, 'aaaa1111');
ok(a.find('welcome') && a.find('welcome').peers.length === 0, 'first member gets an empty welcome');
const b = await client(room, 'bbbb2222');
ok(b.find('welcome') && b.find('welcome').peers.join() === 'aaaa1111', 'second member is told about the first');
await sleep(150);
ok(a.find('peer') && a.find('peer').id === 'bbbb2222', 'first member is told the second joined');

a.send({ t: 'send', to: 'bbbb2222', data: { k: 'offer', sdp: 'x'.repeat(5000) } });
await sleep(200);
const got = b.find('msg');
ok(got && got.from === 'aaaa1111' && got.data.k === 'offer' && got.data.sdp.length === 5000, 'a message is forwarded with its sender and payload intact');

a.send({ t: 'send', to: 'nobody99', data: {} });
await sleep(150);
ok(a.find('gone') && a.find('gone').id === 'nobody99', 'a send to an absent peer is reported as gone');

a.send('ping'); await sleep(150);
ok(a.msgs.includes('pong'), 'ping is answered with pong');

// a reload: same id joins again, the old socket is replaced and the others are told
const a2 = await client(room, 'aaaa1111');
await sleep(200);
ok(a.closed === 4001, 'the old socket is closed with 4001 when the same id rejoins (got ' + a.closed + ')');
ok(a2.find('welcome') && a2.find('welcome').peers.join() === 'bbbb2222', 'the rejoining member is welcomed with the remaining peers');
ok(b.msgs.filter(m => m && m.t === 'peer' && m.id === 'aaaa1111').length >= 1, 'the others are told it rejoined');

// bad input
const bad = await client('x', 'zz');
ok(bad.closed !== null && !bad.find('welcome'), 'a too-short room code is refused');
const spam = await client(room, 'cccc3333');
spam.send('x'.repeat(70 * 1024)); await sleep(300);
ok(spam.closed === 1009, 'an oversized message closes the socket (got ' + spam.closed + ')');

// leaving
b.ws.close(); await sleep(300);
ok(a2.msgs.some(m => m && m.t === 'left' && m.id === 'bbbb2222'), 'the others are told when a member leaves');

// room full
const full = [];
for (let i = 0; i < 12; i++) full.push(await client('F' + room, 'full' + String(i).padStart(4, '0')));
const extra = await client('F' + room, 'extra999');
ok(full.every(c => c.find('welcome')) && !extra.find('welcome'), 'the 13th member is refused (room limit is 12)');
full.forEach(c => c.ws.close()); extra.ws.close();

// --- lobby ---
const L = 'L' + Math.random().toString(36).slice(2, 6).toUpperCase();
let r = await post('/rooms', { room: L, name: 'Test <b>lobby</b>', host: 'Alice', track: 8, players: 1 });
ok(r.status === 200 && /^[0-9a-f]{32}$/.test(r.body.hostToken), 'registering a room returns a host token');
const token = r.body.hostToken;
let list = await (await fetch(BASE + '/rooms')).json();
const entry = list.rooms.find(x => x.room === L);
ok(entry && !/[<>]/.test(entry.name), 'the room is listed and angle brackets are stripped from the name');
ok(entry && entry.host === 'Alice' && entry.track === 8 && entry.players === 1 && entry.max === 12 && !('hostToken' in entry), 'the list shows public fields only (no token)');
r = await post('/rooms', { room: L, name: 'Hijack', host: 'Eve', track: 1, players: 1 });
ok(r.status === 409, 'another host cannot take a live room code (got ' + r.status + ')');
r = await post('/rooms', { room: L, name: 'Test lobby', host: 'Alice', track: 8, players: 1, hostToken: token });
ok(r.status === 200 && r.body.hostToken === token, 'the same host can re-register with its token');
r = await post('/rooms/update', { room: L, hostToken: 'wrong', players: 5 });
ok(r.status === 404, 'a wrong token cannot update the room (got ' + r.status + ')');
r = await post('/rooms/update', { room: L, hostToken: token, players: 3, track: 12 });
list = await (await fetch(BASE + '/rooms')).json();
ok(r.status === 200 && list.rooms.find(x => x.room === L).players === 3 && list.rooms.find(x => x.room === L).track === 12, 'the host can update the player count and track');
await post('/rooms/update', { room: L, hostToken: token, started: true });
list = await (await fetch(BASE + '/rooms')).json();
ok(!list.rooms.find(x => x.room === L), 'a started race disappears from the lobby');
await post('/rooms', { room: L, name: 'again', host: 'Alice', hostToken: token });
await post('/rooms/close', { room: L, hostToken: token });
list = await (await fetch(BASE + '/rooms')).json();
ok(!list.rooms.find(x => x.room === L), 'closing removes the room');
r = await post('/rooms', { room: 'a', name: 'bad' });
ok(r.status === 400, 'a bad room code is rejected');
const h = await fetch(BASE + '/health');
ok(h.status === 200, 'health endpoint answers');

console.log(failed ? `\n${failed} FAILED` : '\nall passed');
process.exit(failed ? 1 : 0);
