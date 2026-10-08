// The game's race HUD (time, lap counter, item box) drawn from its own UI textures (web/assets/ui, made by tools/export_hud.py).
//
// Geometry comes from the game's layouts (Race.szs / Race_U.szs: game_image_number, game_image_lap, item_window .brlyt): a 456-unit
// tall layout centred on the screen, y up. The layouts only hold sizes and offsets inside each group; where the game puts each
// group on screen (and at what scale) is set by code, so those anchors were measured from a real frame of the game.
// Textures are grayscale intensity images; the game colours them (black -> a "white colour" per group), done here with a multiply.
(function () {
  'use strict';
  const IMG = {};
  const tint = {};      // "name|r,g,b" -> canvas
  const load = name => IMG[name] || (IMG[name] = (() => { const i = new Image(); i.src = `assets/ui/${name}.png`; return i; })());
  const tinted = (name, rgb) => {
    const key = name + '|' + rgb;
    if (tint[key]) return tint[key];
    const im = load(name);
    if (!im.complete || !im.naturalWidth) return null;
    const c = document.createElement('canvas'); c.width = im.naturalWidth; c.height = im.naturalHeight;
    const g = c.getContext('2d');
    g.drawImage(im, 0, 0);
    g.globalCompositeOperation = 'multiply'; g.fillStyle = `rgb(${rgb})`; g.fillRect(0, 0, c.width, c.height);
    g.globalCompositeOperation = 'destination-in'; g.drawImage(im, 0, 0);
    return tint[key] = c;
  };

  // colour = fore + (back - fore) x intensity, as the game's layout materials do (fore and back are 'r,g,b' strings); alpha kept
  const ramp = (name, fore, back) => {
    const key = name + '|ramp|' + fore + '|' + back;
    if (tint[key]) return tint[key];
    const im = load(name);
    if (!im.complete || !im.naturalWidth) return null;
    const c = document.createElement('canvas'); c.width = im.naturalWidth; c.height = im.naturalHeight;
    const g = c.getContext('2d'); g.drawImage(im, 0, 0);
    const d = g.getImageData(0, 0, c.width, c.height), f = fore.split(',').map(Number), b = back.split(',').map(Number);
    for (let i = 0; i < d.data.length; i += 4) { const t = d.data[i] / 255; for (let k = 0; k < 3; k++) d.data[i + k] = f[k] + (b[k] - f[k]) * t; }
    g.putImageData(d, 0, 0);
    return tint[key] = c;
  };

  const CHAR_ICONS = ['mario', 'baby_peach', 'waluigi', 'koopa', 'baby_daisy', 'karon', 'baby_mario', 'luigi', 'kinopio', 'donky', 'yoshi', 'wario',
    'baby_luigi', 'kinopico', 'noko', 'daisy', 'peach', 'catherine', 'didy', 'teresa', 'koopa_jr', 'hone_koopa', 'fuky', 'roseta'].map(n => `st_${n}_32x32`);
  const YELLOW_TEXT = '241,229,30', YELLOW_DIGIT = '249,238,32';

  // Anchors measured from the game (units of the 456-tall layout; x from the screen edge, y from the top). scale multiplies the layout.
  const ANCHOR = {
    time: { edge: 'right', x: 146, y: 47, scale: 1 },
    lap: { edge: 'right', x: 78, y: 105, scale: 0.8 },
    item: { edge: 'left', x: 95.5, y: 77, scale: 1.7, iconSize: 45 },
    // the minimap: the course's map area (the map model's posLD/posRU corners) fitted into a `size` square, centred here
    rank: { edge: 'left', x: 82, y: 376, scale: 1 },      // VS race position (bottom left; placement is a guess)
    msg: { x: 0, y: 0, scale: 1, top: 'f2ff00ff', bottom: 'ffffffff' },      // the centred message (layout units from the screen centre)
    map: { edge: 'right', x: 166.5, y: 292, size: 285, add: 53, dark: 222, ringWidth: 1.5, icon: 0.6 },
  };

  class GameHud {
    // opts.canvas + opts.size() let a test page draw to a fixed-size canvas instead of the whole window
    constructor(opts = {}) {
      this.size = opts.size || (() => ({ W: Math.round(window.innerWidth * (window.devicePixelRatio || 1)), H: Math.round(window.innerHeight * (window.devicePixelRatio || 1)) }));
      this.canvas = opts.canvas;
      if (!this.canvas) {
        this.canvas = document.createElement('canvas');
        this.canvas.style.cssText = 'position:fixed;left:0;top:0;width:100%;height:100%;pointer-events:none;z-index:5';
        document.body.appendChild(this.canvas);
      }
      this.state = { visible: false, timeMs: 0, lap: 1, laps: 3, items: 0 };
      this.a = ANCHOR;
      this.map = null; this.racers = [];          // set by setTrack / setPlayer
      this.mapCache = { key: '', canvas: null };
      load('tt_map_chara_searchlight'); CHAR_ICONS.forEach(load);
      for (let i = 1; i <= 12; i++) load('tt_position_no_st_64x64_' + String(i).padStart(2, '0'));
      this.msgs = null; this.msg = null; this.loadMessages();
      ['tt_item_box_glass_type_02', 'tt_item_kinoko', 'tt_item_kinoko_2', 'tt_item_kinoko_3', 'tt_d_number_3d_slash', 'tt_d_number_3d_coron',
        'tt_d_number_3d_coron_00', 'tt_time_E', 'tt_lap_E'].forEach(load);
      for (let i = 0; i < 10; i++) load('tt_d_number_3d_' + String(i).padStart(2, '0'));
    }

    set(s) { Object.assign(this.state, s); }

    // folder: the course's asset folder (assets/tracks/<folder>/minimap.json, made by tools/export_minimap.py)
    async setTrack(folder) {
      this.map = null; this.mapCache.key = '';
      try {
        const j = await (await fetch(`assets/tracks/${folder}/minimap.json`)).json();
        this.map = { corners: j.corners, bbox: j.bbox, v: Int32Array.from(j.v), t: Uint32Array.from(j.t) };
      } catch (e) { console.warn('no minimap for', folder, e); }
    }
    // p: { x, z (world), fx, fz (heading), character (game id) }
    setPlayer(p) { this.racers = p ? [p] : []; }
    // every racer on the map, the player last (drawn on top)
    setRacers(list) { this.racers = list; }

    // The map mesh drawn once (per size) into two canvases. In the game the map is blended into the frame: the road adds a
    // little light (about +53 on every channel, measured on four courses over very different backgrounds) and its outline
    // darkens slightly (about x0.87). A separate canvas layer per blend mode does the same over the 3D view.
    mapImages(u) {
      const A = this.a.map, m = this.map, [x0, z0, x1, z1] = m.corners;
      const s = A.size / Math.max(x1 - x0, z1 - z0);                   // layout units per world unit
      const key = Math.round(s * u * 1e6) + '|' + m.corners.join();
      if (this.mapCache.key === key) return { s, ...this.mapCache };
      const w = Math.ceil((x1 - x0) * s * u), h = Math.ceil((z1 - z0) * s * u), k = s * u;
      const mk = () => { const c = document.createElement('canvas'); c.width = w; c.height = h; return c; };
      const fill = mk(), ring = mk();
      const trace = (g, lw) => {
        g.lineWidth = lw; g.lineJoin = 'round'; g.beginPath();
        for (let t = 0; t < m.t.length; t += 3) {
          const a = m.t[t], b = m.t[t + 1], c = m.t[t + 2];
          g.moveTo((m.v[2 * a] - x0) * k, (m.v[2 * a + 1] - z0) * k); g.lineTo((m.v[2 * b] - x0) * k, (m.v[2 * b + 1] - z0) * k);
          g.lineTo((m.v[2 * c] - x0) * k, (m.v[2 * c + 1] - z0) * k); g.closePath();
        }
      };
      let g = fill.getContext('2d'); g.fillStyle = g.strokeStyle = `rgb(${A.add},${A.add},${A.add})`; trace(g, 0.8); g.fill(); g.stroke();
      g = ring.getContext('2d'); g.fillStyle = g.strokeStyle = `rgb(${A.dark},${A.dark},${A.dark})`; trace(g, A.ringWidth * 2 * u); g.fill(); g.stroke();
      g.globalCompositeOperation = 'destination-out'; g.fillStyle = g.strokeStyle = '#000'; trace(g, 0.8); g.fill(); g.stroke();   // keep only the ring outside the road
      this.mapCache = { key, fill, ring };
      return { s, fill, ring };
    }

    // canvases stacked on the main one, each with its own CSS blend mode
    blendLayer(mode) {
      const c = document.createElement('canvas');
      c.style.cssText = this.canvas.style.cssText || 'position:absolute;left:0;top:0';
      c.style.mixBlendMode = mode;
      this.canvas.parentNode.insertBefore(c, this.canvas);
      return c;
    }

    // ---- race messages: the countdown numbers, GO!, FINISH! and NEW RECORD! ----
    // They are text panes (game_image/blyt/go.brlyt, new_record.brlyt) set in the game's kart font and animated by the layouts' .brlan
    // files (scale, alpha and colours over time), exported by tools/export_hud.py into hud_messages.json.
    async loadMessages() {
      if (this.msgs) return;
      try {
        const j = await (await fetch('assets/ui/hud_messages.json')).json();
        j.sheets = [0, 1].map(i => load(`font_kart_${i}`));
        this.msgs = j;
      } catch (e) { console.warn('no race messages', e); }
    }

    // text: '3' | '2' | '1' | 'GO!' | 'FINISH!' | 'NEW RECORD!'; layout: 'go' (the default) or 'new_record'
    message(text, layout = 'go') {
      this.msg = { text, layout, t0: performance.now() };
    }

    // cubic Hermite over the animation's keyframes (frame, value, slope); step keys (dtype 1) hold their value
    static anim(keys, dtype, f) {
      if (!keys.length) return null;
      if (dtype !== 2) { let v = keys[0][1]; for (const k of keys) if (k[0] <= f) v = k[1]; return v; }
      if (f <= keys[0][0]) return keys[0][1];
      for (let i = 0; i < keys.length; i++) {
        const k0 = keys[i], k1 = keys[i + 1];
        if (!k1) return k0[1];
        if (f >= k0[0] && f < k1[0]) {
          const dt = k1[0] - k0[0], t = (f - k0[0]) / dt, t2 = t * t, t3 = t2 * t;
          return (2 * t3 - 3 * t2 + 1) * k0[1] + (t3 - 2 * t2 + t) * dt * k0[2] + (-2 * t3 + 3 * t2) * k1[1] + (t3 - t2) * dt * k1[2];
        }
      }
      return keys[keys.length - 1][1];
    }

    // the pane values (scale, alpha) of one text pane at animation frame f
    paneAt(layout, name, f) {
      const an = this.msgs[layout].anims[layout === 'go' ? 'go_fade_in' : 'new_record_fade_in'], out = {};
      for (const e of an.entries) if (e.name === name && !e.mat) for (const t of e.tags) for (const it of t.items) {
        const v = GameHud.anim(it.keys, it.dtype, f);
        if (t.type === 'RLPA') { if (it.target === 6) out.sx = v; else if (it.target === 7) out.sy = v; else if (it.target === 0) out.x = v; else if (it.target === 1) out.y = v; }
        else if (t.type === 'RLVC' && it.target === 16) out.alpha = v / 255;
      }
      return out;
    }

    // one line of text in the kart font, colour = glyph intensity x a top-to-bottom gradient (both as hex RGBA), drawn centred at cx, cy
    drawText(g, text, cx, cy, sx, sy, fontW, fontH, top, bottom, alpha, k) {
      const F = this.msgs.font, gw = fontW / F.cellW * sx, gh = fontH / F.cellH * sy;
      const glyphs = [...text].map(ch => { const gi = F.map[ch.codePointAt(0)]; return { gi, w: F.widths[gi] }; }).filter(x => x.w);
      const total = glyphs.reduce((a, x) => a + x.w[2], 0) * gw;
      const col = h => [parseInt(h.slice(0, 2), 16), parseInt(h.slice(2, 4), 16), parseInt(h.slice(4, 6), 16)];
      const ct = col(top), cb = col(bottom);
      const per = F.cols * F.rows, cw = F.cellW + 1, ch = F.cellH + 1;
      let x = cx - total * k / 2;
      const y0 = cy - F.cellH * gh * k / 2;
      g.globalAlpha = Math.max(0, Math.min(1, alpha));
      for (const { gi, w } of glyphs) {
        const sheet = this.msgs.sheets[Math.floor(gi / per)], idx = gi % per;
        if (!sheet.complete) continue;
        const sxp = (idx % F.cols) * cw, syp = Math.floor(idx / F.cols) * ch;
        // glyph coloured through a small canvas: glyph x gradient, alpha from the glyph
        const c = document.createElement('canvas'); c.width = F.cellW; c.height = F.cellH;
        const cg = c.getContext('2d');
        cg.drawImage(sheet, sxp, syp, F.cellW, F.cellH, 0, 0, F.cellW, F.cellH);
        cg.globalCompositeOperation = 'multiply';
        const gr = cg.createLinearGradient(0, 0, 0, F.cellH); gr.addColorStop(0, `rgb(${ct})`); gr.addColorStop(1, `rgb(${cb})`);
        cg.fillStyle = gr; cg.fillRect(0, 0, F.cellW, F.cellH);
        cg.globalCompositeOperation = 'destination-in'; cg.drawImage(sheet, sxp, syp, F.cellW, F.cellH, 0, 0, F.cellW, F.cellH);
        g.drawImage(c, x + w[0] * gw * k, y0, F.cellW * gw * k, F.cellH * gh * k);
        x += w[2] * gw * k;
      }
      g.globalAlpha = 1;
    }

    drawMessage(g, W, H, u) {
      if (this.msg && this.msgs && true) {
        const m = this.msg, f = this.msgFrame ?? (performance.now() - m.t0) * 59.94 / 1000, L = this.msgs[m.layout];
        const an = L.anims[m.layout === 'go' ? 'go_fade_in' : 'new_record_fade_in'];
        if (f > an.frames + 1) this.msg = null;
        else {
          const k = u * this.a.msg.scale, cx = W / 2 + this.a.msg.x * u, cy = H / 2 - this.a.msg.y * u;
          for (const p of L.panes) {
            const v = this.paneAt(m.layout, p.name, f), sx = v.sx ?? p.sx, sy = v.sy ?? p.sy, alpha = v.alpha ?? (p.alpha / 255);
            if (p.name === 'text_01' || !sx || alpha <= 0) continue;         // the coloured glow layer is not drawn yet
            const light = p.name === 'text_light';
            this.drawText(g, m.text, cx + p.x * k, cy - p.y * k, sx, sy, p.fontW, p.fontH, light ? 'ffffffff' : this.a.msg.top, light ? 'ffffffff' : this.a.msg.bottom, alpha, k);
          }
        }
      }
    }

    draw() {
      const cv = this.canvas, { W, H } = this.size();
      if (cv.width !== W || cv.height !== H) { cv.width = W; cv.height = H; }
      const g = cv.getContext('2d');
      g.clearRect(0, 0, W, H);
      if (this.layers && (!this.state.visible || !this.map)) for (const L of Object.values(this.layers)) L.getContext('2d').clearRect(0, 0, L.width, L.height);
      const u = Math.min(H / 456, W / 832);
      if (!this.state.visible) { this.drawMessage(g, W, H, u); return; }   // one layout unit in pixels (a window narrower than 16:9 shrinks the HUD instead of overflowing)
      g.imageSmoothingEnabled = true; g.imageSmoothingQuality = 'high';
      const place = a => ({ cx: (a.edge === 'right' ? W - a.x * u : a.x * u), cy: a.y * u, k: a.scale * u, iconSize: a.iconSize });
      // pane: image drawn centred at (x, y) layout units from the group origin (y up), w x h layout units
      const pane = (P, name, rgb, x, y, w, h) => {
        const img = tinted(name, rgb); if (!img) return;
        g.drawImage(img, P.cx + (x - w / 2) * P.k, P.cy - (y + h / 2) * P.k, w * P.k, h * P.k);
      };

      // ---- TIME (game_image_number.brlyt) ----
      if (!this.only || this.only === 'time') {
        const P = place(this.a.time), t = Math.max(0, this.state.timeMs);
        const min = Math.min(99, Math.floor(t / 60000)), sec = Math.floor(t / 1000) % 60, ms = Math.floor(t % 1000);
        const d = n => 'tt_d_number_3d_' + String(n).padStart(2, '0');
        const O = -81;      // race_null: the first digit; the rest of the group sits relative to it
        pane(P, 'tt_time_E', YELLOW_TEXT, -97 + O, -0.5 + -0.4, 240 * 0.7, 40 * 0.7);
        const dig = (n, x) => pane(P, d(n), YELLOW_DIGIT, O + x, -0.4, 32, 32);
        dig(Math.floor(min / 10), 0); dig(min % 10, 24);
        pane(P, 'tt_d_number_3d_coron_00', YELLOW_DIGIT, O + 41, -0.5, 32, 32);
        dig(Math.floor(sec / 10), 60); dig(sec % 10, 84);
        pane(P, 'tt_d_number_3d_coron', YELLOW_DIGIT, O + 100, -0.5, 32, 32);
        dig(Math.floor(ms / 100), 119); dig(Math.floor(ms / 10) % 10, 142); dig(ms % 10, 166);
      }

      // ---- LAP (game_image_lap.brlyt) ----
      if (!this.only || this.only === 'lap') {
        const P = place(this.a.lap), s = this.state;
        const d = n => 'tt_d_number_3d_' + String(n).padStart(2, '0');
        const RX = -18.1, RY = 13.6;      // race_null: the current lap digit (64x64)
        pane(P, 'tt_lap_E', YELLOW_TEXT, RX - 135.9, RY - 7.1, 240 * 0.9, 40 * 0.9);
        pane(P, d(Math.min(s.lap, 9)), YELLOW_DIGIT, RX, RY, 64, 64);
        pane(P, 'tt_d_number_3d_slash', YELLOW_DIGIT, RX + 23.9, RY - 12.4, 40, 40);
        pane(P, d(s.laps), YELLOW_DIGIT, RX + 43.1, RY - 14.0, 64 * 0.5, 64 * 0.5);
      }

      // ---- minimap ----
      if (this.map && (!this.only || this.only === 'map')) {
        const A = this.a.map, P = place(A), { s, fill, ring } = this.mapImages(u), [x0, z0, x1, z1] = this.map.corners;
        const mx = P.cx, my = P.cy, cxw = (x0 + x1) / 2, czw = (z0 + z1) / 2;
        if (!this.layers) this.layers = { ring: this.blendLayer('multiply'), fill: this.blendLayer(CSS.supports('mix-blend-mode', 'plus-lighter') ? 'plus-lighter' : 'screen') };
        for (const [name, img] of [['ring', ring], ['fill', fill]]) {
          const L = this.layers[name]; if (L.width !== W || L.height !== H) { L.width = W; L.height = H; }
          const lg = L.getContext('2d'); lg.clearRect(0, 0, W, H);
          lg.drawImage(img, Math.round(mx - img.width / 2), Math.round(my - img.height / 2));
        }
        for (const p of this.racers) {
          const ix = mx + (p.x - cxw) * s * u, iy = my + (p.z - czw) * s * u, k = A.icon * u;
          const at = (name, rgb, dx, dy, size, rot) => {
            const img = tinted(name, rgb); if (!img) return;
            g.save(); g.translate(ix + dx * k, iy + dy * k); if (rot) g.rotate(rot);
            g.drawImage(img, -size * k / 2, -size * k / 2, size * k, size * k); g.restore();
          };
          // the heading searchlight (60x64, scale 1.2, pivoting on its bottom centre), yellow; only the player has one
          const light = p.light ? tinted('tt_map_chara_searchlight', '255,255,0') : null;
          if (light) {
            g.save(); g.translate(ix, iy); g.rotate(Math.atan2(p.fx, -p.fz));
            g.drawImage(light, -30 * 1.2 * k, -64 * 1.2 * k, 60 * 1.2 * k, 64 * 1.2 * k); g.restore();
          }
          const icon = CHAR_ICONS[p.character] || CHAR_ICONS[0];
          at(icon, '0,0,0', 1.5, 0, 32 * 2.0); at(icon, '0,0,0', -1.5, 0, 32 * 2.0);   // the black outline: two enlarged shadows
          at(icon, '255,255,255', 0, 0, 32 * 1.6);
        }
      }

      // ---- race position (game_image_position.brlyt): a 90x64 picture, its shadow 6 left / 7 down in black at 59% ----
      if (this.state.rank > 0 && (!this.only || this.only === 'rank')) {
        const P = place(this.a.rank), name = 'tt_position_no_st_64x64_' + String(Math.min(12, this.state.rank)).padStart(2, '0');
        const shadow = ramp(name, '0,0,0', '0,0,0'), main = ramp(name, '0,0,100', '60,160,255');
        if (shadow && main) {
          g.globalAlpha = 150 / 255; g.drawImage(shadow, P.cx + (-6 - 45) * P.k, P.cy - (-7 + 32) * P.k, 90 * P.k, 64 * P.k);
          g.globalAlpha = 1; g.drawImage(main, P.cx + (0 - 45) * P.k, P.cy - (0 + 32) * P.k, 90 * P.k, 64 * P.k);
        }
      }

      // ---- item window (item_window.brlyt): 50x50 box, 40x40 icon ----
      if (this.state.items > 0 && (!this.only || this.only === 'item')) {
        const P = place(this.a.item), n = Math.min(3, this.state.items);
        pane(P, 'tt_item_box_glass_type_02', YELLOW_TEXT, 0, 0, 50, 50);
        const is = P.iconSize || 45;
        pane(P, n > 1 ? 'tt_item_kinoko_' + n : 'tt_item_kinoko', '255,255,255', 0, 0, is, is);
      }

      this.drawMessage(g, W, H, u);
    }
  }
  window.GameHud = GameHud;
})();
