# Online play: the relay

Players race each other peer to peer over WebRTC. A small **relay** only introduces browsers to each other (it passes the
WebRTC handshake messages) and keeps the list of public lobbies. It never sees race data. It is a Cloudflare Worker in
`server/worker/`, because GitHub Pages can only serve static files.

```
 your browser  <------ WebRTC data channels (race data) ------>  other players' browsers
      |                                                                  |
      +--------- WebSocket (handshake only) --> relay <------------------+
                                  GET/POST /rooms (public lobby list)
```

## Deploy it (once, about 10 minutes, free plan is enough for friends-and-family use)

1. Make a free account at https://dash.cloudflare.com/sign-up.
2. Install Node.js 20 or newer from https://nodejs.org .
3. In a terminal:
   ```
   cd server/worker
   npm install
   npx wrangler login
   npx wrangler deploy
   ```
   `wrangler login` opens a browser window for you to sign in; nothing is stored in this repository.
   `deploy` prints the relay's address, something like `https://kinoko-relay.<your-name>.workers.dev`.
4. Put that address in `web/config.js`:
   ```js
   window.KINOKO_RELAY = 'https://kinoko-relay.<your-name>.workers.dev';
   ```
   Commit and push; the Pages site then uses it by default. Players can still type another relay under "Relay server" in the Online panel.
5. Optional but recommended: only let your own page use the relay. In `server/worker/wrangler.toml` change
   ```
   ALLOWED_ORIGINS = "https://<your-github-name>.github.io"
   ```
   and run `npx wrangler deploy` again. (`*` allows any website. Local testing from `http://localhost:8081` then needs
   `"https://<you>.github.io,http://localhost:8081"`.)

## Try it locally (no account needed)

```
cd server/worker
npm install
npx wrangler dev --port 8787
```
Open two browser tabs on the page (served from a local `web/` folder that has `assets/`), open **Online** in each, host a room in one and
join it from the public list in the other. The default relay address is `http://localhost:8787`, which is what `wrangler dev` serves.
`node test.mjs` (in `server/worker`, with `wrangler dev` running) checks the relay's protocol: joining, forwarding, reconnecting,
size and room limits, and the lobby list.

## What it does and limits

- **Rooms:** a room code (3 to 24 letters/digits), at most 12 players. The same player id joining again (a page reload, which is how a race
  starts) replaces its old connection.
- **Public lobby list:** the host registers the room and sends a heartbeat every 20 s; an entry disappears 75 s after the last one, when the host
  leaves, or when the race starts. Hosts are identified by a random token kept in their tab, so nobody else can edit or close a room.
- **Abuse limits:** messages over 64 KB close the connection, a socket is limited to 400 messages per 10 s, a client address can register
  10 rooms per minute, the lobby holds at most 300 rooms.
- **Not covered:** the page only uses Google's public STUN servers. Players behind strict NATs, mobile data or some corporate networks may not
  be able to connect; fixing that needs a TURN server (Cloudflare, Metered and others offer one) added to `ICE` in `web/online.js`.
- **Privacy:** players see each other's IP addresses (that is how peer to peer works) and the page contacts Google's STUN servers.
  The relay's code stores only the lobby entries above, for as long as they live, and writes no logs of its own (Cloudflare itself keeps its usual request logs).
- **Cost:** the relay uses Durable Objects with WebSocket hibernation, so an idle room costs nothing. Check Cloudflare's current free-plan
  limits before relying on it; they change.
