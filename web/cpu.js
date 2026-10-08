// CPU racers for VS mode.
//
// Kinoko simulates the karts (physics, checkpoints, laps) but has no CPU driving code: each CPU is a normal kart whose controller is
// written by the host every frame. This driver is NOT the game's own CPU AI (that code is not available); it follows the course's
// own enemy route (the KMP's ENPT/ENPH, the path the game's CPUs drive), aiming at a point a little ahead of it on its own lane,
// holding the accelerator, easing off before sharp bends, drifting through them and reversing out when it gets stuck. It is isolated
// here so the real behaviour can replace it.
(function () {
  'use strict';

  // The enemy route as one closed ring of points: start at the group nearest to `startPos` and follow each group's first "next" link
  // until the ring closes.
  function buildRing(route, startPos) {
    const P = route.points, G = route.groups;
    let g0 = 0, best = 1e18;
    G.forEach((g, i) => { for (let k = 0; k < g.len; k++) { const p = P[g.start + k], d = Math.hypot(p[0] - startPos[0], p[2] - startPos[1]); if (d < best) { best = d; g0 = i; } } });
    const ring = [], seen = new Set();
    let g = g0;
    while (g !== undefined && !seen.has(g)) {
      seen.add(g);
      for (let k = 0; k < G[g].len; k++) ring.push(P[G[g].start + k]);
      g = G[g].next[0];
    }
    return ring;
  }

  class CpuDriver {
    // G: the Kinoko module, idx: this racer's player index (1..11), ring: from buildRing
    constructor(G, idx, ring, opts = {}) {
      this.G = G; this.idx = idx; this.ring = ring;
      this.steerGain = opts.steerGain ?? 2.2;
      this.lane = opts.lane ?? (((idx * 0.618) % 1) - 0.5) * 1.2;   // -0.6..0.6 of each point's deviation
      this.lookBase = opts.lookBase ?? 900;                         // world units aimed ahead at standstill
      this.lookSpeed = opts.lookSpeed ?? 9;                         // extra units per unit of speed
      this.driftAngle = opts.driftAngle ?? 0.5;                     // heading error (radians) that starts a drift
      this.pos = G._malloc(12); this.rot = G._malloc(16); this.mot = G._malloc(20);
      this.cur = 0; this.reverse = 0; this.drifting = false;
      // Stuck detection: real displacement over a short window, not the kart's own speed field (which can read
      // nonzero while the kart is wedged against geometry with its wheels just spinning against a wall).
      this.hist = []; this.histLen = 45;                 // ~0.75s of positions
      this.stuckFrames = 0; this.cooldown = 0; this.escalation = 0; this.kick = 0; this.escapeDir = 1;
    }

    // Reads the racer's current state and writes the controller for the next physics step.
    // sinceStart: physics frames since the race started (negative before)
    drive(sinceStart = 999) {
      const G = this.G, F = G.HEAPF32, R = this.ring, n = R.length;
      G._kinoko_set_focus(this.idx);
      G._kinoko_get_kart_pos(this.pos); G._kinoko_get_kart_rot(this.rot); G._kinoko_get_kart_state(this.mot);
      G._kinoko_set_focus(0);
      const p = this.pos / 4, q = this.rot / 4;
      const px = F[p], pz = F[p + 2], qx = F[q], qy = F[q + 1], qz = F[q + 2], qw = F[q + 3];
      const fx = 2 * (qx * qz + qw * qy), fz = 1 - 2 * (qx * qx + qy * qy);      // heading: the kart's +z axis
      const speed = F[this.mot / 4];
      let stick = 0, buttons = 1;                                                 // hold A
      if (sinceStart >= 0 && n > 2) {
        // progress along the ring: the nearest point among the next few (never going back)
        let bi = this.cur, bd = 1e18;
        for (let k = 0; k < 8; k++) { const i = (this.cur + k) % n, d = Math.hypot(R[i][0] - px, R[i][2] - pz); if (d < bd) { bd = d; bi = i; } }
        this.cur = bi;
        // aim at the point `want` units further along the ring (on this racer's lane)
        const lanePt = i => { const a = R[i % n], b = R[(i + 1) % n]; let dx = b[0] - a[0], dz = b[2] - a[2]; const l = Math.hypot(dx, dz) || 1; dx /= l; dz /= l;
          return [a[0] - dz * a[3] * this.lane, a[2] + dx * a[3] * this.lane]; };
        let want = this.lookBase + this.lookSpeed * Math.max(0, speed), ax = px, az = pz, tx = px, tz = pz;
        for (let k = 1; k < 30; k++) {
          const [cx, cz] = lanePt(this.cur + k), seg = Math.hypot(cx - ax, cz - az);
          if (seg >= want) { const t = Math.min(1, want / seg); tx = ax + (cx - ax) * t; tz = az + (cz - az) * t; break; }
          want -= seg; ax = cx; az = cz; tx = cx; tz = cz;
        }
        const dx = tx - px, dz = tz - pz;
        const err = Math.atan2(fx * dz - fz * dx, fx * dx + fz * dz);              // + : the target is to the right of the heading (clockwise from above)
        stick = Math.max(-1, Math.min(1, err * this.steerGain));
        if (Math.abs(err) > 1.2 && speed > 50) buttons = 0;                       // sharp bend: lift off

        // Stuck detection: how far the kart has actually moved over the last histLen frames. A kart wedged in a
        // corner can still report a nonzero internal speed (wheels spinning against the wall), so this measures
        // real displacement instead.
        this.hist.push([px, pz]); if (this.hist.length > this.histLen) this.hist.shift();
        const progress = this.hist.length === this.histLen ? Math.hypot(px - this.hist[0][0], pz - this.hist[0][1]) : 1e9;
        if (this.cooldown > 0) this.cooldown--;

        if (this.reverse > 0) {
          this.reverse--; buttons = 2; stick = this.escapeDir;
          if (this.reverse === 0) { this.kick = 20; this.cooldown = this.histLen; this.hist = []; }
        } else if (this.kick > 0) {
          this.kick--; buttons = 1; stick = this.escapeDir;
        } else {
          if (progress < 500 && sinceStart > 120 && this.cooldown === 0) this.stuckFrames++; else this.stuckFrames = Math.max(0, this.stuckFrames - 2);
          if (progress > 2500) this.escalation = 0;                                // clearly moving again: forget past failures
          if (this.stuckFrames > 40) {
            // Escalating escape: each further attempt (without real progress in between) reverses longer and alternates
            // which way it turns, since repeating the exact same manoeuvre against the same wall just gets stuck again.
            this.reverse = 40 + Math.min(5, this.escalation) * 25;
            this.escapeDir = this.escalation % 2 === 0 ? 1 : -1;
            this.escalation++; this.stuckFrames = 0;
          }
        }

        // drift through bends
        const want_drift = !this.reverse && !this.kick && Math.abs(err) > this.driftAngle && speed > 70;
        if (want_drift) buttons |= 2 | 8;
        this.drifting = want_drift;
        this.dbg = { err: +err.toFixed(2), stick: +stick.toFixed(2), speed: +speed.toFixed(0), cur: this.cur, bd: Math.round(bd),
                     progress: Math.round(progress), stuck: this.stuckFrames, esc: this.escalation, rev: this.reverse, kick: this.kick };
      }
      G._kinoko_set_player_input(this.idx, buttons, stick, 0, 0);
    }
  }

  window.CpuDriver = CpuDriver;
  window.buildEnemyRing = buildRing;
})();
