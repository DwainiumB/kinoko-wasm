// Boot gate for hosting the page without the game's assets (GitHub Pages etc.).
// If web/assets is reachable from the server (local dev) the app starts at once. Otherwise the visitor is asked to pick the
// assets folder that Kinoko's tools produced on their own machine (tools/export_*.py write it to web/assets); sw.js then serves
// every assets/... request from that folder, so none of it is ever uploaded anywhere.
// The app itself is the inline <script type="text/plain" id="mainApp"> in index.html, which this script starts when ready.
(() => {
  const MARKER = 'common/Common.szs';
  const DB = 'kinoko-assets', STORE = 'handles';
  let provider = null, pending = null;   // pending: starts the app once a folder is connected   // { name, get(path) -> Promise<File|null> }

  const idb = () => new Promise((res, rej) => {
    const r = indexedDB.open(DB, 1);
    r.onupgradeneeded = () => r.result.createObjectStore(STORE);
    r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error);
  });
  const idbGet = async k => { try { const db = await idb(); return await new Promise(res => { const q = db.transaction(STORE).objectStore(STORE).get(k); q.onsuccess = () => res(q.result); q.onerror = () => res(null); }); } catch (e) { return null; } };
  const idbPut = async (k, v) => { try { const db = await idb(); await new Promise(res => { const t = db.transaction(STORE, 'readwrite'); t.objectStore(STORE).put(v, k); t.oncomplete = t.onerror = () => res(); }); } catch (e) {} };

  // ---- providers -------------------------------------------------------------------------------------------------------
  // A directory handle (Chromium's showDirectoryPicker). `root` is the folder that holds common/, tracks/, karts/...
  function handleProvider(root, name) {
    const dirs = new Map([['', root]]);
    const dirOf = async parts => {
      const key = parts.join('/');
      if (dirs.has(key)) return dirs.get(key);
      const parent = await dirOf(parts.slice(0, -1));
      const d = parent && await parent.getDirectoryHandle(parts[parts.length - 1]).catch(() => null);
      dirs.set(key, d);
      return d;
    };
    return {
      name,
      async get(path) {
        const parts = path.split('/').filter(Boolean), file = parts.pop();
        const d = await dirOf(parts);
        const fh = d && await d.getFileHandle(file).catch(() => null);
        return fh ? fh.getFile() : null;
      },
    };
  }
  // A flat list of Files (<input webkitdirectory>, or a dropped folder); paths come from webkitRelativePath.
  function listProvider(files, name) {
    const map = new Map();
    for (const f of files) map.set(f.webkitRelativePath || f._rel || f.name, f);
    // The picked folder may be the assets folder itself, web/, or the repository: find the prefix in front of 'common/Common.szs'.
    let prefix = null;
    for (const k of map.keys()) if (k.endsWith('/' + MARKER) || k === MARKER) { prefix = k.slice(0, k.length - MARKER.length); break; }
    if (prefix === null) prefix = '';
    return { name, get: async path => map.get(prefix + path) || null, prefix };
  }

  // The picked handle might be web/ or the repo rather than assets/ itself.
  async function findRoot(handle) {
    const has = async (d, p) => (await new handleProvider(d, '').get(p)) !== null;
    if (await has(handle, MARKER)) return handle;
    for (const sub of ['assets', 'web/assets']) {
      let d = handle;
      try { for (const s of sub.split('/')) d = await d.getDirectoryHandle(s); } catch (e) { continue; }
      if (await has(d, MARKER)) return d;
    }
    return null;
  }

  // ---- service worker plumbing -----------------------------------------------------------------------------------------
  navigator.serviceWorker && navigator.serviceWorker.addEventListener('message', async ev => {
    if (!ev.data || ev.data.type !== 'kinoko-asset' || !ev.ports[0]) return;
    let file = null;
    try { file = provider ? await provider.get(ev.data.path.replace(/^assets\//, '')) : null; } catch (e) {}
    ev.ports[0].postMessage({ file });
  });
  async function registerSW() {
    if (!('serviceWorker' in navigator)) throw new Error('This browser has no service worker support (needed to read your local files).');
    await navigator.serviceWorker.register('sw.js');
    await navigator.serviceWorker.ready;
    if (!navigator.serviceWorker.controller) await new Promise(res => { navigator.serviceWorker.addEventListener('controllerchange', res, { once: true }); setTimeout(res, 3000); });
  }

  // ---- UI --------------------------------------------------------------------------------------------------------------
  let ui = null;
  function showUI() {
    if (ui) return ui;
    const el = document.createElement('div');
    el.style.cssText = 'position:fixed;inset:0;z-index:1000;background:#111;color:#eee;font:15px/1.5 sans-serif;display:flex;align-items:center;justify-content:center;padding:16px;box-sizing:border-box';
    el.innerHTML = `<div style="max-width:620px">
      <h2 style="margin:0 0 8px">Kinoko Web</h2>
      <p>This page contains the physics engine only &mdash; no Mario Kart Wii assets are hosted here.
         It runs on the game files from <b>your own copy of the game</b>, read locally by your browser; nothing is uploaded.</p>
      <ol style="padding-left:20px">
        <li>Get the exporter scripts: <a href="https://github.com/DwainiumB/kinoko-wasm/tree/main/tools" style="color:#9fd8ff" target="_blank" rel="noopener">the <code>tools</code> folder</a>
            (download the repo as a ZIP, or clone it). You need Python 3 with <code>numpy</code> and <code>pillow</code>.</li>
        <li>Extract your own Mario Kart Wii disc to a folder, then run
            <code>python tools/export_all.py &lt;that folder&gt;</code> (about 6 minutes). It writes a <code>web/assets</code> folder.
            Tested with a <b>PAL</b> game dump only; other regions are untested.
            <a href="https://github.com/DwainiumB/kinoko-wasm/blob/main/docs/WEB_PLAYER.md" style="color:#9fd8ff" target="_blank" rel="noopener">Step-by-step guide</a>.</li>
        <li>Pick that <code>assets</code> folder below.</li>
      </ol>
      <div id="gateBtns" style="margin:14px 0"></div>
      <div id="gateMsg" style="color:#9fd8ff;white-space:pre-wrap"></div>
    </div>`;
    document.body.appendChild(el);
    const btn = (label, fn) => { const b = document.createElement('button'); b.textContent = label; b.style.cssText = 'font-size:15px;padding:8px 14px;margin-right:8px;background:#0078d4;color:#fff;border:0;border-radius:4px;cursor:pointer'; b.onclick = fn; el.querySelector('#gateBtns').appendChild(b); return b; };
    ui = { el, btn, msg: t => { el.querySelector('#gateMsg').textContent = t; }, clear: () => { el.querySelector('#gateBtns').textContent = ''; } };
    return ui;
  }

  // Checks the folder looks right; returns an error message or null.
  async function validate(p) {
    if (!(await p.get(MARKER))) return `That folder has no ${MARKER}. Pick the assets folder your export tools wrote (web/assets).\n`
      + 'If it was made by an older tools/export_all.py, copy Race/Common.szs from your game files into assets/common/ (or update the repo and run: '
      + 'python tools/export_all.py <your game folder> --only common), then try again.';
    return null;
  }

  async function connect(p) {
    const err = await validate(p);
    if (err) return err;
    provider = p;
    await registerSW();
    return null;
  }

  async function pickFlow(ready) {
    const u = showUI();
    pending = () => { pending = null; u.el.remove(); ui = null; ready(); };
    const finish = async p => {
      u.msg('Checking ' + p.name + ' ...');
      let err;
      try { err = await connect(p); } catch (e) { err = 'Could not start the file bridge: ' + e.message; }
      if (err) { provider = null; u.msg(err); return; }
      pending && pending();
    };
    const stored = await idbGet('dir');
    if (stored) {
      u.btn('Reconnect "' + stored.name + '"', async () => {
        try {
          if ((await stored.requestPermission({ mode: 'read' })) !== 'granted') { u.msg('Permission was not granted.'); return; }
          const root = await findRoot(stored);
          if (!root) { u.msg('That folder no longer contains the assets.'); return; }
          await finish(handleProvider(root, stored.name));
        } catch (e) { u.msg('Could not reopen it: ' + e.message); }
      });
    }
    if (window.showDirectoryPicker) {
      u.btn(stored ? 'Choose a different folder' : 'Choose assets folder', async () => {
        try {
          const h = await window.showDirectoryPicker({ id: 'kinoko-assets', mode: 'read' });
          const root = await findRoot(h);
          if (!root) { u.msg(`No ${MARKER} found in "${h.name}". Pick the assets folder your export tools wrote (web/assets).\n`
            + 'If it was made by an older tools/export_all.py, copy Race/Common.szs from your game files into assets/common/ (or update the repo and run: '
            + 'python tools/export_all.py <your game folder> --only common), then try again.'); return; }
          await idbPut('dir', h);
          await finish(handleProvider(root, h.name));
        } catch (e) { if (e.name !== 'AbortError') u.msg('Could not open it: ' + e.message); }
      });
    } else {
      // Firefox / Safari: no persistent handles, so the folder has to be picked on every visit.
      const inp = document.createElement('input');
      inp.type = 'file'; inp.webkitdirectory = true; inp.multiple = true; inp.style.display = 'none';
      inp.onchange = () => { if (inp.files.length) finish(listProvider(Array.from(inp.files), 'selected folder')); };
      u.el.appendChild(inp);
      u.btn('Choose assets folder', () => inp.click());
      u.msg('Your browser cannot remember the folder, so you will be asked each visit.');
    }
  }

  // Test / scripting hook: hand the gate a ready-made file map { 'common/Common.szs': File, ... }.
  window.KinokoAssets = {
    useProvider: async (get, name = 'provider') => { const e = await connect({ name, get }); if (!e && pending) pending(); return e; },
    useFiles: async (map, name = 'files') => window.KinokoAssets.useProvider(async p => map.get(p) || null, name),
    get connected() { return !!provider; },
  };

  // ---- boot ------------------------------------------------------------------------------------------------------------
  const domReady = new Promise(r => document.readyState === 'loading' ? document.addEventListener('DOMContentLoaded', r) : r());
  const startApp = () => {
    const s = document.createElement('script');
    s.textContent = document.getElementById('mainApp').textContent;
    document.body.appendChild(s);
  };
  (async () => {
    await domReady;
    // Local dev server that really has the assets: nothing to do (add ?pick=1 to force the picker).
    let local = false;
    if (!/[?&]pick=1/.test(location.search)) {
      try { local = (await fetch('assets/' + MARKER, { method: 'HEAD', cache: 'no-store' })).ok; } catch (e) {}
    }
    if (local) { startApp(); return; }
    // A folder picked earlier: if the browser still lets this page read it, go straight in without showing the picker. This is what
    // makes a page reload (an online race starts by reloading every player's page) seamless; Chromium keeps the permission for the
    // tab, or for good if "Allow on every visit" was chosen in its prompt. Otherwise fall through to the picker's Reconnect button.
    try {
      const stored = await idbGet('dir');
      if (stored && stored.queryPermission && (await stored.queryPermission({ mode: 'read' })) === 'granted') {
        const root = await findRoot(stored);
        if (root && !(await connect(handleProvider(root, stored.name)))) { startApp(); return; }
      }
    } catch (e) { provider = null; }
    await pickFlow(startApp);
  })();
})();
