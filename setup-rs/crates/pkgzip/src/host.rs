//! (pkgzip::host; crates/port re-exports it.) The commands pmu runs for a port's builds, run on the porter's computer when pmu itself runs in
//! a container (setup-rs/tools/pmu.sh): the runtime's and the tools' build scripts (Docker), the
//! port's own `build.commands`, asmpatch's build (.NET 8 SDK) and its survey. The container has
//! neither Docker nor .NET; the computer has both, as it had for the Python, which ran there.
//!
//! `$PMU_HOST_EXEC` names a folder both see at the same path. Each command is written there as a
//! shell script (`<id>.run`: the output goes to the computer's terminal; `<id>.cap`: stdout and stderr
//! kept in `<id>.out` and `<id>.err`), pmu.sh's loop runs it in the folder pmu was started in and
//! writes its exit status to `<id>.rc`, and pmu waits for that. Without `$PMU_HOST_EXEC` the command
//! runs here, as before.
use std::ffi::OsStr;
use std::fs;
use std::io;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::process::ExitStatusExt;
use std::path::PathBuf;
use std::process::{Command, ExitStatus, Output};
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::Duration;

/// The request folder, when commands run on the computer outside pmu's container.
pub fn dir() -> Option<PathBuf> {
    std::env::var_os("PMU_HOST_EXEC").filter(|d| !d.is_empty()).map(PathBuf::from)
}

/// Commands run outside the container (pmu.sh).
pub fn active() -> bool {
    dir().is_some()
}

fn quote(s: &OsStr) -> String {
    let s = String::from_utf8_lossy(s.as_bytes());
    format!("'{}'", s.replace('\'', "'\"'\"'"))
}

/// The command as a POSIX shell script: its folder, its environment changes, then the program.
pub fn script(cmd: &Command) -> String {
    let mut s = String::from("#!/bin/sh\n");
    if let Some(d) = cmd.get_current_dir() {
        s += &format!("cd {} || exit 127\n", quote(d.as_os_str()));
    }
    for (k, v) in cmd.get_envs() {
        let k = String::from_utf8_lossy(k.as_bytes());
        match v {
            Some(v) => s += &format!("export {k}={}\n", quote(v)),
            None => s += &format!("unset {k}\n"),
        }
    }
    s += "exec ";
    s += &quote(cmd.get_program());
    for a in cmd.get_args() {
        s.push(' ');
        s += &quote(a);
    }
    s += " </dev/null\n";
    s
}

/// One request in folder d: the script written, then its exit status (and output) waited for.
pub fn request_in(d: &std::path::Path, cmd: &Command, capture: bool) -> io::Result<(ExitStatus, Vec<u8>, Vec<u8>)> {
    static N: AtomicU32 = AtomicU32::new(0);
    let id = format!("{}-{}", std::process::id(), N.fetch_add(1, Ordering::SeqCst));
    let p = |ext: &str| d.join(format!("{id}.{ext}"));
    fs::write(p("tmp"), script(cmd))?;
    fs::rename(p("tmp"), p(if capture { "cap" } else { "run" }))?;
    let rc = loop {
        if let Ok(t) = fs::read_to_string(p("rc")) {
            if let Ok(n) = t.trim().parse::<i32>() {
                break n;
            }
        }
        std::thread::sleep(Duration::from_millis(100));
    };
    let (out, err) = if capture {
        (fs::read(p("out")).unwrap_or_default(), fs::read(p("err")).unwrap_or_default())
    } else {
        (Vec::new(), Vec::new())
    };
    for ext in ["sh", "out", "err", "rc"] {
        let _ = fs::remove_file(p(ext));
    }
    // (a shell's status: 128 + n for a signal, as a wait status the code n << 8)
    Ok((ExitStatus::from_raw((rc & 0xff) << 8), out, err))
}

/// cmd.status(), on the computer when pmu runs in pmu.sh's container.
pub fn status(cmd: &mut Command) -> io::Result<ExitStatus> {
    match dir() {
        Some(d) => request_in(&d, cmd, false).map(|r| r.0),
        None => cmd.status(),
    }
}

/// cmd.output(), on the computer (stdin closed there) when pmu runs in pmu.sh's container.
pub fn output(cmd: &mut Command) -> io::Result<Output> {
    match dir() {
        Some(d) => request_in(&d, cmd, true).map(|(status, stdout, stderr)| Output { status, stdout, stderr }),
        None => cmd.output(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn script_text() {
        let mut c = Command::new("/bin/sh");
        c.arg("-c").arg("echo 'x'").current_dir("/a b").env("K", "v'w").env_remove("Z");
        let s = script(&c);
        assert!(s.contains("cd '/a b' || exit 127\n"));
        assert!(s.contains("export K='v'\"'\"'w'\n"));
        assert!(s.contains("unset Z\n"));
        assert!(s.ends_with("exec '/bin/sh' '-c' 'echo '\"'\"'x'\"'\"'' </dev/null\n"));
    }
}
