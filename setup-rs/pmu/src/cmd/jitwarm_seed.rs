//! `pmu jitwarm-seed <Managed folder | game APK> <out file> <base type>...`: the setup's list for ua_host's JIT warm-up
//! (UA_JIT_WARM, runtime/host/jitwarm.c: `data/jitwarm-seed.txt`), written on the handheld from the player's own
//! Assembly-CSharp.dll: every method with a body of every type that derives from one of the named base types
//! (`Namespace.Name`, within that assembly or, as a direct base, of another one: `UnityEngine.MonoBehaviour`; `*`:
//! every type) and of the types nested in them (their coroutines and closures), less
//! generic methods, the methods of generic types and static constructors (the warm-up never compiles those).
//! Lines are ua_host's own list format: `# mvid <assembly> <module version id>/<TypeDef rows>/<MethodDef rows>` (the
//! warm-up drops entries of another build), then `<assembly> <token> *` (`*`: any name; the token and the build
//! name the method). Nothing of the game is shipped: the list is made from the player's copy at setup, like
//! the setup's other outputs. Prints "jitwarm-seed: <n> methods of <t> types (<bases>)".
use androiddata::clrmeta::{Assembly, Coded, METHODDEF, NESTEDCLASS, TYPEDEF, TYPEREF};
use std::collections::{HashMap, HashSet};
use std::process::ExitCode;

const GENERICPARAM: u8 = 0x2A;
const ASSEMBLY: &str = "Assembly-CSharp";

/// Mono's text for a module version id (mono_image_get_guid: .NET's form, upper case).
fn guid_text(g: &[u8; 16]) -> String {
    format!(
        "{:02X}{:02X}{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
        g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]
    )
}

pub fn seed(a: &Assembly, bases: &[String]) -> Result<(String, usize, usize), String> {
    let e = |x: androiddata::clrmeta::Error| x.to_string();
    let types = a.types().map_err(e)?;
    let module = a.rows(0).map_err(e)?;
    let mvid = a
        .guid(
            *module
                .first()
                .and_then(|r| r.get(2))
                .ok_or("no Module row")?,
        )
        .map_err(e)?;
    // generic types and methods (GenericParam owners)
    let (mut gtypes, mut gmethods) = (HashSet::new(), HashSet::new());
    for r in a.rows(GENERICPARAM).map_err(e)?.iter() {
        match Assembly::coded(Coded::TypeOrMethodDef, r[2]).map_err(e)? {
            (Some(TYPEDEF), row) => gtypes.insert(row),
            (Some(METHODDEF), row) => gmethods.insert(row),
            _ => false,
        };
    }
    let enclosing: HashMap<u32, u32> = a
        .rows(NESTEDCLASS)
        .map_err(e)?
        .iter()
        .map(|r| (r[0], r[1]))
        .collect();
    let name_row: HashMap<&str, u32> = types
        .iter()
        .enumerate()
        .map(|(i, t)| (t.full.as_str(), i as u32 + 1))
        .collect();
    let all = bases.iter().any(|b| b == "*");
    let want: HashSet<u32> = bases
        .iter()
        .filter_map(|b| name_row.get(b.as_str()).copied())
        .collect();
    // a type derives from a wanted base through TypeDef bases of this assembly
    let derives = |row: u32| {
        let mut k = row;
        let mut seen = HashSet::new();
        while seen.insert(k) {
            match types.get(k as usize - 1).map(|t| t.extends) {
                Some((Some(TYPEDEF), b)) if b > 0 => {
                    if want.contains(&b) {
                        return true;
                    }
                    k = b;
                }
                // a base of another assembly (UnityEngine.MonoBehaviour): by its name
                Some((Some(TYPEREF), b)) if b > 0 => {
                    return a
                        .typeref_name(b)
                        .map(|n| bases.contains(&n))
                        .unwrap_or(false);
                }
                _ => return false,
            }
        }
        false
    };
    let methods = a.rows(METHODDEF).map_err(e)?;
    let mut out = format!(
        "# mvid {ASSEMBLY} {}/{}/{}\n",
        guid_text(&mvid),
        a.count(TYPEDEF),
        a.count(METHODDEF)
    );
    let (mut n, mut nt) = (0, 0);
    for row in 1..=types.len() as u32 {
        // the type, or a type it is nested in, derives from a base; none of them generic
        let mut k = row;
        let mut ok = false;
        let mut generic = false;
        let mut seen = HashSet::new();
        loop {
            generic |= gtypes.contains(&k);
            ok |= all || derives(k);
            match enclosing.get(&k) {
                Some(&o) if seen.insert(k) => k = o,
                _ => break,
            }
        }
        if !ok || generic {
            continue;
        }
        nt += 1;
        let t = &types[row as usize - 1];
        for m in t.methods.0..t.methods.1 {
            let Some(r) = methods.get(m as usize - 1) else {
                break;
            };
            let name = a.string(r[3]).map_err(e)?;
            if r[0] == 0 || gmethods.contains(&m) || name == ".cctor" {
                continue; // (no body: abstract, extern, runtime)
            }
            out.push_str(&format!("{ASSEMBLY} {:08x} *\n", 0x0600_0000u32 | m));
            n += 1;
        }
    }
    Ok((out, n, nt))
}

pub fn run(args: &[String]) -> ExitCode {
    if args.len() < 3 {
        eprintln!("usage: pmu jitwarm-seed <Managed folder | game APK> <out file> <base type>...");
        return ExitCode::from(2);
    }
    // a Managed folder, or the game's APK (its assets/bin/Data/Managed/Assembly-CSharp.dll, as the setup packed it)
    let a = if args[0].ends_with(".apk") {
        let entry = format!("assets/bin/Data/Managed/{ASSEMBLY}.dll");
        match pkgzip::zip::Archive::open(&args[0]).and_then(|z| z.read(&entry)) {
            Ok(d) => Assembly::from_bytes(&entry, d),
            Err(err) => {
                eprintln!("jitwarm-seed: {}: {err}", args[0]);
                return ExitCode::from(1);
            }
        }
    } else {
        Assembly::open(std::path::Path::new(&args[0]).join(format!("{ASSEMBLY}.dll")))
    };
    let path = &args[0];
    let a = match a {
        Ok(a) => a,
        Err(err) => {
            eprintln!("jitwarm-seed: {path}: {err}");
            return ExitCode::from(1);
        }
    };
    match seed(&a, &args[2..]) {
        Ok((text, n, nt)) => {
            let tmp = format!("{}.tmp", args[1]);
            if let Err(err) =
                std::fs::write(&tmp, text).and_then(|_| std::fs::rename(&tmp, &args[1]))
            {
                eprintln!("jitwarm-seed: {}: {err}", args[1]);
                return ExitCode::from(1);
            }
            println!(
                "jitwarm-seed: {n} methods of {nt} types ({})",
                args[2..].join(", ")
            );
            ExitCode::SUCCESS
        }
        Err(err) => {
            eprintln!("jitwarm-seed: {err}");
            ExitCode::from(1)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::guid_text;

    #[test]
    fn guid_as_mono_prints_it() {
        // .NET's text form: the first three fields little-endian, the rest in order, upper case
        let g = [
            0x06, 0xBD, 0xDA, 0x52, 0xD9, 0x09, 0x64, 0x46, 0x89, 0x58, 0xA7, 0xE4, 0x19, 0x5B,
            0x3B, 0xFE,
        ];
        assert_eq!(guid_text(&g), "52DABD06-09D9-4664-8958-A7E4195B3BFE");
    }
}
