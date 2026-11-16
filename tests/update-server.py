#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stand-in for GitHub releases, for tests/update-e2e.sh.

    update-server.py ROOT CERT KEY PORTFILE

Serves ROOT over HTTPS on 127.0.0.1 (a free port, written to PORTFILE):

    /releases/download/vVER/NAME   -> ROOT/vVER/NAME
    /releases/latest/download/NAME -> 302 to /redirect/latest/NAME
    /redirect/latest/NAME          -> 302 to /releases/download/vLATEST/NAME

LATEST is the text of ROOT/latest; without it the latest URLs answer 404,
as GitHub does before the first release. Two redirects, like GitHub's
(release page, then the asset host). If ROOT/short exists, zip downloads
stop after half of their Content-Length.
"""
import http.server
import os
import ssl
import sys

root, cert, key, portfile = sys.argv[1:5]


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("server: " + fmt % args + "\n")

    def redirect(self, path):
        self.send_response(302)
        self.send_header("Location", path)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def not_found(self):
        body = b"<!DOCTYPE html><html><body>Not Found</body></html>\n"
        self.send_response(404)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parts = self.path.split("/")
        latest_file = os.path.join(root, "latest")
        if self.path.startswith("/releases/latest/download/") and len(parts) == 5:
            if not os.path.exists(latest_file):
                return self.not_found()
            return self.redirect("/redirect/latest/" + parts[4])
        if self.path.startswith("/redirect/latest/") and len(parts) == 4:
            with open(latest_file) as f:
                latest = f.read().strip()
            return self.redirect("/releases/download/v%s/%s" % (latest, parts[3]))
        if not (self.path.startswith("/releases/download/v") and len(parts) == 5):
            return self.not_found()
        name = parts[4]
        path = os.path.join(root, parts[3], name)
        if ".." in self.path or not os.path.isfile(path):
            return self.not_found()
        with open(path, "rb") as f:
            body = f.read()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if name.endswith(".zip") and os.path.exists(os.path.join(root, "short")):
            self.wfile.write(body[: len(body) // 2])
            self.wfile.flush()
            self.close_connection = True
            return
        self.wfile.write(body)


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)
server.socket = ctx.wrap_socket(server.socket, server_side=True)
with open(portfile + ".tmp", "w") as f:
    f.write(str(server.server_address[1]))
os.rename(portfile + ".tmp", portfile)
server.serve_forever()
