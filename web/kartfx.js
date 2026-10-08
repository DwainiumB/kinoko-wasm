// Kart effects driven by the game state, played with the game's own particle effects (see nw4r_ef.js).
//
// Only effects whose trigger and attach point follow directly from the game data are played:
//   MT charged    rk_driftSpark1* (blue) / 2* (orange), attached to the rear wheels, plus the one-shot burst (...1T)
//   ground        terrain dust chosen from the KCL flag (type, variant) under the rear wheels
// (The game's own code that picks effects per event is not available, so nothing else is guessed.)
(function () {
  const F = { ACCEL: 1, BRAKE: 2, GROUND: 4, HOP: 8, DRIFT: 16, BOOST: 32, MUSHROOM: 64, WHEELIE: 128, TRICK: 256,
              LONG_AIR: 512, WALL: 1 << 10, SSMT: 1 << 18, JUMP_PAD: 1 << 11, ZIPPER: 1 << 12, CANNON: 1 << 13, RESPAWN: 1 << 14 };

  // What the kart is driving on, from the ground texture under it (a ray straight down onto the track)
  class SurfaceProbe {
    constructor(meshes) {
      this.meshes = meshes; this.ray = new THREE.Raycaster(); this.cache = new Map(); this.frame = 0;
      this.kind = null; this.color = null; this.down = new THREE.Vector3(0, -1, 0); this.o = new THREE.Vector3();
    }
    colorOf(map) {
      if (this.cache.has(map)) return this.cache.get(map);
      let out = null;
      try {
        const cv = document.createElement('canvas'); cv.width = cv.height = 8;
        const cx = cv.getContext('2d'); cx.drawImage(map.image, 0, 0, 8, 8);
        const d = cx.getImageData(0, 0, 8, 8).data;
        let r = 0, g = 0, b = 0;
        for (let i = 0; i < d.length; i += 4) { r += d[i]; g += d[i + 1]; b += d[i + 2]; }
        const n = d.length / 4;
        out = [r / n / 255, g / n / 255, b / n / 255];
      } catch (e) { /* cross-origin or not decoded yet */ }
      this.cache.set(map, out);
      return out;
    }
    // effect name for the ground under pos (or null on hard surfaces)
    classify(c) {
      if (!c) return null;
      const [r, g, b] = c, mx = Math.max(r, g, b), mn = Math.min(r, g, b), sat = mx > 0 ? (mx - mn) / mx : 0;
      if (mx > 0.75 && sat < 0.12) return 'snow';
      if (sat < 0.16 || mx < 0.15) return null;                         // grey: asphalt, stone
      if (g > r * 1.08 && g > b * 1.1) return 'grass';
      if (r >= g && g >= b && mx > 0.55) return 'sand';                   // bright yellow-tan
      if (r >= b) return 'dirt';
      return null;
    }
    update(pos, active) {
      if (!active || this.frame++ % 4) return this.kind;
      this.ray.set(this.o.set(pos.x, pos.y + 150, pos.z), this.down);
      const hit = this.ray.intersectObjects(this.meshes, false)[0];
      const map = hit && hit.object.material && hit.object.material.map;
      this.color = map ? this.colorOf(map) : null;
      this.kind = this.classify(this.color);
      return this.kind;
    }
  }

  // Terrain effects from the KCL flag under a wheel (base type, variant); see the KCL flag table on mkwiiki
  const SAND = 'rk_dirtSand', DIRT = 'rk_dirtSmokeA', GRASS = 'rk_weed', MUD = 'rk_mud', STONE = 'rk_stone', WATER = 'rk_water',
        SNOW = 'rk_snowRoad1', DEEPSNOW = 'rk_deepSnow', FLOWER = 'rk_flower', ICE = 'rk_ice';
  const TERRAIN = {
    0x00: { 1: DIRT, 5: SNOW },
    0x01: { 0: SAND, 1: DIRT, 5: SAND },
    0x02: { 0: SAND, 1: DIRT, 2: WATER, 3: GRASS, 4: SAND, 6: STONE, 7: STONE },
    0x03: { 0: SAND, 1: DIRT, 2: MUD, 4: GRASS, 5: SAND, 6: STONE },
    0x04: { 0: SAND, 1: DIRT, 2: MUD, 3: FLOWER, 4: GRASS, 5: DEEPSNOW, 6: SAND },
    0x05: { 0: ICE },
    0x16: { 7: MUD },
    0x17: { 2: GRASS, 4: GRASS, 6: DIRT },
  };
  const terrainFx = (type, variant) => (TERRAIN[type] && TERRAIN[type][variant]) || null;
  const SURFACE_FX = { sand: ['rk_dirtSand'], dirt: ['rk_dirtSmokeA'], grass: ['rk_weed'], snow: ['rk_snowRoad1'] };
  const sets = (prefix, parts) => parts.map(p => prefix + p);
  const DRIFT_PARTS = ['_Chip00', '_Spark00', '_Spark01'];

  class KartEffects {
    // api: kart (Object3D), tires () -> [Object3D] (kart-local tire holders, Kinoko order), radius () -> wheel radius
    constructor(lib, api) {
      this.lib = lib; this.api = api; this.active = new Map(); this.prev = { flags: 0, drift: 0, air: 0 };
      this.probe = api.probe; this.airFrames = 0; this.frame = 0; this.timed = [];
      this.m = new THREE.Matrix4(); this.t = new THREE.Matrix4();
    }

    // Plays a burst effect and stops it after `frames` physics steps. The mini-turbo stage-change burst
    // (rk_driftSpark*1T*) is marked in its own data as an infinite/looping emitter (commonFlag's "infinite
    // duration" bit is set, emitterLife 65535): in the real game whatever triggers it must also stop it a
    // short time later, which is normal for this effect system but isn't code we have, so left alone it
    // never stops and sits at the kart's wheel forever. How long the real trigger waits isn't known; this
    // guesses a quick flash long enough for a couple of the emitter's own particle bursts (interval ~4).
    burst(names, node, frames = 20) {
      const fx = names.filter(Boolean).map(n => this.lib.play(n, node)).filter(Boolean);
      if (fx.length) this.timed.push({ fx, until: this.frame + frames });
    }

    isBike() { return this.api.tires().length === 2; }

    // matrix of a point in kart space; the offset is given in kart-local coordinates
    at(x, y, z) {
      return () => {
        const k = this.api.kart; k.updateMatrixWorld(true);
        return new THREE.Matrix4().multiplyMatrices(k.matrixWorld, this.t.makeTranslation(x, y, z));
      };
    }

    // the bottom of a rear wheel (side: +1 left, -1 right, 0 centre of the axle)
    wheel(side) {
      return () => {
        const tires = this.api.tires(), r = this.api.radius(), k = this.api.kart;
        const rear = tires.filter(t => t.position.z < 0);
        const t = rear.find(w => side === 0 || Math.sign(w.position.x) === side) || rear[0];
        const p = t ? t.position : { x: side * 58, y: -1.4, z: -43 };
        k.updateMatrixWorld(true);
        return new THREE.Matrix4().multiplyMatrices(k.matrixWorld, this.t.makeTranslation(p.x, p.y - r, p.z));
      };
    }

    exhausts() {
      const tires = this.api.tires(), rear = tires.filter(t => t.position.z < 0);
      const z = (rear[0] ? rear[0].position.z : -43) - 22, y = (rear[0] ? rear[0].position.y : 0) + 8;
      return this.isBike() ? [[0, y, z]] : [[-22, y, z], [22, y, z]];
    }

    // start/stop a continuous group of effects
    hold(key, on, make) {
      const cur = this.active.get(key);
      if (on && !cur) this.active.set(key, make().filter(Boolean));
      else if (!on && cur) { cur.forEach(e => e.stop()); this.active.delete(key); }
    }

    once(names, node) { for (const n of names) this.lib.play(n, node); }

    update(s) {
      if (this.override) s = Object.assign({}, s, this.override);     // test hook: force a state from the console
      const lib = this.lib;
      if (!lib.data) return;
      this.frame++;
      // expire timed bursts (see burst()); the underlying effect data marks them infinite, so they need an explicit stop
      this.timed = this.timed.filter(t => {
        if (this.frame < t.until) return true;
        // A plain stop() only retires each effect's top-level emitter, and this resource doesn't mark itself to take
        // its children down with it (commonFlag bit 0x1, checked in Emitter.retire()); worse, each new particle can
        // spawn its own child emitter in turn (its "child effects" action track), which is itself unretired and
        // "infinite duration" too -- a self-perpetuating chain that a per-emitter retire can't catch all of. Marking
        // the whole Effect dead removes it from EffectSystem's own list outright, which stops that regardless of how
        // many descendants it has grown by now.
        t.fx.forEach(fx => { fx.dead = true; });
        return false;
      });
      const on = f => (s.flags & f) !== 0;
      const ground = on(F.GROUND), drift = on(F.DRIFT) && ground, boost = on(F.BOOST | F.MUSHROOM);
      const bike = this.isBike(), st = s.driftState | 0;
      const moving = s.speedRatio > 0.05;

      // --- mini-turbo charge sparks at the rear wheels (offsets in the effect data are relative to the wheel):
      //     blue set at the first stage, orange at the second, each with its one-shot burst when the stage is reached
      const ssmt = on(F.SSMT) && ground && !drift;             // stand-still mini-turbo charged (A + B held for 75 frames)
      const stage = drift ? st : ssmt ? 2 : 0;
      const blue = stage === 2, orange = stage === 3;
      const pre = (n) => bike ? [`rk_driftSpark${n}LB`, `rk_driftSpark${n}RB`] : [`rk_driftSpark${n}L`, `rk_driftSpark${n}R`];
      const build = (n) => {
        const [L, R] = pre(n).map(p => (lib.def(p + DRIFT_PARTS[1]) ? p : p.replace(/B$/, '')));
        return [...sets(L, DRIFT_PARTS).map(nm => lib.play(nm, this.wheel(1))), ...sets(R, DRIFT_PARTS).map(nm => lib.play(nm, this.wheel(-1)))];
      };
      this.hold('spark1', blue, () => build(1));
      this.hold('spark2', orange, () => build(2));
      if ((blue || orange) && this.prev.st !== stage) {
        const n = orange ? 2 : 1;
        const [L, R] = pre(n).map(p => p + '1T');
        for (const [p, side] of [[L, 1], [R, -1]]) {
          const names = DRIFT_PARTS.map(part => {
            const name = p + part, name2 = p.replace(/B1T$/, '1T') + part;
            return lib.def(name) ? name : lib.def(name2) ? name2 : null;
          });
          this.burst(names, this.wheel(side));
        }
      }
      this.prev.st = stage;

      // --- wheelie dust trail: kicked up by the rear wheel while the front wheel is up (bikes only
      // -- karts never get the WHEELIE status bit). Initially wired to rk_wheelie, but a real-footage
      // comparison showed that effect is wrong for this: it's a single fixed-direction jet (a
      // "nami"/gradient-tail texture, reads as a wave/splash effect meant for something else), while
      // real wheelie dust is a compact, symmetric puff hugging the ground right at the wheel.
      // rk_wheelSpin0/1/S match instead (same real-footage comparison): a disc emitter spanning a
      // wide arc behind the wheel with RADIAL outward velocity and real smoke textures
      // (rk_smokeQrt/rk_smokeDir) -- the same multi-part-effects-played-together pattern already used
      // for the drift-spark Chip00/Spark00/Spark01 set above. rk_wheelSpin2 is deliberately excluded:
      // its own angle range sits entirely to one side (-118 to -37 degrees, not centered) with an
      // extra 90-degree roll on top, and including it visibly skewed the whole puff sideways in a
      // real-footage comparison -- it likely belongs to a different context (e.g. a directional splash
      // aligned to some other axis) rather than this symmetric ground effect. All three mark themselves
      // infinite-duration like the MT sparks, so held/stopped explicitly.
      this.hold('wheelie', on(F.WHEELIE) && ground, () =>
              ['rk_wheelSpin0', 'rk_wheelSpin1', 'rk_wheelSpinS'].map(n => lib.play(n, this.wheel(0))));

      // --- dust of the surface being driven on
      // the game's KCL flags under the wheels when the physics reports them, else a guess from the ground colour
      let kind = null;
      if (s.floor && s.floor.length) {
        const rear = s.floor.slice(bike ? 1 : 2), pick = rear.find(f => f.type >= 0 && terrainFx(f.type, f.variant)) || null;
        kind = pick ? terrainFx(pick.type, pick.variant) : null;
      } else {
        const g = this.probe.update(s.pos, ground && moving);
        kind = g ? SURFACE_FX[g][0] : null;
      }
      // a hop lifts the wheels off the ground for a moment: keep the surface effect (and the last known surface) running through a short airborne spell instead of
      // stopping it and replaying its start-up burst on every landing (the ice effect re-appearing on each hop)
      const prevAir = this.air || 0;
      this.air = ground ? 0 : prevAir + 1;
      // rk_ice is a one-shot burst (its emitter lives a single frame), not a trail: it belongs to a landing, so it is played once when the kart comes down from a real
      // jump (not on a hop, and never while driving or accelerating on the ice)
      if (ground && prevAir >= 25 && s.floor && s.floor.some(f => f.type === 5 && f.variant === 0)) lib.play(ICE, this.at(0, this.api.groundY(), 0));
      // airborne, the floor under the wheels is not what is being driven on (a hop over grass beside the road reported grass): only a surface effect that was already
      // running carries through the short spell, nothing new starts
      if (ground) this.lastKind = kind;   // (also when it is null: hard road must not leave an old grass surface behind to be replayed in the air)
      const gnd = ground || this.air <= 20;
      if (!ground) kind = this.air <= 20 ? (this.lastKind || null) : null;
      const fxNames = kind && kind !== ICE && gnd && moving && !drift ? [kind] : null;
      const key = fxNames ? 'surface:' + kind : null;
      for (const k of [...this.active.keys()]) if (k.startsWith('surface:') && k !== key) this.hold(k, false);
      if (key) this.hold(key, true, () => fxNames.map(n => lib.play(n, this.at(0, this.api.groundY(), 0))));
    }
  }

  window.SurfaceProbe = SurfaceProbe;
  window.KartEffects = KartEffects;
})();
