// Online play transport for Kinoko web: rooms, WebRTC peer connections and the pose packet format.
//
// The page talks to a small signalling relay (server/worker, a Cloudflare Worker) only to set up WebRTC; every race message then goes
// peer to peer over two data channels per pair: 'ctrl' (reliable, JSON) and 'fast' (unordered, no retransmits, binary
// pose packets, so a lost packet is never waited for).
//
// Every player simulates only their own kart (Kinoko is deterministic); the others are drawn as live ghosts from the
// pose packets they stream, the same data a recorded ghost is drawn from.
(function () {
  'use strict';
  const ICE = [{ urls: 'stun:stun.l.google.com:19302' }, { urls: 'stun:stun1.l.google.com:19302' }];

  // ---- pose packet: one per physics step ----
  // 31 32-bit words: frame, pos[3], quat[4], flags, speed, hop, stick, stage, timeMs, spin, wheels[4] x {x, y, z, radius}
  const POSE_WORDS = 31;
  function packPose(p) {
    const b = new ArrayBuffer(POSE_WORDS * 4), f = new Float32Array(b), u = new Uint32Array(b), i = new Int32Array(b);
    u[0] = p.frame;
    f.set(p.pos, 1); f.set(p.quat, 4);
    u[8] = p.flags >>> 0; f[9] = p.speed; f[10] = p.hop; f[11] = p.stick;
    i[12] = p.stage; i[13] = p.timeMs; f[14] = p.spin;
    for (let w = 0; w < 4 && w < p.wheels.length; w++) f.set(p.wheels[w], 15 + 4 * w);
    return b;
  }
  function unpackPose(b) {
    if (b.byteLength !== POSE_WORDS * 4) return null;
    const f = new Float32Array(b), u = new Uint32Array(b), i = new Int32Array(b);
    return { frame: u[0], pos: f.subarray(1, 4), quat: f.subarray(4, 8), flags: u[8], speed: f[9], hop: f[10], stick: f[11],
             stage: i[12], timeMs: i[13], spin: f[14], wheels: f.subarray(15, 31) };
  }

  const randId = () => Array.from(crypto.getRandomValues(new Uint8Array(8)), x => x.toString(16).padStart(2, '0')).join('');

  class OnlineRoom {
    // cb: onPeerOpen(id), onPeerGone(id), onMsg(id, obj), onPose(id, pose), onStatus(text)
    constructor({ relay, room, id, cb }) {
      this.relay = relay.replace(/\/$/, ''); this.room = room; this.id = id || randId(); this.cb = cb || {};
      this.peers = new Map();     // id -> {pc, ctrl, fast, open, rtt, pendingIce, remoteSet}
      this.ws = null; this.closed = false; this.welcomed = false; this.retry = 0;
    }

    async connect() {
      await this.open();
      this.pinger = setInterval(() => this.broadcast({ t: 'ping', ts: performance.now() }), 1000);
    }

    // One WebSocket to the relay's room. Resolves once the relay has welcomed us (and we have started to meet the members
    // already inside); after that a dropped socket is re-opened by itself, because only a (re)join needs the relay.
    open() {
      return new Promise((ok, no) => {
        const url = `${this.relay.replace(/^http/, 'ws')}/room/${encodeURIComponent(this.room)}?id=${encodeURIComponent(this.id)}`;
        let ws; try { ws = this.ws = new WebSocket(url); } catch (e) { return no(new Error('bad relay address: ' + this.relay)); }
        let first = !this.welcomed;
        ws.onmessage = ev => {
          if (ev.data === 'pong') return;
          let m; try { m = JSON.parse(ev.data); } catch (e) { return; }
          if (m.t === 'welcome') {
            this.welcomed = true; this.retry = 0;
            // after a reconnect the players we are already connected to must not be torn down and re-offered
            m.peers.forEach(p => { const P = this.peers.get(p); if (!(P && P.open)) this.meet(p); });
            if (first) { first = false; ok(); } else this.cb.onStatus && this.cb.onStatus('Reconnected to the relay');
          } else this.onRelay(m).catch(e => console.warn('relay message', e));
        };
        ws.onopen = () => { clearInterval(this.keep); this.keep = setInterval(() => { try { ws.send('ping'); } catch (e) {} }, 25000); };
        ws.onclose = ev => {
          clearInterval(this.keep);
          if (this.closed || ws !== this.ws) return;
          if (ev.code === 4001) { this.closed = true; this.cb.onStatus && this.cb.onStatus('This room was joined from another tab'); return; }
          if (first) return no(new Error(ev.reason || `could not reach the relay at ${this.relay} (or the room is full)`));
          this.cb.onStatus && this.cb.onStatus('Lost the relay, reconnecting...');
          setTimeout(() => { if (!this.closed) this.open().catch(() => {}); }, Math.min(10000, 500 * 2 ** this.retry++));
        };
      });
    }

    // The peer with the greater id makes the offer, so two peers that find each other at once never collide.
    meet(id) {
      this.drop(id);
      const P = this.peers.get(id) || this.make(id);
      if (this.id > id) this.offer(id, P);
    }

    make(id) {
      const pc = new RTCPeerConnection({ iceServers: ICE });
      const P = { id, pc, ctrl: pc.createDataChannel('ctrl', { negotiated: true, id: 0 }),
                  fast: pc.createDataChannel('fast', { negotiated: true, id: 1, ordered: false, maxRetransmits: 0 }),
                  open: false, rtt: 0, pendingIce: [], remoteSet: false };
      P.fast.binaryType = 'arraybuffer';
      this.peers.set(id, P);
      pc.onicecandidate = e => { if (e.candidate) this.signal(id, { k: 'ice', c: e.candidate }); };
      P.ctrl.onopen = () => { P.open = true; this.cb.onPeerOpen && this.cb.onPeerOpen(id); };
      P.ctrl.onclose = () => { if (P.open) { P.open = false; this.cb.onPeerGone && this.cb.onPeerGone(id); } };
      pc.onconnectionstatechange = () => { if (pc.connectionState === 'failed') { this.cb.onStatus && this.cb.onStatus('Connection to a player failed (a strict NAT may need a TURN server)'); } };
      P.ctrl.onmessage = e => {
        let m; try { m = JSON.parse(e.data); } catch (x) { return; }
        if (m.t === 'ping') return this.sendTo(id, { t: 'pong', ts: m.ts });
        if (m.t === 'pong') { P.rtt = performance.now() - m.ts; return; }
        this.cb.onMsg && this.cb.onMsg(id, m);
      };
      P.fast.onmessage = e => { const p = e.data instanceof ArrayBuffer ? unpackPose(e.data) : null; if (p && this.cb.onPose) this.cb.onPose(id, p); };
      return P;
    }

    async offer(id, P) {
      const o = await P.pc.createOffer(); await P.pc.setLocalDescription(o);
      this.signal(id, { k: 'offer', sdp: P.pc.localDescription });
    }

    async onRelay(m) {
      if (m.t === 'peer') return this.meet(m.id);
      if (m.t !== 'msg') return;
      const id = m.from, d = m.data;
      let P = this.peers.get(id);
      if (d.k === 'offer') {
        this.drop(id); P = this.make(id);
        await P.pc.setRemoteDescription(d.sdp); P.remoteSet = true;
        const a = await P.pc.createAnswer(); await P.pc.setLocalDescription(a);
        this.signal(id, { k: 'answer', sdp: P.pc.localDescription });
        P.pendingIce.splice(0).forEach(c => P.pc.addIceCandidate(c).catch(() => {}));
      } else if (d.k === 'answer' && P) {
        await P.pc.setRemoteDescription(d.sdp); P.remoteSet = true;
        P.pendingIce.splice(0).forEach(c => P.pc.addIceCandidate(c).catch(() => {}));
      } else if (d.k === 'ice' && P) {
        if (P.remoteSet) P.pc.addIceCandidate(d.c).catch(() => {}); else P.pendingIce.push(d.c);
      }
    }

    signal(to, data) {
      if (this.ws && this.ws.readyState === 1) this.ws.send(JSON.stringify({ t: 'send', to, data }));
    }

    drop(id) {
      const P = this.peers.get(id); if (!P) return;
      this.peers.delete(id);
      const was = P.open; P.open = false;
      try { P.pc.close(); } catch (e) {}
      if (was) this.cb.onPeerGone && this.cb.onPeerGone(id);
    }

    openPeers() { return [...this.peers.values()].filter(p => p.open); }
    sendTo(id, obj) { const P = this.peers.get(id); if (P && P.open && P.ctrl.readyState === 'open') P.ctrl.send(JSON.stringify(obj)); }
    broadcast(obj) { const s = JSON.stringify(obj); this.openPeers().forEach(P => { if (P.ctrl.readyState === 'open') P.ctrl.send(s); }); }
    sendPose(buf) { this.openPeers().forEach(P => { if (P.fast.readyState === 'open') P.fast.send(buf); }); }

    close() {
      this.closed = true; clearInterval(this.pinger);
      clearInterval(this.keep);
      if (this.ws) { try { this.ws.close(1000); } catch (e) {} }
      [...this.peers.keys()].forEach(id => this.drop(id));
    }
  }

  // The public lobby list on the relay (see server/worker). Every call rejects with an Error whose message is the relay's reason.
  const lobbyCall = async (relay, path, body) => {
    const r = await fetch(relay.replace(/\/$/, '') + path, body === undefined ? {} : { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body), keepalive: true });
    let j = {}; try { j = await r.json(); } catch (e) {}
    if (!r.ok) throw new Error(j.error || `relay answered ${r.status}`);
    return j;
  };
  const OnlineLobby = {
    list: relay => lobbyCall(relay, '/rooms').then(j => j.rooms || []),
    register: (relay, info) => lobbyCall(relay, '/rooms', info),            // { room, name, host, track, players, hostToken? } -> { hostToken }
    update: (relay, info) => lobbyCall(relay, '/rooms/update', info),       // { room, hostToken, players?, track?, started? }
    close: (relay, info) => lobbyCall(relay, '/rooms/close', info),
  };

  window.OnlineRoom = OnlineRoom; window.OnlineLobby = OnlineLobby; window.packPose = packPose; window.unpackPose = unpackPose; window.POSE_WORDS = POSE_WORDS;
})();
