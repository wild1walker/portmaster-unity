//! `--macho`: the internal calls the build's code may make, checked against the Android player.
//! A method of the engine's modules marked as an internal call has no body. il2cpp's code asks
//! the runtime for the player's function by the method's full name at the first call
//! ("UnityEngine.Screen::get_width()", the parameters' types in the brackets), a text in the
//! game's binary; the player registers each function it has under the name without the
//! brackets, a text in libunity.so. The macOS modules have a few such methods the Android
//! player has no function for, and the game gets a MissingMethodException where it calls one.
//! Those the build's code names are listed here: the runtime's bridge answers the ones it knows
//! (runtime/macho/bridge/bridge.c, own_icalls), and the others are what to look at when the
//! game logs that exception (docs/macho-route.md). A text of the binary counts only if the
//! build's metadata has that method, marked as an internal call, so the list holds no text
//! that merely looks like one. Whether the game ever makes the call is not known from the
//! build: a class the game never uses keeps its calls too, where the build's stripping left it.
use crate::il2cppmeta::Metadata;
use std::collections::{BTreeSet, HashSet};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};

/// The bridge's own answers, as bridge.c's own_icalls (the test below reads it).
pub const ANSWERED: &[&str] = &["UnityEngine.Screen::GetMainWindowDisplayInfo_Injected"];

#[derive(Debug, Default)]
pub struct Census {
    /// internal calls the build's code asks for by name
    pub asked: usize,
    /// those the Android player has no function for: answered by the bridge, and not
    pub answered: Vec<String>,
    pub absent: Vec<String>,
}

const LONGEST: usize = 1024;

/// Each run of printable ASCII that a NUL ends, in `len` bytes of a file from `off`. Read in
/// pieces: a game's binary is hundreds of megabytes, and a setup on a 1 GB handheld does not
/// hold it.
fn texts(path: &str, off: u64, len: u64, each: &mut dyn FnMut(&[u8])) -> Result<(), String> {
    let err = |e: std::io::Error| format!("{path}: {e}");
    let mut f = File::open(path).map_err(err)?;
    f.seek(SeekFrom::Start(off)).map_err(err)?;
    let mut buf = vec![0u8; 1 << 20];
    let mut run: Vec<u8> = Vec::with_capacity(LONGEST);
    let (mut skip, mut left) = (false, len);
    while left > 0 {
        let want = left.min(buf.len() as u64) as usize;
        let n = f.read(&mut buf[..want]).map_err(err)?;
        if n == 0 {
            return Err(format!("{path}: ends {left} bytes before the part to read does"));
        }
        left -= n as u64;
        for &c in &buf[..n] {
            if (0x21..=0x7e).contains(&c) {
                if run.len() == LONGEST {
                    skip = true;
                }
                if !skip {
                    run.push(c);
                }
                continue;
            }
            if c == 0 && !skip && !run.is_empty() {
                each(&run);
            }
            run.clear();
            skip = false;
        }
    }
    Ok(())
}

/// "Type::Method" of a text with the shape of an internal call's full name
/// ("Ns.Outer/Inner::Method(Ns.Type,Ns.Type&)"); None for any other text.
fn call_of(t: &[u8]) -> Option<&str> {
    if *t.last()? != b')' {
        return None;
    }
    let head = &t[..t.iter().position(|&c| c == b'(')?];
    let sep = head.windows(2).position(|w| w == b"::")?;
    if sep == 0 || sep + 2 == head.len() || head.iter().filter(|&&c| c == b':').count() != 2 {
        return None;
    }
    std::str::from_utf8(head).ok()
}

/// The names in a player's library that may be a function's registration: every text with
/// "::" and no bracket.
fn player_names(libunity: &str) -> Result<HashSet<Vec<u8>>, String> {
    let len = std::fs::metadata(libunity).map_err(|e| format!("{libunity}: {e}"))?.len();
    let mut names = HashSet::new();
    texts(libunity, 0, len, &mut |t| {
        if !t.contains(&b'(') && t.windows(2).any(|w| w == b"::") {
            names.insert(t.to_vec());
        }
    })?;
    Ok(names)
}

/// `binary`'s arm64 part (`slice`: offset, size) against the player's library.
pub fn census(binary: &str, slice: (u64, u64), libunity: &str, meta: &Metadata) -> Result<Census, String> {
    let calls = meta.internal_calls()?;
    let mut asked: BTreeSet<(String, String)> = BTreeSet::new();
    texts(binary, slice.0, slice.1, &mut |t| {
        if let Some(name) = call_of(t).filter(|n| calls.contains(*n)) {
            asked.insert((name.to_string(), String::from_utf8_lossy(t).into_owned()));
        }
    })?;
    let player = player_names(libunity)?;
    let mut c = Census { asked: asked.len(), ..Default::default() };
    for (name, full) in asked {
        if player.contains(name.as_bytes()) {
            continue;
        }
        if ANSWERED.contains(&name.as_str()) {
            c.answered.push(full);
        } else {
            c.absent.push(full);
        }
    }
    // A player whose names are not read this way would make every call look absent.
    if c.asked > 0 && (c.answered.len() + c.absent.len()) * 2 > c.asked {
        return Err(format!(
            "{libunity}: {} of the build's {} internal calls have no name in it: not a player library this reads",
            c.answered.len() + c.absent.len(),
            c.asked
        ));
    }
    Ok(c)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_of_calls() {
        assert_eq!(call_of(b"UnityEngine.Screen::get_width()"), Some("UnityEngine.Screen::get_width"));
        assert_eq!(
            call_of(b"UnityEngine.ParticleSystem/MainModule::set_x_Injected(UnityEngine.ParticleSystem/MainModule&,System.Single)"),
            Some("UnityEngine.ParticleSystem/MainModule::set_x_Injected")
        );
        for no in [
            &b"UnityEngine.Screen::get_width"[..],
            b"il2cpp::vm::Class::Init()",
            b"::get_width()",
            b"UnityEngine.Screen::()",
            b"UnityEngine.Screen:get_width()",
            b"get_width()",
            b")",
            b"",
        ] {
            assert_eq!(call_of(no), None, "{}", String::from_utf8_lossy(no));
        }
    }

    #[test]
    fn texts_of_a_file() {
        let dir = std::env::temp_dir().join(format!("icalls-texts-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let p = dir.join("f");
        // a text across the pieces' border, one cut by a byte that is no text, one too long,
        // one the part to read ends in, and one before the part
        let mut b = vec![b'x'; 5];
        b.extend_from_slice(b"\0first\0");
        b.resize((1 << 20) + 5 - 3, 1);
        b.extend_from_slice(b"\0across\0a b\0\x01tail\0");
        b.extend(vec![b'l'; LONGEST + 1]);
        b.extend_from_slice(b"\0after\0cut");
        let len = b.len() as u64 - 5;
        b.extend_from_slice(b" off\0");
        std::fs::write(&p, &b).unwrap();
        let mut got = Vec::new();
        texts(p.to_str().unwrap(), 5, len, &mut |t| got.push(String::from_utf8_lossy(t).into_owned())).unwrap();
        assert_eq!(got, ["first", "across", "b", "tail", "after"]);
        assert!(texts(p.to_str().unwrap(), 5, len + 100, &mut |_| ()).unwrap_err().contains("bytes before"));
        std::fs::remove_dir_all(&dir).unwrap();
    }

    /// An opt-in check on a real build: PMU_ICALLS_TEST=<global-metadata.dat>:<GameAssembly>:<libunity.so>
    #[test]
    fn real_build() {
        let Ok(spec) = std::env::var("PMU_ICALLS_TEST") else {
            return;
        };
        let p: Vec<&str> = spec.split(':').collect();
        let meta = Metadata::open(std::path::Path::new(p[0])).unwrap();
        let (off, size, _) = crate::macho::arm64_slice(p[1]).unwrap();
        let c = census(p[1], (off, size), p[2], &meta).unwrap();
        eprintln!("{} asked; answered: {:?}; absent: {:?}", c.asked, c.answered, c.absent);
    }

    // The table here names what the bridge answers: every name of its own_icalls, when the
    // runtime's sources are beside this crate.
    #[test]
    fn answered_as_the_bridge() {
        let c = concat!(env!("CARGO_MANIFEST_DIR"), "/../../../runtime/macho/bridge/bridge.c");
        let Ok(text) = std::fs::read_to_string(c) else { return };
        let table = &text[text.find("} own_icalls[] = {").expect("the table")..];
        let table = &table[..table.find("};").expect("the table's end")];
        let theirs: Vec<&str> = table.lines().filter_map(|l| l.trim().strip_prefix("{ \"")?.split('"').next()).collect();
        assert_eq!(ANSWERED, theirs);
    }
}
