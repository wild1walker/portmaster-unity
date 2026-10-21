"""server.py <port> [<cert> <key>]: a local HTTP(S) server for tls_test.sh (127.0.0.1 only): GET answers
"hello from tls.test", POST/PUT answer "got <n> bytes", /big a 300000-byte body."""
import http.server, ssl, sys

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def reply(self, body, ctype='text/plain'):
        self.send_response(200)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('X-Echo', self.headers.get('X-Test', '-'))
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def do_GET(self):
        self.reply(b'x' * 300000 if self.path == '/big' else b'hello from tls.test\n')

    do_HEAD = do_GET

    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0)
        data = self.rfile.read(n)
        self.reply(b'got %d bytes %s\n' % (len(data), data[:16]))

    do_PUT = do_POST

    def log_message(self, fmt, *a):
        sys.stderr.write('[server %s] %s\n' % (sys.argv[1], fmt % a))

s = http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), H)
if len(sys.argv) > 3:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(sys.argv[2], sys.argv[3])
    s.socket = ctx.wrap_socket(s.socket, server_side=True)
s.serve_forever()
