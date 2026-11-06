use crate::api::{self, Config, Kind};
use crate::coord::{Beat, LeaseMeta};
use crate::exec::{self, RunResult};
use crate::wire::{self, Peer};
use axum::body::Body;
use hyper::{Method, StatusCode};
use std::collections::HashSet;
use std::fs::File;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio::sync::watch;

pub struct WorkRoot {
    pub dir: PathBuf,
    _lock: File,
}

impl Drop for WorkRoot {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

pub fn sweep(base: &Path, older_than: Duration) -> usize {
    let Ok(entries) = std::fs::read_dir(base) else {
        return 0;
    };
    let mut removed = 0;
    for e in entries.flatten() {
        let path = e.path();
        let named = e.file_name().to_string_lossy().starts_with("w-");
        let Ok(meta) = std::fs::symlink_metadata(&path) else {
            continue;
        };
        if !named || !meta.is_dir() {
            continue;
        }
        let abandoned = match File::open(path.join(".lock")) {
            Ok(f) => f.try_lock().is_ok(),
            Err(_) => true,
        };
        let old = meta
            .modified()
            .ok()
            .and_then(|t| t.elapsed().ok())
            .is_some_and(|age| age > older_than);
        if abandoned && old && std::fs::remove_dir_all(&path).is_ok() {
            removed += 1;
        }
    }
    removed
}

pub fn claim(base: &Path, older_than: Duration) -> std::io::Result<WorkRoot> {
    std::fs::create_dir_all(base)?;
    let removed = sweep(base, older_than);
    if removed > 0 {
        tracing::info!(removed, base = %base.display(), "removed job folders left by a crashed worker");
    }
    let dir = tempfile::Builder::new()
        .prefix("w-")
        .tempdir_in(base)?
        .keep();
    let lock = File::create(dir.join(".lock"))?;
    lock.try_lock().map_err(std::io::Error::other)?;
    Ok(WorkRoot { dir, _lock: lock })
}

pub async fn run_checked(cfg: &Config, meta: &LeaseMeta, dir: &Path) -> RunResult {
    match api::validate_job(meta.kind, &meta.args, Some(dir), &cfg.profiles) {
        Ok(()) => {
            let argv = exec::resolve(&meta.args, dir, &cfg.profiles);
            exec::run_kura(
                &cfg.kura_bin,
                argv,
                dir,
                meta.timeout_ms.min(24 * 3600 * 1000),
            )
            .await
        }
        Err(e) => {
            tracing::warn!(job = meta.id, error = %e, "refused a job outside the allow-list");
            RunResult {
                code: Some(64),
                out: String::new(),
                err: format!("invalid job: {e}"),
                timed_out: false,
            }
        }
    }
}

struct Worker {
    cfg: Config,
    peer: Peer,
    id: String,
    root: WorkRoot,
    running: Mutex<HashSet<i64>>,
    beat_ms: AtomicU64,
}

fn env_ms(name: &str, dflt: u64) -> u64 {
    std::env::var(name)
        .ok()
        .and_then(|v| v.trim().parse().ok())
        .unwrap_or(dflt)
}

pub async fn run(
    cfg: Config,
    peer: Peer,
    url: String,
    slots: usize,
    name: String,
    stop: watch::Receiver<bool>,
) -> Result<(), String> {
    let version = exec::engine_version(&cfg.kura_bin)
        .map_err(|e| format!("cannot run the engine at {}: {e}", cfg.kura_bin.display()))?;
    let base = std::env::var_os("KURA_WORK_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| std::env::temp_dir().join("kura-worker"));
    let lease = Duration::from_millis(env_ms("KURA_LEASE_MS", 30000));
    let root =
        claim(&base, lease).map_err(|e| format!("cannot prepare {}: {e}", base.display()))?;
    let started = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis())
        .unwrap_or(0);
    let id = format!("{name}-{}-{started}", std::process::id());
    let w = Arc::new(Worker {
        peer,
        cfg,
        id: id.clone(),
        root,
        running: Mutex::new(HashSet::new()),
        beat_ms: AtomicU64::new(5000),
    });
    tracing::info!(worker = id, coordinator = url, slots, %version, "worker started");
    let beat = {
        let w = w.clone();
        let mut stop = stop.clone();
        tokio::spawn(async move {
            loop {
                w.heartbeat(&name, slots, &version).await;
                let wait = Duration::from_millis(w.beat_ms.load(Ordering::Relaxed));
                tokio::select! {
                    _ = tokio::time::sleep(wait) => {}
                    _ = stop.changed() => if *stop.borrow() && w.running.lock().map(|r| r.is_empty()).unwrap_or(true) { break },
                }
            }
        })
    };
    let mut handles = Vec::new();
    for _ in 0..slots {
        let w = w.clone();
        let stop = stop.clone();
        handles.push(tokio::spawn(async move { w.slot(stop).await }));
    }
    for h in handles {
        let _ = h.await;
    }
    beat.abort();
    let _ = beat.await;
    tracing::info!(worker = id, "worker stopped");
    Ok(())
}

impl Worker {
    async fn heartbeat(&self, name: &str, slots: usize, version: &str) {
        let running: Vec<i64> = self
            .running
            .lock()
            .map(|r| r.iter().copied().collect())
            .unwrap_or_default();
        let beat = Beat {
            id: self.id.clone(),
            name: name.to_string(),
            slots: slots as i64,
            running,
            version: version.to_string(),
        };
        match self.peer.json("/internal/heartbeat", &beat).await {
            Ok((StatusCode::OK, body)) => {
                if let Some(ms) = serde_json::from_slice::<serde_json::Value>(&body)
                    .ok()
                    .and_then(|v| v["heartbeatMs"].as_u64())
                {
                    self.beat_ms.store(ms.clamp(100, 5000), Ordering::Relaxed);
                }
            }
            Ok((status, _)) => tracing::warn!(%status, "heartbeat rejected"),
            Err(e) => tracing::warn!(error = %e, "heartbeat failed"),
        }
    }

    async fn slot(&self, mut stop: watch::Receiver<bool>) {
        let mut backoff = Duration::from_millis(500);
        while !*stop.borrow() {
            let headers = [("x-kura-worker", self.id.clone())];
            let lease = self.peer.send(
                Method::POST,
                "/internal/lease",
                &headers,
                Body::empty(),
                Duration::from_secs(40),
            );
            let res = tokio::select! {
                r = lease => r,
                _ = stop.changed() => continue,
            };
            match res {
                Ok(r) if r.status() == StatusCode::OK => {
                    backoff = Duration::from_millis(500);
                    self.run_one(r).await;
                }
                Ok(r) if r.status() == StatusCode::NO_CONTENT => {}
                Ok(r) => {
                    tracing::warn!(status = %r.status(), "lease refused");
                    tokio::time::sleep(backoff).await;
                    backoff = (backoff * 2).min(Duration::from_secs(10));
                }
                Err(e) => {
                    tracing::warn!(error = %e, "coordinator unreachable");
                    tokio::time::sleep(backoff).await;
                    backoff = (backoff * 2).min(Duration::from_secs(10));
                }
            }
        }
    }

    async fn run_one(&self, res: hyper::Response<hyper::body::Incoming>) {
        let dir = match tempfile::Builder::new()
            .prefix("job-")
            .tempdir_in(&self.root.dir)
        {
            Ok(d) => d,
            Err(e) => return tracing::error!(error = %e, "cannot create a job directory"),
        };
        let (meta, _) =
            match wire::decode::<LeaseMeta, _, _>(wire::body_stream(res), dir.path(), u64::MAX)
                .await
            {
                Ok(x) => x,
                Err(e) => {
                    return tracing::warn!(error = %e, "job download failed, the lease will expire")
                }
            };
        self.running.lock().map(|mut r| r.insert(meta.id)).ok();
        tracing::info!(job = meta.id, attempt = meta.attempt, "job started");
        let result = run_checked(&self.cfg, &meta, dir.path()).await;
        tracing::info!(job = meta.id, code = ?result.code, timed_out = result.timed_out, "job ran");
        self.upload(&meta, &result, dir.path()).await;
        self.running.lock().map(|mut r| r.remove(&meta.id)).ok();
    }

    async fn upload(&self, meta: &LeaseMeta, result: &RunResult, dir: &Path) {
        let outputs: Vec<String> = if meta.kind == Kind::Convert && result.code == Some(0) {
            exec::output_files(&meta.args)
                .into_iter()
                .filter(|n| exec::safe_name(n) && dir.join(n).is_file())
                .collect()
        } else {
            Vec::new()
        };
        let path = format!(
            "/internal/jobs/{}/complete?attempt={}",
            meta.id, meta.attempt
        );
        let deadline = tokio::time::Instant::now() + Duration::from_secs(600);
        while tokio::time::Instant::now() < deadline {
            let body = match wire::encode(result, dir, &outputs).await {
                Ok(b) => b,
                Err(e) => {
                    return tracing::error!(job = meta.id, error = %e, "cannot read the job output")
                }
            };
            let headers = [("x-kura-worker", self.id.clone())];
            match self
                .peer
                .send(
                    Method::POST,
                    &path,
                    &headers,
                    body,
                    Duration::from_secs(600),
                )
                .await
            {
                Ok(r) if r.status() == StatusCode::OK => {
                    return tracing::info!(job = meta.id, "result uploaded")
                }
                Ok(r)
                    if r.status() == StatusCode::CONFLICT
                        || r.status() == StatusCode::NOT_FOUND =>
                {
                    return tracing::warn!(
                        job = meta.id,
                        "result no longer wanted, the job was reassigned"
                    )
                }
                Ok(r) => {
                    tracing::warn!(job = meta.id, status = %r.status(), "result upload refused, retrying")
                }
                Err(e) => {
                    tracing::warn!(job = meta.id, error = %e, "result upload failed, retrying")
                }
            }
            tokio::time::sleep(Duration::from_secs(1)).await;
        }
        tracing::error!(job = meta.id, "gave up uploading the result");
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::exec::Arg;

    fn cfg() -> Config {
        Config {
            host: "127.0.0.1".into(),
            port: 0,
            max_upload: 1,
            concurrency: 1,
            queue_limit: 0,
            timeout_ms: 1000,
            api_token: String::new(),
            cluster_token: String::new(),
            kura_bin: PathBuf::from("/nonexistent/kura"),
            profiles: PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../pdfa-engine/profiles"),
            site: PathBuf::new(),
        }
    }

    #[tokio::test]
    async fn worker_refuses_jobs_outside_the_allow_list() {
        let d = tempfile::tempdir().unwrap();
        std::fs::write(d.path().join("in.pdf"), b"%PDF").unwrap();
        let args = vec![
            Arg::Lit("--level".into()),
            Arg::Lit("2b".into()),
            Arg::Lit("--font-folder".into()),
            Arg::Lit("/".into()),
            Arg::File("in.pdf".into()),
            Arg::Out("out.pdf".into()),
        ];
        let meta = LeaseMeta {
            id: 1,
            attempt: 1,
            kind: Kind::Convert,
            args,
            timeout_ms: 1000,
        };
        let r = run_checked(&cfg(), &meta, d.path()).await;
        assert_eq!(r.code, Some(64));
        assert!(r.err.contains("--font-folder is not allowed"));
        let (status, body) = api::engine_failure(&r);
        assert_eq!(
            (status, body["errorCode"].as_str()),
            (StatusCode::BAD_REQUEST, Some("BAD_REQUEST"))
        );
    }

    #[test]
    fn sweep_removes_only_abandoned_old_roots() {
        let base = tempfile::tempdir().unwrap();
        let live = claim(base.path(), Duration::ZERO).unwrap();
        std::fs::create_dir_all(live.dir.join("job-live")).unwrap();
        let dead = base.path().join("w-dead");
        std::fs::create_dir_all(dead.join("job-abc")).unwrap();
        File::create(dead.join(".lock")).unwrap();
        std::fs::create_dir_all(base.path().join("unrelated")).unwrap();
        std::thread::sleep(Duration::from_millis(20));
        assert_eq!(sweep(base.path(), Duration::from_secs(3600)), 0);
        assert_eq!(sweep(base.path(), Duration::ZERO), 1);
        assert!(!dead.exists());
        assert!(live.dir.join("job-live").exists());
        assert!(base.path().join("unrelated").exists());
        let dir = live.dir.clone();
        drop(live);
        assert!(!dir.exists());
    }
}
