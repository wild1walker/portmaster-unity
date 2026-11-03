//! libvpx's VP8 decoder and encoder through csrc/vpx_capi.c.
use crate::Picture;
use std::ffi::{c_char, c_int, c_ulong, c_void, CStr};

#[cfg(not(no_vpx))]
extern "C" {
    fn vd_new(threads: c_int) -> *mut c_void;
    fn vd_decode(d: *mut c_void, data: *const u8, n: usize) -> c_int;
    #[allow(clippy::too_many_arguments)]
    fn vd_frame(
        d: *mut c_void,
        y: *mut *const u8,
        ys: *mut c_int,
        u: *mut *const u8,
        us: *mut c_int,
        v: *mut *const u8,
        vs: *mut c_int,
        w: *mut c_int,
        h: *mut c_int,
    ) -> c_int;
    fn vd_error(d: *mut c_void) -> *const c_char;
    fn vd_free(d: *mut c_void);
    fn ve_new(
        w: c_int,
        h: c_int,
        threads: c_int,
        log2_partitions: c_int,
        cpu_used: c_int,
        cq_level: c_int,
        max_kbps: c_int,
    ) -> *mut c_void;
    #[allow(clippy::too_many_arguments)]
    fn ve_encode(
        e: *mut c_void,
        y: *const u8,
        ys: c_int,
        u: *const u8,
        us: c_int,
        v: *const u8,
        vs: c_int,
        pts_ms: i64,
        duration_ms: c_ulong,
        key: c_int,
    ) -> c_int;
    fn ve_packet(e: *mut c_void, data: *mut *const u8, n: *mut usize, pts: *mut i64, key: *mut c_int) -> c_int;
    fn ve_error(e: *mut c_void) -> *const c_char;
    fn ve_free(e: *mut c_void);
}

#[cfg(no_vpx)]
mod absent {
    //! no libvpx on this architecture (build.rs): every call fails
    use std::ffi::{c_char, c_int, c_ulong, c_void};
    pub unsafe fn vd_new(_: c_int) -> *mut c_void { std::ptr::null_mut() }
    pub unsafe fn vd_decode(_: *mut c_void, _: *const u8, _: usize) -> c_int { -1 }
    #[allow(clippy::too_many_arguments)]
    pub unsafe fn vd_frame(_: *mut c_void, _: *mut *const u8, _: *mut c_int, _: *mut *const u8, _: *mut c_int,
                           _: *mut *const u8, _: *mut c_int, _: *mut c_int, _: *mut c_int) -> c_int { 0 }
    pub unsafe fn vd_error(_: *mut c_void) -> *const c_char { c"no libvpx in this build".as_ptr() }
    pub unsafe fn vd_free(_: *mut c_void) {}
    pub unsafe fn ve_new(_: c_int, _: c_int, _: c_int, _: c_int, _: c_int, _: c_int, _: c_int) -> *mut c_void {
        std::ptr::null_mut()
    }
    #[allow(clippy::too_many_arguments)]
    pub unsafe fn ve_encode(_: *mut c_void, _: *const u8, _: c_int, _: *const u8, _: c_int, _: *const u8, _: c_int,
                            _: i64, _: c_ulong, _: c_int) -> c_int { -1 }
    pub unsafe fn ve_packet(_: *mut c_void, _: *mut *const u8, _: *mut usize, _: *mut i64, _: *mut c_int) -> c_int { 0 }
    pub unsafe fn ve_error(_: *mut c_void) -> *const c_char { c"no libvpx in this build".as_ptr() }
    pub unsafe fn ve_free(_: *mut c_void) {}
}
#[cfg(no_vpx)]
use absent::*;

/// Whether this build has libvpx (arm64 builds only: build.rs).
pub const AVAILABLE: bool = cfg!(not(no_vpx));

fn msg(p: *const c_char) -> String {
    if p.is_null() {
        return "?".into();
    }
    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
}

/// A VP8 decoder.
pub struct Decoder(*mut c_void);
unsafe impl Send for Decoder {}

impl Decoder {
    pub fn new(threads: usize) -> Result<Decoder, String> {
        let d = unsafe { vd_new(threads as c_int) };
        if d.is_null() {
            return Err(if AVAILABLE { "VP8 decoder: init failed".into() } else { "VP8: no libvpx in this build".into() });
        }
        Ok(Decoder(d))
    }

    /// One compressed frame in; the picture it shows, if any (an alt-ref frame shows none).
    pub fn decode(&mut self, data: &[u8]) -> Result<Option<Picture>, String> {
        if unsafe { vd_decode(self.0, data.as_ptr(), data.len()) } != 0 {
            return Err(format!("VP8 decode: {}", msg(unsafe { vd_error(self.0) })));
        }
        let (mut y, mut u, mut v) = (std::ptr::null(), std::ptr::null(), std::ptr::null());
        let (mut ys, mut us, mut vs, mut w, mut h) = (0, 0, 0, 0, 0);
        let r = unsafe { vd_frame(self.0, &mut y, &mut ys, &mut u, &mut us, &mut v, &mut vs, &mut w, &mut h) };
        if r < 0 {
            return Err("VP8 decode: not an 8-bit 4:2:0 picture".into());
        }
        if r == 0 {
            return Ok(None);
        }
        let (w, h) = (w as usize, h as usize);
        let (cw, ch) = (w.div_ceil(2), h.div_ceil(2));
        let plane = |p: *const u8, stride: c_int, pw: usize, ph: usize| {
            let mut out = Vec::with_capacity(pw * ph);
            for r in 0..ph {
                out.extend_from_slice(unsafe { std::slice::from_raw_parts(p.add(r * stride as usize), pw) });
            }
            out
        };
        Ok(Some(Picture { w, h, y: plane(y, ys, w, h), u: plane(u, us, cw, ch), v: plane(v, vs, cw, ch) }))
    }
}

impl Drop for Decoder {
    fn drop(&mut self) {
        unsafe { vd_free(self.0) }
    }
}

/// The encoder's settings (csrc/vpx_capi.c's ve_new).
#[derive(Clone, Copy, Debug)]
pub struct EncodeSettings {
    pub threads: usize,
    /// token partitions: 1 << log2_partitions (0 - 3)
    pub log2_partitions: u32,
    /// libvpx's speed for the good-quality deadline (0 - 5)
    pub cpu_used: i32,
    /// constant quality (0 - 63, lower is better)
    pub cq_level: i32,
    /// the rate the quality may use at most
    pub max_kbps: u32,
}

/// One compressed frame from the encoder.
pub struct Packet {
    pub data: Vec<u8>,
    pub pts_ms: i64,
    pub key: bool,
}

/// A VP8 encoder (time base 1 ms; key frames only where asked).
pub struct Encoder(*mut c_void);
unsafe impl Send for Encoder {}

impl Encoder {
    pub fn new(w: usize, h: usize, s: &EncodeSettings) -> Result<Encoder, String> {
        let e = unsafe {
            ve_new(
                w as c_int,
                h as c_int,
                s.threads as c_int,
                s.log2_partitions as c_int,
                s.cpu_used,
                s.cq_level,
                s.max_kbps as c_int,
            )
        };
        if e.is_null() {
            return Err(if AVAILABLE { "VP8 encoder: init failed".into() } else { "VP8: no libvpx in this build".into() });
        }
        Ok(Encoder(e))
    }

    fn packets(&mut self) -> Vec<Packet> {
        let mut out = Vec::new();
        loop {
            let (mut p, mut n, mut pts, mut key) = (std::ptr::null(), 0usize, 0i64, 0);
            if unsafe { ve_packet(self.0, &mut p, &mut n, &mut pts, &mut key) } == 0 {
                return out;
            }
            out.push(Packet { data: unsafe { std::slice::from_raw_parts(p, n) }.to_vec(), pts_ms: pts, key: key != 0 });
        }
    }

    /// One picture in (time and duration in ms); the compressed frames that come out of it.
    pub fn encode(&mut self, p: &Picture, pts_ms: i64, duration_ms: u64, key: bool) -> Result<Vec<Packet>, String> {
        let cw = p.w.div_ceil(2);
        let r = unsafe {
            ve_encode(
                self.0,
                p.y.as_ptr(),
                p.w as c_int,
                p.u.as_ptr(),
                cw as c_int,
                p.v.as_ptr(),
                cw as c_int,
                pts_ms,
                duration_ms as c_ulong,
                key as c_int,
            )
        };
        if r != 0 {
            return Err(format!("VP8 encode: {}", msg(unsafe { ve_error(self.0) })));
        }
        Ok(self.packets())
    }

    /// The frames still inside the encoder.
    pub fn finish(&mut self) -> Result<Vec<Packet>, String> {
        let r = unsafe { ve_encode(self.0, std::ptr::null(), 0, std::ptr::null(), 0, std::ptr::null(), 0, 0, 0, 0) };
        if r != 0 {
            return Err(format!("VP8 encode: {}", msg(unsafe { ve_error(self.0) })));
        }
        Ok(self.packets())
    }
}

impl Drop for Encoder {
    fn drop(&mut self) {
        unsafe { ve_free(self.0) }
    }
}
