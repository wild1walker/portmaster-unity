//! il2cpp_bee.py: Unity 2021.2+'s il2cpp builds its C++ with Bee, whose native backend the editor
//! installers carry for their own system only. A stand-in takes its place (`install`): it lets
//! il2cpp's build program write the graph (bee.dag.json) and notes the target; `run_graph` then
//! runs the graph's nodes the target needs: shell commands (on several threads), files written
//! from the graph (WriteTextFile, WriteFile's payloads) and copies, each command skipped when an
//! earlier run's record (pmunity-done.jsonl: its command, environment and inputs' contents)
//! still matches.
use crate::hash;
use crate::util::{self, join};
use crate::{exit, fail, Error, Result};
use androiddata::pyjson::{self, Json};
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};

pub const TARGET: &str = "FinalProgram";
pub const ARGS_FILE: &str = "pmunity-backend.args";
pub const DONE_FILE: &str = "pmunity-done.jsonl";

pub const STUB: &str = r#"#!/bin/sh
# pmunity (portmaster-unity, MIT): stands in for Unity's bee_backend, which the Windows editor
# installer has only as a Windows program. It lets il2cpp's build program write the build graph
# (bee.dag.json); pmunity then runs the graph's commands itself (convert/pmunity/il2cpp_bee.py).
for a do
  case $a in
    --dagfilejson=*)
      printf '%s\n' "$@" > pmunity-backend.args
      exit 0 ;;
  esac
done
exit 4
"#;

pub const LAUNCHER: &str = r#"#!/bin/sh
# pmunity: il2cpp.dll on the .NET in PATH (what il2cpp hands its build program as the way to run it)
exec dotnet exec "$(dirname "$0")/il2cpp.dll" "$@"
"#;

pub fn deploy_dir(editor: &str) -> String {
    util::joins(&[editor, "il2cpp", "build", "deploy"])
}

/// uses_bee: this il2cpp builds with Bee and its native backend (2021.2+).
pub fn uses_bee(editor: &str) -> bool {
    util::isdir(&join(&deploy_dir(editor), "bee_backend"))
}

fn put(path: &str, text: &str) -> Result<()> {
    if std::fs::read(path).is_ok_and(|d| d == text.as_bytes()) && util::executable(path) {
        return Ok(());
    }
    util::makedirs(util::dirname(path))?;
    let part = format!("{path}.part");
    std::fs::write(&part, text).map_err(util::at(&part))?;
    util::chmod(&part, 0o755)?;
    util::replace(&part, path)
}

/// install: the backend stand-in and the il2cpp launcher (false: this il2cpp has no Bee).
pub fn install(editor: &str) -> Result<bool> {
    if !uses_bee(editor) {
        return Ok(false);
    }
    let d = deploy_dir(editor);
    for arch in ["linux-x64", "linux-arm64"] {
        put(
            &util::joins(&[&d, "bee_backend", arch, "bee_backend"]),
            STUB,
        )?;
    }
    if !util::exists(&join(&d, "il2cpp")) {
        put(&join(&d, "il2cpp"), LAUNCHER)?;
    }
    Ok(true)
}

pub fn graph_path(cache: &str) -> String {
    util::joins(&[cache, "buildstate", "bee.dag.json"])
}

/// target_of: the target the backend was asked for (its last argument), else TARGET.
pub fn target_of(dag: &str) -> String {
    if let Ok(t) = std::fs::read_to_string(join(util::dirname(dag), ARGS_FILE)) {
        let args: Vec<&str> = t.lines().filter(|a| !a.is_empty()).collect();
        if let Some(l) = args.last() {
            if !l.starts_with('-') {
                return l.to_string();
            }
        }
    }
    TARGET.to_string()
}

/// One node of the graph, with what the runner needs of it.
#[derive(Clone, Debug)]
pub struct Node {
    pub raw: Json,
    pub annotation: Option<String>,
    pub action: Option<String>,
    pub inputs: Vec<String>,
    pub outputs: Vec<String>,
    pub to_build: Vec<usize>,
    pub to_use: Vec<usize>,
    pub kind: String,
    pub env: Vec<(String, String)>,
}

impl Node {
    fn from(j: Json) -> Result<Node> {
        let strs = |k: &str| -> Vec<String> {
            j.get(k)
                .and_then(Json::as_list)
                .map(|l| l.iter().map(Json::py_str).collect())
                .unwrap_or_default()
        };
        let idx = |k: &str| -> Vec<usize> {
            j.get(k)
                .and_then(Json::as_list)
                .map(|l| {
                    l.iter()
                        .filter_map(|x| x.as_int())
                        .map(|x| x as usize)
                        .collect()
                })
                .unwrap_or_default()
        };
        let kind = j
            .get("ActionType")
            .filter(|v| v.truthy())
            .map(Json::py_str)
            .unwrap_or_else(|| "RunShellCommand".into());
        let mut env: Vec<(String, String)> = Vec::new();
        for e in j
            .get("Env")
            .and_then(Json::as_list)
            .cloned()
            .unwrap_or_default()
        {
            if let Some(k) = e.get("Key") {
                let k = k.py_str();
                let v = e.get("Value").map(Json::py_str).unwrap_or("None".into());
                match env.iter_mut().find(|x| x.0 == k) {
                    Some(x) => x.1 = v,
                    None => env.push((k, v)),
                }
            }
        }
        Ok(Node {
            annotation: j
                .get("Annotation")
                .and_then(|a| a.as_str().map(str::to_string)),
            action: j.get("Action").and_then(|a| a.as_str().map(str::to_string)),
            inputs: strs("Inputs"),
            outputs: strs("Outputs"),
            to_build: idx("ToBuildDependencies"),
            to_use: idx("ToUseDependencies"),
            kind,
            env,
            raw: j,
        })
    }

    fn annotation_str(&self) -> String {
        self.raw
            .get("Annotation")
            .map(Json::py_str)
            .unwrap_or_else(|| "None".into())
    }
}

pub struct Graph {
    pub path: String,
    pub nodes: Vec<Node>,
    pub named: Vec<(String, usize)>,
    pub root: String,
    pub payloads: Option<String>,
    writer: HashMap<String, usize>,
}

impl Graph {
    pub fn open(path: &str) -> Result<Graph> {
        let b = util::read(path)?;
        let d = pyjson::loads_bytes(&b)
            .map_err(|e| Error::Fail(format!("json.decoder.JSONDecodeError: {e}")))?;
        let Some(Json::List(nodes)) = d.get("Nodes").cloned() else {
            return fail(format!("KeyError: 'Nodes' ({path})"));
        };
        let nodes: Vec<Node> = nodes.into_iter().map(Node::from).collect::<Result<_>>()?;
        let named: Vec<(String, usize)> = match d.get("NamedNodes") {
            Some(Json::Obj(o)) => o
                .iter()
                .filter_map(|(k, v)| v.as_int().map(|i| (k.clone(), i as usize)))
                .collect(),
            _ => Vec::new(),
        };
        let rel = d
            .get("RelativePathToRoot")
            .filter(|v| v.truthy())
            .map(Json::py_str)
            .unwrap_or_else(|| ".".into());
        let root = util::normpath(&join(util::dirname(&util::abspath(path)), &rel));
        let payloads = d.get("PayloadsFile").filter(|v| v.truthy()).map(|p| {
            let p = p.py_str();
            if util::isabs(&p) {
                p
            } else {
                join(&root, &p)
            }
        });
        let mut g = Graph {
            path: path.to_string(),
            nodes,
            named,
            root,
            payloads,
            writer: HashMap::new(),
        };
        for (i, n) in g.nodes.iter().enumerate() {
            for o in &n.outputs {
                g.writer.entry(g.path_of(o)).or_insert(i);
            }
        }
        Ok(g)
    }

    pub fn path_of(&self, p: &str) -> String {
        if util::isabs(p) {
            p.to_string()
        } else {
            join(&self.root, p)
        }
    }

    pub fn payload(&self, n: &Node) -> Result<Vec<u8>> {
        let Some(off) = n.raw.get("PayloadOffset") else {
            return Ok(n
                .raw
                .get("WriteTextFilePayload")
                .filter(|v| v.truthy())
                .map(Json::py_str)
                .unwrap_or_default()
                .into_bytes());
        };
        let off = off.as_int().unwrap_or(0) as u64;
        let len =
            n.raw
                .get("PayloadLength")
                .and_then(Json::as_int)
                .ok_or_else(|| Error::Fail("KeyError: 'PayloadLength'".into()))? as u64;
        let pf = self
            .payloads
            .clone()
            .ok_or_else(|| Error::Fail("TypeError: no PayloadsFile".into()))?;
        let data = crate::fetch::local(&pf, off, len)?;
        if data.len() as u64 != len {
            return exit(format!("{pf}: {}: payload cut short", n.annotation_str()));
        }
        Ok(data)
    }

    pub fn index(&self, target: &str) -> Result<usize> {
        if let Some((_, i)) = self.named.iter().find(|(k, _)| k == target) {
            return Ok(*i);
        }
        for (i, n) in self.nodes.iter().enumerate() {
            if n.annotation.as_deref() == Some(target) {
                return Ok(i);
            }
        }
        exit(format!(
            "{}: no node \"{target}\" in the build graph",
            self.path
        ))
    }

    pub fn deps(&self, i: usize) -> Vec<usize> {
        let n = &self.nodes[i];
        let mut out = n.to_build.clone();
        for p in &n.inputs {
            if let Some(&j) = self.writer.get(&self.path_of(p)) {
                if j != i {
                    out.push(j);
                }
            }
        }
        out
    }

    pub fn use_deps(&self, i: usize) -> &[usize] {
        &self.nodes[i].to_use
    }

    /// needed: the nodes building `target` takes (both kinds of dependencies, transitively).
    pub fn needed(&self, target: &str) -> Result<BTreeSet<usize>> {
        let mut seen = BTreeSet::new();
        let mut todo = vec![self.index(target)?];
        while let Some(i) = todo.pop() {
            if i >= self.nodes.len() {
                return fail("IndexError: list index out of range");
            }
            if !seen.insert(i) {
                continue;
            }
            todo.extend(self.deps(i));
            todo.extend(self.use_deps(i));
        }
        Ok(seen)
    }

    /// prerequisites: what must be done before node i runs.
    pub fn prerequisites(&self, i: usize) -> BTreeSet<usize> {
        let mut out = BTreeSet::new();
        let mut todo = self.deps(i);
        while let Some(j) = todo.pop() {
            if j >= self.nodes.len() || !out.insert(j) {
                continue;
            }
            todo.extend(self.use_deps(j));
        }
        out.remove(&i);
        out
    }
}

/// Files' contents' digests, each file read once a run (by size and time).
#[derive(Default)]
pub struct Digests {
    memo: Mutex<HashMap<(String, u64, i128), String>>,
}

impl Digests {
    pub fn get(&self, path: &str) -> Result<String> {
        let Ok(st) = std::fs::metadata(path) else {
            return Ok("-".into());
        };
        let mt = st
            .modified()
            .ok()
            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
            .map_or(0, |d| d.as_nanos() as i128);
        let k = (path.to_string(), st.len(), mt);
        if let Some(v) = self.memo.lock().unwrap().get(&k) {
            return Ok(v.clone());
        }
        let h = hash::sha1_file(path)?;
        self.memo.lock().unwrap().insert(k, h.clone());
        Ok(h)
    }
}

fn opt_list(j: Option<&Json>) -> Json {
    match j {
        Some(v) => v.clone(),
        None => Json::Null,
    }
}

/// signature: a command's record: its command line, environment, its inputs' contents.
pub fn signature(g: &Graph, n: &Node, digest: &Digests) -> Result<String> {
    let env = Json::Obj(
        n.env
            .iter()
            .map(|(k, v)| (k.clone(), Json::Str(v.clone())))
            .collect(),
    );
    let head = Json::List(vec![
        Json::Str(n.kind.clone()),
        opt_list(n.raw.get("Action")),
        opt_list(n.raw.get("Inputs")),
        opt_list(n.raw.get("Outputs")),
        env,
    ]);
    let mut h = hash::Sha256::default();
    h.update(pyjson::dumps(&head, None).as_bytes());
    for p in &n.inputs {
        let d = digest.get(&g.path_of(p))?;
        h.update(format!("{p}\0{d}\0").as_bytes());
    }
    Ok(h.hexdigest())
}

fn load_done(path: &str) -> HashMap<String, String> {
    let mut done = HashMap::new();
    let Ok(t) = std::fs::read(path) else {
        return done;
    };
    for line in String::from_utf8_lossy(&t).split_inclusive('\n') {
        if let Ok(Json::List(l)) = pyjson::loads(line.trim_end_matches('\n')) {
            if l.len() == 2 {
                done.insert(l[0].py_str(), l[1].py_str());
            }
        }
    }
    done
}

fn key(n: &Node) -> String {
    match n.outputs.first() {
        Some(o) => o.clone(),
        None => n
            .raw
            .get("Annotation")
            .map(Json::py_str)
            .unwrap_or_default(),
    }
}

/// write_file: WriteTextFile / WriteFile nodes (a file already the same is left as it is).
pub fn write_file(g: &Graph, n: &Node) -> Result<()> {
    let dst = g.path_of(
        n.outputs
            .first()
            .ok_or_else(|| Error::Fail("IndexError: list index out of range".into()))?,
    );
    let data = g.payload(n)?;
    if std::fs::read(&dst).is_ok_and(|d| d == data) {
        return Ok(());
    }
    let dir = util::dirname(&dst);
    util::makedirs(if dir.is_empty() { "." } else { dir })?;
    std::fs::write(&dst, data).map_err(util::at(&dst))
}

/// copy_files: CopyFiles nodes (a copy of the same size and second is left as it is).
pub fn copy_files(g: &Graph, n: &Node) -> Result<()> {
    for (s, d) in n.inputs.iter().zip(&n.outputs) {
        let (s, d) = (g.path_of(s), g.path_of(d));
        if let (Ok(a), Ok(b)) = (std::fs::metadata(&s), std::fs::metadata(&d)) {
            let sec = |m: &std::fs::Metadata| {
                m.modified()
                    .ok()
                    .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                    .map(|d| d.as_secs())
            };
            if a.len() == b.len() && sec(&a) == sec(&b) {
                continue;
            }
        }
        let dir = util::dirname(&d);
        util::makedirs(if dir.is_empty() { "." } else { dir })?;
        if util::lexists(&d) {
            util::unlink(&d)?;
        }
        util::copy2(&s, &d)?;
    }
    Ok(())
}

/// prepare: output folders made, outputs removed when the node asks (OverwriteOutputs false),
/// its target directories made.
pub fn prepare(g: &Graph, n: &Node) -> Result<()> {
    let overwrite_false = n.raw.get("OverwriteOutputs") == Some(&Json::Bool(false));
    for p in &n.outputs {
        let p = g.path_of(p);
        let dir = util::dirname(&p);
        util::makedirs(if dir.is_empty() { "." } else { dir })?;
        if overwrite_false && util::lexists(&p) {
            util::unlink(&p)?;
        }
    }
    if let Some(Json::List(l)) = n.raw.get("TargetDirectories") {
        for p in l {
            util::makedirs(&g.path_of(&p.py_str()))?;
        }
    }
    Ok(())
}

/// A shell for the graph's commands: sh(command, cwd, env) -> (exit code, output), called from
/// several threads.
pub trait Shell: Sync {
    fn sh(&self, command: &str, cwd: &str, env: &[(String, String)]) -> Result<(i32, String)>;
}

/// run_graph: what `target` (default: what the backend was asked for) needs from the graph at
/// `dag`, `jobs` commands at a time. progress(done, total, seconds) -> a line logged every
/// `every` seconds. Returns (commands run, commands done before).
#[allow(clippy::too_many_arguments)]
pub fn run_graph(
    dag: &str,
    shell: &dyn Shell,
    jobs: usize,
    log: &dyn Fn(&str),
    logfile: &str,
    target: Option<&str>,
    progress: &mut dyn FnMut(usize, usize, f64) -> Option<String>,
    every: f64,
) -> Result<(usize, usize)> {
    let g = Graph::open(dag)?;
    let target = target.map(str::to_string).unwrap_or_else(|| target_of(dag));
    let need = g.needed(&target)?;
    let mut pre: BTreeMap<usize, BTreeSet<usize>> = BTreeMap::new();
    for &i in &need {
        let p: BTreeSet<usize> = g.prerequisites(i).intersection(&need).copied().collect();
        pre.insert(i, p);
    }
    let mut users: BTreeMap<usize, Vec<usize>> = need.iter().map(|&i| (i, Vec::new())).collect();
    for (&i, ps) in &pre {
        for &p in ps {
            users.get_mut(&p).unwrap().push(i);
        }
    }
    let mut left: BTreeMap<usize, usize> = pre.iter().map(|(&i, ps)| (i, ps.len())).collect();
    let donefile = join(util::dirname(dag), DONE_FILE);
    let done = load_done(&donefile);
    let digest = Digests::default();
    let shells = need
        .iter()
        .filter(|&&i| {
            g.nodes[i].kind == "RunShellCommand"
                && g.nodes[i].action.as_deref().is_some_and(|a| !a.is_empty())
        })
        .count();
    let (mut ran, mut before) = (0usize, 0usize);
    let mut failed: Vec<(usize, i32, String)> = Vec::new();
    let t0 = util::now();
    let mut last = t0;
    let lf = Mutex::new(
        std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(logfile)
            .map_err(util::at(logfile))?,
    );
    let mut df = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&donefile)
        .map_err(util::at(&donefile))?;
    use std::io::Write;
    let lw = |s: &str| {
        let mut f = lf.lock().unwrap();
        let _ = f.write_all(s.as_bytes());
        let _ = f.flush();
    };
    lw(&format!(
        "graph {dag}: target {target}, {} nodes, {shells} commands\n",
        need.len()
    ));
    let jobs = jobs.max(1);
    let result: Result<()> = (|| {
        // the order the Python takes ready nodes in: a list used as a stack, filled in the order
        // of its dict (the nodes in index order)
        let mut ready: Vec<usize> = left
            .iter()
            .filter(|(_, &c)| c == 0)
            .map(|(&i, _)| i)
            .collect();
        let mut running: BTreeMap<usize, String> = BTreeMap::new();
        let g = &g;
        let (tx, rx) = mpsc::channel::<(usize, Result<(i32, String)>)>();
        let work: Arc<Mutex<Vec<usize>>> = Arc::new(Mutex::new(Vec::new()));
        let (wtx, wrx) = mpsc::channel::<Option<usize>>();
        let wrx = Mutex::new(wrx);
        std::thread::scope(|s| -> Result<()> {
            let _ = &work;
            for _ in 0..jobs {
                let tx = tx.clone();
                let wrx = &wrx;
                s.spawn(move || loop {
                    let job = wrx.lock().unwrap().recv();
                    let Ok(Some(i)) = job else { break };
                    let n = &g.nodes[i];
                    let r = shell.sh(n.action.as_deref().unwrap_or(""), &g.root, &n.env);
                    if tx.send((i, r)).is_err() {
                        break;
                    }
                });
            }
            let stop = |wtx: &mpsc::Sender<Option<usize>>| {
                for _ in 0..jobs {
                    let _ = wtx.send(None);
                }
            };
            let complete = |i: usize, left: &mut BTreeMap<usize, usize>, ready: &mut Vec<usize>| {
                for &u in &users[&i] {
                    let c = left.get_mut(&u).unwrap();
                    *c -= 1;
                    if *c == 0 {
                        ready.push(u);
                    }
                }
            };
            let r = (|| -> Result<()> {
                while !ready.is_empty() || !running.is_empty() {
                    while !ready.is_empty() && failed.is_empty() && running.len() < jobs {
                        let i = ready.pop().unwrap();
                        let n = &g.nodes[i];
                        match n.kind.as_str() {
                            "WriteTextFile" | "WriteFile" => {
                                write_file(g, n)?;
                                complete(i, &mut left, &mut ready);
                            }
                            "CopyFiles" => {
                                copy_files(g, n)?;
                                complete(i, &mut left, &mut ready);
                            }
                            "RunShellCommand" => {
                                if n.action.as_deref().is_none_or(str::is_empty) {
                                    complete(i, &mut left, &mut ready);
                                    continue;
                                }
                                let sig = signature(g, n, &digest)?;
                                if done.get(&key(n)) == Some(&sig)
                                    && n.outputs.iter().all(|p| util::exists(&g.path_of(p)))
                                {
                                    before += 1;
                                    complete(i, &mut left, &mut ready);
                                    continue;
                                }
                                prepare(g, n)?;
                                running.insert(i, sig);
                                let _ = wtx.send(Some(i));
                            }
                            k => {
                                return exit(format!(
                                    "{dag}: {}: a {k} node, which this backend stand-in does not run (il2cpp_bee.py)",
                                    n.annotation_str()
                                ))
                            }
                        }
                    }
                    if running.is_empty() {
                        break;
                    }
                    let mut fin = Vec::new();
                    match rx.recv_timeout(std::time::Duration::from_secs(2)) {
                        Ok(x) => {
                            fin.push(x);
                            while let Ok(x) = rx.try_recv() {
                                fin.push(x);
                            }
                        }
                        Err(mpsc::RecvTimeoutError::Timeout) => {}
                        Err(_) => return fail("the graph's threads stopped"),
                    }
                    for (i, r) in fin {
                        let sig = running.remove(&i).unwrap_or_default();
                        let (rc, out) = r?;
                        let n = &g.nodes[i];
                        if !out.trim().is_empty() || rc != 0 {
                            lw(&format!(
                                "[{}] exit {rc}\n{out}{}",
                                n.annotation_str(),
                                if out.ends_with('\n') { "" } else { "\n" }
                            ));
                        }
                        if rc == 0 {
                            let missing: Vec<&String> = n
                                .outputs
                                .iter()
                                .filter(|p| !util::exists(&g.path_of(p)))
                                .collect();
                            let allow = n
                                .raw
                                .get("AllowUnwrittenOutputFiles")
                                .is_some_and(Json::truthy);
                            if !missing.is_empty() && !allow {
                                let m: Vec<&str> = missing.iter().map(|s| s.as_str()).collect();
                                failed.push((
                                    i,
                                    rc,
                                    format!("{out}\n(did not write {})", m.join(", ")),
                                ));
                                continue;
                            }
                            ran += 1;
                            let line = pyjson::dumps(
                                &Json::List(vec![Json::Str(key(n)), Json::Str(sig)]),
                                None,
                            );
                            let _ = df.write_all(format!("{line}\n").as_bytes());
                            let _ = df.flush();
                            complete(i, &mut left, &mut ready);
                        } else {
                            failed.push((i, rc, out));
                        }
                    }
                    if util::now() - last >= every {
                        last = util::now();
                        if let Some(m) = progress(ran + before, shells, util::now() - t0) {
                            log(&m);
                        }
                    }
                }
                Ok(())
            })();
            stop(&wtx);
            // (a command still running when an error stopped the loop: its thread ends with it)
            while !running.is_empty() {
                match rx.recv() {
                    Ok((i, _)) => {
                        running.remove(&i);
                    }
                    Err(_) => break,
                }
            }
            r
        })?;
        Ok(())
    })();
    lw(&format!(
        "graph done: {ran} commands run, {before} done before, {} failed, {:.0} s\n",
        failed.len(),
        util::now() - t0
    ));
    result?;
    if let Some((i, rc, out)) = failed.first() {
        let n = &g.nodes[*i];
        let lines: Vec<&str> = out.trim().lines().collect();
        let tail = lines[lines.len().saturating_sub(30)..].join("\n");
        return exit(format!(
            "{} failed (exit {rc}):\n{tail}\ncommand: {}\nfull log: {logfile}",
            n.annotation_str(),
            n.raw
                .get("Action")
                .map(Json::py_str)
                .unwrap_or("None".into())
        ));
    }
    let pending: Vec<String> = left
        .iter()
        .filter(|(_, &c)| c > 0)
        .map(|(&i, _)| g.nodes[i].annotation_str())
        .collect();
    if !pending.is_empty() {
        let first: Vec<String> = pending
            .iter()
            .take(4)
            .map(|p| pkgzip::py::str_repr(p))
            .collect();
        return exit(format!(
            "{dag}: {} nodes never became ready (a dependency cycle?): [{}]",
            pending.len(),
            first.join(", ")
        ));
    }
    Ok((ran, before))
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Sh;
    impl Shell for Sh {
        fn sh(&self, command: &str, cwd: &str, env: &[(String, String)]) -> Result<(i32, String)> {
            let o = std::process::Command::new("/bin/sh")
                .arg("-c")
                .arg(command)
                .current_dir(cwd)
                .envs(env.iter().map(|(a, b)| (a, b)))
                .output()
                .unwrap();
            Ok((
                o.status.code().unwrap_or(-1),
                String::from_utf8_lossy(&o.stdout).into_owned(),
            ))
        }
    }

    #[test]
    fn graph_runs_and_resumes() {
        let d = std::env::temp_dir().join(format!("il2cpp-bee-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        let st = d.join("buildstate");
        std::fs::create_dir_all(&st).unwrap();
        std::fs::write(st.join("payloads"), b"PAYLOAD-BYTES").unwrap();
        let dag = r#"{"Nodes": [
          {"Annotation": "lump", "ActionType": "WriteTextFile", "WriteTextFilePayload": "int x;\n", "Outputs": ["out/lump.c"]},
          {"Annotation": "pay", "ActionType": "WriteFile", "PayloadOffset": 8, "PayloadLength": 5, "Outputs": ["out/p.txt"]},
          {"Annotation": "cc", "Action": "cat out/lump.c out/p.txt > out/a.o; echo $X", "Inputs": ["out/lump.c", "out/p.txt"], "Outputs": ["out/a.o"], "Env": [{"Key": "X", "Value": "hi"}]},
          {"Annotation": "link", "Action": "cat out/a.o > out/lib.so", "Inputs": ["out/a.o"], "Outputs": ["out/lib.so"]},
          {"Annotation": "copy", "ActionType": "CopyFiles", "Inputs": ["out/lib.so"], "Outputs": ["final/lib.so"]},
          {"Annotation": "FinalProgram", "ToBuildDependencies": [4]},
          {"Annotation": "unused", "Action": "false", "Outputs": ["x"]}
        ], "NamedNodes": {"FinalProgram": 5}, "PayloadsFile": "buildstate/payloads", "RelativePathToRoot": ".."}"#;
        let dagp = st.join("bee.dag.json");
        std::fs::write(&dagp, dag).unwrap();
        let logf = d.join("build.log");
        let r = run_graph(
            dagp.to_str().unwrap(),
            &Sh,
            3,
            &|_| {},
            logf.to_str().unwrap(),
            None,
            &mut |_, _, _| None,
            30.0,
        )
        .unwrap();
        assert_eq!(r, (2, 0));
        assert_eq!(
            std::fs::read(d.join("final/lib.so")).unwrap(),
            b"int x;\nBYTES"
        );
        let log = std::fs::read_to_string(&logf).unwrap();
        assert!(log.contains("[cc] exit 0\nhi\n"), "{log}");
        // again: nothing to run
        let r = run_graph(
            dagp.to_str().unwrap(),
            &Sh,
            3,
            &|_| {},
            logf.to_str().unwrap(),
            None,
            &mut |_, _, _| None,
            30.0,
        )
        .unwrap();
        assert_eq!(r, (0, 2));
        // an input changed: what follows from it runs again
        std::fs::write(st.join("payloads"), b"PAYLOAD-BYTEZ").unwrap();
        let r = run_graph(
            dagp.to_str().unwrap(),
            &Sh,
            1,
            &|_| {},
            logf.to_str().unwrap(),
            None,
            &mut |_, _, _| None,
            30.0,
        )
        .unwrap();
        assert_eq!(r, (2, 0));
        // a failing command stops the build with its output
        let bad = dag.replace("cat out/a.o > out/lib.so", "echo broken; exit 3");
        std::fs::write(&dagp, bad).unwrap();
        let _ = std::fs::remove_file(d.join("out/lib.so"));
        let e = run_graph(
            dagp.to_str().unwrap(),
            &Sh,
            2,
            &|_| {},
            logf.to_str().unwrap(),
            None,
            &mut |_, _, _| None,
            30.0,
        )
        .unwrap_err();
        assert!(
            e.to_string()
                .starts_with("link failed (exit 3):\nbroken\ncommand: echo broken; exit 3"),
            "{e}"
        );
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn stub_installed() {
        let d = std::env::temp_dir().join(format!("il2cpp-beei-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        let ed = d.to_str().unwrap();
        assert!(!install(ed).unwrap());
        std::fs::create_dir_all(d.join("il2cpp/build/deploy/bee_backend")).unwrap();
        assert!(install(ed).unwrap());
        let s = std::fs::read_to_string(
            d.join("il2cpp/build/deploy/bee_backend/linux-x64/bee_backend"),
        )
        .unwrap();
        assert_eq!(s, STUB);
        assert!(util::executable(
            d.join("il2cpp/build/deploy/il2cpp").to_str().unwrap()
        ));
        let _ = std::fs::remove_dir_all(&d);
    }
}
