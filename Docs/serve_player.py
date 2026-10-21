#!/usr/bin/env python3
"""
YouKnow A/B Listening Lab — Dedicated Audio HTTP Server
Serves Docs/ directory with proper audio MIME types and HTTP range request support,
then automatically opens your default browser.
"""
import http.server
import socketserver
import webbrowser
import os
import sys

class AudioHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        '.wav': 'audio/wav',
        '.html': 'text/html; charset=utf-8',
        '.js': 'application/javascript',
        '.css': 'text/css',
    }

    def end_headers(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Cache-Control', 'no-cache, no-store, must-revalidate')
        super().end_headers()

def run():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)
    
    port = 8000
    httpd = None
    for p in range(8000, 8030):
        try:
            httpd = socketserver.TCPServer(("127.0.0.1", p), AudioHandler)
            port = p
            break
        except OSError:
            continue

    if not httpd:
        print("Error: Could not bind to any port in range 8000-8030.")
        sys.exit(1)

    url = f"http://127.0.0.1:{port}/ab_player.html"
    print("=" * 68)
    print(f"🎛️  YouKnow A/B Listening Lab Server Active!")
    print(f"🔗  URL: {url}")
    print("=" * 68)
    print("Opening browser...")
    webbrowser.open(url)
    print("Press Ctrl+C to terminate.")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping audio server.")
        httpd.server_close()

if __name__ == '__main__':
    run()
