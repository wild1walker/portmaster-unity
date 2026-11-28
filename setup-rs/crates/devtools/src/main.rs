//! `pmu-dev <tool> [args...]`: the tools of tools/ and runtime/ that were Python scripts, one module
//! a tool, the same arguments and output. Built on the porter's computer (setup-rs/tools/rs.sh:
//! `cargo run -q -p devtools --bin pmu-dev -- <tool> ...`), or for a handheld (aarch64 musl:
//! `cargo build --release --target aarch64-unknown-linux-musl -p devtools`) where a tool runs there.
use std::process::ExitCode;

mod astc_guide;
mod benchsum;
mod census3d;
mod devmon;
mod elf;
mod fontgen;
mod glrec_gen;
mod hitchsum;
mod il2cpp_lock;
mod media;
mod memlog;
mod prefs;
mod profsum;
mod re;
mod regress;
mod rigstats;
mod shaders;
mod shexport;
mod stacks;
mod syms;
mod typetrees;
mod uatest;
mod unityhooks;
mod util;

type Run = fn(&[String]) -> Result<u8, String>;

fn ok(r: Result<(), String>) -> Result<u8, String> {
    r.map(|()| 0)
}

const TOOLS: &[(&str, &str, Run)] = &[
    ("census3d", "what a 3D game's frames are made of, per scene, from its data: census3d <Game_Data> [--json out.json] [--top N] [--bundles REGEX]", census3d::run),
    ("fontgen", "runtime/host/osk_font.h from an X11 PCF font: fontgen <font.pcf[.gz]>", |a| ok(fontgen::run(a))),
    ("glrec-gen", "runtime/host/glrec_gen.h from Khronos' GLES headers: glrec-gen <dir with gl32.h and gl2ext.h>", |a| ok(glrec_gen::run(a))),
    ("profsum", "a UA_PROF recording summed up: profsum <prof-N.txt> [--syms DIR] [--lib FILE] [--thread T] [--top N] ...", profsum::run),
    ("benchsum", "tools/device/profbench.sh runs summed up: benchsum [--syms DIR] [--top N] [--brief] <run folder>...", benchsum::run),
    ("hitchsum", "UA_HITCH_TRACE / UA_GL_PROGLOG / UA_PROF_EVERY lines summed up: hitchsum <log> [--each] [--glprog] [--prof] ...", hitchsum::run),
    ("sym", "names for libunity.so offsets ($UA_SYMS, $UA_LIBUNITY_SYM): sym <hex offset>...", syms::sym_run),
    ("il2cpp-lock", "setup-rs/crates/il2cpp/src/lock.rs from Debian's index and Microsoft's .NET 8 release (network): il2cpp-lock [--packages FILE] [--dotnet V] [--keep-dotnet]", il2cpp_lock::run),
    ("memlog", "a rehearsed setup's memory log summed up (tools/port/rehearse.sh REHEARSE_MEMLOG=1): memlog <mem.log>", memlog::run),
    ("rigstats", "the box64 rig's screenshots as PNG and its numbers (tools/box64/rig.sh): rigstats <out dir> [--png]", rigstats::run),
    ("flatshaders", "fragment shaders writing grey, or without their texture fetches (--notex): flatshaders [--notex] <dump dir> <override dir> [hash...]", shaders::flat_run),
    ("deadfrags", "a frame's fragments of alpha 0 counted: deadfrags shaders <dump> <trace log> <override dir> [...] | sum <run dir>...", shaders::dead_run),
    ("png", "a screenshot (PPM) as a PNG: png <in.ppm> <out.png>", media::png_run),
    ("montage", "a folder's first four shot-<n>.ppm in a 2x2 PNG (rehearse.sh's shots.png): montage <dir> <out.png>", media::montage_run),
    ("pcmwav", "the emulated rig's OpenSL ES output as WAV files, with how loud: pcmwav <emu dir> <out dir>", media::pcmwav_run),
    ("imgdiff", "screenshots of two runs compared exactly: imgdiff <dirA> <dirB> [suffix]", media::imgdiff_run),
    ("uatest-sum", "a handheld run's results summed up (tools/device/test.sh): uatest-sum <out dir>", uatest::run),
    ("typetrees", "the class database's type trees tools/prepatch needs: typetrees <out.json> ($PMU_TPK or the built-in one)", typetrees::run),
    ("profcpu", "(on the handheld) per-thread CPU and the thermal state of a process every N s: profcpu <pid> [interval] [top]", devmon::profcpu),
    ("memprof", "(on the handheld) a process's memory by mapping every N s: memprof <pid> [interval] [top]", devmon::memprof),
    ("shaders-export", "a converted game's GLES3 programs as .vert/.frag/.comp files for shadercheck: shaders-export <data> <dir> [--bundles RX]", shexport::run),
    ("stacks", "(on the handheld) each thread's syscall and stack of a running ua_host: stacks <pid> [name-filter] [words]", stacks::run),
    ("unityhooks", "runtime/host/unityhooks.c's rows for a player: unityhooks <libunity.so> <libunity.sym.so> [--unity <version abi>]", unityhooks::run),
    ("regress", "tools/regress: the list of games through the tools, compared with the baseline: regress fast|rig|list|bisect|recheck ... (tools/regress/README.md)", regress::run_cli),
    ("astc-guide", "an ASTC guide file (each hard texture block's encoder effort, chosen on a PC): astc-guide <data> <guide> --config CONFIG [--target medium] [--tol 0] [--set k=v] [--only RX]", astc_guide::run),
    ("prefs", "a game's PlayerPrefs between a PC (Linux player's prefs) and the handheld (ua_host's shared_prefs): prefs show <file> | copy <from> <to> [--all]", prefs::run),
    ("elf-facts", "ELF facts of Android libraries as JSON (relocation formats, notes, TLS; tools/re/survey_player.sh): elf-facts LIB.so...", re::facts_run),
    ("elf-imports", "undefined symbols per library with provider and ABI notes: elf-imports OUTDIR LIB... [--glibc DIR]", re::imports_run),
    ("jni-tables", "RegisterNatives tables of an ARM32 library: jni-tables LIB.so [LIB.sym.so]", re::arm::jnitables_run),
    ("strrefs", "per-function string references of an ARM32 library: strrefs LIB.so LIB.sym.so [FILTER_REGEX]", re::arm::strrefs_run),
    ("jni-bindings", "strrefs output as the Java members libunity binds: jni-bindings STRREFS.txt OUT.tsv OUT_direct.txt", re::arm::jnibindings_run),
    ("dis", "functions disassembled by name (or str:REGEX, AArch64): dis LIB.so SYM.so NAME_REGEX [MAX]", re::arm::dis_run),
    ("strings", "printable strings of a file matching a regex, each once: strings FILE REGEX [--nul] [--sort]", re::arm::strings_run),
];

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let tool = args.first().and_then(|n| TOOLS.iter().find(|t| t.0 == n));
    let Some((name, _, run)) = tool else {
        if let Some(n) = args.first().filter(|n| !n.starts_with('-')) {
            eprintln!("pmu-dev: no tool {n}");
        }
        eprintln!("usage: pmu-dev <tool> [args...]");
        for (n, d, _) in TOOLS {
            eprintln!("  {n:12} {d}");
        }
        return ExitCode::from(2);
    };
    match run(&args[1..]) {
        Ok(code) => ExitCode::from(code),
        Err(e) => {
            eprintln!("pmu-dev {name}: {e}");
            ExitCode::from(1)
        }
    }
}
