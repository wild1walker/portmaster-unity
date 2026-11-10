//! `macho_plugins <Game>.app [--names]`: what `pmu android-data --macho` does with the native
//! plugins of a macOS build (macho_plugins::survey), one line per plugin as the setup's log has
//! it; nothing is written. `--names` adds, under each plugin, the names it asks of the system
//! that the runtime's loader has no answer for (docs/macho-route.md, "The data").
fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    let names = a.iter().any(|x| x == "--names");
    let apps: Vec<&String> = a.iter().filter(|x| !x.starts_with("--")).collect();
    if apps.len() != 1 {
        eprintln!("usage: macho_plugins <Game>.app [--names]");
        std::process::exit(2);
    }
    use androiddata::macho_plugins::{facts, lines, survey, unanswered, Verdict};
    let found = survey(&androiddata::macho::data_of(apps[0]));
    let there = found
        .iter()
        .filter(|p| p.verdict == Verdict::Copy)
        .map(|p| p.name.clone())
        .collect();
    for (p, (_, line)) in found.iter().zip(lines(&found)) {
        println!("{line}");
        if !names {
            continue;
        }
        let Ok(f) = facts(&p.binary) else { continue };
        for (name, lib) in unanswered(&f, &there) {
            println!("    {name} ({lib})");
        }
    }
}
