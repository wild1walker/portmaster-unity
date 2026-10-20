"""server.py <port> <cert> <key>: a local HTTPS server for tls_test.sh (127.0.0.1 only)."""
import http.server, ssl, sys

class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'hello from tls.test\n'
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *a):
        sys.stderr.write('[server %s] %s\n' % (sys.argv[1], fmt % a))

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(sys.argv[2], sys.argv[3])
s = http.server.HTTPServer(('127.0.0.1', int(sys.argv[1])), H)
s.socket = ctx.wrap_socket(s.socket, server_side=True)
s.serve_forever()
