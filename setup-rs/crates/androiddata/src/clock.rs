//! The local time of day for the log (time.strftime('%H:%M:%S')).

// (libc::time_t: deprecated for musl's coming 64-bit change, which aarch64 has already)
#[allow(deprecated)]
pub fn hms() -> String {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as libc::time_t)
        .unwrap_or(0);
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    // SAFETY: now and tm are valid for the call; localtime_r writes only tm.
    let ok = !unsafe { libc::localtime_r(&now, &mut tm) }.is_null();
    if !ok {
        return "00:00:00".into();
    }
    format!("{:02}:{:02}:{:02}", tm.tm_hour, tm.tm_min, tm.tm_sec)
}
