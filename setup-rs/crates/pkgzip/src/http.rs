//! HTTP(S) for the setup's downloads from Unity's servers, as nsis.py's Source and urllib do it:
//! GET requests on one kept-alive connection a host (reopened after errors and when the server
//! closes it), redirects followed (301, 302, 303, 307, 308; up to 5), the environment's proxy
//! (`https_proxy` / `http_proxy`, `no_proxy`: https through a CONNECT tunnel, http with the whole
//! URL as the request's target), a 120 s timeout on every read and write. TLS is rustls (ring)
//! with the system's CA certificates: `$SSL_CERT_FILE`, `$SSL_CERT_DIR`, else the first of the
//! usual bundle files (what the setup's patchscript also looks for).
use crate::{Error, Result};
use rustls::pki_types::pem::PemObject;
use rustls::pki_types::{CertificateDer, ServerName};
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::sync::Arc;
use std::time::Duration;

const TIMEOUT: Duration = Duration::from_secs(120);
/// The CA bundles looked for when $SSL_CERT_FILE is not set (the patchscript's list).
pub const CA_FILES: [&str; 4] = [
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/pki/tls/certs/ca-bundle.crt",
    "/etc/ssl/cert.pem",
    "/etc/ssl/ca-bundle.pem",
];

fn io(m: impl Into<String>) -> std::io::Error {
    std::io::Error::other(m.into())
}

/// A URL's parts (urllib.parse.urlsplit's, for http and https).
#[derive(Clone, Debug, PartialEq)]
pub struct Url {
    pub scheme: String,
    /// host[:port] as the URL has it
    pub netloc: String,
    pub host: String,
    pub port: u16,
    /// the path and query: what a request line names
    pub target: String,
}

impl Url {
    pub fn parse(url: &str) -> std::io::Result<Url> {
        let (scheme, rest) = url
            .split_once("://")
            .ok_or_else(|| io(format!("unknown url type: {url:?}")))?;
        let scheme = scheme.to_ascii_lowercase();
        let default = match scheme.as_str() {
            "http" => 80,
            "https" => 443,
            _ => return Err(io(format!("unknown url type: {scheme:?}"))),
        };
        let end = rest.find(['/', '?', '#']).unwrap_or(rest.len());
        let netloc = rest[..end].to_string();
        let mut target = rest[end..].split('#').next().unwrap_or("").to_string();
        if !target.starts_with('/') {
            target.insert(0, '/');
        }
        let hostport = netloc.rsplit('@').next().unwrap_or("");
        let (host, port) = match hostport.rsplit_once(':') {
            Some((h, p)) if !h.ends_with(']') || hostport.starts_with('[') => match p.parse() {
                Ok(p) => (h, p),
                Err(_) if p.is_empty() => (h, default),
                Err(_) => return Err(io(format!("nonnumeric port: '{p}'"))),
            },
            _ => (hostport, default),
        };
        let host = host
            .trim_start_matches('[')
            .trim_end_matches(']')
            .to_ascii_lowercase();
        Ok(Url {
            scheme,
            netloc,
            host,
            port,
            target,
        })
    }

    /// urllib.parse.urljoin(self, location) for a redirect's Location.
    pub fn join(&self, loc: &str) -> String {
        if loc.contains("://") {
            return loc.to_string();
        }
        if let Some(r) = loc.strip_prefix("//") {
            return format!("{}://{r}", self.scheme);
        }
        if loc.starts_with('/') {
            return format!("{}://{}{loc}", self.scheme, self.netloc);
        }
        let path = self.target.split('?').next().unwrap_or("/");
        let dir = &path[..path.rfind('/').map_or(0, |i| i + 1)];
        format!("{}://{}{dir}{loc}", self.scheme, self.netloc)
    }
}

/// The proxy for a URL from the environment (urllib.request.getproxies, proxy_bypass), as a URL.
fn proxy_for(u: &Url) -> Option<Url> {
    let env = |k: &str| std::env::var(k).ok().filter(|v| !v.is_empty());
    if let Some(np) = env("no_proxy").or_else(|| env("NO_PROXY")) {
        for e in np.split(',').map(str::trim).filter(|e| !e.is_empty()) {
            if e == "*" {
                return None;
            }
            let (eh, ep) = match e.rsplit_once(':') {
                Some((h, p)) if p.parse::<u16>().is_ok() => (h, Some(p.parse::<u16>().unwrap())),
                _ => (e, None),
            };
            let eh = eh.trim_start_matches('.').to_ascii_lowercase();
            if ep.is_some_and(|p| p != u.port) {
                continue;
            }
            if u.host == eh || u.host.ends_with(&format!(".{eh}")) {
                return None;
            }
        }
    }
    let p = env(&format!("{}_proxy", u.scheme))
        .or_else(|| env(&format!("{}_PROXY", u.scheme.to_uppercase())))?;
    let p = if p.contains("://") {
        p
    } else {
        format!("http://{p}")
    };
    let mut pu = Url::parse(&p).ok()?;
    if !p
        .split("://")
        .nth(1)
        .unwrap_or("")
        .split('/')
        .next()
        .unwrap_or("")
        .contains(':')
    {
        pu.port = 8080; // (http.client: the proxy's port, else 8080 as nsis.py gives it)
    }
    Some(pu)
}

/// The system's CA certificates, once.
fn tls_config() -> std::io::Result<Arc<rustls::ClientConfig>> {
    static CFG: std::sync::OnceLock<std::result::Result<Arc<rustls::ClientConfig>, String>> =
        std::sync::OnceLock::new();
    CFG.get_or_init(|| {
        let mut roots = rustls::RootCertStore::empty();
        let mut files: Vec<std::path::PathBuf> = Vec::new();
        if let Some(f) = std::env::var_os("SSL_CERT_FILE") {
            files.push(f.into());
        }
        if let Some(d) = std::env::var_os("SSL_CERT_DIR") {
            for d in std::env::split_paths(&d) {
                if let Ok(rd) = std::fs::read_dir(&d) {
                    let mut names: Vec<_> = rd.filter_map(|e| e.ok().map(|e| e.path())).collect();
                    names.sort();
                    files.extend(names);
                }
            }
        }
        if files.is_empty() {
            files.extend(
                CA_FILES
                    .iter()
                    .map(std::path::PathBuf::from)
                    .filter(|p| p.is_file())
                    .take(1),
            );
        }
        for f in &files {
            let Ok(data) = std::fs::read(f) else { continue };
            for c in CertificateDer::pem_slice_iter(&data).flatten() {
                let _ = roots.add(c);
            }
        }
        if roots.is_empty() {
            return Err(
                "no CA certificates to check Unity's servers with (set SSL_CERT_FILE to the system's CA bundle)"
                    .into(),
            );
        }
        let provider = Arc::new(rustls::crypto::ring::default_provider());
        let cfg = rustls::ClientConfig::builder_with_provider(provider)
            .with_safe_default_protocol_versions()
            .map_err(|e| e.to_string())?
            .with_root_certificates(roots)
            .with_no_client_auth();
        Ok(Arc::new(cfg))
    })
    .clone()
    .map_err(io)
}

enum Stream {
    Plain(TcpStream),
    Tls(Box<rustls::StreamOwned<rustls::ClientConnection, TcpStream>>),
}

impl Read for Stream {
    fn read(&mut self, b: &mut [u8]) -> std::io::Result<usize> {
        match self {
            Stream::Plain(s) => s.read(b),
            Stream::Tls(s) => match s.read(b) {
                // (a server that closes without close_notify: the end of the body for us)
                Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => Ok(0),
                r => r,
            },
        }
    }
}

impl Write for Stream {
    fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
        match self {
            Stream::Plain(s) => s.write(b),
            Stream::Tls(s) => s.write(b),
        }
    }

    fn flush(&mut self) -> std::io::Result<()> {
        match self {
            Stream::Plain(s) => s.flush(),
            Stream::Tls(s) => s.flush(),
        }
    }
}

fn tcp(host: &str, port: u16) -> std::io::Result<TcpStream> {
    let mut last = io(format!("{host}: no address"));
    for a in (host, port).to_socket_addrs()? {
        match TcpStream::connect_timeout(&a, TIMEOUT) {
            Ok(s) => {
                s.set_read_timeout(Some(TIMEOUT))?;
                s.set_write_timeout(Some(TIMEOUT))?;
                let _ = s.set_nodelay(true);
                return Ok(s);
            }
            Err(e) => last = e,
        }
    }
    Err(last)
}

/// A response: its status, headers (names lower case) and body.
pub struct Response {
    pub status: u16,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
}

impl Response {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers
            .iter()
            .find(|(k, _)| k == name)
            .map(|(_, v)| v.as_str())
    }
}

/// HTTP/1.1 GET requests, one kept-alive connection at a time (http.client.HTTPConnection's).
#[derive(Default)]
pub struct Client {
    conn: Option<(String, String, BufReader<Stream>)>,
}

fn read_line(r: &mut impl BufRead) -> std::io::Result<String> {
    let mut l = Vec::new();
    r.read_until(b'\n', &mut l)?;
    if l.is_empty() {
        return Err(io(
            "RemoteDisconnected: Remote end closed connection without response",
        ));
    }
    Ok(String::from_utf8_lossy(&l)
        .trim_end_matches(['\r', '\n'])
        .to_string())
}

impl Client {
    pub fn close(&mut self) {
        self.conn = None;
    }

    fn connect(u: &Url) -> std::io::Result<BufReader<Stream>> {
        let proxy = proxy_for(u);
        let s = match &proxy {
            Some(p) => tcp(&p.host, p.port)?,
            None => tcp(&u.host, u.port)?,
        };
        if u.scheme != "https" {
            return Ok(BufReader::new(Stream::Plain(s)));
        }
        let mut s = s;
        if proxy.is_some() {
            // a CONNECT tunnel through the proxy
            write!(
                s,
                "CONNECT {h}:{p} HTTP/1.0\r\nHost: {h}:{p}\r\n\r\n",
                h = u.host,
                p = u.port
            )?;
            let mut r = BufReader::new(&mut s);
            let status = read_line(&mut r)?;
            let code = status.split_whitespace().nth(1).unwrap_or("");
            if code != "200" {
                return Err(io(format!("Tunnel connection failed: {status}")));
            }
            while !read_line(&mut r)?.is_empty() {}
        }
        let name = ServerName::try_from(u.host.clone()).map_err(|e| io(e.to_string()))?;
        let c =
            rustls::ClientConnection::new(tls_config()?, name).map_err(|e| io(e.to_string()))?;
        Ok(BufReader::new(Stream::Tls(Box::new(
            rustls::StreamOwned::new(c, s),
        ))))
    }

    /// One GET of a URL (no redirect followed), on the kept connection to its host.
    pub fn get(&mut self, url: &str, headers: &[(&str, &str)]) -> std::io::Result<Response> {
        let u = Url::parse(url)?;
        let key = (u.scheme.clone(), u.netloc.clone());
        if self
            .conn
            .as_ref()
            .is_none_or(|(s, n, _)| (s, n) != (&key.0, &key.1))
        {
            self.conn = Some((key.0.clone(), key.1.clone(), Self::connect(&u)?));
        }
        let proxied = u.scheme == "http" && proxy_for(&u).is_some();
        let r = self.request(&u, url, proxied, headers);
        match r {
            Ok((resp, keep)) => {
                if !keep {
                    self.conn = None;
                }
                Ok(resp)
            }
            Err(e) => {
                self.conn = None;
                Err(e)
            }
        }
    }

    fn request(
        &mut self,
        u: &Url,
        url: &str,
        proxied: bool,
        headers: &[(&str, &str)],
    ) -> std::io::Result<(Response, bool)> {
        let r = &mut self.conn.as_mut().unwrap().2;
        let target = if proxied { url } else { u.target.as_str() };
        let host = if (u.scheme == "https" && u.port == 443) || (u.scheme == "http" && u.port == 80)
        {
            u.host.clone()
        } else {
            format!("{}:{}", u.host, u.port)
        };
        let mut req =
            format!("GET {target} HTTP/1.1\r\nHost: {host}\r\nAccept-Encoding: identity\r\n");
        for (k, v) in headers {
            req.push_str(&format!("{k}: {v}\r\n"));
        }
        req.push_str("\r\n");
        r.get_mut().write_all(req.as_bytes())?;
        r.get_mut().flush()?;
        let status_line = loop {
            let l = read_line(r)?;
            // (1xx responses before the real one)
            let code = l.split_whitespace().nth(1).unwrap_or("");
            if code.starts_with('1') && code.len() == 3 {
                while !read_line(r)?.is_empty() {}
                continue;
            }
            break l;
        };
        let mut parts = status_line.splitn(3, ' ');
        let version = parts.next().unwrap_or("").to_string();
        let status: u16 = parts
            .next()
            .and_then(|c| c.parse().ok())
            .ok_or_else(|| io(format!("BadStatusLine: {status_line:?}")))?;
        let mut hs = Vec::new();
        loop {
            let l = read_line(r)?;
            if l.is_empty() {
                break;
            }
            if let Some((k, v)) = l.split_once(':') {
                hs.push((k.trim().to_ascii_lowercase(), v.trim().to_string()));
            }
        }
        let resp_h = |k: &str| hs.iter().find(|(n, _)| n == k).map(|(_, v)| v.clone());
        let conn_h = resp_h("connection")
            .unwrap_or_default()
            .to_ascii_lowercase();
        let mut keep = if version == "HTTP/1.0" {
            conn_h.contains("keep-alive")
        } else {
            !conn_h.contains("close")
        };
        let mut body = Vec::new();
        let no_body = status == 204 || status == 304;
        if no_body {
        } else if resp_h("transfer-encoding")
            .is_some_and(|t| t.to_ascii_lowercase().contains("chunked"))
        {
            loop {
                let l = read_line(r)?;
                let n = usize::from_str_radix(l.split(';').next().unwrap_or("").trim(), 16)
                    .map_err(|_| io(format!("IncompleteRead: bad chunk size {l:?}")))?;
                if n == 0 {
                    while !read_line(r)?.is_empty() {}
                    break;
                }
                let at = body.len();
                body.resize(at + n, 0);
                r.read_exact(&mut body[at..])
                    .map_err(|e| io(format!("IncompleteRead: {e}")))?;
                read_line(r)?;
            }
        } else if let Some(n) = resp_h("content-length").and_then(|v| v.parse::<usize>().ok()) {
            body.resize(n, 0);
            r.read_exact(&mut body)
                .map_err(|e| io(format!("IncompleteRead: {e}")))?;
        } else {
            r.read_to_end(&mut body)?;
            keep = false;
        }
        Ok((
            Response {
                status,
                headers: hs,
                body,
            },
            keep,
        ))
    }

    /// A GET with redirects followed (up to `depth` more), as nsis.Source._get and urlopen do.
    pub fn get_following(
        &mut self,
        url: &str,
        headers: &[(&str, &str)],
        depth: u32,
    ) -> std::io::Result<Response> {
        let r = self.get(url, headers)?;
        if matches!(r.status, 301 | 302 | 303 | 307 | 308) && depth > 0 {
            if let Some(loc) = r.header("location") {
                let next = Url::parse(url)?.join(loc);
                return self.get_following(&next, headers, depth - 1);
            }
        }
        Ok(r)
    }
}

/// urllib.parse.quote_plus of a query value.
pub fn quote_plus(s: &str) -> String {
    let mut out = String::new();
    for b in s.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'_' | b'.' | b'-' | b'~' => {
                out.push(b as char)
            }
            b' ' => out.push('+'),
            b => out.push_str(&format!("%{b:02X}")),
        }
    }
    out
}

/// urlopen(url).read() with the status checked (urllib raises HTTPError for 4xx / 5xx).
pub fn fetch(url: &str) -> Result<Vec<u8>> {
    let mut c = Client::default();
    let r = c
        .get_following(
            url,
            &[
                ("User-Agent", "Python-urllib/3.11"),
                ("Connection", "close"),
            ],
            10,
        )
        .map_err(|e| Error::Fail(format!("<urlopen error {e}>")))?;
    if r.status >= 400 {
        return Err(Error::Fail(format!("HTTP Error {}: {url}", r.status)));
    }
    Ok(r.body)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn urls() {
        let u =
            Url::parse("https://download.unity3d.com/download_unity/x/Setup.exe?a=1#f").unwrap();
        assert_eq!(
            (u.host.as_str(), u.port, u.target.as_str()),
            (
                "download.unity3d.com",
                443,
                "/download_unity/x/Setup.exe?a=1"
            )
        );
        let u = Url::parse("http://127.0.0.1:8000").unwrap();
        assert_eq!(
            (u.host.as_str(), u.port, u.target.as_str()),
            ("127.0.0.1", 8000, "/")
        );
        assert_eq!(u.join("/a/b"), "http://127.0.0.1:8000/a/b");
        assert_eq!(u.join("https://x.y/z"), "https://x.y/z");
        let u = Url::parse("http://h/dir/f.exe").unwrap();
        assert_eq!(u.join("g.exe"), "http://h/dir/g.exe");
        assert!(Url::parse("ftp://h/x").is_err());
        assert_eq!(quote_plus("6000.0.61f1"), "6000.0.61f1");
        assert_eq!(quote_plus("a b/c"), "a+b%2Fc");
    }

    /// A local server: range requests on a kept-alive connection, a redirect, a closed connection.
    #[test]
    fn range_requests_on_a_local_server() {
        use std::net::TcpListener;
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = l.local_addr().unwrap().port();
        let data: Vec<u8> = (0..1000u32).map(|i| (i % 251) as u8).collect();
        let d2 = data.clone();
        let t = std::thread::spawn(move || {
            let mut conns = 0;
            for s in l.incoming().take(2) {
                conns += 1;
                let mut s = s.unwrap();
                let mut r = BufReader::new(s.try_clone().unwrap());
                loop {
                    let mut req = Vec::new();
                    loop {
                        let mut line = String::new();
                        if r.read_line(&mut line).unwrap() == 0 {
                            break;
                        }
                        if line == "\r\n" {
                            break;
                        }
                        req.push(line);
                    }
                    if req.is_empty() {
                        break;
                    }
                    let target = req[0].split(' ').nth(1).unwrap().to_string();
                    if target == "/old" {
                        s.write_all(
                            b"HTTP/1.1 302 Found\r\nLocation: /new\r\nContent-Length: 0\r\n\r\n",
                        )
                        .unwrap();
                        continue;
                    }
                    let rng = req.iter().find(|h| h.starts_with("Range: bytes=")).unwrap();
                    let (a, b) = rng.trim()["Range: bytes=".len()..].split_once('-').unwrap();
                    let (a, b): (usize, usize) = (a.parse().unwrap(), b.parse().unwrap());
                    let body = &d2[a..=b];
                    let close = target == "/close";
                    s.write_all(
                        format!(
                            "HTTP/1.1 206 Partial Content\r\nContent-Length: {}\r\n{}\r\n",
                            body.len(),
                            if close { "Connection: close\r\n" } else { "" }
                        )
                        .as_bytes(),
                    )
                    .unwrap();
                    s.write_all(body).unwrap();
                    if close {
                        break;
                    }
                }
            }
            conns
        });
        let mut c = Client::default();
        let base = format!("http://127.0.0.1:{port}");
        for (a, b) in [(0, 9), (500, 999)] {
            let r = c
                .get_following(
                    &format!("{base}/old"),
                    &[("Range", &format!("bytes={a}-{b}"))],
                    5,
                )
                .unwrap();
            assert_eq!(r.status, 206);
            assert_eq!(r.body, data[a..=b]);
        }
        let r = c
            .get(&format!("{base}/close"), &[("Range", "bytes=1-2")])
            .unwrap();
        assert_eq!(r.body, data[1..=2]);
        let r = c
            .get(&format!("{base}/new"), &[("Range", "bytes=3-3")])
            .unwrap();
        assert_eq!(r.body, data[3..=3]);
        drop(c);
        assert_eq!(t.join().unwrap(), 2);
    }
}
