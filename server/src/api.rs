use crate::coord::Coord;
use crate::exec::{self, Arg, RunResult};
use crate::jsjson;
use crate::wire::{self, Peer};
use axum::body::Body;
use axum::extract::{Request, State};
use axum::http::{header, HeaderMap, HeaderValue, Method, StatusCode};
use axum::response::Response;
use base64::Engine;
use futures_util::{Stream, StreamExt};
use serde::{Deserialize, Serialize};
use serde_json::{json, Map, Value};
use std::collections::HashMap;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, RwLock};
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::sync::{OwnedSemaphorePermit, Semaphore};

pub const LEVELS: [&str; 18] = [
    "1b", "1a", "2b", "2u", "2a", "3b", "3u", "3a", "4", "4f", "4e", "x1a", "x3", "x4", "x6", "e1",
    "vt1", "vt3",
];
const CHECK_ONLY: [&str; 7] = ["x4p", "x5g", "x5n", "x5pg", "x6n", "x6p", "vt2"];
const BOOL_FLAGS: [(&str, &str); 6] = [
    ("ua", "--ua"),
    ("allowVisualRisk", "--allow-visual-risk"),
    ("rasterizePages", "--rasterize-pages"),
    ("outlineFonts", "--outline-fonts"),
    ("analyze", "--analyze"),
    ("embedSource", "--embed-source"),
];
const TEXT_FLAGS: [(&str, &str); 10] = [
    ("lang", "--lang"),
    ("outputCondition", "--output-condition"),
    ("outputConditionInfo", "--output-condition-info"),
    ("registry", "--registry"),
    ("vtRecords", "--vt-records"),
    ("attachXmlName", "--attach-xml-name"),
    ("facturxProfile", "--facturx-profile"),
    ("embedSourceName", "--embed-source-name"),
    ("rasterDpi", "--raster-dpi"),
    ("imageMaxPpi", "--image-max-ppi"),
];
const FILE_FIELDS: [(&str, &str, &str); 5] = [
    ("xml", "--attach-xml", "attachment.xml"),
    ("destProfile", "--dest-profile", "dest.icc"),
    ("defaultRgb", "--default-rgb", "rgb.icc"),
    ("defaultCmyk", "--default-cmyk", "cmyk.icc"),
    ("defaultGray", "--default-gray", "gray.icc"),
];
const STATIC: [(&str, &str, &str); 13] = [
    ("/index.html", "index.html", "text/html; charset=utf-8"),
    (
        "/preflight.html",
        "preflight.html",
        "text/html; charset=utf-8",
    ),
    ("/app.js", "app.js", "text/javascript; charset=utf-8"),
    (
        "/preflight.js",
        "preflight.js",
        "text/javascript; charset=utf-8",
    ),
    ("/ui.js", "ui.js", "text/javascript; charset=utf-8"),
    ("/pdf.js", "pdf.js", "text/javascript; charset=utf-8"),
    (
        "/profiles.js",
        "profiles.js",
        "text/javascript; charset=utf-8",
    ),
    (
        "/engine.js",
        "engine-http.js",
        "text/javascript; charset=utf-8",
    ),
    (
        "/icons/phosphor.js",
        "icons/phosphor.js",
        "text/javascript; charset=utf-8",
    ),
    ("/app.css", "app.css", "text/css; charset=utf-8"),
    ("/logo.svg", "logo.svg", "image/svg+xml"),
    (
        "/fonts/dm-sans-latin.woff2",
        "fonts/dm-sans-latin.woff2",
        "font/woff2",
    ),
    (
        "/fonts/dm-sans-latin-ext.woff2",
        "fonts/dm-sans-latin-ext.woff2",
        "font/woff2",
    ),
];
const DOCS_URL: &str = "https://kura.bentopdf.com/docs/";

#[derive(Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Debug)]
#[serde(rename_all = "lowercase")]
pub enum Kind {
    Convert,
    Check,
    Verify,
}

#[derive(Clone, Debug)]
pub struct Config {
    pub host: String,
    pub port: u16,
    pub max_upload: u64,
    pub concurrency: usize,
    pub queue_limit: usize,
    pub timeout_ms: u64,
    pub api_token: String,
    pub cluster_token: String,
    pub kura_bin: PathBuf,
    pub profiles: PathBuf,
    pub site: PathBuf,
}

pub enum Exec {
    Local,
    Queue(Arc<Coord>),
    Remote(Peer),
}

pub struct App {
    pub cfg: Config,
    pub version: Arc<RwLock<String>>,
    pub profiles_json: String,
    pub exec: Exec,
    pub tmp_root: PathBuf,
    sem: Arc<Semaphore>,
    waiting: AtomicUsize,
}

pub struct Ran {
    pub result: RunResult,
    pub out_dir: PathBuf,
    pub release: Option<(Arc<Coord>, i64)>,
}

pub enum ExecError {
    Busy,
    Failed(String),
}

impl App {
    pub fn new(cfg: Config, version: Arc<RwLock<String>>, exec: Exec, tmp_root: PathBuf) -> Self {
        let profiles_json = jsjson::to_string(&load_profiles(&cfg.profiles));
        App {
            sem: Arc::new(Semaphore::new(cfg.concurrency)),
            cfg,
            version,
            profiles_json,
            exec,
            tmp_root,
            waiting: AtomicUsize::new(0),
        }
    }

    pub fn version(&self) -> String {
        self.version.read().map(|v| v.clone()).unwrap_or_default()
    }

    pub fn profile_count(&self) -> usize {
        serde_json::from_str::<Vec<Value>>(&self.profiles_json)
            .map(|v| v.len())
            .unwrap_or(0)
    }

    async fn acquire(&self) -> Option<OwnedSemaphorePermit> {
        if let Ok(p) = self.sem.clone().try_acquire_owned() {
            return Some(p);
        }
        if self.waiting.fetch_add(1, Ordering::SeqCst) >= self.cfg.queue_limit {
            self.waiting.fetch_sub(1, Ordering::SeqCst);
            return None;
        }
        struct Leave<'a>(&'a AtomicUsize);
        impl Drop for Leave<'_> {
            fn drop(&mut self) {
                self.0.fetch_sub(1, Ordering::SeqCst);
            }
        }
        let _leave = Leave(&self.waiting);
        self.sem.clone().acquire_owned().await.ok()
    }

    fn load(&self) -> (usize, usize) {
        match &self.exec {
            Exec::Queue(c) => c.load(),
            _ => (
                self.cfg.concurrency - self.sem.available_permits(),
                self.waiting.load(Ordering::SeqCst),
            ),
        }
    }

    pub async fn run(
        &self,
        dir: &Path,
        kind: Kind,
        args: Vec<Arg>,
        name: &str,
        dedupe: bool,
    ) -> Result<Ran, ExecError> {
        match &self.exec {
            Exec::Local => {
                let argv = exec::resolve(&args, dir, &self.cfg.profiles);
                let result =
                    exec::run_kura(&self.cfg.kura_bin, argv, dir, self.cfg.timeout_ms).await;
                Ok(Ran {
                    result,
                    out_dir: dir.to_path_buf(),
                    release: None,
                })
            }
            Exec::Queue(c) => {
                let id =
                    tokio::task::block_in_place(|| c.submit(dir, kind, &args, name, None, dedupe))?;
                let (result, out_dir) = c.wait(id).await;
                Ok(Ran {
                    result,
                    out_dir,
                    release: Some((c.clone(), id)),
                })
            }
            Exec::Remote(peer) => {
                remote_run(peer, dir, kind, args, name, dedupe, self.cfg.timeout_ms).await
            }
        }
    }
}

#[derive(Serialize, Deserialize)]
pub struct Submit {
    pub kind: Kind,
    pub args: Vec<Arg>,
    pub name: String,
    pub dedupe: bool,
}

async fn remote_run(
    peer: &Peer,
    dir: &Path,
    kind: Kind,
    args: Vec<Arg>,
    name: &str,
    dedupe: bool,
    timeout_ms: u64,
) -> Result<Ran, ExecError> {
    let fail = |e: io::Error| ExecError::Failed(e.to_string());
    let files = exec::input_files(&args);
    let meta = Submit {
        kind,
        args,
        name: name.to_string(),
        dedupe,
    };
    let body = wire::encode(&meta, dir, &files).await.map_err(fail)?;
    let res = peer
        .send(
            Method::POST,
            "/internal/submit",
            &[],
            body,
            Duration::from_millis(timeout_ms + 600_000),
        )
        .await
        .map_err(fail)?;
    if res.status() == StatusCode::TOO_MANY_REQUESTS {
        return Err(ExecError::Busy);
    }
    if res.status() != StatusCode::OK {
        return Err(ExecError::Failed(format!(
            "coordinator answered {}",
            res.status()
        )));
    }
    let bytes = http_body_util::BodyExt::collect(res.into_body())
        .await
        .map_err(|e| ExecError::Failed(e.to_string()))?;
    let id = serde_json::from_slice::<Value>(&bytes.to_bytes())
        .ok()
        .and_then(|v| v["id"].as_i64());
    let id = id.ok_or_else(|| ExecError::Failed("coordinator gave no job id".into()))?;
    let deadline =
        tokio::time::Instant::now() + Duration::from_millis(timeout_ms.saturating_mul(4) + 600_000);
    while tokio::time::Instant::now() < deadline {
        let path = format!("/internal/jobs/{id}/result");
        match peer
            .send(
                Method::GET,
                &path,
                &[],
                Body::empty(),
                Duration::from_secs(60),
            )
            .await
        {
            Ok(res) if res.status() == StatusCode::OK => {
                match wire::decode::<RunResult, _, _>(wire::body_stream(res), dir, u64::MAX).await {
                    Ok((result, _)) => {
                        let ack = format!("/internal/jobs/{id}/ack");
                        let _ = peer
                            .send(
                                Method::POST,
                                &ack,
                                &[],
                                Body::empty(),
                                Duration::from_secs(10),
                            )
                            .await;
                        return Ok(Ran {
                            result,
                            out_dir: dir.to_path_buf(),
                            release: None,
                        });
                    }
                    Err(e) => {
                        tracing::warn!(job = id, error = %e, "result download failed, retrying")
                    }
                }
            }
            Ok(res) if res.status() == StatusCode::NO_CONTENT => continue,
            Ok(res) if res.status() == StatusCode::NOT_FOUND => {
                return Err(ExecError::Failed("coordinator lost the job".into()))
            }
            Ok(res) => tracing::warn!(job = id, status = %res.status(), "unexpected result status"),
            Err(e) => tracing::warn!(job = id, error = %e, "coordinator unreachable, retrying"),
        }
        tokio::time::sleep(Duration::from_secs(1)).await;
    }
    Err(ExecError::Failed(
        "gave up waiting for the coordinator".into(),
    ))
}

fn load_profiles(root: &Path) -> Value {
    let mut out: Vec<(String, Value)> = Vec::new();
    let Ok(folders) = std::fs::read_dir(root) else {
        return Value::Array(vec![]);
    };
    for folder in folders.flatten() {
        if !folder.file_type().map(|t| t.is_dir()).unwrap_or(false) {
            continue;
        }
        let fname = folder.file_name().to_string_lossy().into_owned();
        let Ok(files) = std::fs::read_dir(folder.path()) else {
            continue;
        };
        for file in files.flatten() {
            let name = file.file_name().to_string_lossy().into_owned();
            let Some(stem) = name.strip_suffix(".json") else {
                continue;
            };
            let Ok(text) = std::fs::read_to_string(file.path()) else {
                continue;
            };
            let Ok(doc) = serde_json::from_str::<Value>(&text) else {
                continue;
            };
            if doc.is_null() {
                continue;
            }
            let pick = |k: &str| match doc.get(k) {
                None | Some(Value::Null) => Value::String(String::new()),
                Some(v) => v.clone(),
            };
            let id = format!("{fname}/{stem}");
            let mut m = Map::new();
            m.insert("id".into(), Value::String(id.clone()));
            m.insert("name".into(), pick("name"));
            m.insert("description".into(), pick("description"));
            out.push((id, Value::Object(m)));
        }
    }
    out.sort_by(|a, b| a.0.cmp(&b.0));
    Value::Array(out.into_iter().map(|(_, v)| v).collect())
}

pub fn json_response(status: StatusCode, body: String) -> Response {
    Response::builder()
        .status(status)
        .header(header::CONTENT_TYPE, "application/json")
        .header(header::CONTENT_LENGTH, body.len())
        .header("x-content-type-options", "nosniff")
        .body(Body::from(body))
        .expect("valid response")
}

fn error_body(code: &str, message: &str, extra: Option<(&str, Value)>) -> Value {
    let mut m = Map::new();
    m.insert("ok".into(), Value::Bool(false));
    m.insert("errorCode".into(), Value::String(code.into()));
    m.insert("error".into(), Value::String(message.into()));
    if let Some((k, v)) = extra {
        m.insert(k.into(), v);
    }
    Value::Object(m)
}

pub fn fail(status: StatusCode, code: &str, message: &str) -> Response {
    json_response(status, jsjson::to_string(&error_body(code, message, None)))
}

fn fail_extra(status: StatusCode, code: &str, message: &str, key: &str, v: Value) -> Response {
    json_response(
        status,
        jsjson::to_string(&error_body(code, message, Some((key, v)))),
    )
}

pub struct Query(Vec<(String, String)>);

impl Query {
    pub fn parse(raw: &str) -> Self {
        Query(
            form_urlencoded::parse(raw.as_bytes())
                .into_owned()
                .collect(),
        )
    }

    pub fn get(&self, k: &str) -> Option<&str> {
        self.0.iter().find(|(n, _)| n == k).map(|(_, v)| v.as_str())
    }
}

pub enum Field {
    Text(String),
    File(PathBuf),
}

pub struct BadArgs(
    pub StatusCode,
    pub &'static str,
    pub String,
    pub Option<(&'static str, Value)>,
);

impl BadArgs {
    pub fn body(&self) -> Value {
        error_body(self.1, &self.2, self.3.clone())
    }

    fn response(&self) -> Response {
        json_response(self.0, jsjson::to_string(&self.body()))
    }
}

fn text_value(v: &str) -> Option<&str> {
    let units = v.encode_utf16().count();
    if units == 0 || units > 512 || v.chars().any(|c| (c as u32) < 0x20 || c as u32 == 0x7f) {
        return None;
    }
    Some(v)
}

fn is_true(v: Option<&str>) -> bool {
    matches!(v, Some("true") | Some("1") | Some("on"))
}

pub fn library_profile(profiles: &Path, id: &str) -> bool {
    let valid = |s: &str| {
        !s.is_empty()
            && s.len() <= 100
            && s.bytes()
                .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
    };
    match id.split_once('/') {
        Some((a, b)) if valid(a) && valid(b) => {
            let path = exec::profile_path(profiles, id);
            match (path.canonicalize(), profiles.canonicalize()) {
                (Ok(path), Ok(root)) => path.starts_with(root),
                _ => false,
            }
        }
        _ => false,
    }
}

fn number(v: &str) -> bool {
    let mut parts = v.splitn(2, '.');
    let whole = parts.next().unwrap_or("");
    let frac = parts.next();
    v.len() <= 10
        && !whole.is_empty()
        && whole.bytes().all(|b| b.is_ascii_digit())
        && frac.is_none_or(|f| !f.is_empty() && f.bytes().all(|b| b.is_ascii_digit()))
}

fn value_ok(flag: &str, v: &str) -> bool {
    if text_value(v).is_none() {
        return false;
    }
    match flag {
        "--raster-dpi" | "--image-max-ppi" => number(v),
        "--vt-records" => v
            .bytes()
            .all(|b| b.is_ascii_digit() || b == b',' || b == b'-' || b == b' '),
        "--facturx-profile" => {
            v.len() <= 32
                && v.bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b == b' ' || b == b'_' || b == b'-')
        }
        _ => true,
    }
}

fn password_ok(p: &str) -> bool {
    !p.is_empty() && p.len() <= 65536 && !p.contains('\0')
}

pub fn validate_job(
    kind: Kind,
    args: &[Arg],
    dir: Option<&Path>,
    profiles: &Path,
) -> Result<(), String> {
    let mut it = args.iter().peekable();
    let present = |name: &str| match dir {
        Some(d) if !d.join(name).is_file() => Err(format!("the job is missing {name}")),
        _ => Ok(()),
    };
    let short = |s: &str| s.chars().take(64).collect::<String>();
    let expect = |a: Option<&Arg>, want: &str| match a {
        Some(Arg::Lit(s)) if s == want => Ok(()),
        _ => Err(format!("expected {want}")),
    };
    if kind == Kind::Verify {
        expect(it.next(), "--verify-password")?;
        if matches!(it.peek(), Some(Arg::Lit(s)) if s == "--password") {
            it.next();
            match it.next() {
                Some(Arg::Lit(p)) if password_ok(p) => {}
                _ => return Err("invalid value for --password".into()),
            }
        }
        match it.next() {
            Some(Arg::File(n)) if n == "in.pdf" => present(n)?,
            _ => return Err("the input must be in.pdf".into()),
        }
        return match it.next() {
            None => Ok(()),
            Some(_) => Err("unexpected trailing argument".into()),
        };
    }
    if kind == Kind::Check {
        expect(it.next(), "--check")?;
    }
    expect(it.next(), "--level")?;
    let level_ok =
        |l: &str| LEVELS.contains(&l) || (kind == Kind::Check && CHECK_ONLY.contains(&l));
    match it.next() {
        Some(Arg::Lit(l)) if level_ok(l) => {}
        _ => return Err("invalid level".into()),
    }
    let mut seen: Vec<&str> = Vec::new();
    while let Some(Arg::Lit(flag)) = it.peek() {
        let flag = flag.as_str();
        it.next();
        if seen.contains(&flag) {
            return Err(format!("option {} given twice", short(flag)));
        }
        seen.push(flag);
        if BOOL_FLAGS.iter().any(|(_, f)| *f == flag) {
            continue;
        }
        if TEXT_FLAGS.iter().any(|(_, f)| *f == flag) {
            match it.next() {
                Some(Arg::Lit(v)) if value_ok(flag, v) => continue,
                _ => return Err(format!("invalid value for {flag}")),
            }
        }
        if flag == "--password" {
            match it.next() {
                Some(Arg::Lit(p)) if password_ok(p) => continue,
                _ => return Err("invalid value for --password".into()),
            }
        }
        if flag == "--profile" {
            match it.next() {
                Some(Arg::Profile(id)) if library_profile(profiles, id) => continue,
                Some(Arg::File(n)) if n == "profile.json" => {
                    present(n)?;
                    continue;
                }
                _ => return Err("invalid value for --profile".into()),
            }
        }
        if let Some((_, _, name)) = FILE_FIELDS.iter().find(|(_, f, _)| *f == flag) {
            match it.next() {
                Some(Arg::File(n)) if n == name => {
                    present(n)?;
                    continue;
                }
                _ => return Err(format!("invalid value for {flag}")),
            }
        }
        return Err(format!("option {} is not allowed", short(flag)));
    }
    match it.next() {
        Some(Arg::File(n)) if n == "in.pdf" || n == "in.jpg" => present(n)?,
        _ => return Err("the input must be in.pdf or in.jpg".into()),
    }
    if kind == Kind::Convert {
        match it.next() {
            Some(Arg::Out(n)) if n == "out.pdf" => {}
            _ => return Err("the output must be out.pdf".into()),
        }
    }
    match it.next() {
        None => Ok(()),
        Some(_) => Err("unexpected trailing argument".into()),
    }
}

fn store(field: &Field, dest: &Path) -> io::Result<()> {
    match field {
        Field::Text(s) => std::fs::write(dest, s.as_bytes()),
        Field::File(p) => std::fs::rename(p, dest),
    }
}

#[allow(clippy::result_large_err)]
pub fn build_args(
    q: &Query,
    password: Option<String>,
    fields: &HashMap<String, Field>,
    dir: &Path,
    kind: Kind,
    profiles: &Path,
) -> Result<Vec<Arg>, BadArgs> {
    let text = |k: &str| match fields.get(k) {
        Some(Field::Text(s)) => Some(s.as_str()),
        _ => None,
    };
    let get = |k: &str| q.get(k).or_else(|| text(k));
    let allowed: Vec<&str> = if kind == Kind::Check {
        LEVELS.iter().chain(CHECK_ONLY.iter()).copied().collect()
    } else {
        LEVELS.to_vec()
    };
    let level = match q.get("level") {
        Some(l) => Some(l),
        None => text("level"),
    };
    let level = match level {
        Some(l) if allowed.contains(&l) => l,
        _ => {
            return Err(BadArgs(
                StatusCode::BAD_REQUEST,
                "BAD_LEVEL",
                format!("level must be one of {}", allowed.join(", ")),
                Some(("levels", json!(allowed))),
            ))
        }
    };
    let lit = |s: &str| Arg::Lit(s.to_string());
    let mut args = if kind == Kind::Check {
        vec![lit("--check"), lit("--level"), lit(level)]
    } else {
        vec![lit("--level"), lit(level)]
    };
    for (k, flag) in BOOL_FLAGS {
        if is_true(get(k)) {
            args.push(lit(flag));
        }
    }
    for (k, flag) in TEXT_FLAGS {
        let Some(raw) = get(k) else { continue };
        if raw.is_empty() {
            continue;
        }
        let Some(v) = text_value(raw).filter(|v| value_ok(flag, v)) else {
            return Err(BadArgs(
                StatusCode::BAD_REQUEST,
                "BAD_REQUEST",
                format!("invalid value for {k}"),
                None,
            ));
        };
        args.push(lit(flag));
        args.push(lit(v));
    }
    let password = password.or_else(|| text("password").map(str::to_string));
    if let Some(p) = password.filter(|p| !p.is_empty()) {
        args.push(lit("--password"));
        args.push(Arg::Lit(p));
    }
    let internal = |e: io::Error| {
        BadArgs(
            StatusCode::INTERNAL_SERVER_ERROR,
            "INTERNAL",
            e.to_string(),
            None,
        )
    };
    match q.get("profile").filter(|p| !p.is_empty()) {
        Some(id) => {
            if !library_profile(profiles, id) {
                return Err(BadArgs(
                    StatusCode::BAD_REQUEST,
                    "BAD_PROFILE",
                    "no bundled profile with that name".into(),
                    Some(("profiles", json!("/api/profiles"))),
                ));
            }
            args.push(lit("--profile"));
            args.push(Arg::Profile(id.to_string()));
        }
        None => {
            if let Some(f) = fields.get("profile") {
                store(f, &dir.join("profile.json")).map_err(internal)?;
                args.push(lit("--profile"));
                args.push(Arg::File("profile.json".into()));
            }
        }
    }
    for (k, flag, name) in FILE_FIELDS {
        let Some(f) = fields.get(k) else { continue };
        store(f, &dir.join(name)).map_err(internal)?;
        args.push(lit(flag));
        args.push(Arg::File(name.into()));
    }
    Ok(args)
}

pub fn sniff(head: &[u8]) -> Option<&'static str> {
    if head.len() >= 3 && head[..3] == [0xff, 0xd8, 0xff] {
        return Some("jpg");
    }
    let h = &head[..head.len().min(1024)];
    if h.windows(4).any(|w| w == b"%PDF") {
        return Some("pdf");
    }
    None
}

pub async fn sniff_file(p: &Path) -> io::Result<Option<&'static str>> {
    let mut f = tokio::fs::File::open(p).await?;
    let mut buf = Vec::with_capacity(1024);
    (&mut f).take(1024).read_to_end(&mut buf).await?;
    Ok(sniff(&buf))
}

pub fn parse_report(out: &str) -> Option<Map<String, Value>> {
    let line = out.split('\n').find(|l| l.starts_with('{'))?;
    let mut m = match serde_json::from_str::<Value>(line).ok()? {
        Value::Object(m) => m,
        _ => return None,
    };
    m.shift_remove("file");
    Some(m)
}

pub fn engine_failure(r: &RunResult) -> (StatusCode, Value) {
    if r.timed_out || r.code == Some(3) {
        return (
            StatusCode::GATEWAY_TIMEOUT,
            error_body("TIMEOUT", "the job exceeded the time limit", None),
        );
    }
    let report = parse_report(&r.out);
    if r.code == Some(2) {
        if let Some(mut m) = report {
            let pick = |m: &Map<String, Value>, k: &str, d: &str| match m.get(k) {
                None | Some(Value::Null) => Value::String(d.into()),
                Some(v) => v.clone(),
            };
            let code = pick(&m, "errorCode", "REJECTED");
            let error = pick(&m, "error", "input rejected");
            m.insert("ok".into(), Value::Bool(false));
            m.insert("errorCode".into(), code);
            m.insert("error".into(), error);
            return (StatusCode::UNPROCESSABLE_ENTITY, Value::Object(m));
        }
    }
    if r.code == Some(64) {
        let line = r
            .err
            .split('\n')
            .find(|l| !l.trim().is_empty())
            .unwrap_or("invalid options");
        return (
            StatusCode::BAD_REQUEST,
            error_body("BAD_REQUEST", line, None),
        );
    }
    let tail: String = r.err.chars().take(2000).collect();
    tracing::error!(code = ?r.code, stderr = %tail, "engine did not produce a report");
    (
        StatusCode::INTERNAL_SERVER_ERROR,
        error_body("INTERNAL", "the engine did not produce a report", None),
    )
}

pub enum Outcome {
    Pdf(Map<String, Value>, PathBuf),
    Json(StatusCode, Value),
}

pub fn outcome(kind: Kind, r: &RunResult, out_dir: &Path) -> Outcome {
    let ok01 = matches!(r.code, Some(0) | Some(1));
    let failed = |r: &RunResult| {
        let (s, v) = engine_failure(r);
        Outcome::Json(s, v)
    };
    match kind {
        Kind::Convert if r.code == Some(0) => {
            let report = parse_report(&r.out).unwrap_or_else(|| {
                let mut m = Map::new();
                m.insert("ok".into(), Value::Bool(true));
                m
            });
            Outcome::Pdf(report, out_dir.join("out.pdf"))
        }
        Kind::Check if ok01 => match parse_report(&r.out) {
            Some(m) => Outcome::Json(StatusCode::OK, Value::Object(m)),
            None => failed(&RunResult {
                code: Some(-1),
                ..r.clone()
            }),
        },
        Kind::Verify if ok01 => Outcome::Json(
            StatusCode::OK,
            json!({"ok": true, "valid": r.code == Some(0)}),
        ),
        _ => failed(r),
    }
}

fn header_value(s: &str) -> HeaderValue {
    HeaderValue::from_str(s).unwrap_or_else(|_| {
        HeaderValue::from_bytes(
            &s.chars()
                .map(|c| if (c as u32) < 256 { c as u8 } else { b'?' })
                .collect::<Vec<u8>>(),
        )
        .unwrap_or(HeaderValue::from_static(""))
    })
}

async fn respond(app: &App, kind: Kind, ran: &Ran, report_json: bool) -> io::Result<Response> {
    match outcome(kind, &ran.result, &ran.out_dir) {
        Outcome::Json(s, v) => Ok(json_response(s, jsjson::to_string(&v))),
        Outcome::Pdf(mut report, path) => {
            if report_json {
                let pdf = tokio::fs::read(&path).await?;
                report.insert(
                    "pdf".into(),
                    Value::String(base64::engine::general_purpose::STANDARD.encode(pdf)),
                );
                return Ok(json_response(
                    StatusCode::OK,
                    jsjson::to_string(&Value::Object(report)),
                ));
            }
            let file = tokio::fs::File::open(&path).await?;
            let len = file.metadata().await?.len();
            let level = match report.get("level") {
                None | Some(Value::Null) => String::new(),
                Some(v) => jsjson::js_string(v),
            };
            let engine = match report.get("engine") {
                None | Some(Value::Null) => app.version(),
                Some(v) => jsjson::js_string(v),
            };
            let changes = match report.get("issues") {
                None | Some(Value::Null) => "0".into(),
                Some(Value::Array(a)) => a.len().to_string(),
                Some(Value::String(s)) => s.encode_utf16().count().to_string(),
                Some(_) => "undefined".into(),
            };
            Ok(Response::builder()
                .status(StatusCode::OK)
                .header(header::CONTENT_TYPE, "application/pdf")
                .header(header::CONTENT_LENGTH, len)
                .header("x-kura-level", header_value(&level))
                .header("x-kura-engine", header_value(&engine))
                .header("x-kura-changes", header_value(&changes))
                .body(Body::from_stream(tokio_util::io::ReaderStream::new(file)))
                .expect("valid response"))
        }
    }
}

enum ReadError {
    TooLarge,
    Internal(String),
}

struct Parsed {
    file: Option<PathBuf>,
    fields: HashMap<String, Field>,
}

const DRAIN_BYTES: u64 = 64 << 20;
const DRAIN_TIME: Duration = Duration::from_secs(10);

async fn drain<S, E>(s: &mut S)
where
    S: Stream<Item = Result<bytes::Bytes, E>> + Unpin,
{
    let _ = tokio::time::timeout(DRAIN_TIME, async {
        let mut left = DRAIN_BYTES;
        while let Some(Ok(c)) = s.next().await {
            match left.checked_sub(c.len() as u64) {
                Some(l) => left = l,
                None => break,
            }
        }
    })
    .await;
}

fn limited(
    body: Body,
    limit: u64,
    over: Arc<AtomicBool>,
) -> impl Stream<Item = Result<bytes::Bytes, io::Error>> + Send + Unpin + 'static {
    let state = (body.into_data_stream(), 0u64, over);
    Box::pin(futures_util::stream::unfold(
        state,
        move |(mut s, mut seen, over)| async move {
            let chunk = match s.next().await? {
                Ok(c) => c,
                Err(e) => return Some((Err(io::Error::other(e)), (s, seen, over))),
            };
            seen += chunk.len() as u64;
            if seen > limit {
                over.store(true, Ordering::SeqCst);
                drain(&mut s).await;
                return Some((
                    Err(io::Error::other("upload limit exceeded")),
                    (s, seen, over),
                ));
            }
            Some((Ok(chunk), (s, seen, over)))
        },
    ))
}

async fn save<S, E>(mut s: S, path: &Path) -> io::Result<()>
where
    S: Stream<Item = Result<bytes::Bytes, E>> + Unpin,
    E: std::error::Error + Send + Sync + 'static,
{
    let mut f = tokio::fs::File::create(path).await?;
    while let Some(chunk) = s.next().await {
        f.write_all(&chunk.map_err(io::Error::other)?).await?;
    }
    f.flush().await?;
    Ok(())
}

async fn read_request(
    headers: &HeaderMap,
    body: Body,
    dir: &Path,
    limit: u64,
    multipart: bool,
) -> Result<Parsed, ReadError> {
    let declared = headers
        .get(header::CONTENT_LENGTH)
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.trim().parse::<u64>().ok());
    if declared.is_some_and(|d| d > limit) {
        drain(&mut body.into_data_stream()).await;
        return Err(ReadError::TooLarge);
    }
    let over = Arc::new(AtomicBool::new(false));
    let stream = limited(body, limit, over.clone());
    let classify = |e: String| {
        if over.load(Ordering::SeqCst) {
            ReadError::TooLarge
        } else {
            ReadError::Internal(e)
        }
    };
    let ctype = headers
        .get(header::CONTENT_TYPE)
        .map(latin1)
        .unwrap_or_default();
    let mut fields = HashMap::new();
    if !multipart || !ctype.starts_with("multipart/form-data") {
        let path = dir.join("upload");
        save(stream, &path)
            .await
            .map_err(|e| classify(e.to_string()))?;
        return Ok(Parsed {
            file: Some(path),
            fields,
        });
    }
    let boundary =
        multer::parse_boundary(&ctype).map_err(|e| ReadError::Internal(e.to_string()))?;
    let mut form = multer::Multipart::new(stream, boundary);
    let mut file = None;
    let mut n = 0;
    loop {
        let field = match form.next_field().await {
            Ok(Some(f)) => f,
            Ok(None) => break,
            Err(e) => return Err(classify(e.to_string())),
        };
        let Some(name) = field.name().map(str::to_string) else {
            continue;
        };
        if field.file_name().is_some() {
            let path = dir.join(format!("part-{n}"));
            n += 1;
            save(field, &path)
                .await
                .map_err(|e| classify(e.to_string()))?;
            if name == "file" {
                file = Some(path);
            } else {
                fields.insert(name, Field::File(path));
            }
        } else {
            let text = field.text().await.map_err(|e| classify(e.to_string()))?;
            if name == "file" {
                return Err(ReadError::Internal("the file field must be a file".into()));
            }
            fields.insert(name, Field::Text(text));
        }
    }
    Ok(Parsed { file, fields })
}

fn latin1(v: &HeaderValue) -> String {
    v.as_bytes().iter().map(|&b| b as char).collect()
}

pub fn ct_eq(a: &[u8], b: &[u8]) -> bool {
    use sha2::{Digest, Sha256};
    let (a, b) = (Sha256::digest(a), Sha256::digest(b));
    a.iter()
        .zip(b.iter())
        .fold(0u8, |acc, (x, y)| acc | (x ^ y))
        == 0
}

pub fn bearer_ok(headers: &HeaderMap, token: &str, allow_x_api: bool) -> bool {
    let auth = headers.get(header::AUTHORIZATION).map(latin1);
    let provided = match auth.as_deref().and_then(|h| h.strip_prefix("Bearer ")) {
        Some(t) => t.to_string(),
        None if allow_x_api => headers.get("x-api-token").map(latin1).unwrap_or_default(),
        None => String::new(),
    };
    ct_eq(provided.as_bytes(), token.as_bytes())
}

fn authorized(app: &App, headers: &HeaderMap) -> bool {
    app.cfg.api_token.is_empty() || bearer_ok(headers, &app.cfg.api_token, true)
}

pub fn sweep_temp(root: &Path, older_than: Duration) {
    let Ok(entries) = std::fs::read_dir(root) else {
        return;
    };
    let mut removed = 0;
    for e in entries.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        let ours = name.len() == 11
            && name.starts_with("kura-")
            && name[5..].bytes().all(|b| b.is_ascii_alphanumeric());
        let stale = e
            .metadata()
            .ok()
            .filter(|m| m.is_dir())
            .and_then(|m| m.modified().ok())
            .and_then(|t| t.elapsed().ok())
            .is_some_and(|age| age > older_than);
        if ours && stale && std::fs::remove_dir_all(e.path()).is_ok() {
            removed += 1;
        }
    }
    if removed > 0 {
        tracing::info!(removed, "removed job folders left by an earlier run");
    }
}

pub fn too_large(limit: u64) -> Response {
    fail_extra(
        StatusCode::PAYLOAD_TOO_LARGE,
        "TOO_LARGE",
        &format!("the upload limit is {limit} bytes"),
        "maxBytes",
        json!(limit),
    )
}

fn unexpected() -> Response {
    fail(
        StatusCode::INTERNAL_SERVER_ERROR,
        "INTERNAL",
        "unexpected server error",
    )
}

async fn handle_job(app: Arc<App>, req: Request, kind: Kind, q: Query) -> Response {
    let Some(_permit) = app.acquire().await else {
        return fail(
            StatusCode::TOO_MANY_REQUESTS,
            "BUSY",
            "queue full, retry later",
        );
    };
    let dir = match tempfile::Builder::new()
        .prefix("kura-")
        .tempdir_in(&app.tmp_root)
    {
        Ok(d) => d,
        Err(e) => {
            tracing::error!(error = %e, "cannot create a job directory");
            return unexpected();
        }
    };
    match job(&app, req, kind, &q, dir.path()).await {
        Ok(r) => r,
        Err(r) => *r,
    }
}

async fn job(
    app: &App,
    req: Request,
    kind: Kind,
    q: &Query,
    dir: &Path,
) -> Result<Response, Box<Response>> {
    let (parts, body) = req.into_parts();
    let limit = app.cfg.max_upload;
    let parsed = match read_request(&parts.headers, body, dir, limit, kind != Kind::Verify).await {
        Ok(p) => p,
        Err(ReadError::TooLarge) => return Err(too_large(limit).into()),
        Err(ReadError::Internal(e)) => {
            tracing::error!(error = %e, "could not read the request");
            return Err(unexpected().into());
        }
    };
    let password = parts.headers.get("x-password").map(latin1);
    let args = if kind == Kind::Verify {
        let upload = parsed.file.ok_or_else(unexpected)?;
        tokio::fs::rename(&upload, dir.join("in.pdf"))
            .await
            .map_err(|_| unexpected())?;
        let mut args = vec![Arg::Lit("--verify-password".into())];
        if let Some(p) = password.filter(|p| !p.is_empty()) {
            args.push(Arg::Lit("--password".into()));
            args.push(Arg::Lit(p));
        }
        args.push(Arg::File("in.pdf".into()));
        args
    } else {
        let Some(upload) = parsed.file else {
            return Err(fail(
                StatusCode::BAD_REQUEST,
                "BAD_REQUEST",
                "send the document as the request body or as the \"file\" field",
            )
            .into());
        };
        let Some(ext) = sniff_file(&upload).await.map_err(|_| unexpected())? else {
            return Err(fail(
                StatusCode::BAD_REQUEST,
                "NOT_A_PDF",
                "the body is neither a PDF nor a JPEG",
            )
            .into());
        };
        let mut args = build_args(q, password, &parsed.fields, dir, kind, &app.cfg.profiles)
            .map_err(|e| e.response())?;
        let name = format!("in.{ext}");
        tokio::fs::rename(&upload, dir.join(&name))
            .await
            .map_err(|_| unexpected())?;
        args.push(Arg::File(name));
        if kind == Kind::Convert {
            args.push(Arg::Out("out.pdf".into()));
        }
        args
    };
    if let Err(e) = validate_job(kind, &args, Some(dir), &app.cfg.profiles) {
        return Err(fail(StatusCode::BAD_REQUEST, "BAD_REQUEST", &e).into());
    }
    let dedupe = is_true(q.get("dedupe"));
    let name = format!(
        "api:{}",
        serde_json::to_value(kind)
            .ok()
            .and_then(|v| v.as_str().map(str::to_string))
            .unwrap_or_default()
    );
    let ran = match app.run(dir, kind, args, &name, dedupe).await {
        Ok(r) => r,
        Err(ExecError::Busy) => {
            return Err(fail(
                StatusCode::TOO_MANY_REQUESTS,
                "BUSY",
                "queue full, retry later",
            )
            .into())
        }
        Err(ExecError::Failed(e)) => {
            tracing::error!(error = %e, "job could not run");
            return Err(unexpected().into());
        }
    };
    let res = respond(app, kind, &ran, q.get("report") == Some("json")).await;
    if let Some((c, id)) = &ran.release {
        tokio::task::block_in_place(|| c.release(*id));
    }
    res.map_err(|e| {
        tracing::error!(error = %e, "could not build the response");
        Box::new(unexpected())
    })
}

async fn serve_static(app: &App, file: &str, ctype: &'static str) -> Response {
    match tokio::fs::read(app.cfg.site.join(file)).await {
        Ok(buf) => Response::builder()
            .status(StatusCode::OK)
            .header(header::CONTENT_TYPE, ctype)
            .header(header::CONTENT_LENGTH, buf.len())
            .header(header::CACHE_CONTROL, "no-cache")
            .header("x-content-type-options", "nosniff")
            .body(Body::from(buf))
            .expect("valid response"),
        Err(_) => fail(StatusCode::NOT_FOUND, "NOT_FOUND", "no such file"),
    }
}

pub fn normalize(path: &str) -> String {
    let segs: Vec<&str> = path.split('/').skip(1).collect();
    let mut out: Vec<&str> = Vec::new();
    for (i, s) in segs.iter().enumerate() {
        let last = i + 1 == segs.len();
        let l = s.to_ascii_lowercase();
        if l == "." || l == "%2e" {
            if last {
                out.push("");
            }
        } else if l == ".." || l == ".%2e" || l == "%2e." || l == "%2e%2e" {
            out.pop();
            if last {
                out.push("");
            }
        } else {
            out.push(s);
        }
    }
    format!("/{}", out.join("/"))
}

pub async fn handle(State(app): State<Arc<App>>, req: Request) -> Response {
    let path = normalize(req.uri().path());
    let q = Query::parse(req.uri().query().unwrap_or(""));
    let method = req.method().clone();
    let coord = match &app.exec {
        Exec::Queue(c) => Some(c.clone()),
        _ => None,
    };
    if method == Method::GET || method == Method::HEAD {
        let p = path.as_str();
        if p == "/" {
            return serve_static(&app, "index.html", "text/html; charset=utf-8").await;
        }
        if p == "/preflight" {
            return serve_static(&app, "preflight.html", "text/html; charset=utf-8").await;
        }
        if let Some((_, file, ctype)) = STATIC.iter().find(|(k, _, _)| *k == p) {
            return serve_static(&app, file, ctype).await;
        }
        if p == "/healthz" {
            let (active, queued) = app.load();
            let v =
                json!({"ok": true, "version": app.version(), "active": active, "queued": queued});
            return json_response(StatusCode::OK, jsjson::to_string(&v));
        }
        if p == "/api/profiles" {
            return json_response(StatusCode::OK, app.profiles_json.clone());
        }
        if p == "/docs" || p.starts_with("/docs/") {
            let rest = p.strip_prefix("/docs").unwrap_or("");
            let rest = rest.strip_prefix('/').unwrap_or(rest);
            return Response::builder()
                .status(StatusCode::FOUND)
                .header(header::LOCATION, header_value(&format!("{DOCS_URL}{rest}")))
                .body(Body::from_stream(futures_util::stream::empty::<
                    Result<bytes::Bytes, io::Error>,
                >()))
                .expect("valid response");
        }
        if let Some(c) = &coord {
            if p == "/status" {
                return crate::coord::status_page();
            }
            if p == "/status.json" {
                if !authorized(&app, req.headers()) {
                    return fail(
                        StatusCode::UNAUTHORIZED,
                        "UNAUTHORIZED",
                        "a valid API token is required",
                    );
                }
                return json_response(StatusCode::OK, c.status_json(&app.version()));
            }
        }
    }
    if let Some(c) = &coord {
        if path.starts_with("/internal/") {
            return crate::coord::internal(c.clone(), &app, req, &path).await;
        }
    }
    if method == Method::POST && path.starts_with("/api/") {
        if !authorized(&app, req.headers()) {
            return fail(
                StatusCode::UNAUTHORIZED,
                "UNAUTHORIZED",
                "a valid API token is required",
            );
        }
        match path.as_str() {
            "/api/convert" => return handle_job(app, req, Kind::Convert, q).await,
            "/api/check" => return handle_job(app, req, Kind::Check, q).await,
            "/api/verify-password" => return handle_job(app, req, Kind::Verify, q).await,
            _ => {}
        }
    }
    fail(StatusCode::NOT_FOUND, "NOT_FOUND", "no such route")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn paths_normalize_like_url() {
        assert_eq!(normalize("/docs/../index.html"), "/index.html");
        assert_eq!(normalize("/a/%2E%2e/b"), "/b");
        assert_eq!(normalize("/../../etc/passwd"), "/etc/passwd");
        assert_eq!(normalize("/x/."), "/x/");
        assert_eq!(normalize("/"), "/");
    }

    #[test]
    fn sniffs_like_the_old_server() {
        assert_eq!(sniff(b"\xff\xd8\xff\xe0"), Some("jpg"));
        assert_eq!(sniff(b"junk%PDF-1.7"), Some("pdf"));
        let mut late = vec![b' '; 1021];
        late.extend_from_slice(b"%PDF");
        assert_eq!(sniff(&late), None);
        assert_eq!(sniff(b""), None);
    }

    #[test]
    fn args_follow_the_old_order_and_rules() {
        let dir = tempfile::tempdir().unwrap();
        let q = Query::parse("level=2b&ua=on&lang=en-US&rasterDpi=&outlineFonts=yes");
        let mut fields = HashMap::new();
        fields.insert("analyze".into(), Field::Text("1".into()));
        fields.insert("xml".into(), Field::Text("<x/>".into()));
        let a = build_args(
            &q,
            Some(String::new()),
            &fields,
            dir.path(),
            Kind::Check,
            Path::new("/nope"),
        )
        .ok()
        .unwrap();
        let lit = |s: &str| Arg::Lit(s.into());
        assert_eq!(
            a,
            vec![
                lit("--check"),
                lit("--level"),
                lit("2b"),
                lit("--ua"),
                lit("--analyze"),
                lit("--lang"),
                lit("en-US"),
                lit("--attach-xml"),
                Arg::File("attachment.xml".into())
            ]
        );
        let bad = build_args(
            &Query::parse("level=x4p"),
            None,
            &HashMap::new(),
            dir.path(),
            Kind::Convert,
            Path::new("/"),
        );
        assert_eq!(bad.err().unwrap().1, "BAD_LEVEL");
        let bad = build_args(
            &Query::parse("level=2b&lang=a%0Ab"),
            None,
            &HashMap::new(),
            dir.path(),
            Kind::Convert,
            Path::new("/"),
        );
        assert_eq!(bad.err().unwrap().2, "invalid value for lang");
        let bad = build_args(
            &Query::parse("level=2b&profile=../x"),
            None,
            &HashMap::new(),
            dir.path(),
            Kind::Convert,
            Path::new("/"),
        );
        assert_eq!(bad.err().unwrap().1, "BAD_PROFILE");
    }

    #[test]
    fn engine_failures_map_to_statuses() {
        let r = |code, out: &str, err: &str| RunResult {
            code,
            out: out.into(),
            err: err.into(),
            timed_out: false,
        };
        assert_eq!(
            engine_failure(&r(Some(3), "", "")).0,
            StatusCode::GATEWAY_TIMEOUT
        );
        let (s, v) = engine_failure(&r(
            Some(2),
            "x\n{\"file\":\"a\",\"ok\":false,\"errorCode\":null,\"suggestedLevel\":\"2b\"}\n",
            "",
        ));
        assert_eq!(s, StatusCode::UNPROCESSABLE_ENTITY);
        assert_eq!(
            jsjson::to_string(&v),
            r#"{"ok":false,"errorCode":"REJECTED","suggestedLevel":"2b","error":"input rejected"}"#
        );
        let (s, v) = engine_failure(&r(Some(64), "", "\n  \nbad flag\nmore"));
        assert_eq!(
            (s, v["error"].as_str()),
            (StatusCode::BAD_REQUEST, Some("bad flag"))
        );
        assert_eq!(
            engine_failure(&r(None, "", "")).0,
            StatusCode::INTERNAL_SERVER_ERROR
        );
    }

    fn job_dir() -> tempfile::TempDir {
        let d = tempfile::tempdir().unwrap();
        for n in [
            "in.pdf",
            "in.jpg",
            "profile.json",
            "dest.icc",
            "attachment.xml",
        ] {
            std::fs::write(d.path().join(n), b"x").unwrap();
        }
        d
    }

    fn lits(v: &[&str]) -> Vec<Arg> {
        v.iter().map(|s| Arg::Lit(s.to_string())).collect()
    }

    fn convert(extra: &[Arg]) -> Vec<Arg> {
        let mut a = lits(&["--level", "2b"]);
        a.extend_from_slice(extra);
        a.push(Arg::File("in.pdf".into()));
        a.push(Arg::Out("out.pdf".into()));
        a
    }

    #[test]
    fn allow_list_accepts_what_the_api_builds() {
        let d = job_dir();
        let profiles = Path::new(env!("CARGO_MANIFEST_DIR")).join("../pdfa-engine/profiles");
        let q = Query::parse("level=x4&ua=1&lang=en-US&rasterDpi=150&imageMaxPpi=72.5&vtRecords=1-3,4-6&facturxProfile=BASIC WL&outputCondition=FOGRA39&profile=press/book-text-pages-check-and-fix");
        let mut fields = HashMap::new();
        fields.insert("destProfile".into(), Field::Text("icc".into()));
        let mut args = build_args(
            &q,
            Some("pw".into()),
            &fields,
            d.path(),
            Kind::Convert,
            &profiles,
        )
        .ok()
        .unwrap();
        args.push(Arg::File("in.pdf".into()));
        args.push(Arg::Out("out.pdf".into()));
        assert_eq!(
            validate_job(Kind::Convert, &args, Some(d.path()), &profiles),
            Ok(())
        );
        let verify = vec![
            Arg::Lit("--verify-password".into()),
            Arg::Lit("--password".into()),
            Arg::Lit("p".into()),
            Arg::File("in.pdf".into()),
        ];
        assert_eq!(
            validate_job(Kind::Verify, &verify, Some(d.path()), &profiles),
            Ok(())
        );
    }

    #[test]
    fn allow_list_rejects_everything_else() {
        let d = job_dir();
        let p = Path::new(env!("CARGO_MANIFEST_DIR")).join("../pdfa-engine/profiles");
        let bad: Vec<(Kind, Vec<Arg>)> = vec![
            (Kind::Convert, convert(&lits(&["--font-folder", "/etc"]))),
            (Kind::Convert, convert(&lits(&["--batch"]))),
            (Kind::Convert, convert(&lits(&["--watch"]))),
            (Kind::Convert, convert(&lits(&["--output", "/tmp/x"]))),
            (Kind::Convert, convert(&lits(&["--report", "/tmp/r.json"]))),
            (
                Kind::Convert,
                convert(&lits(&["--dest-profile", "/etc/passwd"])),
            ),
            (
                Kind::Convert,
                convert(&[
                    Arg::Lit("--dest-profile".into()),
                    Arg::File("../dest.icc".into()),
                ]),
            ),
            (Kind::Convert, convert(&lits(&["--profile", "/etc/passwd"]))),
            (
                Kind::Convert,
                convert(&[
                    Arg::Lit("--profile".into()),
                    Arg::Profile("../../etc/passwd".into()),
                ]),
            ),
            (
                Kind::Convert,
                convert(&[
                    Arg::Lit("--profile".into()),
                    Arg::Profile("press/../../x".into()),
                ]),
            ),
            (
                Kind::Convert,
                convert(&[
                    Arg::Lit("--profile".into()),
                    Arg::Profile("press/no-such".into()),
                ]),
            ),
            (
                Kind::Convert,
                convert(&[Arg::Lit("--profile".into()), Arg::File("other.json".into())]),
            ),
            (Kind::Convert, convert(&lits(&["--lang", &"a".repeat(513)]))),
            (Kind::Convert, convert(&lits(&["--lang", "a\nb"]))),
            (Kind::Convert, convert(&lits(&["--raster-dpi", "1e9"]))),
            (Kind::Convert, convert(&lits(&["--raster-dpi", "150; rm"]))),
            (Kind::Convert, convert(&lits(&["--image-max-ppi", "-5"]))),
            (Kind::Convert, convert(&lits(&["--vt-records", "../x"]))),
            (
                Kind::Convert,
                convert(&lits(&["--facturx-profile", "/etc/x"])),
            ),
            (Kind::Convert, convert(&lits(&["--password", "a\0b"]))),
            (Kind::Convert, convert(&lits(&["--ua", "--ua"]))),
            (Kind::Convert, convert(&lits(&["--lang"]))),
            (
                Kind::Convert,
                vec![
                    Arg::Lit("--level".into()),
                    Arg::Lit("x4p".into()),
                    Arg::File("in.pdf".into()),
                    Arg::Out("out.pdf".into()),
                ],
            ),
            (
                Kind::Convert,
                vec![
                    Arg::Lit("--level".into()),
                    Arg::Lit("2b".into()),
                    Arg::Lit("/etc/passwd".into()),
                    Arg::Out("out.pdf".into()),
                ],
            ),
            (
                Kind::Convert,
                vec![
                    Arg::Lit("--level".into()),
                    Arg::Lit("2b".into()),
                    Arg::File("../in.pdf".into()),
                    Arg::Out("out.pdf".into()),
                ],
            ),
            (
                Kind::Convert,
                vec![
                    Arg::Lit("--level".into()),
                    Arg::Lit("2b".into()),
                    Arg::File("in.pdf".into()),
                    Arg::Out("/tmp/out.pdf".into()),
                ],
            ),
            (
                Kind::Convert,
                vec![
                    Arg::Lit("--level".into()),
                    Arg::Lit("2b".into()),
                    Arg::File("in.pdf".into()),
                ],
            ),
            (Kind::Convert, {
                let mut a = convert(&[]);
                a.push(Arg::Lit("extra".into()));
                a
            }),
            (Kind::Check, convert(&[])),
            (
                Kind::Check,
                vec![
                    Arg::Lit("--check".into()),
                    Arg::Lit("--level".into()),
                    Arg::Lit("2b".into()),
                    Arg::File("in.pdf".into()),
                    Arg::Out("out.pdf".into()),
                ],
            ),
            (
                Kind::Verify,
                vec![
                    Arg::Lit("--verify-password".into()),
                    Arg::Lit("--batch".into()),
                    Arg::File("in.pdf".into()),
                ],
            ),
            (
                Kind::Verify,
                vec![
                    Arg::Lit("--verify-password".into()),
                    Arg::File("in.jpg".into()),
                ],
            ),
            (Kind::Convert, vec![]),
        ];
        for (kind, args) in bad {
            assert!(
                validate_job(kind, &args, Some(d.path()), &p).is_err(),
                "{kind:?} {args:?} was accepted"
            );
        }
        let missing = tempfile::tempdir().unwrap();
        assert_eq!(
            validate_job(Kind::Convert, &convert(&[]), Some(missing.path()), &p),
            Err("the job is missing in.pdf".into())
        );
    }

    #[test]
    fn odd_values_are_refused_at_the_api() {
        let d = tempfile::tempdir().unwrap();
        for q in [
            "level=2b&rasterDpi=abc",
            "level=2b&imageMaxPpi=1.2.3",
            "level=2b&vtRecords=1;2",
            "level=2b&facturxProfile=../../x",
        ] {
            let r = build_args(
                &Query::parse(q),
                None,
                &HashMap::new(),
                d.path(),
                Kind::Convert,
                Path::new("/"),
            );
            assert_eq!(r.err().map(|e| e.1), Some("BAD_REQUEST"), "{q}");
        }
        assert!(ct_eq(b"secret", b"secret"));
        assert!(!ct_eq(b"secret", b"secreT"));
        assert!(!ct_eq(b"secret", b"secret1"));
    }
}
