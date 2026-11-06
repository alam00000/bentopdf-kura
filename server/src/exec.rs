use serde::{Deserialize, Serialize};
use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::time::Duration;
use tokio::io::AsyncReadExt;
use tokio::process::Command;

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(tag = "t", content = "v")]
pub enum Arg {
    Lit(String),
    File(String),
    Out(String),
    Profile(String),
}

#[derive(Serialize, Deserialize, Clone, Debug, Default)]
pub struct RunResult {
    pub code: Option<i32>,
    pub out: String,
    pub err: String,
    pub timed_out: bool,
}

impl RunResult {
    pub fn lost(err: String) -> Self {
        RunResult {
            code: Some(-1),
            out: String::new(),
            err,
            timed_out: false,
        }
    }
}

pub fn profile_path(profiles: &Path, id: &str) -> PathBuf {
    profiles.join(format!("{id}.json"))
}

pub fn resolve(args: &[Arg], dir: &Path, profiles: &Path) -> Vec<OsString> {
    args.iter()
        .map(|a| match a {
            Arg::Lit(s) => OsString::from(s),
            Arg::File(n) | Arg::Out(n) => dir.join(n).into_os_string(),
            Arg::Profile(id) => profile_path(profiles, id).into_os_string(),
        })
        .collect()
}

pub fn input_files(args: &[Arg]) -> Vec<String> {
    args.iter()
        .filter_map(|a| {
            if let Arg::File(n) = a {
                Some(n.clone())
            } else {
                None
            }
        })
        .collect()
}

pub fn output_files(args: &[Arg]) -> Vec<String> {
    args.iter()
        .filter_map(|a| {
            if let Arg::Out(n) = a {
                Some(n.clone())
            } else {
                None
            }
        })
        .collect()
}

pub fn safe_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 100
        && !name.starts_with('.')
        && name
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'_' || b == b'-')
}

const OUT_CAP: u64 = 256 * 1024 * 1024;
const ERR_CAP: u64 = 65536;

pub async fn run_kura(bin: &Path, args: Vec<OsString>, cwd: &Path, timeout_ms: u64) -> RunResult {
    let spawned = Command::new(bin)
        .args(args)
        .current_dir(cwd)
        .env("PDFA_TIMEOUT", timeout_ms.div_ceil(1000).to_string())
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true)
        .spawn();
    let mut child = match spawned {
        Ok(c) => c,
        Err(e) => return RunResult::lost(e.to_string()),
    };
    let mut stdout = child.stdout.take().expect("piped stdout");
    let mut stderr = child.stderr.take().expect("piped stderr");
    let read_out = async {
        let mut buf = Vec::new();
        let _ = (&mut stdout).take(OUT_CAP).read_to_end(&mut buf).await;
        let _ = tokio::io::copy(&mut stdout, &mut tokio::io::sink()).await;
        buf
    };
    let read_err = async {
        let mut buf = Vec::new();
        let _ = (&mut stderr).take(ERR_CAP).read_to_end(&mut buf).await;
        let _ = tokio::io::copy(&mut stderr, &mut tokio::io::sink()).await;
        buf
    };
    let limit = Duration::from_millis(timeout_ms + 5000);
    let run = async { tokio::join!(child.wait(), read_out, read_err) };
    match tokio::time::timeout(limit, run).await {
        Ok((status, out, err)) => RunResult {
            code: status.ok().and_then(|s| s.code()),
            out: String::from_utf8_lossy(&out).into_owned(),
            err: String::from_utf8_lossy(&err).into_owned(),
            timed_out: false,
        },
        Err(_) => {
            let _ = child.start_kill();
            let _ = child.wait().await;
            RunResult {
                code: None,
                out: String::new(),
                err: String::new(),
                timed_out: true,
            }
        }
    }
}

pub fn engine_version(bin: &Path) -> Result<String, String> {
    let out = std::process::Command::new(bin)
        .arg("--version")
        .stdin(Stdio::null())
        .output()
        .map_err(|e| e.to_string())?;
    if !out.status.success() {
        return Err(format!("exit {}", out.status));
    }
    Ok(String::from_utf8_lossy(&out.stdout).trim().to_string())
}
