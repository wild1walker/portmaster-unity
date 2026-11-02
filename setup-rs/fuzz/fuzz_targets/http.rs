//! An HTTP response as `player fetch` and `winbcl` read one from Unity's servers (or a proxy):
//! the status line, 1xx responses, headers, a chunked or sized body, then the URL a redirect's
//! Location makes.
#![no_main]
use libfuzzer_sys::fuzz_target;
use pkgzip::http::{read_response, Url};

fuzz_target!(|data: &[u8]| {
    let mut r = std::io::BufReader::new(data);
    if let Ok((resp, _)) = read_response(&mut r, 1 << 20) {
        if let Some(loc) = resp.header("location") {
            if let Ok(u) = Url::parse("https://download.unity3d.com/download_unity/x/Setup.exe") {
                let _ = Url::parse(&u.join(loc));
            }
        }
    }
});
