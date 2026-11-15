//! `pmu jnitrace <trace.json>`: what a game's native code asked Java for on a phone, from a jnitrace
//! log (Frida's jnitrace, `-o trace.json`: one JSON object a JNI call), as rows `tools/re/jnicover`
//! checks against runtime/jni: `class TAB member TAB signature TAB kind TAB calls`, kind one of class,
//! method, static-method, field, static-field, native (RegisterNatives). Method and field rows come
//! from Get[Static]MethodID / Get[Static]FieldID, `calls` from the Call*/Get*/Set* calls through those
//! ids; a class row from FindClass. The summary (classes, members, calls, the busiest members) goes to
//! stderr. Run on a phone where the game runs as it should, then:
//!   pmu jnitrace trace.json > used.tsv && jnicover < used.tsv | grep -v '^ok'
//! lists what the handheld's Java side does not answer (docs/apk.md, "Debugging").
use serde_json::Value;
use std::collections::BTreeMap;
use std::process::ExitCode;

#[derive(Default)]
pub struct Used {
    /// (class, member, signature, kind) -> calls through it
    pub rows: BTreeMap<(String, String, String, String), u64>,
    pub calls: u64,
}

fn arg<'a>(e: &'a Value, i: usize) -> Option<&'a Value> {
    e.get("args")?.get(i)
}
fn data(v: Option<&Value>) -> Option<String> {
    v?.get("data")?.as_str().map(str::to_string)
}
/// A class's name as jnitrace gives it with the jclass ("0x..." where it could not name it: left out)
fn meta(v: Option<&Value>) -> Option<String> {
    v?.get("metadata")?.as_str().filter(|s| !s.starts_with("0x")).map(str::to_string)
}
fn id(v: Option<&Value>) -> Option<String> {
    v?.get("value")?.as_str().map(str::to_string)
}

/// The rows of a trace: every class looked up, every member id made (with the calls made through it)
/// and every native registered.
pub fn read(trace: &Value) -> Result<Used, String> {
    let list = trace.as_array().ok_or("not a jnitrace JSON log (an array of calls)")?;
    let mut u = Used::default();
    // a member id -> its row (ids are per class and stay valid: jnitrace prints each id's value)
    let mut ids: BTreeMap<String, (String, String, String, String)> = BTreeMap::new();
    for e in list {
        let Some(name) = e.pointer("/method/name").and_then(Value::as_str) else { continue };
        if e.get("struct").and_then(Value::as_str) != Some("JNIEnv") {
            continue;
        }
        match name {
            "FindClass" => {
                if let Some(c) = data(arg(e, 1)) {
                    u.rows.entry((c, "-".into(), "-".into(), "class".into())).or_insert(0);
                }
            }
            "GetMethodID" | "GetStaticMethodID" | "GetFieldID" | "GetStaticFieldID" => {
                let (Some(c), Some(m), Some(s)) = (meta(arg(e, 1)), data(arg(e, 2)), data(arg(e, 3))) else { continue };
                let kind = match name {
                    "GetMethodID" if m == "<init>" => "ctor",
                    "GetMethodID" => "method",
                    "GetStaticMethodID" => "static-method",
                    "GetFieldID" => "field",
                    _ => "static-field",
                };
                let key = (c, m, s, kind.to_string());
                u.rows.entry(key.clone()).or_insert(0);
                if let Some(v) = id(e.get("ret")) {
                    ids.insert(v, key);
                }
            }
            "RegisterNatives" => {
                let Some(c) = meta(arg(e, 1)) else { continue };
                let Some(ms) = arg(e, 2).and_then(|v| v.get("data")).and_then(Value::as_array) else { continue };
                for m in ms {
                    let (Some(n), Some(s)) = (data(m.get("name")), data(m.get("sig"))) else { continue };
                    u.rows.entry((c.clone(), n, s, "native".into())).or_insert(0);
                }
            }
            _ => {
                // Call*Method*, NewObject*, Get*/Set*Field: the id argument (the first one jnitrace names)
                let through = name.starts_with("Call") || name.starts_with("NewObject")
                    || ((name.starts_with("Get") || name.starts_with("Set")) && name.contains("Field"));
                if !through {
                    continue;
                }
                let Some(args) = e.get("args").and_then(Value::as_array) else { continue };
                let key = args.iter().filter_map(|a| a.get("value").and_then(Value::as_str))
                    .find_map(|v| ids.get(v).cloned());
                if let Some(k) = key {
                    *u.rows.entry(k).or_insert(0) += 1;
                    u.calls += 1;
                }
            }
        }
    }
    Ok(u)
}

pub fn run(args: &[String]) -> ExitCode {
    let Some(path) = args.first() else {
        eprintln!("usage: pmu jnitrace <trace.json>   (a jnitrace -o log; rows for tools/re/jnicover on stdout)");
        return ExitCode::from(2);
    };
    let text = match std::fs::read_to_string(path) {
        Ok(t) => t,
        Err(e) => {
            eprintln!("pmu jnitrace: {path}: {e}");
            return ExitCode::from(1);
        }
    };
    let v: Value = match serde_json::from_str(&text) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("pmu jnitrace: {path}: not JSON: {e}");
            return ExitCode::from(1);
        }
    };
    let u = match read(&v) {
        Ok(u) => u,
        Err(e) => {
            eprintln!("pmu jnitrace: {path}: {e}");
            return ExitCode::from(1);
        }
    };
    for ((c, m, s, k), n) in &u.rows {
        println!("{c}\t{m}\t{s}\t{k}\t{n}");
    }
    let classes = u.rows.keys().filter(|k| k.3 == "class").count();
    eprintln!("pmu jnitrace: {classes} classes, {} members, {} calls through them", u.rows.len() - classes, u.calls);
    let mut busy: Vec<_> = u.rows.iter().filter(|(_, n)| **n > 0).collect();
    busy.sort_by(|a, b| b.1.cmp(a.1));
    for ((c, m, s, _), n) in busy.iter().take(10) {
        eprintln!("  {n:>8}  {c}.{m}{s}");
    }
    ExitCode::SUCCESS
}

#[cfg(test)]
mod tests {
    use super::*;

    // a made-up trace in jnitrace's shape: a class, a method id and two calls through it, a static
    // field, natives, and a call through an id the trace never made (left out)
    const TRACE: &str = r#"[
      {"struct":"JNIEnv","method":{"name":"FindClass"},"args":[{"value":"0x1"},{"value":"0x2","data":"com/example/Billing"}],"ret":{"value":"0x81","metadata":"com/example/Billing"}},
      {"struct":"JNIEnv","method":{"name":"GetMethodID"},"args":[{"value":"0x1"},{"value":"0x81","metadata":"com/example/Billing"},{"value":"0x3","data":"isReady"},{"value":"0x4","data":"()Z"}],"ret":{"value":"0x9001","metadata":"isReady()Z"}},
      {"struct":"JNIEnv","method":{"name":"CallBooleanMethod"},"args":[{"value":"0x1"},{"value":"0x55"},{"value":"0x9001","metadata":"isReady()Z"}],"ret":{"value":"0x0"}},
      {"struct":"JNIEnv","method":{"name":"CallBooleanMethodV"},"args":[{"value":"0x1"},{"value":"0x55"},{"value":"0x9001"}],"ret":{"value":"0x0"}},
      {"struct":"JNIEnv","method":{"name":"GetMethodID"},"args":[{"value":"0x1"},{"value":"0x81","metadata":"com/example/Billing"},{"value":"0x3","data":"<init>"},{"value":"0x4","data":"()V"}],"ret":{"value":"0x9002"}},
      {"struct":"JNIEnv","method":{"name":"GetStaticFieldID"},"args":[{"value":"0x1"},{"value":"0x81","metadata":"com/example/Billing"},{"value":"0x3","data":"OK"},{"value":"0x4","data":"I"}],"ret":{"value":"0x9003"}},
      {"struct":"JNIEnv","method":{"name":"GetStaticIntField"},"args":[{"value":"0x1"},{"value":"0x81"},{"value":"0x9003"}],"ret":{"value":"0x0"}},
      {"struct":"JNIEnv","method":{"name":"CallVoidMethod"},"args":[{"value":"0x1"},{"value":"0x55"},{"value":"0x7777"}],"ret":{}},
      {"struct":"JNIEnv","method":{"name":"RegisterNatives"},"args":[{"value":"0x1"},{"value":"0x81","metadata":"com/example/Billing"},{"value":"0x5","data":[{"name":{"data":"nativeDone"},"sig":{"data":"()V"},"fnPtr":{"value":"0x6"}}]},{"value":"0x1"}],"ret":{"value":"0x0"}},
      {"struct":"JavaVM","method":{"name":"GetEnv"},"args":[],"ret":{}}
    ]"#;

    #[test]
    fn rows_and_calls() {
        let u = read(&serde_json::from_str(TRACE).unwrap()).unwrap();
        let k = |c: &str, m: &str, s: &str, kind: &str| (c.to_string(), m.to_string(), s.to_string(), kind.to_string());
        assert_eq!(u.rows.get(&k("com/example/Billing", "-", "-", "class")), Some(&0));
        assert_eq!(u.rows.get(&k("com/example/Billing", "isReady", "()Z", "method")), Some(&2));
        assert_eq!(u.rows.get(&k("com/example/Billing", "<init>", "()V", "ctor")), Some(&0));
        assert_eq!(u.rows.get(&k("com/example/Billing", "OK", "I", "static-field")), Some(&1));
        assert_eq!(u.rows.get(&k("com/example/Billing", "nativeDone", "()V", "native")), Some(&0));
        assert_eq!(u.rows.len(), 5);
        assert_eq!(u.calls, 3);
    }

    #[test]
    fn not_a_trace() {
        assert!(read(&serde_json::json!({"a": 1})).is_err());
    }
}
