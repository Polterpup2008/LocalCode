"""Loopback HTTP fixture for native transport tests; no model, token or external network."""
import argparse
import http.server
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import threading
import time


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    next_connection = 0
    count_lock = threading.Lock()

    def handle_error(self, request, client_address):
        if isinstance(sys.exc_info()[1], (ConnectionResetError, ConnectionAbortedError, BrokenPipeError)):
            return
        super().handle_error(request, client_address)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def setup(self):
        super().setup()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with self.server.count_lock:
            self.server.next_connection += 1
            self.connection_id = self.server.next_connection

    def log_message(self, *_):
        pass

    def do_HEAD(self):
        self.handle_request()

    def do_GET(self):
        self.handle_request()

    def do_POST(self):
        self.handle_request()

    def handle_request(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        path = self.path.split('?')[0]
        if path in ('/redirect', '/redirect-other'):
            self.send_response(302)
            self.send_header('Location', '/echo' if path == '/redirect' else
                             f'http://localhost:{self.server.server_port}/echo')
            self.send_header('Content-Length', '12')
            self.end_headers()
            if self.command != 'HEAD':
                self.wfile.write(b'intermediate')
            return
        if path == '/loop':
            self.send_response(302)
            self.send_header('Location', '/loop')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if path == '/sse':
            parts = [b'data: caf', b'\xc3', b'\xa9\n\n', b'data: [DONE]\n\n']
        elif path == '/heartbeat':
            parts = [b'a', b'b', b'c', b'd', b'e', b'f']
        elif path == '/big':
            parts = [bytes(range(256)) * 8]
        elif path == '/missing':
            parts = [b'not found']
        elif path == '/stall':
            parts = [b'late']
        else:
            parts = [json.dumps({'method': self.command, 'body': body.decode('utf-8'),
                                 'authorization': self.headers.get('Authorization', ''),
                                 'cookie': self.headers.get('Cookie', ''),
                                 'connection': self.connection_id}).encode()]
        self.send_response(404 if path == '/missing' else 200)
        self.send_header('Content-Type', 'text/event-stream' if path == '/sse' else 'application/octet-stream')
        self.send_header('Content-Length', str(sum(map(len, parts))))
        if path == '/set-cookie':
            self.send_header('Set-Cookie', 'fixture=must-not-leak; Path=/')
        self.end_headers()
        if self.command == 'HEAD':
            return
        try:
            if path == '/stall':
                time.sleep(3)
            for part in parts:
                if path in ('/sse', '/heartbeat'):
                    time.sleep(.08)
                self.wfile.write(part)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--performance-output', type=Path)
    parser.add_argument('--certificate', type=Path)
    parser.add_argument('--private-key', type=Path)
    parser.add_argument('--trusted-tls-url')
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    server = Server(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = os.environ.copy()
    env['LOCALCODE_TEST_HTTP_BASE_URL'] = f'http://127.0.0.1:{server.server_port}'
    tls_server = None
    tls_thread = None
    if bool(args.certificate) != bool(args.private_key):
        parser.error('--certificate and --private-key must be supplied together')
    if args.certificate:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(args.certificate), str(args.private_key))
        tls_server = Server(('127.0.0.1', 0), Handler)
        tls_server.socket = context.wrap_socket(tls_server.socket, server_side=True)
        tls_thread = threading.Thread(target=tls_server.serve_forever, daemon=True)
        tls_thread.start()
        env['LOCALCODE_TEST_HTTP_UNTRUSTED_TLS_URL'] = f'https://localhost:{tls_server.server_port}/echo'
    if args.trusted_tls_url:
        env['LOCALCODE_TEST_HTTP_TRUSTED_TLS_URL'] = args.trusted_tls_url
    if args.performance_output:
        env['LOCALCODE_TEST_HTTP_PERFORMANCE_OUTPUT'] = str(args.performance_output.resolve())
    try:
        result = subprocess.run([str(binary), '[http-transport]'], env=env, timeout=180)
        return result.returncode
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)
        if tls_server:
            tls_server.shutdown()
            tls_server.server_close()
            tls_thread.join(timeout=3)


if __name__ == '__main__':
    raise SystemExit(main())
