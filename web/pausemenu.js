// The game's in-race pause menu, drawn from its own layout files (exported by tools/export_pause_menu.py into assets/ui/pause_menu.json):
//   button/blyt/common_w004_menu.brlyt   one menu button: panes stacked back to front (base, shadow, frame, pattern, text, glow text, highlights)
//   button/anim/common_w004_menu_*.brlan  its free / select animations (button scale, glow text, dark overlay)
//   message_window/blyt/black_down_race.brlyt  the dimming pattern behind the menu
// and the kart font from hud_messages.json. Panes are rasterised once (texture x vertex colours x material colours) and composed per frame.
// What is read from the files: pane sizes/positions/origins, vertex colours, UV tiling, material fore/back colours, animation keyframes.
// What is approximated: the TEV stages (only "colour = lerp(fore, back, texture intensity) x vertex colour" is modelled), the spacing between
// buttons (set by the game's code, not by the layout) and the fade in/out of the whole menu.
(function () {
  const IMG = {};
  const loadImg = name => IMG[name] || (IMG[name] = new Promise(res => { const i = new Image(); i.onload = () => res(i); i.onerror = () => res(null); i.src = `assets/ui/${name}.png`; }));
  const S = 2;                                     // raster scale: layout units -> layer pixels
  const lerp = (a, b, t) => a + (b - a) * t;
  const hex4 = h => [0, 2, 4, 6].map(i => parseInt(h.slice(i, i + 2), 16));

  // Maya-style animation value at frame f (hermite keys [frame, value, slope] or step keys [frame, value])
  function curve(keys, dtype, f) {
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

  // pane values (sx, sy, alpha 0..255) an animation sets at frame f
  function paneValues(anim, f, out) {
    for (const e of anim.entries) if (!e.mat) for (const t of e.tags) for (const it of t.items) {
      const v = curve(it.keys, it.dtype, f); if (v === null) continue;
      const o = out[e.name] || (out[e.name] = {});
      if (t.type === 'RLPA') { if (it.target === 6) o.sx = v; else if (it.target === 7) o.sy = v; else if (it.target === 0) o.x = v; else if (it.target === 1) o.y = v; }
      else if (t.type === 'RLVC' && it.target === 16) o.alpha = v;
    }
    return out;
  }

  class PauseArt {
    constructor(hud) { this.hud = hud; this.ready = false; this.sel = 0; this.items = []; this.t0 = 0; this.btn = []; this.layers = {}; this.cache = new Map(); }

    async load() {
      if (this.ready) return;
      const [j, bok, chek] = await Promise.all([fetch('assets/ui/pause_menu.json').then(r => r.json()), loadImg('pause_pat_bokashi'), loadImg('pause_pat_chek'), this.hud.loadMessages()]);
      this.J = j; this.tex = { 'tt_pattern_chek_bokashi_32x32.tpl': this.texData(bok), 'tt_pattern_chek_32x32.tpl': this.texData(chek) };
      if (!this.hud.msgs || !this.tex['tt_pattern_chek_bokashi_32x32.tpl']) throw new Error('pause menu art missing');
      await Promise.all(this.hud.msgs.sheets.map(i => i.complete ? 0 : new Promise(r => { i.onload = i.onerror = r; })));   // the font sheets must be in before any label is drawn
      this.fsheets = await Promise.all([...Array(j.font.sheetCount).keys()].map(i => loadImg(`pause_font_${i}`)));
      this.ready = true;
    }

    texData(img) {
      if (!img) return null;
      const c = document.createElement('canvas'); c.width = img.width; c.height = img.height;
      const g = c.getContext('2d'); g.drawImage(img, 0, 0);
      return { w: c.width, h: c.height, d: g.getImageData(0, 0, c.width, c.height).data };
    }

    // pane -> canvas: per pixel, colour = lerp(fore, back, texture intensity) x bilinear vertex colour, alpha likewise
    raster(p) {
      const W = Math.max(1, Math.round(p.w * S)), H = Math.max(1, Math.round(p.h * S));
      const c = document.createElement('canvas'); c.width = W; c.height = H;
      const g = c.getContext('2d'), im = g.createImageData(W, H), d = im.data;
      const T = p.tex && p.tex.length ? this.tex[p.tex[0]] : null, uv = p.uv || [0, 0, 1, 0, 0, 1, 1, 1];
      const vt = p.vtx.map(hex4), pattern = p.fore[3] !== p.back[3];       // fore/back alpha differ: the texture also shapes the alpha
      for (let y = 0; y < H; y++) {
        const v = (y + .5) / H;
        for (let x = 0; x < W; x++) {
          const u = (x + .5) / W, o = (y * W + x) * 4;
          let I = 1, A = 1;
          if (T) {
            const tu = lerp(lerp(uv[0], uv[2], u), lerp(uv[4], uv[6], u), v), tv = lerp(lerp(uv[1], uv[3], u), lerp(uv[5], uv[7], u), v);
            const fx = (tu * T.w - .5), fy = (tv * T.h - .5), x0 = Math.floor(fx), y0 = Math.floor(fy), ax = fx - x0, ay = fy - y0;
            const px = (xx, yy) => ((((yy % T.h) + T.h) % T.h) * T.w + (((xx % T.w) + T.w) % T.w)) * 4;
            let ii = 0, aa = 0;
            for (const [dx, dy, w] of [[0, 0, (1 - ax) * (1 - ay)], [1, 0, ax * (1 - ay)], [0, 1, (1 - ax) * ay], [1, 1, ax * ay]]) { const q = px(x0 + dx, y0 + dy); ii += T.d[q] * w; aa += T.d[q + 3] * w; }
            I = ii / 255; A = aa / 255;
          }
          const col = [0, 1, 2].map(k => lerp(p.fore[k], p.back[k], I));
          let alpha = pattern ? lerp(p.fore[3], p.back[3], I) / 255 : p.back[3] / 255 * A;
          // vertex colours TL, TR, BL, BR, bilinear
          const vc = [0, 1, 2, 3].map(ch => (1 - u) * (1 - v) * vt[0][ch] + u * (1 - v) * vt[1][ch] + (1 - u) * v * vt[2][ch] + u * v * vt[3][ch]);
          d[o] = col[0] * vc[0] / 255; d[o + 1] = col[1] * vc[1] / 255; d[o + 2] = col[2] * vc[2] / 255; d[o + 3] = 255 * alpha * vc[3] / 255;
        }
      }
      g.putImageData(im, 0, 0);
      return c;
    }

    // a label as an offscreen canvas. The game's kart font only has capitals and digits, so other glyphs come from kart_kanji_font (the
    // fallback font). Glyph intensity is mapped fore -> back like the text pane's material.
    textLayer(label, pane, wUnits, kind) {
      if (pane.name === 'text') pane = Object.assign({}, pane, kind === 'free' ? { fore: [90, 70, 20, 0], back: [190, 184, 172, 255] } : { fore: [140, 105, 20, 0], back: [255, 255, 255, 255] });   // selected = text_light_02_stop frame 0; free = the dimmer text seen in game
      const key = label + '|' + pane.name + '|' + (kind || '');
      let c = this.cache.get(key); if (c) return c;
      const fonts = [{ F: this.hud.msgs.font, sheets: this.hud.msgs.sheets }, { F: this.J.font, sheets: this.fsheets }];
      const run = [...label].map(ch => {
        for (const ft of fonts) { const gi = ft.F.map[ch.codePointAt(0)]; if (gi !== undefined && ft.F.widths[gi]) return { ft, gi, w: ft.F.widths[gi], k: pane.fontW / ft.F.cellW, kh: pane.fontH / ft.F.cellH }; }
        return null;
      }).filter(Boolean);
      const natural = run.reduce((a, r) => a + r.w[2] * r.k, 0), fit = Math.min(1, wUnits / Math.max(1, natural));   // long names squash, like the game's text panes
      const W = Math.round(wUnits * S) + 8, H = Math.round(pane.fontH * 1.4 * S);
      c = document.createElement('canvas'); c.width = W; c.height = H;
      const g = c.getContext('2d');
      let x = W / 2 - natural * fit * S / 2; const y0 = H / 2 - pane.fontH * S / 2;
      for (const r of run) {
        const F = r.ft.F, per = F.cols * F.rows, sheet = r.ft.sheets[Math.floor(r.gi / per)], idx = r.gi % per;
        if (sheet) g.drawImage(sheet, (idx % F.cols) * (F.cellW + 1), Math.floor(idx / F.cols) * (F.cellH + 1), F.cellW, F.cellH,
                               x + r.w[0] * r.k * fit * S, y0, F.cellW * r.k * fit * S, F.cellH * r.kh * S);
        x += r.w[2] * r.k * fit * S;
      }
      const im = g.getImageData(0, 0, W, H), d = im.data;
      for (let i = 0; i < d.length; i += 4) {
        const I = d[i] / 255, a = d[i + 3];
        d[i] = lerp(pane.fore[0], pane.back[0], I); d[i + 1] = lerp(pane.fore[1], pane.back[1], I); d[i + 2] = lerp(pane.fore[2], pane.back[2], I); d[i + 3] = a;
      }
      g.putImageData(im, 0, 0);
      this.cache.set(key, c);
      return c;
    }

    // ---- state ----
    // items: [{label, current}], sel: index. Frames count at the game's 59.94 per second.
    setItems(items, sel) {
      this.items = items;
      this.btn = items.map((it, i) => ({ state: i === sel ? 'sel' : 'free', t0: performance.now() }));
      this.sel = sel; this.top = 0; this.scrollTo(sel, true);
    }
    select(i) {
      if (i === this.sel || !this.btn[i]) return;
      const now = performance.now();
      this.btn[this.sel] = { state: 'free', t0: now, from: 'sel' }; this.btn[i] = { state: 'sel', t0: now, from: 'free' };
      this.sel = i; this.scrollTo(i);
    }
    // the window of rows that fit on screen follows the selection
    get rows() { return 5; }
    scrollTo(i, snap) {
      const n = this.items.length, r = this.rows;
      this.top = Math.max(0, Math.min(Math.max(0, n - r), i - Math.floor(r / 2)));
    }

    // pane values of one button right now
    values(b, now) {
      const f = (now - b.t0) * 59.94 / 1000, A = this.J.anims, out = {};
      if (b.state === 'sel') {
        if (b.from === 'free' && f < A.free_to_select.frames) paneValues(A.free_to_select, f, out);
        else if (b.from === 'free') paneValues(A.select, (f - A.free_to_select.frames) % A.select.frames, out);
        else paneValues(A.select, f % A.select.frames, out);
      } else {
        if (b.from === 'sel' && f < A.select_to_free.frames) paneValues(A.select_to_free, f, out); else paneValues(A.free, 0, out);
      }
      return out;
    }

    // pane origin (0..8 as a 3x3 grid, 4 = centre) -> offset of the pane's top-left from its position
    static topLeft(p, w, h) { const col = p.origin % 3, row = Math.floor(p.origin / 3); return [-w * col / 2, h * row / 2]; }

    // The button body is set by the game's code, not by the layout file, so it is matched to a Dolphin recording of the real pause menu:
    // a black outline, a thin yellow rim and a dark see-through fill; the selected button has a brighter rim and an almost clear fill.
    // The pane values (button scale, glow text, "color_down" overlay) are still the layout's own animations.
    drawButton(g, label, cx, cy, k, v, alphaAll) {
      const P = this.J.button.panes, root = P.find(p => p.name === 'pause_w_fade_n'), rv = v[root.name] || {};
      const rs = rv.sx ?? root.sx, down = Math.max(0, Math.min(1, (v.color_down ? v.color_down.alpha : 100) / 100)), t = 1 - down;   // 0 free .. 1 selected
      g.save(); g.translate(cx, cy); g.scale(rs * k, rs * k); g.globalAlpha = alphaAll;
      const W = 506, H = 65, mix = (a, b) => Math.round(lerp(a, b, t));
      g.fillStyle = '#000'; g.fillRect(-W / 2, -H / 2, W, H);                                          // outer black outline
      g.fillStyle = `rgb(${mix(116, 200)},${mix(117, 191)},${mix(10, 3)})`; g.fillRect(-W / 2 + 2, -H / 2 + 2, W - 4, H - 4);   // yellow rim
      g.globalCompositeOperation = 'destination-out'; g.globalAlpha = 1; g.fillRect(-W / 2 + 5, -H / 2 + 5, W - 10, H - 10); g.globalCompositeOperation = 'source-over';   // hollow
      g.globalAlpha = alphaAll * lerp(0.62, 0.06, t); g.fillStyle = '#0a0802'; g.fillRect(-W / 2 + 5, -H / 2 + 5, W - 10, H - 10);                   // fill
      for (const p of P) {
        if (p.type !== 'txt1') continue;
        const pv = v[p.name] || {}, a = (pv.alpha ?? p.alpha) / 255, sx = pv.sx ?? p.sx, sy = pv.sy ?? p.sy;
        if (a <= 0 || !sx) continue;
        g.save(); g.translate(p.x, -p.y); g.scale(sx, sy);
        if (p.name === 'text') {
          // free text is a dimmer grey-white, selected is white (cross-faded)
          for (const [kind, w] of [['free', 1 - t], ['sel', t]]) {
            if (w <= 0.01) continue;
            const T = this.textLayer(label, p, p.w, kind);
            g.globalAlpha = alphaAll * a * w; g.drawImage(T, -T.width / S / 2, -T.height / S / 2, T.width / S, T.height / S);
          }
        } else { g.globalAlpha = alphaAll * a; const T = this.textLayer(label, p, p.w); g.drawImage(T, -T.width / S / 2, -T.height / S / 2, T.width / S, T.height / S); }
        g.restore();
      }
      g.restore();
    }

    // everything: dim, then the visible rows of buttons. open: 0..1 fade
    draw(g, W, H, open) {
      if (!this.ready) return;
      const u = Math.min(H / 456, W / 832), now = performance.now();
      g.clearRect(0, 0, W, H);
      g.save(); g.translate(W / 2, H / 2);
      const pitch = 75, n = this.items.length, rows = Math.min(this.rows, n), y0 = -(rows - 1) * pitch / 2;
      this.hit = [];
      for (let r = 0; r < rows; r++) {
        const i = this.top + r, it = this.items[i]; if (!it) continue;
        const y = y0 + r * pitch, b = this.btn[i];
        const slide = 1 - Math.min(1, Math.max(0, open * 1.6 - r * 0.12));          // buttons slide in a little one after another
        this.drawButton(g, it.label, 0, (y + slide * 26) * u, u, this.values(b, now), open);
        this.hit.push({ i, x: (W / 2 - 250 * u), y: H / 2 + (y - 30) * u, w: 500 * u, h: 60 * u });
        if (it.current) { g.save(); g.globalAlpha = open; g.fillStyle = '#7CFF6B'; g.beginPath(); g.arc(-262 * u, y * u, 5 * u, 0, 7); g.fill(); g.restore(); }
      }
      if (n > rows) {                                                                // scroll marks
        g.fillStyle = 'rgba(255,255,255,.85)'; g.globalAlpha = open;
        if (this.top > 0) { g.beginPath(); g.moveTo(0, (y0 - 52) * u); g.lineTo(-12 * u, (y0 - 38) * u); g.lineTo(12 * u, (y0 - 38) * u); g.fill(); }
        if (this.top + rows < n) { const yb = (y0 + (rows - 1) * pitch + 52) * u; g.beginPath(); g.moveTo(0, yb); g.lineTo(-12 * u, yb - 14 * u); g.lineTo(12 * u, yb - 14 * u); g.fill(); }
        g.globalAlpha = 1;
      }
      g.restore();
    }
    hitTest(x, y) { for (const h of this.hit || []) if (x >= h.x && x <= h.x + h.w && y >= h.y && y <= h.y + h.h) return h.i; return -1; }
  }
  window.PauseArt = PauseArt;
})();
