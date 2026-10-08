// A port of the Wii's nw4r::ef particle engine (as used by Mario Kart Wii) to JavaScript / three.js.
//
// The structure follows the engine's own decompiled sources (kiwi515/ogws, src/nw4r/ef): EffectSystem ->
// Effect -> Emitter -> ParticleManager -> Particle, the per-frame Effect::Calc order, emission forms, velocity
// maths, child emitters through the creation queue, billboard drawing, and the GX TEV colour combiner
// (as a generated fragment shader). Data comes from tools/export_effects.py (RKRace.breff / .breft).
//
// Parts the decompilation lacks (animation-curve executors, fields other than gravity / air resistance / random,
// the directional draw code) are reconstructed from the file data; they are marked "reconstructed".
// One simulation step is one game frame (59.94 Hz).
(function () {
  'use strict';
  const FRAME = 1 / 59.94, PI = Math.PI;

  // ---- random (nw4r::ef::Random) ------------------------------------------------------------------------
  class Random {
    constructor(seed = 0) { this.s = seed >>> 0; }
    srand(s) { this.s = s >>> 0; }
    mix() { this.s = (Math.imul(this.s, 0x343FD) + 0x269EC3) >>> 0; }
    rand() { this.mix(); return this.s >>> 16; }
    float() { this.mix(); return (this.s >>> 16) / 0x10000; }
  }
  const sysRandom = new Random(0);

  // ---- matrices (three.js Matrix4 stands in for MTX34; the bottom row is always 0 0 0 1) ---------------------
  const _E = new THREE.Euler(0, 0, 0, 'ZYX');
  const rotXYZ = (x, y, z) => new THREE.Matrix4().makeRotationFromEuler(_E.set(x, y, z, 'ZYX'));   // Rz * Ry * Rx
  const scaleM = (v) => new THREE.Matrix4().makeScale(v.x, v.y, v.z);
  const transM = (v) => new THREE.Matrix4().makeTranslation(v.x, v.y, v.z);
  const IDENT = new THREE.Matrix4();

  // rotation-only, scale-only and translation parts of a matrix (Emitter::RestructMatrix)
  function restruct(orig, inheritS, inheritR, inheritT) {
    if (inheritS && inheritR && inheritT === 100) return orig.clone();
    if (!inheritS && !inheritR && inheritT === 0) return new THREE.Matrix4();
    const out = new THREE.Matrix4();
    if (inheritT !== 0) {
      const t = new THREE.Vector3().setFromMatrixPosition(orig).multiplyScalar(inheritT / 100);
      out.multiply(transM(t));
    }
    if (inheritR) {
      const x = new THREE.Vector3().setFromMatrixColumn(orig, 0), y = new THREE.Vector3().setFromMatrixColumn(orig, 1);
      if (x.lengthSq() > 0) x.normalize(); else x.set(1, 0, 0);
      if (y.lengthSq() > 0) y.normalize(); else y.set(0, 1, 0);
      const z = new THREE.Vector3().crossVectors(x, y);
      y.crossVectors(z, x);
      out.multiply(new THREE.Matrix4().makeBasis(x, y, z));
    }
    if (inheritS) {
      const s = new THREE.Vector3().setFromMatrixScale(orig);
      out.multiply(scaleM(s));
    }
    return out;
  }

  // ---- animation curves ---------------------------------------------------------------------------------------
  // Track flags: 0x04 sync, 0x08 stop, 0x10 emitter timing, 0x20 infinite loop, 0x40 turn, 0x80 fitting
  function curveFrame(a, tick, life) {
    const n = Math.max(1, a.frames);
    let f = tick;
    if ((a.proc & 0x80) && life !== Infinity && life > 0) f = (tick * n) / life;
    if (a.proc & 0x20) f = f % n;
    else if (a.proc & 0x40) { const m = f % (2 * (n - 1) || 1); f = m > n - 1 ? 2 * (n - 1) - m : m; }
    else f = Math.min(f, n - 1);
    return f;
  }
  function sampleCurve(a, f) {
    if (a.samples) {
      const n = a.samples.length, x = Math.min(Math.max(f, 0), n - 1), i = Math.floor(x), t = x - i;
      const A = a.samples[i], B = a.samples[Math.min(i + 1, n - 1)];
      return A.map((v, k) => v + (B[k] - v) * t);
    }
    const ks = a.keys;
    if (!ks || !ks.length) return null;
    if (f <= ks[0][0]) return ks[0][1].slice();
    for (let i = 1; i < ks.length; i++) {
      if (f <= ks[i][0]) {
        const A = ks[i - 1], B = ks[i], t = (f - A[0]) / Math.max(1e-6, B[0] - A[0]);
        return A[1].map((v, k) => v + (B[1][k] - v) * t);
      }
    }
    return ks[ks.length - 1][1].slice();
  }
  // a curve that only carries a random range (one key, frame 0, value 0)
  function rangeOnly(a) {
    const ks = a.keys;
    return !!(a.range && a.frames <= 1 && ks && ks.length === 1 && ks[0][1].every(v => v === 0));
  }
  // deterministic per-particle random in [0,1) for range tables
  function seedRand(seed, salt) {
    let s = (Math.imul((seed + salt * 40503) >>> 0, 0x343FD) + 0x269EC3) >>> 0;
    s = (Math.imul(s, 0x343FD) + 0x269EC3) >>> 0;
    return (s >>> 16) / 0x10000;
  }

  // ---- particle parameter (nw4r::ef::ParticleParameter) ----------------------------------------------------
  function newParam(P) {
    return {
      color: [[P.color1A.slice(), P.color1B.slice()], [P.color2A.slice(), P.color2B.slice()]],
      size: P.psize.slice(), scale: P.pscale.slice(), rotate: P.prot.slice(),
      texScale: P.tscale.map(v => v.slice()), texRot: P.trot.slice(), texTrans: P.ttrans.map(v => v.slice()),
      tex: P.tex.slice(), wrap: P.wrap, texReverse: P.texReverse, aRef: [P.alphaRef[0], P.alphaRef[1]],
      rotOff: [0, 0, 0], velocity: new THREE.Vector3(), position: new THREE.Vector3(), prevPosition: new THREE.Vector3(),
      momentum: 1,
    };
  }
  const COLOR_KIND = { 0: [0, 0], 4: [0, 1], 8: [1, 0], 12: [1, 1] };          // rgb of layer/index
  const ALPHA_KIND = { 3: [0, 0], 7: [0, 1], 11: [1, 0], 15: [1, 1] };

  class Particle {
    constructor(pm, life, pos, vel, momentum) {
      this.pm = pm; this.tick = 0; this.life = Math.max(1, life);
      this.status = 'wait';               // evaluation status: wait -> done
      this.dead = false; this.calcRemain = 0;
      const em = pm.emitter, P = pm.res.p;
      this.param = newParam(P);
      const par = this.param;
      // rotation offsets (radians -> stored as 0..255 turns)
      for (let i = 0; i < 3; i++) {
        let rad = P.rotOffset[i];
        if (P.rotOffsetRandom[i] > 0) rad += (P.rotOffsetRandom[i] * (rad * (2 * em.random.float() - 1))) / 100;
        par.rotOff[i] = Math.ceil((rad / PI) * 128 - 0.5) % 256;
      }
      this.seed = em.random.rand() & 0xFFFF;
      this.flickRnd = (em.random.rand() % 254) - 127;
      par.momentum = momentum;
      par.position.copy(pos); par.prevPosition.copy(pos); par.velocity.copy(vel);
      this.prevAxis = null;
    }
  }

  // ---- particle manager ---------------------------------------------------------------------------------------
  class ParticleManager {
    constructor(emitter, res, inherit) {
      this.emitter = emitter; this.res = res; this.d = res.e; this.particles = [];
      this.flagS = inherit.s; this.flagR = inherit.r; this.inhT = inherit.t; this.weight = inherit.w;
      this.dirty = true; this.mtx = new THREE.Matrix4();
      this.retired = false;
    }
    globalMtx() {
      if (this.dirty) { this.mtx = restruct(this.emitter.globalMtx(), this.flagS, this.flagR, this.inhT); this.dirty = false; }
      return this.mtx;
    }
    setDirty() { this.dirty = true; }
    create(life, pos, vel, space, momentum, setting, refParticle, calcRemain) {
      if (this.particles.length >= 900) return null;
      const p = new Particle(this, life, pos, vel, momentum);
      const par = p.param;
      if (space) {
        par.position.applyMatrix4(space);
        xdir(par.velocity, space);
        par.prevPosition.copy(par.position);
      }
      // inheritance from the reference (parent) particle (Particle::Initialize)
      if (refParticle && setting) {
        const rp = refParticle.param;
        if (setting.speed) {
          const gs = xdir(rp.velocity.clone(), refParticle.pm.globalMtx());
          xdir(gs, this.globalMtx().clone().invert());
          par.velocity.addScaledVector(gs, setting.speed / 100);
        }
        if (setting.scale) { par.size[0] = (setting.scale * drawSizeX(refParticle)) / 100; par.size[1] = (setting.scale * drawSizeY(refParticle)) / 100; }
        if (setting.alpha) for (let i = 0; i < 2; i++) for (let j = 0; j < 2; j++) par.color[i][j][3] = (setting.alpha * rp.color[i][j][3]) / 100;
        if (setting.color) for (let i = 0; i < 2; i++) for (let j = 0; j < 2; j++) for (let k = 0; k < 3; k++) par.color[i][j][k] = (setting.color * rp.color[i][j][k]) / 100;
        if (setting.flag & 2) for (let k = 0; k < 3; k++) par.rotOff[k] = Math.ceil(((rp.rotate[k] + rp.rotOff[k] * PI / 128) / PI) * 128 - 0.5) % 256;
      }
      p.calcRemain = calcRemain | 0;
      this.particles.push(p);
      return p;
    }
    // ParticleManager::Calc
    calc(effect) {
      const res = this.res, nInit = res.nPtclInit, tracks = res.ptclTracks;
      for (const p of this.particles) {
        if (p.dead || p.status !== 'wait') continue;
        p.status = 'done';
        if (p.calcRemain) effect.existCalcRemain = true;
        p.param.prevPosition.copy(p.param.position);
        if (p.life <= p.tick) { p.dead = true; continue; }
        const st = { addVel: new THREE.Vector3(), constVel: new THREE.Vector3(), mulVel: 1 };
        for (let i = p.tick === 0 ? 0 : nInit; i < tracks.length; i++) {
          const a = tracks[i];
          if (a.proc & 0x08) continue;                                    // stopped
          if (p.tick !== 0 && a.frames <= 1 && a.curve !== 7) continue;   // constants only apply at birth
          let tick, life, seed;
          if (a.proc & 0x10) {                                            // driven by the emitter's clock
            const em = this.emitter;
            tick = em.tick; life = (em.comFlags & 4) ? Infinity : em.emitSpan; seed = em.randSeed;
          } else { tick = p.tick; life = p.life; seed = p.seed; }
          this.exec(p, a, tick, life, seed, st, effect);
        }
        // integrate: fields change the velocity, then the position moves by velocity * momentum
        const par = p.param;
        par.velocity.multiplyScalar(st.mulVel);
        par.velocity.add(st.addVel);
        par.position.addScaledVector(par.velocity, par.momentum);
        par.position.addScaledVector(st.constVel, par.momentum);
        p.tick++;
      }
      this.particles = this.particles.filter(p => !p.dead);
    }
    exec(p, a, tick, life, seed, st, effect) {
      const par = p.param;
      switch (a.curve) {
        case 0: {                                                          // byte tracks: colours, alpha, alpha compare
          if (a.kind === 119 || a.kind === 120) { const v = valueOf(a, tick, life, seed, 0); if (v) par.aRef[a.kind - 119] = v[0]; return; }
          const isA = ALPHA_KIND[a.kind], ci = isA || COLOR_KIND[a.kind];
          if (!ci) return;
          const target = par.color[ci[0]][ci[1]];
          const cv = (a.keys || a.samples) ? sampleCurve(a, curveFrame(a, tick, life)) : null;
          a.comps.forEach((c, i) => {
            let v;
            if (a.range && a.range[i]) v = a.range[i][0] * (1 - (a.range[i][1] / 100) * seedRand(seed, c)) + (cv && !rangeOnly(a) ? cv[i] : 0);
            else if (cv) v = cv[i];
            if (v !== undefined) target[isA ? 3 : c] = Math.min(255, Math.max(0, v));
          });
          return;
        }
        case 3: {                                                          // float tracks
          const v = valueOf(a, tick, life, seed, 0);
          if (!v) return;
          const set = (arr) => a.comps.forEach((c, i) => { if (c < arr.length) arr[c] = v[i]; });
          switch (a.kind) {
            case 16: set(par.size); break;
            case 24: set(par.scale); break;
            case 44: set(par.texScale[0]); break; case 52: set(par.texScale[1]); break; case 60: set(par.texScale[2]); break;
            case 68: par.texRot[0] = v[0]; break; case 72: par.texRot[1] = v[0]; break; case 76: par.texRot[2] = v[0]; break;
            case 80: set(par.texTrans[0]); break; case 88: set(par.texTrans[1]); break; case 96: set(par.texTrans[2]); break;
          }
          return;
        }
        case 6: { const v = valueOf(a, tick, life, seed, 0); if (v) a.comps.forEach((c, i) => { par.rotate[c] = v[i]; }); return; }   // rotate
        case 4: {                                                           // texture pattern: pick a texture by name
          if (a.kind === 104 && a.names && a.names.length) par.tex[0] = a.names[a.names.length > 1 ? Math.floor(seedRand(seed, 7) * a.names.length) % a.names.length : 0];
          return;
        }
        case 5: {                                                           // child effects: reconstructed from the key table
          for (const c of a.children || []) {
            if (c.frame !== tick) continue;
            const name = a.names[c.name];
            const res = name && effect.sys.res(name);
            if (res) effect.queue.push({ res, setting: c, particle: p });
          }
          return;
        }
        case 7: {                                                           // fields (reconstructed)
          const w = a.info ? a.info.match(/.{8}/g).map(h => parseInt(h, 16)) : null;
          if (!w) return;
          const f32 = u => { const dv = new DataView(new ArrayBuffer(4)); dv.setUint32(0, u); return dv.getFloat32(0); };
          switch (a.kind) {
            case 0: {                                                       // gravity: power along the direction given by angles
              const power = f32(w[1]), rx = f32(w[2]), ry = f32(w[3]), rz = f32(w[4]);
              const sx = Math.sin(rx), cx = Math.cos(rx), sy = Math.sin(ry), cy = Math.cos(ry), sz = Math.sin(rz), cz = Math.cos(rz);
              const dir = new THREE.Vector3(sx * sy * cz - cx * sz, sx * sy * sz + cx * cz, sx * cy);
              if ((w[0] >>> 24) & 1) xdir(dir, p.pm.globalMtx());    // local-space field
              // Flag 0x10000: the power is a steady speed instead of an acceleration. Evidence: the ember effect of Grumble
              // Volcano (power 2.5 for particles living 800 frames in a volume 2000 units tall = one crossing per life)
              if ((w[0] >>> 16) & 1) st.constVel.addScaledVector(dir, power); else st.addVel.addScaledVector(dir, power);
              break;
            }
            case 1: st.mulVel *= f32(w[1]); break;                          // air resistance
            case 7: {                                                       // random: a kick every `interval` frames
              const interval = Math.max(1, w[0] & 0xFF), power = f32(w[1]);
              if (p.tick % interval === 0) st.addVel.add(new THREE.Vector3(sysRandom.float() * 2 - 1, sysRandom.float() * 2 - 1, sysRandom.float() * 2 - 1).multiplyScalar(power));
              break;
            }
            default: break;                                                 // magnet, newton, vortex, spin, tail: not used by the kart effects
          }
          return;
        }
        default: return;
      }
    }
  }
  const scaleOf = (m) => new THREE.Vector3().setFromMatrixScale(m).x;
  // rotate/scale a direction by the 3x3 part of m (no translation, no normalisation)
  function xdir(v, m) { const e = m.elements, x = v.x, y = v.y, z = v.z; return v.set(e[0] * x + e[4] * y + e[8] * z, e[1] * x + e[5] * y + e[9] * z, e[2] * x + e[6] * y + e[10] * z); }

  // value(s) of a curve at tick, with the random range of range-table curves (size, rotation, ...)
  function valueOf(a, tick, life, seed) {
    if (!a.keys && !a.samples) return null;
    const cv = sampleCurve(a, curveFrame(a, tick, life));
    if (!a.range || !rangeOnly(a) || !(a.curve === 3 || a.curve === 6)) return cv;
    // range entry per component = (max, slope): the value is spread from max down to max + 32768 * slope;
    // all components of one particle share the same random number (size x and y keep their proportion)
    const u = seedRand(seed, 3);
    return a.comps.map((c, i) => (a.range[i] ? rangeValue(a.range[i], u) : cv[i]));
  }
  const rangeValue = (r, u) => r[0] + r[1] * 32768 * u;                  // max -> max + 32768*slope (the minimum)

  function drawSizeX(p) { return p.param.size[0] * p.param.scale[0]; }
  function drawSizeY(p) {
    const f = p.pm.d.drawFlags & 0x6000, par = p.param;
    if (f === 0x4000) return par.size[1] * par.scale[0];
    if (f === 0x2000) return par.size[0] * par.scale[1];
    if (f === 0x6000) return par.size[0] * par.scale[0];
    return par.size[1] * par.scale[1];
  }

  // ---- emission forms (nw4r/ef/emform) --------------------------------------------------------------------------
  const EPS = 1.1920929e-7, ANGLE_MIN = 1.917476e-4, ANGLE_MAX = 2 * 3.1414969;
  const V = (x = 0, y = 0, z = 0) => new THREE.Vector3(x, y, z);
  const nz = (v) => (v.lengthSq() > 0 ? v.normalize() : v);

  function calcVelocity(em, normal, fromOrigin, fromYAxis) {
    const vel = new THREE.Vector3();
    if (em.velRadiation !== 0) vel.copy(fromOrigin).multiplyScalar(em.velRadiation);
    if (em.velYAxis !== 0) vel.addScaledVector(fromYAxis, em.velYAxis);
    if (em.velRandom !== 0) {
      const rx = em.random.float() * PI * 2, ry = em.random.float() * PI * 2, rz = em.random.float() * PI * 2;
      const sr = Math.sin(rx), cr = Math.cos(rx), sp = Math.sin(ry), cp = Math.cos(ry), sh = Math.sin(rz), ch = Math.cos(rz);
      vel.x += (cr * sp * ch + sr * sh) * em.velRandom;
      vel.y += (cr * sp * sh - sr * ch) * em.velRandom;
      vel.z += em.velRandom * cr * cp;
    }
    if (em.velNormal !== 0) vel.addScaledVector(normal, em.velNormal);
    if (em.velSpec !== 0) {
      const base = rotXYZ(em.specDir[0], em.specDir[1], em.specDir[2]);
      if (em.velDiffusionSpec === 0) {
        vel.addScaledVector(V(0, 1, 0).applyMatrix4(base), em.velSpec);
      } else {
        const m = rotXYZ(em.random.float() * em.velDiffusionSpec, em.random.float() * PI * 2, 0);
        const both = base.clone().multiply(m);
        const e = both.elements;                                           // second column = rotated Y axis
        vel.add(V(e[4], e[5], e[6]).multiplyScalar(em.velSpec));
      }
    }
    if (em.velInitRandom !== 0) vel.multiplyScalar(1 - em.velInitRandom * 0.01 * (em.random.float() * 2 - 1));
    return vel;
  }
  function calcLife(em, life, lifeRnd) {
    let l = life;
    if (lifeRnd !== 0) {
      const r = life * em.random.float() * lifeRnd;
      if (l - r < 1) l = 1; else if (l - r > 65535) l = 65535; else l -= Math.trunc(r);
    }
    return l;
  }
  function emitOne(em, pm, pos, normal, fromOrigin, fromYAxis, life, lifeRnd, space) {
    const vel = calcVelocity(em, normal, fromOrigin, fromYAxis);
    pm.create(calcLife(em, life, lifeRnd), pos, vel, space, 1 + em.velMomentumRandom * 0.01 * em.random.float(), em.inheritSetting, em.refParticle, em.calcRemain);
  }

  const FORMS = {
    9(em, pm, count, flags, P, life, lr, space) {                          // point
      for (let i = 0; i < count; i++) {
        const n = V(); let rx = em.random.float() * 2 - 1;
        n.x = rx >= 0 ? (0.66 + 0.34 * rx) * rx : (0.66 - 0.34 * rx) * rx;
        const radius = Math.sqrt(1 - n.x * n.x); rx = em.random.float() * PI * 2;
        n.y = Math.cos(rx) * radius; n.z = Math.sin(rx) * radius;
        const fy = V(n.x, 0, n.z); if (n.x !== 0 || n.z !== 0) fy.normalize();
        emitOne(em, pm, V(), n, n.clone(), fy, life, lr, space);
      }
    },
    0(em, pm, count, flags, P, life, lr, space) {                          // disc
      const sizeX = Math.abs(P[0]) > EPS ? P[0] : EPS, sizeZ = (flags & 0x2000000) ? sizeX : (Math.abs(P[4]) > EPS ? P[4] : EPS);
      let angle = 0, dangle = 0;
      const angleOffset = (flags & 0x40000) ? P[2] : em.random.float() * PI * 2;
      if (flags & 0x20000) {
        const f = (P[3] - P[2]) % (PI * 2);
        dangle = (f < ANGLE_MIN || f > ANGLE_MAX) ? (P[3] - P[2]) / em.emitDiv : (P[3] - P[2]) / (em.emitDiv - 1);
      }
      for (let i = 0; i < count; i++) {
        let dist = em.random.float(); const inner = P[1] / 100;
        if (flags & 0x1000000) { dist = Math.sqrt(dist + inner * inner * (1 - dist)); } else dist = dist + inner * (1 - dist);
        if (!(flags & 0x20000)) angle = (P[3] - P[2]) * em.random.float();
        const sa = Math.sin(angleOffset + angle), ca = Math.cos(angleOffset + angle);
        const fy = V(sa, 0, -ca);
        const pos = V(fy.x * dist * sizeX, 0, fy.z * dist * sizeZ);
        let normal;
        if (em.velDiffusionNormal === 0) normal = V(0, 1, 0);
        else { const cone = dist * em.velDiffusionNormal; normal = V(Math.sin(cone) * sa, Math.cos(cone), Math.sin(cone) * -ca); }
        emitOne(em, pm, pos, normal, fy.clone(), fy, life, lr, space);
        if (flags & 0x20000) angle += dangle;
      }
    },
    7(em, pm, count, flags, P, life, lr, space) {                          // cylinder
      const sizeX = Math.abs(P[0]) > EPS ? P[0] : EPS, sizeY = Math.abs(P[4]) > EPS ? P[4] : EPS;
      const sizeZ = (flags & 0x2000000) ? sizeX : (Math.abs(P[5]) > EPS ? P[5] : EPS);
      let angle = 0, dangle = 0;
      const angleOffset = (flags & 0x40000) ? P[2] : em.random.float() * PI * 2;
      if (flags & 0x20000) {
        const f = (P[3] - P[2]) % (PI * 2);
        dangle = (f < ANGLE_MIN || f > ANGLE_MAX) ? (P[3] - P[2]) / em.emitDiv : (P[3] - P[2]) / (em.emitDiv - 1);
      }
      for (let i = 0; i < count; i++) {
        let dist = em.random.float(); const inner = P[1] / 100;
        if (flags & 0x1000000) dist = Math.sqrt(dist + inner * inner * (1 - dist)); else dist += inner * (1 - dist);
        if (!(flags & 0x20000)) angle = (P[3] - P[2]) * em.random.float();
        const sa = Math.sin(angleOffset + angle), ca = Math.cos(angleOffset + angle);
        const fy = V(sa, 0, -ca);
        const pos = V(fy.x * dist * sizeX, (flags & 0x20000) ? 0 : (em.random.float() * 2 - 1) * sizeY, fy.z * dist * sizeZ);
        const normal = V(pos.x, 0, pos.z); if (pos.x !== 0 || pos.z !== 0) normal.normalize();
        const fo = pos.clone(); if (fo.lengthSq() > 0) fo.normalize();
        emitOne(em, pm, pos, normal, fo, fy, life, lr, space);
        if (flags & 0x20000) angle += dangle;
      }
    },
    8(em, pm, count, flags, P, life, lr, space) {                          // sphere (random distribution)
      const inner = P[1] / 100;
      const sizeX = Math.abs(P[0]) > EPS ? P[0] : EPS;
      const sizeY = (flags & 0x2000000) ? sizeX : (Math.abs(P[4]) > EPS ? P[4] : EPS), sizeZ = (flags & 0x2000000) ? sizeX : (Math.abs(P[5]) > EPS ? P[5] : EPS);
      let startAngle = P[2]; if (!(flags & 0x40000)) startAngle += em.random.float() * PI * 2;
      for (let i = 0; i < count; i++) {
        let dist = em.random.float();
        if (flags & 0x1000000) { dist = 1 - dist * dist * dist; dist = dist + inner * (1 - dist); } else dist += inner * (1 - dist);
        const angle = (P[3] - P[2]) * em.random.float() + startAngle;
        const x = em.random.float() * PI + PI / 2;
        const pos = V(sizeX * dist * -Math.cos(x) * Math.sin(angle), sizeY * dist * -Math.sin(x), sizeZ * dist * Math.cos(x) * Math.cos(angle));
        const normal = pos.clone(); if (normal.lengthSq() > 0) normal.normalize(); else normal.set(0, 0, 0);
        const fy = pos.clone(); fy.y = 0; if (fy.x !== 0 || fy.z !== 0) fy.normalize();
        emitOne(em, pm, pos, normal, normal.clone(), fy, life, lr, space);
      }
    },
    1(em, pm, count, flags, P, life, lr, space) {                          // line
      for (let i = 0; i < count; i++) {
        let f;
        if (!(flags & 0x20000)) f = em.random.float(); else f = count > 1 ? i / (count - 1) : 0;
        if (flags & 0x4000000) f -= 0.5;
        f *= P[0];
        const sx = Math.sin(P[1]), cx = Math.cos(P[1]), sy = Math.sin(P[2]), cy = Math.cos(P[2]), sz = Math.sin(P[3]), cz = Math.cos(P[3]);
        const pos = V((cx * cz * sy + sx * sz) * f, (-cz * sx + cx * sy * sz) * f, cx * cy * f);
        emitOne(em, pm, pos, V(0, 1, 0), pos.clone(), V(pos.x, 0, pos.z), life, lr, space);
      }
    },
    10(em, pm, count, flags, P, life, lr, space) {                         // torus
      const sizeX = Math.abs(P[0]) > EPS ? P[0] : EPS, sizeY = Math.abs(P[4]) > EPS ? P[4] : EPS;
      const sizeZ = (flags & 0x2000000) ? sizeX : (Math.abs(P[5]) > EPS ? P[5] : EPS);
      let angle = 0, dangle = 0, ring = 0;
      const angleOffset = (flags & 0x40000) ? P[2] : em.random.float() * PI * 2;
      if (flags & 0x20000) {
        const f = (P[3] - P[2]) % (PI * 2);
        dangle = (f < ANGLE_MIN || f > ANGLE_MAX) ? (P[3] - P[2]) / em.emitDiv : (P[3] - P[2]) / (em.emitDiv - 1);
      }
      const loop = (flags & 0x20000) ? count * count : count;
      for (let i = 0; i < loop; i++) {
        const inner = (100 - P[1]) / (100 + P[1]);
        if (!(flags & 0x20000)) { angle = (P[3] - P[2]) * em.random.float(); ring = em.random.float() * PI * 2; }
        const sa = Math.sin(angleOffset + angle), ca = Math.cos(angleOffset + angle), sr = Math.sin(ring), cr = Math.cos(ring);
        const pos = V((inner * cr * sa + sa) * sizeX / (1 + inner), sizeY * sr, (-inner * cr * ca - ca) * sizeZ / (1 + inner));
        const fo = pos.clone(); if (fo.lengthSq() > 0) fo.normalize();
        const fy = V(pos.x, 0, pos.z); if (fy.x !== 0 || fy.z !== 0) fy.normalize();
        const normal = inner === 0 ? V(sizeX * cr * sa, sizeY * sr, -sizeZ * cr * ca) : V((inner * cr * sa) * sizeX / (1 + inner), sizeY * sr, (-inner * cr * ca) * sizeZ / (1 + inner));
        if (normal.lengthSq() > 0) normal.normalize();
        emitOne(em, pm, pos, normal, fo, fy, life, lr, space);
        if (flags & 0x20000) { if ((i + 1) % count === 0) { angle += dangle; ring = 0; } else ring += (PI * 2) / count; }
      }
    },
    5(em, pm, count, flags, P, life, lr, space) {                          // cube (random distribution)
      const ox = Math.max(P[0], 1e-5), oy = Math.max(P[1], 1e-5), oz = Math.max(P[2], 1e-5);
      for (let i = 0; i < count; i++) {
        const pos = V((em.random.float() * 2 - 1) * ox, (em.random.float() * 2 - 1) * oy, (em.random.float() * 2 - 1) * oz);
        const normal = pos.clone(); if (normal.lengthSq() > 0) normal.normalize();
        const fo = pos.clone(); if (fo.lengthSq() <= 1.17549435e-38) fo.set(2 * em.random.float() - 1, 2 * em.random.float() - 1, 2 * em.random.float() - 1);
        if (fo.lengthSq() > 0) fo.normalize();
        const fy = pos.clone(); fy.y = 0; if (fy.lengthSq() <= 1.17549435e-38) { fy.x = 2 * em.random.float() - 1; fy.z = 2 * em.random.float() - 1; }
        if (fy.lengthSq() > 0) fy.normalize();
        emitOne(em, pm, pos, normal, fo, fy, life, lr, space);
      }
    },
  };

  // ---- emitter ---------------------------------------------------------------------------------------------------
  class Emitter {
    constructor(effect, res, drawWeight, parent) {
      this.effect = effect; this.res = res; const d = this.d = res.e;
      this.tick = 0; this.status = 'wait'; this.calcRemain = d.emitPast; this.dead = false; this.retired = false;
      this.comFlags = d.commonFlag; this.emitFlags = d.emitFlag; this.emitSpan = d.emitterLife;
      this.waitTime = d.emitStart; this.intervalWait = 0;
      this.ratio = d.rate; this.random_ = d.emitRandom / 100; this.interval = d.interval; this.intervalRandom = d.emitIntervalRandom / 100;
      this.emitDiv = d.diversion; this.count = 0; this.first = true;
      this.translate = V(...d.trans); this.scale = V(...d.scale); this.rotate = V(...d.rot);
      this.inherit = { s: true, r: true }; this.inheritT = 100;
      this.velInitRandom = d.velInitVelocityRandom; this.velMomentumRandom = d.velMomentumRandom;
      this.velRadiation = d.powerRadiation; this.velYAxis = d.powerY; this.velRandom = d.powerRandom; this.velNormal = d.powerNormal;
      this.velDiffusionNormal = d.diffusionNormal; this.velSpec = d.powerSpec; this.velDiffusionSpec = d.diffusionSpec; this.specDir = d.emitAngle.slice();
      this.params = d.dims.slice();
      this.randSeed = d.randomSeed || (sysRandom.rand() & 0xFFFF) || 1;
      this.random = new Random(this.randSeed);
      this.mtxDirty = true; this.mtx = new THREE.Matrix4();
      this.parent = parent || null; this.refParticle = null; this.inheritSetting = { speed: 0, scale: 0, alpha: 0, color: 0, weight: 128, type: 0, flag: 0 };
      this.form = FORMS[d.shape & 0xFF] || FORMS[9];
      this.managers = [];
      // the emitter's own particle manager
      this.addManager({ s: !!(d.commonFlag & 0x20), r: !!(d.commonFlag & 0x40), t: d.inheritPtclTranslate, w: drawWeight });
    }
    addManager(inh, res) { const pm = new ParticleManager(this, res || this.res, inh); this.managers.push(pm); this.effect.allManagers.push(pm); return pm; }
    globalMtx() {
      if (this.mtxDirty) {
        let m;
        if (!this.parent) m = this.effect.rootMtx.clone();
        else m = restruct(this.parent.globalMtx(), this.inherit.s, this.inherit.r, this.inheritT);
        m.multiply(transM(this.translate));
        m.multiply(rotXYZ(this.rotate.x, this.rotate.y, this.rotate.z));
        m.multiply(scaleM(this.scale));
        this.mtx = m; this.mtxDirty = false;
      }
      return this.mtx;
    }
    setDirty() {
      this.mtxDirty = true; this.managers.forEach(pm => pm.setDirty());
      for (const e of this.effect.emitters) {
        if (e.mtxDirty) continue;
        for (let s = e.parent; s; s = s.parent) if (s === this) { e.mtxDirty = true; e.managers.forEach(pm => pm.setDirty()); break; }
      }
    }
    // Emitter::Emission
    emission(pm, space) {
      if (this.intervalWait > 0) { this.intervalWait--; return; }
      this.intervalWait = this.interval;
      if (this.intervalRandom !== 0) this.intervalWait += Math.ceil((this.interval * this.intervalRandom - 1) * this.random.float()) & 0xFFFF;
      if (this.emitFlags & 0x20000) this.count = this.emitDiv;
      else {
        let n = this.random_ === 0 ? this.ratio : this.ratio + this.ratio * this.random_ * (2 * this.random.float() - 1);
        this.count += n;
        if (this.first && this.ratio !== 0 && this.count < 1) this.count = 1;
      }
      if (this.count >= 1) {
        const d = this.d;
        this.form(this, pm, Math.trunc(this.count), this.emitFlags, this.params, d.particleLife, d.ptclLifeRandom / 100, space);
        this.count -= Math.trunc(this.count);
      }
      this.first = false;
    }
    spaceFor(pm) { return pm.globalMtx().clone().invert().multiply(this.globalMtx()); }
    // Emitter::CalcEmitter: emitter tracks
    calcEmitter() {
      if (this.comFlags & 0x200) return;
      if (this.dead || this.retired || this.status !== 'wait' || this.waitTime > 0) return;
      if (!(this.comFlags & 4)) { if (this.tick >= this.emitSpan) { this.retire(); return; } }
      const span = (this.comFlags & 4) ? Infinity : this.emitSpan;
      let dirty = false;
      const tracks = this.res.emitTracks, first = this.tick === 0 ? 0 : this.res.nEmitInit;
      for (let i = first; i < tracks.length; i++) {
        const a = tracks[i];
        if ((a.proc & 0x08) || (!a.keys && !a.samples)) continue;
        const v = sampleCurve(a, curveFrame(a, this.tick, span));
        if (!v) continue;
        switch (a.kind) {
          case 72: this.velRadiation = v[0]; break;
          case 76: this.velYAxis = v[0]; break;
          case 80: this.velRandom = v[0]; break;
          case 84: this.velNormal = v[0]; break;
          case 92: this.velSpec = v[0]; break;
          case 8: this.ratio = v[0]; break;
          case 44: a.comps.forEach((c, k) => { this.params[c] = v[k]; }); break;
          case 124: a.comps.forEach((c, k) => { this.scale.setComponent(c, v[k]); }); dirty = true; break;
          case 136: a.comps.forEach((c, k) => { this.rotate.setComponent(c, v[k]); }); dirty = true; break;
          case 112: a.comps.forEach((c, k) => { this.translate.setComponent(c, v[k]); }); dirty = true; break;
          default: break;
        }
      }
      if (dirty) this.setDirty();
    }
    calcParticle(eff) { if (!(this.comFlags & 0x200)) for (const pm of this.managers.slice()) pm.calc(eff); }
    calcEmission() {
      if (this.comFlags & 0x200) return;
      if (this.status !== 'wait') return;
      this.status = 'done';
      if (this.calcRemain > 0) this.effect.existCalcRemain = true;
      if (!this.retired) {
        const pm = this.managers[0];
        if (this.waitTime > 0) this.waitTime--;
        else { this.emission(pm, this.spaceFor(pm)); this.tick++; }
      } else if (this.waitTime > 0) this.waitTime--; else this.tick++;
    }
    // billboard emitters (emit flags 15/16) turn to face the camera
    calcBillboard() {
      if (this.dead || !(this.emitFlags & 0x18000)) return;
      this.rotate.set(0, 0, 0); this.mtxDirty = true;
      const glb = this.globalMtx(), cam = this.effect.sys.cameraMtx;
      const mtx = cam.clone().multiply(glb);
      const e = mtx.elements;
      const tmp = new THREE.Matrix4().identity();
      const t = tmp.elements;
      t[0] = Math.hypot(e[0], e[1], e[2]); t[5] = Math.hypot(e[4], e[5], e[6]); t[10] = Math.hypot(e[8], e[9], e[10]);
      if (this.emitFlags & 0x8000) tmp.multiply(rotXYZ(PI / 2, 0, 0));
      const inv = mtx.clone().invert().multiply(tmp);
      inv.multiply(scaleM(V(1 / this.scale.x, 1 / this.scale.y, 1 / this.scale.z)));
      const m = scaleM(this.scale).multiply(inv);
      const q = new THREE.Quaternion().setFromRotationMatrix(new THREE.Matrix4().extractRotation(m));
      const eu = new THREE.Euler().setFromQuaternion(q, 'ZYX');
      this.rotate.set(eu.x, eu.y, eu.z); this.setDirty();
    }
    retire() {
      if (this.retired) return;
      this.retired = true;
      if (this.comFlags & 1) this.managers.forEach(pm => pm.particles.forEach(p => { p.dead = true; }));
      // children synchronised with this emitter end with it
      for (const e of this.effect.emitters) for (let s = e.parent; s; s = s.parent) if (s === this && (this.comFlags & 1)) { e.managers.forEach(pm => pm.particles.forEach(p => { p.dead = true; })); e.retired = true; }
    }
    // Emitter::CreateEmitter: a persistent child emitter placed at a particle
    createChild(res, setting, particle) {
      const e = new Emitter(this.effect, res, setting.weight, this);
      e.parent = this;
      e.inherit = { s: !!(this.comFlags & 0x80), r: !!(this.comFlags & 0x100) };
      e.inheritT = this.d.inheritChildEmitTranslate;
      e.calcRemain += 0;
      this.placeAt(e, particle);
      if (setting.speed || setting.scale || setting.alpha || setting.color || (setting.flag & 2)) { e.refParticle = particle; e.inheritSetting = setting; }
      this.effect.emitters.push(e);
      return e;
    }
    // shift a child emitter so it sits where the particle is
    placeAt(e, particle) {
      const save = e.translate.clone();
      e.translate.set(0, 0, 0); e.mtxDirty = true;
      const local = e.globalMtx().clone().invert();
      const pmM = particle.pm.globalMtx();
      const world = pmM.clone().multiply(transM(particle.param.position));
      const p = new THREE.Vector3().setFromMatrixPosition(world).applyMatrix4(local);
      const rs = rotXYZ(e.rotate.x, e.rotate.y, e.rotate.z).multiply(scaleM(e.scale)).invert();
      p.applyMatrix4(rs);
      e.translate.copy(save).add(p);
      e.mtxDirty = true;
    }
    // Emitter::CreateEmitterTmp: a one-shot child (emits once, its particles live in a shared manager)
    createTmp(res, setting, particle) {
      const t = new Emitter(this.effect, res, setting.weight, this);
      this.effect.allManagers.pop(); t.managers = [];                       // temporary: its own manager is not part of the effect
      t.parent = this;
      t.inherit = { s: !!(this.comFlags & 0x80), r: !!(this.comFlags & 0x100) };
      t.inheritT = this.d.inheritChildEmitTranslate;
      const rd = t.d;
      if ((setting.flag & 1) && setting.speed) {                          // follow the particle's direction of travel
        const dir = xdir(particle.param.position.clone().sub(particle.param.prevPosition), particle.pm.globalMtx());
        if (dir.lengthSq() > 0) {
          dir.normalize(); if (setting.speed < 0) dir.multiplyScalar(-1);
          const y = dir, z0 = Math.abs(y.z) < 0.99 ? V(0, 0, 1) : V(1, 0, 0);
          const x = new THREE.Vector3().crossVectors(y, z0).normalize(), z = new THREE.Vector3().crossVectors(x, y);
          const m = new THREE.Matrix4().makeBasis(x, y, z).multiply(rotXYZ(t.rotate.x, t.rotate.y, t.rotate.z));
          const eu = new THREE.Euler().setFromRotationMatrix(m, 'ZYX'); t.rotate.set(eu.x, eu.y, eu.z);
        }
      }
      this.placeAt(t, particle);
      t.calcEmitter(); t.calcBillboard();
      const inhS = !!(this.comFlags & 0x80) && !!(rd.commonFlag & 0x20), inhR = !!(this.comFlags & 0x100) && !!(rd.commonFlag & 0x40);
      const pT = this.d.inheritChildEmitTranslate, cT = rd.inheritPtclTranslate;
      let inhT = (pT === 0 || cT === 0) ? 0 : pT === 100 ? cT : cT === 100 ? pT : Math.trunc((pT * cT) / 100);
      let pm = this.managers.find(m => m.res === res && m.flagS === inhS && m.flagR === inhR && m.inhT === inhT && m.weight === setting.weight);
      if (!pm) pm = this.addManager({ s: inhS, r: inhR, t: inhT, w: setting.weight }, res);
      if (setting.speed || setting.scale || setting.alpha || setting.color || (setting.flag & 2)) { t.refParticle = particle; t.inheritSetting = setting; }
      t.emission(pm, pm.globalMtx().clone().invert().multiply(t.globalMtx()));
    }
  }

  // ---- effect (one played effect: its emitters, particle managers, creation queue) ----------------------------
  class Effect {
    constructor(sys, res, opts) {
      this.sys = sys; this.res = res; this.rootMtx = new THREE.Matrix4(); this.emitters = []; this.allManagers = [];
      this.queue = []; this.existCalcRemain = false; this.dead = false; this.view = !!opts.view; this.node = opts.node || null;
      this.root = new Emitter(this, res, 128, null);
      this.emitters.push(this.root);
      this.pre = this.root.calcRemain; this.started = false;
    }
    setRootMtx(m) { this.rootMtx.copy(m); this.emitters.forEach(e => { if (!e.parent) e.setDirty(); }); }
    stop() { this.emitters.forEach(e => { if (!e.parent) e.retire(); }); }
    // Effect::Calc
    calc() {
      if (this.node) this.setRootMtx(this.node());
      const run = () => {
        this.emitters.forEach(e => { if (!e.dead && e.status === 'done') e.status = 'wait'; e.managers.forEach(pm => pm.particles.forEach(p => { if (p.status === 'done') p.status = 'wait'; })); });
        this.existCalcRemain = true;
        while (this.existCalcRemain) {
          this.existCalcRemain = false;
          for (let guard = 0; guard < 8; guard++) {
            const list = this.emitters.slice();
            list.forEach(e => e.calcEmitter());
            list.forEach(e => e.calcBillboard());
            list.forEach(e => { if (e.status === 'wait') e.tick++; e.calcParticle(this); if (e.status === 'wait') e.tick--; });
            list.forEach(e => e.calcEmission());
            list.forEach(e => e.calcParticle(this));
            if (!this.queue.length) break;
            const q = this.queue; this.queue = [];
            for (const it of q) {
              const owner = it.particle.pm.emitter;
              if (it.setting.type === 1) {
                // A child emitter that never ends (infinite life) cannot be meant to be re-created by every short-lived
                // parent particle (Koopa Cape's rk_epropeller emits one 1-frame particle per frame: 950+ immortal child
                // emitters after 16 s and ~4 fps) -- keep a single one per parent emitter and effect resource.
                const forever = (it.res.e.commonFlag & 4) || it.res.e.emitterLife >= 65535;
                if (forever && this.emitters.some(e => e.parent === owner && e.res === it.res && !e.retired && !e.dead)) continue;
                owner.createChild(it.res, it.setting, it.particle);
              }
              else owner.createTmp(it.res, it.setting, it.particle);
            }
            list.forEach(e => e.calcParticle(this));
          }
          if (this.existCalcRemain) {                                       // "past" frames: simulate ahead at creation
            this.emitters.forEach(e => { if (!e.dead && e.calcRemain > 0) { e.calcRemain--; if (e.status === 'done') e.status = 'wait'; } e.managers.forEach(pm => pm.particles.forEach(p => { if (p.calcRemain) p.calcRemain--; if (p.status === 'done') p.status = 'wait'; })); });
          }
        }
      };
      run();
      // retire finished emitters and free the effect when nothing is left
      this.emitters = this.emitters.filter(e => { if (e.retired && e.managers.every(pm => !pm.particles.length)) { e.dead = true; e.managers.forEach(pm => { const k = this.allManagers.indexOf(pm); if (k >= 0) this.allManagers.splice(k, 1); }); return false; } return true; });
      for (const pm of this.allManagers.slice()) if (pm.emitter.dead && !pm.particles.length) { const k = this.allManagers.indexOf(pm); if (k >= 0) this.allManagers.splice(k, 1); }
      if (!this.emitters.length) this.dead = true;
    }
  }

  // ---- TEV shader (GX texture environment) -------------------------------------------------------------------------
  const CARG = ['prev.rgb', 'vec3(prev.a)', 'r0.rgb', 'vec3(r0.a)', 'r1.rgb', 'vec3(r1.a)', 'r2.rgb', 'vec3(r2.a)', 'texc', 'vec3(texa)', 'ras.rgb', 'vec3(ras.a)', 'vec3(1.0)', 'vec3(0.5)', 'KC', 'vec3(0.0)'];
  const AARG = ['prev.a', 'r0.a', 'r1.a', 'r2.a', 'texa', 'ras.a', 'KA', '0.0'];
  const KSEL = [1, 7 / 8, 3 / 4, 5 / 8, 1 / 2, 3 / 8, 1 / 4, 1 / 8];
  const OUTREG = ['prev', 'r0', 'r1', 'r2'], SCALE = [1, 2, 4, 0.5], BIAS = [0, 0.5, -0.5];
  // colour input sources: 1 = layer 1 primary, 2 = layer 1 secondary, 3 = layer 2 primary, 4 = layer 2 secondary, 5/6 = products
  const LAYER = ['vec4(0.0)', 'c1p', 'c1s', 'c2p', 'c2s', 'c1p * c1s', 'c2p * c2s'];
  const CMP = ['false', 'a < REF', 'a == REF', 'a <= REF', 'a > REF', 'a != REF', 'a >= REF', 'true'];
  const F = x => (Number.isInteger(x) ? x.toFixed(1) : String(x));
  const kc = (sel) => sel < 8 ? `vec3(${F(KSEL[sel])})` : (sel >= 12 && sel <= 15) ? `k${sel - 12}.rgb` : sel >= 16 ? `vec3(k${(sel - 16) & 3}.${'rgba'[(sel - 16) >> 2]})` : 'vec3(1.0)';
  const ka = (sel) => sel < 8 ? F(KSEL[sel]) : sel >= 16 ? `k${(sel - 16) & 3}.${'rgba'[(sel - 16) >> 2]}` : '1.0';

  function tevShader(E) {
    const ci = E.colorInput, ai = E.alphaInput;
    const L = (arr, i) => LAYER[arr[i]] || 'vec4(0.0)';
    let s = `precision highp float;
uniform sampler2D map0; uniform sampler2D map1; uniform sampler2D map2;
varying vec2 vUv0; varying vec2 vUv1; varying vec2 vUv2; varying vec2 vRef; varying float vFlick;
varying vec4 c1p; varying vec4 c1s; varying vec4 c2p; varying vec4 c2s;
void main() {
  vec4 ras = vec4(1.0), prev = vec4(0.0);
  vec4 r0 = vec4(${L(ci, 1)}.rgb, ${L(ai, 1)}.a), r1 = vec4(${L(ci, 2)}.rgb, ${L(ai, 2)}.a), r2 = vec4(${L(ci, 3)}.rgb, ${L(ai, 3)}.a);
  vec4 k0 = vec4(${L(ci, 4)}.rgb, ${L(ai, 4)}.a), k1 = vec4(${L(ci, 5)}.rgb, ${L(ai, 5)}.a), k2 = vec4(${L(ci, 6)}.rgb, ${L(ai, 6)}.a), k3 = vec4(${L(ci, 7)}.rgb, ${L(ai, 7)}.a);
`;
    for (let i = 0; i < Math.max(1, E.tevStages); i++) {
      const t = E.tevTex[i] < 3 ? E.tevTex[i] : 0, uv = ['vUv0', 'vUv1', 'vUv2'][t];
      const co = E.tevColorOp[i], ao = E.tevAlphaOp[i], ca = E.tevColor[i], aa = E.tevAlpha[i];
      const carg = c => CARG[c].replace('KC', kc(E.kColorSel[i])), aarg = c => AARG[c].replace('KA', ka(E.kAlphaSel[i]));
      s += `  { vec4 tx = texture2D(map${t}, ${uv}); vec3 texc = tx.rgb; float texa = tx.a;
    vec3 A = ${carg(ca[0])}, B = ${carg(ca[1])}, C = ${carg(ca[2])}, D = ${carg(ca[3])};
    vec3 o = (D ${co[0] ? '-' : '+'} ((1.0 - C) * A + C * B) + vec3(${F(BIAS[co[1]] || 0)})) * ${F(SCALE[co[2]] || 1)};
    ${co[3] ? 'o = clamp(o, 0.0, 1.0);' : ''}
    float aA = ${aarg(aa[0])}, aB = ${aarg(aa[1])}, aC = ${aarg(aa[2])}, aD = ${aarg(aa[3])};
    float ao = (aD ${ao[0] ? '-' : '+'} ((1.0 - aC) * aA + aC * aB) + ${F(BIAS[ao[1]] || 0)}) * ${F(SCALE[ao[2]] || 1)};
    ${ao[3] ? 'ao = clamp(ao, 0.0, 1.0);' : ''}
    ${OUTREG[co[4]]}.rgb = o; ${OUTREG[ao[4]]}.a = ao; }
`;
    }
    const [c0, c1, op] = E.alphaCmp;
    const a0 = CMP[c0].replace('REF', 'vRef.x'), a1 = CMP[c1].replace('REF', 'vRef.y');
    const joined = [`(${a0}) && (${a1})`, `(${a0}) || (${a1})`, `((${a0}) != (${a1}))`, `((${a0}) == (${a1}))`][op] || 'true';
    s += `  float a = prev.a;\n  if (!(${joined})) discard;\n  gl_FragColor = vec4(prev.rgb, prev.a * vFlick);\n}`;
    return s;
  }
  const VERT = `
attribute vec4 pc1p; attribute vec4 pc1s; attribute vec4 pc2p; attribute vec4 pc2s;
attribute vec2 uv1; attribute vec2 uv2; attribute vec3 pref;
varying vec2 vUv0; varying vec2 vUv1; varying vec2 vUv2; varying vec2 vRef; varying float vFlick;
varying vec4 c1p; varying vec4 c1s; varying vec4 c2p; varying vec4 c2s;
void main() {
  vUv0 = uv; vUv1 = uv1; vUv2 = uv2; vRef = pref.xy; vFlick = pref.z; c1p = pc1p; c1s = pc1s; c2p = pc2p; c2s = pc2s;
  gl_Position = projectionMatrix * viewMatrix * vec4(position, 1.0);
}`;
  const GXF = () => [THREE.ZeroFactor, THREE.OneFactor, THREE.SrcColorFactor, THREE.OneMinusSrcColorFactor, THREE.SrcAlphaFactor, THREE.OneMinusSrcAlphaFactor, THREE.DstAlphaFactor, THREE.OneMinusDstAlphaFactor];

  // ---- effect system + drawing --------------------------------------------------------------------------------------
  class EffectSystem {
    constructor(scene, camera, base = 'assets/effects') {
      this.scene = scene; this.camera = camera; this.base = base; this.data = null; this.effects = []; this.time = 0;
      this.textures = {}; this.batches = new Map(); this.cameraMtx = new THREE.Matrix4(); this.resCache = new Map();
      this.ready = fetch(`${base}/effects.json`).then(r => r.json()).then(j => { this.data = j; });
    }
    def(name) { return this.data && this.data.effects[name]; }
    // effect resource (data + parsed tracks) by name
    res(name) {
      if (this.resCache.has(name)) return this.resCache.get(name);
      const d = this.data && this.data.effects[name];
      if (!d) return null;
      const ptcl = d.a.filter(a => a.p), emit = d.a.filter(a => !a.p);
      for (const a of d.a) { if (a.curve === 5 && !a.children) a.children = []; }
      const r = { name, e: d.e, p: d.p, ptclTracks: ptcl, emitTracks: emit, nPtclInit: ptcl.filter(a => a.init).length, nEmitInit: 0, batch: null };
      this.resCache.set(name, r);
      return r;
    }
    play(name, node, opts = {}) {
      if (!this.data) return null;
      const res = this.res(name);
      if (!res) return null;
      const fx = new Effect(this, res, { node, view: opts.view });
      this.effects.push(fx);
      return fx;
    }
    texture(name, wrapS, wrapT) {
      const key = `${name}|${wrapS}|${wrapT}`;
      if (this.textures[key]) return this.textures[key];
      const w = (m) => (m === 1 ? THREE.RepeatWrapping : m === 2 ? THREE.MirroredRepeatWrapping : THREE.ClampToEdgeWrapping);
      let t;
      if (!name) t = new THREE.DataTexture(new Uint8Array([255, 255, 255, 255]), 1, 1, THREE.RGBAFormat);
      else { t = new THREE.TextureLoader().load(`${this.base}/tex/${name.replace('#', '_')}.png`); t.flipY = false; }
      t.wrapS = w(wrapS); t.wrapT = w(wrapT); t.encoding = THREE.LinearEncoding;
      this.textures[key] = t;
      return t;
    }
    batch(res) {
      if (res.batch) return res.batch;
      const E = res.e, P = res.p, maxQuads = 500;
      const geo = new THREE.BufferGeometry();
      const mk = (n) => new THREE.BufferAttribute(new Float32Array(maxQuads * 4 * n), n).setUsage(THREE.DynamicDrawUsage);
      geo.setAttribute('position', mk(3)); geo.setAttribute('uv', mk(2)); geo.setAttribute('uv1', mk(2)); geo.setAttribute('uv2', mk(2)); geo.setAttribute('pref', mk(3));
      for (const n of ['pc1p', 'pc1s', 'pc2p', 'pc2s']) geo.setAttribute(n, mk(4));
      const idx = new Uint32Array(maxQuads * 6);
      for (let i = 0; i < maxQuads; i++) { const v = i * 4; idx.set([v, v + 1, v + 2, v, v + 2, v + 3], i * 6); }
      geo.setIndex(new THREE.BufferAttribute(idx, 1)); geo.setDrawRange(0, 0);
      const f = GXF(), [bt, src, dst] = E.blend;
      const wrap = (l) => [(P.wrap >> (l * 4)) & 3, (P.wrap >> (l * 4 + 2)) & 3];
      const texName = (l) => P.tex[l] || (l === 0 ? ((res.ptclTracks.find(a => a.curve === 4 && a.kind === 104) || {}).names || [''])[0] : '');
      const mat = new THREE.ShaderMaterial({
        vertexShader: VERT, fragmentShader: tevShader(E), transparent: true, side: THREE.DoubleSide,
        depthTest: !!(E.drawFlags & 1), depthWrite: !!(E.drawFlags & 2),
        blending: THREE.CustomBlending, blendEquation: THREE.AddEquation, blendSrc: f[src] ?? THREE.SrcAlphaFactor, blendDst: f[dst] ?? THREE.OneMinusSrcAlphaFactor,
        uniforms: { map0: { value: this.texture(texName(0), ...wrap(0)) }, map1: { value: this.texture(texName(1), ...wrap(1)) }, map2: { value: this.texture(texName(2), ...wrap(2)) } },
      });
      const mesh = new THREE.Mesh(geo, mat); mesh.frustumCulled = false; mesh.renderOrder = 8;
      this.scene.add(mesh);
      res.batch = { geo, mesh, count: 0, maxQuads, mat, res };
      this.batches.set(res.name, res.batch);
      return res.batch;
    }
    update(dt) {
      if (!this.data) return;
      this.camera.updateMatrixWorld();
      this.cameraMtx.copy(this.camera.matrixWorldInverse.copy(this.camera.matrixWorld).invert());
      this.time += (dt * (this.speed ?? 1)) / FRAME;
      let steps = Math.min(4, Math.floor(this.time)); this.time -= Math.floor(this.time);
      while (steps-- > 0) this.step();
      this.render();
    }
    step() {
      for (const fx of this.effects.slice()) fx.calc();
      this.effects = this.effects.filter(e => !e.dead);
    }
    advance(n) { for (let i = 0; i < n; i++) this.step(); this.camera.updateMatrixWorld(); this.cameraMtx.copy(this.camera.matrixWorld).invert(); this.render(); }

    // ---- drawing ----
    render() {
      for (const b of this.batches.values()) b.count = 0;
      const cam = this.camera, view = cam.matrixWorldInverse.copy(cam.matrixWorld).invert();
      for (const fx of this.effects) for (const pm of fx.allManagers) this.drawManager(fx, pm, view);
      for (const b of this.batches.values()) { b.geo.setDrawRange(0, b.count * 6); b.mesh.visible = b.count > 0; for (const n in b.geo.attributes) b.geo.attributes[n].needsUpdate = true; }
    }
    drawManager(fx, pm, viewMtx) {
      const d = pm.d, res = pm.res;
      if ((d.drawFlags & 0x400) || (pm.emitter.comFlags & 2) || !pm.particles.length) return;
      const b = this.batch(res), g = b.geo, A = g.attributes;
      const pmM = pm.globalMtx(); const toWorld = fx.view ? new THREE.Matrix4().multiplyMatrices(this.camera.matrixWorld, pmM) : pmM;
      const mv = new THREE.Matrix4().multiplyMatrices(viewMtx, toWorld);         // particle-manager space -> view space
      const camWorld = this.camera.matrixWorld;
      const e = mv.elements;
      const vx = Math.hypot(e[0], e[1], e[2]), vy = Math.hypot(e[4], e[5], e[6]);
      // the world "up" direction as it appears in the view plane keeps billboards upright (rc, rs of DrawNormalBillboard)
      const vm = fx.view ? new THREE.Matrix4() : viewMtx; const ve = vm.elements;
      let mag = Math.hypot(ve[4], ve[5]), rs = 0, rc = 1;
      if (fx.view) { rs = 0; rc = 1; } else if (mag > 0) { rs = -ve[4] / mag; rc = ve[5] / mag; }
      const list = (d.drawFlags & 0x800) ? pm.particles.slice().reverse() : pm.particles;
      const pivot = [d.texPivot[0] / 100, d.texPivot[1] / 100];
      for (const p of list) {
        if (b.count >= b.maxQuads) break;
        if (p.dead) continue;
        const par = p.param;
        const K = window.FX_SIZE ?? 1;                            // sizes exactly as stored in the effect data
        let sx = drawSizeX(p) * K, sy = drawSizeY(p) * K;
        if (sx < 1e-7 || sy < 1e-7) continue;
        const pos = par.position.clone().applyMatrix4(mv);                      // view space
        // rotation (particle rotate + offsets)
        const rot = par.rotate.slice();
        for (let i = 0; i < 3; i++) if (par.rotOff[i] > 0) rot[i] += (par.rotOff[i] * PI) / 128;
        const corners = [];
        if (d.ptype === 4 || (d.ptype === 3 && d.typeOption === 2)) {
          this.directionalCorners(corners, p, pm, d, mv, vx, vy, sx, sy, pivot, rot);
        } else {
          const cr = Math.cos(-rot[2]), sr = Math.sin(-rot[2]);
          const X = [vx * (rc * cr - rs * sr) , vx * (rs * cr + rc * sr)];            // x axis in the view plane
          const Y = [vy * (-rs * cr - rc * sr), vy * (rc * cr - rs * sr)];
          const ox = pivot[0], oy = pivot[1];
          const cx = pos.x + (X[0] * ox * sx + Y[0] * oy * sy), cy = pos.y + (X[1] * ox * sx + Y[1] * oy * sy);
          const pts = [[-1, -1], [-1, 1], [1, 1], [1, -1]];                            // uv (0,1) (0,0) (1,0) (1,1)
          for (const [u, v] of pts) corners.push(new THREE.Vector3(cx + X[0] * u * sx + Y[0] * v * sy, cy + X[1] * u * sx + Y[1] * v * sy, pos.z));
          if (d.zOffset) { const dv = pos.clone().normalize().multiplyScalar(d.zOffset * (pos.z >= 0 ? 1 : -1)); corners.forEach(c => c.add(dv)); }
          corners.forEach(c => c.applyMatrix4(camWorld));
        }
        const o = b.count * 12, co = b.count * 8;
        const UV = [[0, 1], [0, 0], [1, 0], [1, 1]];
        for (let k = 0; k < 4; k++) {
          A.position.array[o + k * 3] = corners[k].x; A.position.array[o + k * 3 + 1] = corners[k].y; A.position.array[o + k * 3 + 2] = corners[k].z;
          const t0 = this.texUV(p, 0, UV[k]), t1 = this.texUV(p, 1, UV[k]), t2 = this.texUV(p, 2, UV[k]);
          A.uv.array[co + k * 2] = t0[0]; A.uv.array[co + k * 2 + 1] = t0[1];
          A.uv1.array[co + k * 2] = t1[0]; A.uv1.array[co + k * 2 + 1] = t1[1];
          A.uv2.array[co + k * 2] = t2[0]; A.uv2.array[co + k * 2 + 1] = t2[1];
        }
        const c0 = this.colors(p, 0), c1 = this.colors(p, 1);
        for (let k = 0; k < 4; k++) {
          A.pc1p.array.set(c0[0], (b.count * 4 + k) * 4); A.pc1s.array.set(c0[1], (b.count * 4 + k) * 4);
          A.pc2p.array.set(c1[0], (b.count * 4 + k) * 4); A.pc2s.array.set(c1[1], (b.count * 4 + k) * 4);
          const o3 = (b.count * 4 + k) * 3; A.pref.array[o3] = par.aRef[0] / 255; A.pref.array[o3 + 1] = par.aRef[1] / 255; A.pref.array[o3 + 2] = 1;
        }
        b.count++;
      }
    }
    // Particle::Draw_GetColor: alpha flicker, bytes -> 0..1
    colors(p, layer) {
      const par = p.param, D = p.pm.d, out = [];
      for (let j = 0; j < 2; j++) {
        const c = par.color[layer][j].slice();
        if (D.alphaFlick[0] !== 0) {
          const [type, cyc0, amp] = D.alphaFlick, rnd = D.alphaFlickRandom || 0;
          const cycle = Math.min(0xFFFF, Math.max(1, cyc0 + Math.trunc((cyc0 * p.flickRnd * rnd) / 12700)));
          const pos = ((p.tick - 1) % cycle + cycle) % cycle;
          let a = 0;
          if (type === 1) a = pos * 2 <= cycle ? 128 - amp + Math.trunc(amp * (pos * 4) / cycle) : 128 + amp * 3 - Math.trunc((amp * pos * 4) / cycle);
          else if (type === 2) a = 128 + amp - Math.trunc((amp * pos * 2) / cycle);
          else if (type === 3) a = 128 - amp + Math.trunc((amp * pos * 2) / cycle);
          else if (type === 4) a = pos * 2 <= cycle ? 128 + amp : 128 - amp;
          else if (type === 5) a = 128 + amp * Math.sin((2 * PI * pos) / cycle);
          a = Math.min(255, Math.max(0, a));
          c[3] = (c[3] * a + 128) >> 8;
        }
        out.push(c.map(v => v / 255));
      }
      return out;
    }
    // GX texture matrix of layer i applied to a uv (see DrawStrategyImpl::_SetupTexture)
    texUV(p, i, uv) {
      const par = p.param, rev = (par.texReverse >> (i * 2)) & 3, wS = (par.wrap >> (i * 4)) & 3, wT = (par.wrap >> (i * 4 + 2)) & 3;
      let scaleS = wS === 2 ? 2 : 1, scaleT = wT === 2 ? 2 : 1, transS = 0, transT = 0;
      if (rev & 1) { scaleS = -scaleS; transS = wS === 2 ? 2 : 1; }
      if (rev & 2) { scaleT = -scaleT; transT = wT === 2 ? 2 : 1; }
      let [u, v] = uv;
      u += par.texTrans[i][0]; v += par.texTrans[i][1];
      if (par.texRot[i] !== 0) { u -= 0.5; v -= 0.5; const c = Math.cos(par.texRot[i]), s = Math.sin(par.texRot[i]); [u, v] = [u * c - v * s, u * s + v * c]; u += 0.5; v += 0.5; }
      u = (u - 0.5) * par.texScale[i][0] + 0.5; v = (v - 0.5) * par.texScale[i][1] + 0.5;
      return [u * scaleS + transS, v * scaleT + transT];
    }
    // directional particles (reconstructed): the quad's long axis follows the "ahead" direction, facing the camera
    directionalCorners(out, p, pm, d, mv, vx, vy, sx, sy, pivot, rot) {
      const par = p.param, pmM = pm.globalMtx();
      const axis = new THREE.Vector3();
      const em = pm.emitter, emM = em.globalMtx();
      const emAxisY = V(emM.elements[4], emM.elements[5], emM.elements[6]).transformDirection(pmM.clone().invert());
      if (emAxisY.lengthSq() === 0) emAxisY.set(0, 1, 0);
      const center = V().setFromMatrixPosition(emM).applyMatrix4(pmM.clone().invert());
      switch (d.typeDir) {
        case 1: axis.copy(par.position).sub(center); break;                              // from the emitter centre
        case 2: axis.copy(emAxisY); break;                                                // the emitter's Y axis
        case 0: default: axis.copy(par.position).sub(par.prevPosition); break;            // direction of travel
      }
      if (d.typeDir === 5) axis.set(pmM.clone().invert().elements[4], pmM.clone().invert().elements[5], pmM.clone().invert().elements[6]);   // no design: world up
      if (axis.lengthSq() < 1.19e-7) axis.copy(emAxisY);
      axis.normalize();
      const viewAxis = axis.clone().transformDirection(mv);                               // view-space direction
      const posV = par.position.clone().applyMatrix4(mv);
      // side = perpendicular to the axis, in the view plane (the quad faces the camera)
      let side = V(viewAxis.y, -viewAxis.x, 0);
      if (side.lengthSq() < 1e-8) side.set(1, 0, 0);
      side.normalize();
      const along = viewAxis.clone(); along.z = 0; if (along.lengthSq() < 1e-8) along.set(0, 1, 0); along.normalize();
      const px = pivot[0] * sx, py = pivot[1] * sy;
      const c = posV.clone().addScaledVector(side, px).addScaledVector(along, py);
      const pts = [[-1, -1], [-1, 1], [1, 1], [1, -1]];
      for (const [u, v] of pts) out.push(c.clone().addScaledVector(side, u * sx * vx).addScaledVector(along, v * sy * vy).applyMatrix4(this.camera.matrixWorld));
    }
  }
  window.EffectSystem = EffectSystem;
})();
