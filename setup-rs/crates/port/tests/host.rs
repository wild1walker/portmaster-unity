//! crates/port host.rs against setup-rs/tools/pmu.sh's request loop (`pmu.sh --serve <dir>`): the
//! commands pmu hands to the computer outside its container run in the folder, with the environment
//! and arguments given, and their exit status and output come back.
use std::fs;
use std::path::Path;
use std::process::Command;

#[test]
fn requests_through_pmu_sh() {
    let d = std::env::temp_dir().join(format!("pmu-host-{}", std::process::id()));
    let _ = fs::remove_dir_all(&d);
    fs::create_dir_all(d.join("work dir")).unwrap();
    let sh = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tools/pmu.sh");
    let mut serve = Command::new("bash").arg(&sh).arg("--serve").arg(&d).spawn().unwrap();

    // a status, the folder and the environment
    let mut c = Command::new("/bin/sh");
    c.arg("-c").arg("pwd > here.txt; echo \"$K\" > k.txt; exit 3").current_dir(d.join("work dir")).env("K", "it's $HOME");
    let (st, _, _) = port::host::request_in(&d, &c, false).unwrap();
    assert_eq!(st.code(), Some(3));
    assert_eq!(fs::read_to_string(d.join("work dir/k.txt")).unwrap(), "it's $HOME\n");
    let here = fs::read_to_string(d.join("work dir/here.txt")).unwrap();
    assert!(here.trim_end().ends_with("/work dir"), "{here}");

    // output kept, stdin closed, arguments as given
    let mut c = Command::new("/bin/sh");
    c.arg("-c").arg("cat; printf '%s|' \"$@\"; echo oops >&2").arg("x").arg("a b").arg("'q'");
    let (st, out, err) = port::host::request_in(&d, &c, true).unwrap();
    assert!(st.success());
    assert_eq!(String::from_utf8(out).unwrap(), "a b|'q'|");
    assert_eq!(String::from_utf8(err).unwrap(), "oops\n");

    // a program that is not there: the shell's 127
    let c = Command::new("/nonexistent/prog");
    let (st, _, _) = port::host::request_in(&d, &c, true).unwrap();
    assert_eq!(st.code(), Some(127));

    // nothing left behind but what the commands wrote
    let mut left: Vec<String> = fs::read_dir(&d).unwrap().map(|e| e.unwrap().file_name().to_string_lossy().into_owned()).collect();
    left.sort();
    assert_eq!(left, ["work dir"]);
    fs::write(d.join("stop"), "").unwrap();
    assert!(serve.wait().unwrap().success());
    let _ = fs::remove_dir_all(&d);
}
