// Kinoko web online relay (Cloudflare Worker).
//
// It only introduces browsers to each other: the race data itself goes peer to peer over WebRTC. Two Durable Objects:
//   Room   one per room code. Holds the members' WebSockets (hibernation API, so an idle room costs nothing) and forwards
//          small JSON messages (WebRTC offers/answers/ICE) between them.
//   Lobby  one global instance: the list of public rooms. Hosts register a room, send a heartbeat every ~20 s and the
//          entry expires 75 s after the last one.
//
// HTTP API (all JSON, CORS as per ALLOWED_ORIGINS):
//   GET  /health
//   GET  /rooms                                  -> { rooms: [{ room, name, host, track, players, max, age }] }
//   POST /rooms          { room, name, host, track, players, hostToken? }  -> { hostToken }     register / re-register
//   POST /rooms/update   { room, hostToken, players?, track?, name?, started? }                  heartbeat
//   POST /rooms/close    { room, hostToken }
//   GET  /room/<code>?id=<peerId>   (WebSocket)
// WebSocket messages, client -> server: "ping" (answered with "pong") and { t:'send', to, data }.
// Server -> client: { t:'welcome', peers:[ids] } once, { t:'peer', id } when someone (re)joins, { t:'msg', from, data },
// { t:'left', id } and { t:'gone', id } (a send to someone who is not connected).
import { DurableObject } from 'cloudflare:workers';

const ROOM_RE = /^[A-Za-z0-9_-]{3,24}$/;
const ID_RE = /^[A-Za-z0-9_-]{4,40}$/;
const MAX_PLAYERS = 12;
const MAX_MSG = 64 * 1024;                          // characters per WebSocket message (an SDP is a few KB)
const RATE_WINDOW_MS = 10000, RATE_MAX = 400;       // messages per socket per window
const LOBBY_TTL_MS = 75000, LOBBY_MAX = 300;
const CREATE_WINDOW_MS = 60000, CREATE_MAX = 10;    // lobby registrations per client address per minute

const clean = (s, n) => String(s ?? '').replace(/[\u0000-\u001f\u007f<>]/g, '').trim().slice(0, n);
const int = (v, lo, hi, d) => { const n = Number.isFinite(+v) && v !== null && v !== '' ? Math.trunc(+v) : d; return n === null ? null : Math.min(hi, Math.max(lo, n)); };

// ---- CORS ---------------------------------------------------------------------------------------------------------------
function allowedOrigin(env, req) {
  const list = String(env.ALLOWED_ORIGINS || '*').split(',').map(s => s.trim()).filter(Boolean);
  const origin = req.headers.get('Origin');
  if (list.includes('*')) return origin || '*';
  return origin && list.includes(origin) ? origin : null;
}
const corsHeaders = (env, req) => {
  const o = allowedOrigin(env, req);
  return o ? { 'Access-Control-Allow-Origin': o, 'Vary': 'Origin', 'Access-Control-Allow-Headers': 'content-type', 'Access-Control-Allow-Methods': 'GET,POST,OPTIONS' } : {};
};
const reply = (env, req, status, obj) => new Response(JSON.stringify(obj), { status, headers: { 'content-type': 'application/json', 'cache-control': 'no-store', ...corsHeaders(env, req) } });

class HttpError extends Error { constructor(status, message) { super(message); this.status = status; } }
async function readJson(req) {
  const text = await req.text();
  if (text.length > 4096) throw new HttpError(413, 'body too large');
  try { return JSON.parse(text || '{}'); } catch (e) { throw new HttpError(400, 'bad json'); }
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    try {
      if (req.method === 'OPTIONS') return new Response(null, { status: 204, headers: corsHeaders(env, req) });
      if (url.pathname === '/health') return reply(env, req, 200, { ok: true });

      // A page that is not on the allow-list gets nothing (WebSockets are not covered by CORS, so check Origin by hand).
      if (req.headers.get('Origin') && !allowedOrigin(env, req)) return reply(env, req, 403, { error: 'origin not allowed' });

      const m = /^\/room\/([^/]+)$/.exec(url.pathname);
      if (m) {
        if (req.headers.get('Upgrade') !== 'websocket') return reply(env, req, 426, { error: 'expected a WebSocket' });
        const room = decodeURIComponent(m[1]), id = url.searchParams.get('id');
        if (!ROOM_RE.test(room) || !ID_RE.test(id || '')) return reply(env, req, 400, { error: 'bad room or id' });
        return env.ROOM.get(env.ROOM.idFromName(room.toUpperCase())).fetch(req);
      }

      if (url.pathname.startsWith('/rooms')) {
        const lobby = env.LOBBY.get(env.LOBBY.idFromName('lobby'));
        if (url.pathname === '/rooms' && req.method === 'GET') return reply(env, req, 200, { rooms: await lobby.list() });
        if (req.method !== 'POST') return reply(env, req, 405, { error: 'method not allowed' });
        const b = await readJson(req);
        if (!ROOM_RE.test(b.room || '')) return reply(env, req, 400, { error: 'bad room' });
        const room = String(b.room).toUpperCase();
        if (url.pathname === '/rooms') {
          const ip = req.headers.get('CF-Connecting-IP') || 'unknown';
          return reply(env, req, 200, await lobby.register({ room, name: b.name, host: b.host, track: b.track, players: b.players, hostToken: b.hostToken }, ip));
        }
        if (url.pathname === '/rooms/update') return reply(env, req, 200, await lobby.update({ ...b, room }));
        if (url.pathname === '/rooms/close') return reply(env, req, 200, await lobby.close({ room, hostToken: b.hostToken }));
      }
      return reply(env, req, 404, { error: 'not found' });
    } catch (e) {
      if (e instanceof HttpError) return reply(env, req, e.status, { error: e.message });
      // Errors raised on purpose inside the Lobby object are tagged "E:<status>:<message>".
      const mm = /E:(\d{3}):(.*)$/.exec(String(e && e.message));
      if (mm) return reply(env, req, +mm[1], { error: mm[2] });
      return reply(env, req, 500, { error: 'server error' });
    }
  },
};

// ---- Room ---------------------------------------------------------------------------------------------------------------
export class Room extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    // Keep-alive pings are answered without waking the object.
    this.ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('ping', 'pong'));
  }

  members() {
    return this.ctx.getWebSockets().map(ws => ({ ws, id: (ws.deserializeAttachment() || {}).id })).filter(m => m.id);
  }

  async fetch(req) {
    const id = new URL(req.url).searchParams.get('id');
    const others = this.members();
    const same = others.filter(m => m.id === id), rest = others.filter(m => m.id !== id);
    if (rest.length >= MAX_PLAYERS) return new Response(JSON.stringify({ error: 'room full' }), { status: 409, headers: { 'content-type': 'application/json' } });

    const pair = new WebSocketPair(), [client, server] = Object.values(pair);
    this.ctx.acceptWebSocket(server);
    server.serializeAttachment({ id, w: Date.now(), n: 0 });
    // The same id joining again (a page reload, or a second tab) replaces the old socket.
    same.forEach(m => { try { m.ws.serializeAttachment(null); m.ws.close(4001, 'replaced by a newer connection'); } catch (e) {} });
    server.send(JSON.stringify({ t: 'welcome', peers: rest.map(m => m.id) }));
    rest.forEach(m => { try { m.ws.send(JSON.stringify({ t: 'peer', id })); } catch (e) {} });
    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws, message) {
    const me = ws.deserializeAttachment();
    if (typeof message !== 'string' || message.length > MAX_MSG) { try { ws.close(1009, 'message too big'); } catch (e) {} return; }
    if (!me) return;
    const now = Date.now();
    if (now - me.w > RATE_WINDOW_MS) { me.w = now; me.n = 0; }
    if (++me.n > RATE_MAX) { try { ws.close(1008, 'too many messages'); } catch (e) {} return; }
    ws.serializeAttachment(me);

    let m; try { m = JSON.parse(message); } catch (e) { return; }
    if (!m || m.t !== 'send' || !ID_RE.test(m.to || '')) return;
    const target = this.members().find(x => x.id === m.to);
    if (!target) return ws.send(JSON.stringify({ t: 'gone', id: m.to }));
    try { target.ws.send(JSON.stringify({ t: 'msg', from: me.id, data: m.data })); } catch (e) { ws.send(JSON.stringify({ t: 'gone', id: m.to })); }
  }

  async webSocketClose(ws, code, reason) {
    const me = ws.deserializeAttachment();
    try { ws.close(code === 1005 || code === 1006 ? 1000 : code, reason); } catch (e) {}
    if (me && me.id) this.members().filter(m => m.ws !== ws && m.id !== me.id).forEach(m => { try { m.ws.send(JSON.stringify({ t: 'left', id: me.id })); } catch (e) {} });
  }

  async webSocketError(ws) { try { ws.close(1011, 'error'); } catch (e) {} }
}

// ---- Lobby --------------------------------------------------------------------------------------------------------------
export class Lobby extends DurableObject {
  constructor(ctx, env) { super(ctx, env); this.hits = new Map(); }

  async prune(now) {
    const all = await this.ctx.storage.list({ prefix: 'room:' });
    const live = [];
    for (const [key, e] of all) { if (now - e.updated > LOBBY_TTL_MS) await this.ctx.storage.delete(key); else live.push(e); }
    return live;
  }

  async list() {
    const now = Date.now();
    return (await this.prune(now)).filter(e => !e.started)
      .sort((a, b) => b.created - a.created)
      .map(e => ({ room: e.room, name: e.name, host: e.host, track: e.track, players: e.players, max: MAX_PLAYERS, age: Math.round((now - e.created) / 1000) }));
  }

  async register(b, ip) {
    const now = Date.now();
    const recent = (this.hits.get(ip) || []).filter(t => now - t < CREATE_WINDOW_MS);
    if (recent.length >= CREATE_MAX) throw new Error('E:429:too many rooms, try again in a minute');
    this.hits.set(ip, [...recent, now]);
    if (this.hits.size > 2000) this.hits.clear();

    const key = 'room:' + b.room, old = await this.ctx.storage.get(key);
    if (old && now - old.updated <= LOBBY_TTL_MS && old.hostToken !== b.hostToken) throw new Error('E:409:that room code is taken');
    if (!old && (await this.prune(now)).length >= LOBBY_MAX) throw new Error('E:503:the lobby is full, try again later');
    const entry = {
      room: b.room, name: clean(b.name, 32) || 'Room', host: clean(b.host, 16) || 'Player', track: int(b.track, 0, 63, null),
      players: int(b.players, 1, MAX_PLAYERS, 1), started: false, created: old ? old.created : now, updated: now,
      hostToken: old && old.hostToken === b.hostToken ? old.hostToken : crypto.randomUUID().replace(/-/g, ''),
    };
    await this.ctx.storage.put(key, entry);
    return { hostToken: entry.hostToken };
  }

  async update(b) {
    const key = 'room:' + b.room, e = await this.ctx.storage.get(key);
    if (!e || e.hostToken !== b.hostToken) throw new Error('E:404:no such room');
    if (b.players !== undefined) e.players = int(b.players, 1, MAX_PLAYERS, e.players);
    if (b.track !== undefined) e.track = int(b.track, 0, 63, e.track);
    if (b.name !== undefined) e.name = clean(b.name, 32) || e.name;
    if (b.started !== undefined) e.started = !!b.started;
    e.updated = Date.now();
    await this.ctx.storage.put(key, e);
    return { ok: true };
  }

  async close(b) {
    const key = 'room:' + b.room, e = await this.ctx.storage.get(key);
    if (e && e.hostToken === b.hostToken) await this.ctx.storage.delete(key);
    return { ok: true };
  }
}
