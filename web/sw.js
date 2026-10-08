// Serves everything under assets/ from the folder the visitor picked on their own machine (see assetgate.js), so the page
// can be hosted (e.g. GitHub Pages) without distributing any game assets. A request for assets/<path> is forwarded to an open
// page, which reads the file from the picked folder and sends it back; if no page has a folder connected, the request falls
// through to the network (that is how a local dev server that really has web/assets keeps working).
const TYPES = {
  json: 'application/json', png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', glb: 'model/gltf-binary', gltf: 'model/gltf+json',
  wasm: 'application/wasm', js: 'text/javascript', txt: 'text/plain', wav: 'audio/wav', ogg: 'audio/ogg', mp3: 'audio/mpeg',
};

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));

self.addEventListener('fetch', e => {
  const u = new URL(e.request.url);
  if (u.origin !== location.origin || e.request.method !== 'GET') return;
  const scope = new URL(self.registration.scope).pathname;
  if (!u.pathname.startsWith(scope + 'assets/')) return;
  e.respondWith(serve(e.request, decodeURIComponent(u.pathname.slice(scope.length))));
});

// Asks one page for a file. Resolves to the File, or null if that page has no folder / no such file.
function ask(client, path) {
  return new Promise(resolve => {
    const ch = new MessageChannel();
    const timer = setTimeout(() => resolve(null), 15000);
    ch.port1.onmessage = ev => { clearTimeout(timer); resolve(ev.data && ev.data.file || null); };
    client.postMessage({ type: 'kinoko-asset', path }, [ch.port2]);
  });
}

async function serve(req, path) {
  const pages = await self.clients.matchAll({ type: 'window', includeUncontrolled: true });
  for (const c of pages) {
    const file = await ask(c, path);
    if (!file) continue;
    const ext = path.split('.').pop().toLowerCase();
    const headers = { 'Content-Type': file.type || TYPES[ext] || 'application/octet-stream', 'Accept-Ranges': 'bytes', 'Cache-Control': 'no-store' };
    const range = /^bytes=(\d*)-(\d*)$/.exec(req.headers.get('range') || '');
    if (range && (range[1] || range[2])) {
      const size = file.size;
      let start = range[1] ? +range[1] : Math.max(0, size - +range[2]);
      let end = range[1] && range[2] ? Math.min(+range[2], size - 1) : size - 1;
      if (start >= size) return new Response(null, { status: 416, headers: { 'Content-Range': `bytes */${size}` } });
      headers['Content-Range'] = `bytes ${start}-${end}/${size}`;
      return new Response(file.slice(start, end + 1), { status: 206, headers });
    }
    return new Response(file, { status: 200, headers });
  }
  return fetch(req);
}
