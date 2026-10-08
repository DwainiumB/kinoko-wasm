"""Static server for web/ on :8081 that never lets the browser cache (so edits show on a normal reload)."""
import functools
import http.server
import os

WEB = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'web')


class NoCache(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cache-Control', 'no-store, max-age=0')
        super().end_headers()


http.server.ThreadingHTTPServer(('0.0.0.0', 8081), functools.partial(NoCache, directory=WEB)).serve_forever()
