//! `pmu apk-asmpatch <apk> <work dir> <asmpatch command>`: tools/asmpatch on the assemblies of a
//! game set up already, in its APK (setup/update: a newer release's patches). The APK's
//! assets/bin/Data/Managed/*.dll go into <work>/in, the command (shell words, as android-data's
//! --asmpatch) runs on them with <work>/out, and what it wrote goes back into the APK
//! (Archive::put_stored: after the APK's end, nothing there overwritten; an earlier put that was
//! interrupted is cut back first). Patches the setup applied say "done" and change nothing, so an
//! APK that has them all is left as it is. <work> is removed at the end.
use std::path::Path;
use std::process::ExitCode;

const MANAGED: &str = "assets/bin/Data/Managed/";

pub fn run(args: &[String]) -> ExitCode {
    let [apk, work, cmd] = args else {
        eprintln!("usage: pmu apk-asmpatch <apk> <work dir> <asmpatch command>");
        return ExitCode::from(2);
    };
    let r = patch(apk, work, cmd);
    let _ = std::fs::remove_dir_all(work);
    super::status("apk-asmpatch", r)
}

fn fail<T>(m: String) -> pkgzip::Result<T> {
    Err(pkgzip::Error::Fail(m))
}

fn patch(apk: &str, work: &str, cmd: &str) -> pkgzip::Result<()> {
    if pkgzip::zip::recover_put(apk)? {
        println!("{apk}: an interrupted update's new bytes cut off");
    }
    let z = pkgzip::zip::Archive::open(apk)?;
    let names: Vec<String> = z
        .names()
        .filter(|n| {
            n.strip_prefix(MANAGED)
                .is_some_and(|f| f.ends_with(".dll") && !f.contains('/'))
        })
        .map(str::to_string)
        .collect();
    if names.is_empty() {
        println!("{apk}: no {MANAGED}*.dll (an IL2CPP build): nothing to patch");
        return Ok(());
    }
    let (inp, out) = (format!("{work}/in"), format!("{work}/out"));
    let _ = std::fs::remove_dir_all(work);
    std::fs::create_dir_all(&inp).map_err(pkgzip::io_at(&inp))?;
    for n in &names {
        let f = format!("{inp}/{}", &n[MANAGED.len()..]);
        std::fs::write(&f, z.read(n)?).map_err(pkgzip::io_at(&f))?;
    }
    let mut words = match androiddata::paths::shlex_split(cmd) {
        Ok(w) if !w.is_empty() => w,
        Ok(_) => return fail("no asmpatch command".into()),
        Err(e) => return fail(format!("ValueError: {e}")),
    };
    words.push(inp.clone());
    words.push(out.clone());
    let r = std::process::Command::new(&words[0])
        .args(&words[1..])
        .output()
        .map_err(|e| pkgzip::Error::Fail(format!("{e}: {}", words[0])))?;
    for l in String::from_utf8_lossy(&r.stdout).lines() {
        println!("asmpatch: {l}");
    }
    if !r.status.success() {
        return fail(format!(
            "asmpatch failed (exit {}):\n{}",
            r.status.code().unwrap_or(-1),
            String::from_utf8_lossy(&r.stderr)
        ));
    }
    let mut written: Vec<String> = match std::fs::read_dir(&out) {
        Ok(d) => d
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|n| n.ends_with(".dll"))
            .collect(),
        Err(_) => Vec::new(),
    };
    written.sort();
    let mut puts = Vec::new();
    for f in &written {
        let name = format!("{MANAGED}{f}");
        if z.get(&name).is_none() {
            return fail(format!("asmpatch wrote {f}, which {apk} does not have"));
        }
        let data = std::fs::read(Path::new(&out).join(f)).map_err(pkgzip::io_at(f))?;
        if z.read(&name)? == data {
            println!("same {name}");
            continue;
        }
        puts.push((name, data));
    }
    if puts.is_empty() {
        println!("{apk}: its assemblies have every patch already");
        return Ok(());
    }
    let refs: Vec<(&str, &[u8])> = puts
        .iter()
        .map(|(n, d)| (n.as_str(), d.as_slice()))
        .collect();
    z.put_stored(apk, &refs)?;
    for (n, d) in &puts {
        println!("put {n} ({} bytes)", d.len());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use pkgzip::zipwrite::{write_zip, Src};

    /// a shell script stands in for asmpatch: it changes B.dll, writes A.dll as it was (asmpatch
    /// never does; "same") and leaves the rest
    #[test]
    fn changed_assemblies_put_back() {
        let t = std::env::temp_dir().join(format!("pmu-apk-asmpatch-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&t);
        std::fs::create_dir_all(&t).unwrap();
        let t = t.to_string_lossy().into_owned();
        let apk = format!("{t}/Game.apk");
        let e = |n: &str, d: &[u8]| (n.to_string(), Src::Bytes(d.to_vec()));
        write_zip(
            &apk,
            &[
                e("assets/bin/Data/Managed/A.dll", b"aaaa"),
                e("assets/bin/Data/Managed/B.dll", b"bbbb"),
                e("assets/bin/Data/Managed/etc/mono/config", b"cfg"),
                e("assets/bin/Data/data.unity3d", b"data"),
            ],
            &mut |_| {},
            4,
        )
        .unwrap();
        let script = format!("{t}/asmpatch.sh");
        std::fs::write(
            &script,
            "#!/bin/sh\n[ -f \"$1/A.dll\" ] && [ ! -e \"$1/etc\" ] || exit 1\nmkdir -p \"$2\"\n\
             cp \"$1/A.dll\" \"$2/\"\nprintf 'patched B' > \"$2/B.dll\"\necho \"ok   fake: $3\"\n",
        )
        .unwrap();
        let work = format!("{t}/work");
        let cmd = format!("sh '{script}'");
        patch(&apk, &work, &cmd).unwrap();
        let z = pkgzip::zip::Archive::open(&apk).unwrap();
        assert_eq!(z.read("assets/bin/Data/Managed/B.dll").unwrap(), b"patched B");
        assert_eq!(z.read("assets/bin/Data/Managed/A.dll").unwrap(), b"aaaa");
        assert_eq!(z.read("assets/bin/Data/data.unity3d").unwrap(), b"data");
        // again: B.dll as the script writes it already, nothing put
        drop(z);
        let size = std::fs::metadata(&apk).unwrap().len();
        patch(&apk, &work, &cmd).unwrap();
        assert_eq!(std::fs::metadata(&apk).unwrap().len(), size);
        // a failing asmpatch: the APK as it was
        assert!(patch(&apk, &work, "false").is_err());
        assert_eq!(std::fs::metadata(&apk).unwrap().len(), size);
        std::fs::remove_dir_all(&t).ok();
    }
}
