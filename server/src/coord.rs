use crate::api::{self, App, ExecError, Field, Kind, Outcome, Query, Submit};
use crate::exec::{self, Arg, RunResult};
use crate::jsjson;
use crate::wire;
use axum::body::Body;
use axum::extract::Request;
use axum::http::{header, Method, StatusCode};
use axum::response::Response;
use rusqlite::{params, Connection, OptionalExtension};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, RwLock};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tokio::sync::Notify;

pub struct HotFolder {
    pub root: PathBuf,
    pub query: String,
    pub kind: Kind,
    pub dedupe: bool,
}

pub struct Settings {
    pub max_attempts: i64,
    pub lease_ms: i64,
    pub ttl_ms: i64,
    pub queue_limit: usize,
    pub timeout_ms: u64,
    pub max_upload: u64,
    pub profiles: PathBuf,
}

pub struct Coord {
    db: Mutex<Connection>,
    jobs: PathBuf,
    pub tmp: PathBuf,
    queued: Notify,
    finished: Notify,
    set: Settings,
    version: Arc<RwLock<String>>,
    pub folders: Vec<HotFolder>,
}

#[derive(Serialize, Deserialize)]
pub struct LeaseMeta {
    pub id: i64,
    pub attempt: i64,
    pub kind: Kind,
    pub args: Vec<Arg>,
    pub timeout_ms: u64,
}

#[derive(Serialize, Deserialize)]
pub struct Beat {
    pub id: String,
    pub name: String,
    pub slots: i64,
    pub running: Vec<i64>,
    pub version: String,
}

fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

fn kind_str(k: Kind) -> &'static str {
    match k {
        Kind::Convert => "convert",
        Kind::Check => "check",
        Kind::Verify => "verify",
    }
}

fn kind_of(s: &str) -> Kind {
    match s {
        "check" => Kind::Check,
        "verify" => Kind::Verify,
        _ => Kind::Convert,
    }
}

fn finished_state(kind: Kind, r: &RunResult, dir: &Path) -> &'static str {
    match api::outcome(kind, r, dir) {
        Outcome::Pdf(..) | Outcome::Json(StatusCode::OK, _) => "done",
        _ => "failed",
    }
}

fn found<T>(r: rusqlite::Result<T>) -> Option<T> {
    r.optional().ok().flatten()
}

const SCHEMA: &str = "
PRAGMA journal_mode=WAL;
PRAGMA synchronous=FULL;
CREATE TABLE IF NOT EXISTS jobs(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  kind TEXT NOT NULL,
  args TEXT NOT NULL,
  name TEXT NOT NULL,
  folder TEXT,
  file TEXT,
  hash TEXT,
  state TEXT NOT NULL,
  attempts INTEGER NOT NULL DEFAULT 0,
  worker TEXT,
  lease_until INTEGER,
  result TEXT,
  delivered INTEGER NOT NULL DEFAULT 0,
  purged INTEGER NOT NULL DEFAULT 0,
  created INTEGER NOT NULL,
  updated INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS jobs_state ON jobs(state, id);
CREATE INDEX IF NOT EXISTS jobs_hash ON jobs(hash);
CREATE INDEX IF NOT EXISTS jobs_origin ON jobs(folder, file, delivered);
CREATE TABLE IF NOT EXISTS workers(
  id TEXT PRIMARY KEY, name TEXT, slots INTEGER, running INTEGER, version TEXT, last_seen INTEGER);
";

pub fn parse_hot_folder(spec: &str, profiles: &Path) -> Result<HotFolder, String> {
    let (root, query) = spec.split_once('?').unwrap_or((spec, ""));
    let q = Query::parse(query);
    let kind = match q.get("mode") {
        None | Some("convert") => Kind::Convert,
        Some("check") => Kind::Check,
        Some(m) => {
            return Err(format!(
                "hot folder {root}: mode must be convert or check, not {m}"
            ))
        }
    };
    let probe = tempfile::tempdir().map_err(|e| e.to_string())?;
    if let Err(e) = api::build_args(&q, None, &HashMap::new(), probe.path(), kind, profiles) {
        return Err(format!("hot folder {root}: {}", e.2));
    }
    let root = std::path::absolute(root).map_err(|e| e.to_string())?;
    for sub in ["input", "done", "failed"] {
        std::fs::create_dir_all(root.join(sub))
            .map_err(|e| format!("{}: {e}", root.join(sub).display()))?;
    }
    Ok(HotFolder {
        root,
        query: query.to_string(),
        kind,
        dedupe: matches!(q.get("dedupe"), Some("true" | "1" | "on")),
    })
}

impl Coord {
    pub fn open(
        data: &Path,
        set: Settings,
        version: Arc<RwLock<String>>,
        folders: Vec<HotFolder>,
    ) -> Result<Arc<Self>, String> {
        let jobs = data.join("jobs");
        let tmp = data.join("tmp");
        let _ = std::fs::remove_dir_all(&tmp);
        for d in [data, &jobs, &tmp] {
            std::fs::create_dir_all(d).map_err(|e| format!("{}: {e}", d.display()))?;
        }
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(data, std::fs::Permissions::from_mode(0o700));
        }
        let db = Connection::open(data.join("queue.sqlite")).map_err(|e| e.to_string())?;
        db.execute_batch(SCHEMA).map_err(|e| e.to_string())?;
        db.busy_timeout(Duration::from_secs(5))
            .map_err(|e| e.to_string())?;
        let c = Arc::new(Coord {
            db: Mutex::new(db),
            jobs,
            tmp,
            queued: Notify::new(),
            finished: Notify::new(),
            set,
            version,
            folders,
        });
        c.deliver_pending();
        Ok(c)
    }

    fn db(&self) -> std::sync::MutexGuard<'_, Connection> {
        self.db.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn dir(&self, id: i64) -> PathBuf {
        wire::job_dir(&self.jobs, id)
    }

    fn ids(&self, sql: &str, p: impl rusqlite::Params) -> Vec<i64> {
        let db = self.db();
        let Ok(mut st) = db.prepare(sql) else {
            return vec![];
        };
        st.query_map(p, |r| r.get(0))
            .map(|rows| rows.flatten().collect())
            .unwrap_or_default()
    }

    pub fn load(&self) -> (usize, usize) {
        let db = self.db();
        let count = |s: &str| {
            db.query_row("SELECT COUNT(*) FROM jobs WHERE state=?1", [s], |r| {
                r.get::<_, i64>(0)
            })
            .unwrap_or(0) as usize
        };
        (count("running"), count("queued"))
    }

    pub fn submit(
        &self,
        src: &Path,
        kind: Kind,
        args: &[Arg],
        name: &str,
        hot: Option<(&str, &str)>,
        dedupe: bool,
    ) -> Result<i64, ExecError> {
        let failed = |e: String| ExecError::Failed(e);
        let hash = if dedupe {
            Some(hash_job(kind, args, src).map_err(|e| failed(e.to_string()))?)
        } else {
            None
        };
        let mut db = self.db();
        if hot.is_none() {
            let queued: i64 = db
                .query_row(
                    "SELECT COUNT(*) FROM jobs WHERE state='queued' AND folder IS NULL",
                    [],
                    |r| r.get(0),
                )
                .map_err(|e| failed(e.to_string()))?;
            if queued as usize >= self.set.queue_limit {
                return Err(ExecError::Busy);
            }
        }
        let now = now_ms();
        let tx = db.transaction().map_err(|e| failed(e.to_string()))?;
        tx.execute(
            "INSERT INTO jobs(kind,args,name,folder,file,hash,state,created,updated) VALUES(?1,?2,?3,?4,?5,?6,'queued',?7,?7)",
            params![kind_str(kind), serde_json::to_string(args).unwrap_or_default(), name, hot.map(|h| h.0), hot.map(|h| h.1), hash, now],
        )
        .map_err(|e| failed(e.to_string()))?;
        let id = tx.last_insert_rowid();
        let dest = self.dir(id);
        std::fs::rename(src, &dest).map_err(|e| failed(e.to_string()))?;
        let mut cached = false;
        if let Some(h) = &hash {
            let prev: Option<(i64, String)> = found(tx.query_row(
                "SELECT id, result FROM jobs WHERE hash=?1 AND state='done' AND purged=0 AND id<>?2 ORDER BY id DESC LIMIT 1",
                params![h, id],
                |r| Ok((r.get(0)?, r.get(1)?)),
            ));
            if let Some((prev, result)) = prev {
                let from = self.dir(prev);
                let copied = exec::output_files(args).iter().all(|n| {
                    let (a, b) = (from.join(n), dest.join(n));
                    std::fs::hard_link(&a, &b)
                        .or_else(|_| std::fs::copy(&a, &b).map(|_| ()))
                        .is_ok()
                });
                if copied {
                    tx.execute(
                        "UPDATE jobs SET state='done', result=?1, args='[]', worker='dedupe', updated=?2 WHERE id=?3",
                        params![result, now, id],
                    )
                    .map_err(|e| failed(e.to_string()))?;
                    cached = true;
                }
            }
        }
        if let Err(e) = tx.commit() {
            let _ = std::fs::remove_dir_all(&dest);
            return Err(failed(e.to_string()));
        }
        drop(db);
        tracing::info!(job = id, name, dedupe_hit = cached, "job queued");
        if cached {
            self.finished.notify_waiters();
            if hot.is_some() {
                self.deliver(id);
            }
        } else {
            self.queued.notify_waiters();
        }
        Ok(id)
    }

    fn try_finished(&self, id: i64) -> Option<Option<RunResult>> {
        let row: Option<(String, Option<String>)> = found(self.db().query_row(
            "SELECT state, result FROM jobs WHERE id=?1",
            [id],
            |r| Ok((r.get(0)?, r.get(1)?)),
        ));
        match row {
            None => Some(None),
            Some((s, r)) if s == "done" || s == "failed" => Some(Some(
                r.and_then(|r| serde_json::from_str(&r).ok())
                    .unwrap_or_else(|| RunResult::lost("no result".into())),
            )),
            _ => None,
        }
    }

    pub async fn wait_for(&self, id: i64, max: Duration) -> Option<Option<RunResult>> {
        let deadline = tokio::time::Instant::now() + max;
        loop {
            let notified = self.finished.notified();
            tokio::pin!(notified);
            notified.as_mut().enable();
            if let Some(r) = self.try_finished(id) {
                return Some(r);
            }
            if tokio::time::Instant::now() >= deadline {
                return None;
            }
            let _ = tokio::time::timeout_at(
                deadline.min(tokio::time::Instant::now() + Duration::from_secs(5)),
                notified,
            )
            .await;
        }
    }

    pub async fn wait(&self, id: i64) -> (RunResult, PathBuf) {
        loop {
            if let Some(r) = self.wait_for(id, Duration::from_secs(3600)).await {
                return (
                    r.unwrap_or_else(|| RunResult::lost("job vanished".into())),
                    self.dir(id),
                );
            }
        }
    }

    pub fn release(&self, id: i64) {
        let db = self.db();
        let keep: bool = db
            .query_row("SELECT hash IS NOT NULL FROM jobs WHERE id=?1", [id], |r| {
                r.get(0)
            })
            .unwrap_or(false);
        if !keep {
            let _ = std::fs::remove_dir_all(self.dir(id));
            let _ = db.execute("UPDATE jobs SET purged=1, delivered=1 WHERE id=?1", [id]);
        } else {
            let _ = db.execute("UPDATE jobs SET delivered=1 WHERE id=?1", [id]);
        }
    }

    fn lease(&self, worker: &str) -> Option<LeaseMeta> {
        let now = now_ms();
        let row: Option<(i64, i64, String, String)> = found(self.db().query_row(
            "UPDATE jobs SET state='running', attempts=attempts+1, worker=?1, lease_until=?2, updated=?3
             WHERE id=(SELECT id FROM jobs WHERE state='queued' ORDER BY id LIMIT 1)
             RETURNING id, attempts, kind, args",
            params![worker, now + self.set.lease_ms, now],
            |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)),
        ));
        let (id, attempt, kind, args) = row?;
        tracing::info!(job = id, attempt, worker, "job leased");
        Some(LeaseMeta {
            id,
            attempt,
            kind: kind_of(&kind),
            args: serde_json::from_str(&args).unwrap_or_default(),
            timeout_ms: self.set.timeout_ms,
        })
    }

    async fn lease_wait(&self, worker: &str, max: Duration) -> Option<LeaseMeta> {
        let deadline = tokio::time::Instant::now() + max;
        loop {
            let notified = self.queued.notified();
            tokio::pin!(notified);
            notified.as_mut().enable();
            if let Some(m) = self.lease(worker) {
                return Some(m);
            }
            if tokio::time::timeout_at(deadline, notified).await.is_err() {
                return None;
            }
        }
    }

    fn heartbeat(&self, b: &Beat) {
        let now = now_ms();
        let db = self.db();
        let _ = db.execute(
            "INSERT INTO workers(id,name,slots,running,version,last_seen) VALUES(?1,?2,?3,?4,?5,?6)
             ON CONFLICT(id) DO UPDATE SET name=?2, slots=?3, running=?4, version=?5, last_seen=?6",
            params![
                b.id,
                b.name,
                b.slots,
                b.running.len() as i64,
                b.version,
                now
            ],
        );
        for id in &b.running {
            let _ = db.execute(
                "UPDATE jobs SET lease_until=?1 WHERE id=?2 AND worker=?3 AND state='running'",
                params![now + self.set.lease_ms, id, b.id],
            );
        }
        if let Ok(mut v) = self.version.write() {
            if v.is_empty() {
                v.clone_from(&b.version);
            }
        }
    }

    fn holds(&self, id: i64, attempt: i64, worker: &str) -> bool {
        holds(&self.db(), id, attempt, worker)
    }

    fn finish(
        &self,
        id: i64,
        result: &RunResult,
        staged: Option<(&Path, &[String])>,
        attempt: Option<(i64, &str)>,
    ) -> bool {
        let dir = self.dir(id);
        let (ok, folder, state) = {
            let mut db = self.db();
            let Ok(tx) = db.transaction() else {
                return false;
            };
            let row: Option<(String, Option<String>, String)> = found(tx.query_row(
                "SELECT kind, folder, state FROM jobs WHERE id=?1",
                [id],
                |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
            ));
            let Some((kind, folder, state)) = row else {
                return false;
            };
            if state == "done"
                || state == "failed"
                || attempt.is_some_and(|(a, w)| !holds(&tx, id, a, w))
            {
                return false;
            }
            if let Some((from, names)) = staged {
                if names
                    .iter()
                    .any(|n| std::fs::rename(from.join(n), dir.join(n)).is_err())
                {
                    return false;
                }
            }
            let state = finished_state(kind_of(&kind), result, &dir);
            let updated = tx.execute(
                "UPDATE jobs SET state=?1, result=?2, args='[]', lease_until=NULL, updated=?3 WHERE id=?4",
                params![state, serde_json::to_string(result).unwrap_or_default(), now_ms(), id],
            );
            (updated.is_ok() && tx.commit().is_ok(), folder, state)
        };
        if ok {
            tracing::info!(job = id, state, code = ?result.code, "job finished");
            self.finished.notify_waiters();
            if folder.is_some() {
                self.deliver(id);
            }
        }
        ok
    }

    pub fn reap(&self) {
        let now = now_ms();
        let expired: Vec<(i64, i64, Option<String>)> = {
            let db = self.db();
            let Ok(mut st) = db.prepare(
                "SELECT id, attempts, worker FROM jobs WHERE state='running' AND lease_until<?1",
            ) else {
                return;
            };
            st.query_map([now], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)))
                .map(|rows| rows.flatten().collect())
                .unwrap_or_default()
        };
        for (id, attempts, worker) in expired {
            if attempts >= self.set.max_attempts {
                tracing::warn!(job = id, attempts, worker = ?worker, "lease expired, attempts exhausted");
                self.finish(
                    id,
                    &RunResult::lost(format!("worker lost {attempts} time(s)")),
                    None,
                    None,
                );
            } else {
                tracing::warn!(job = id, attempts, worker = ?worker, "lease expired, requeued");
                let _ = self.db().execute(
                    "UPDATE jobs SET state='queued', worker=NULL, lease_until=NULL, updated=?1 WHERE id=?2 AND state='running'",
                    params![now, id],
                );
                self.queued.notify_waiters();
            }
        }
    }

    pub fn janitor(&self) {
        let cutoff = now_ms() - self.set.ttl_ms;
        let old = self.ids(
            "SELECT id FROM jobs WHERE state IN ('done','failed') AND purged=0 AND updated<?1 AND (folder IS NULL OR delivered=1)",
            [cutoff],
        );
        for id in old {
            let _ = std::fs::remove_dir_all(self.dir(id));
            let _ = self
                .db()
                .execute("UPDATE jobs SET purged=1 WHERE id=?1", [id]);
        }
        let week = now_ms() - 7 * 24 * 3600 * 1000;
        let db = self.db();
        let _ = db.execute("DELETE FROM jobs WHERE purged=1 AND updated<?1", [week]);
        let _ = db.execute("DELETE FROM workers WHERE last_seen<?1", [week]);
    }

    fn deliver_pending(&self) {
        for id in self.ids("SELECT id FROM jobs WHERE state IN ('done','failed') AND folder IS NOT NULL AND delivered=0", []) {
            self.deliver(id);
        }
    }

    fn deliver(&self, id: i64) {
        let row: Option<(String, String, String, Option<String>)> = found(self.db().query_row(
            "SELECT kind, folder, file, result FROM jobs WHERE id=?1 AND folder IS NOT NULL",
            [id],
            |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)),
        ));
        let Some((kind, folder, file, result)) = row else {
            return;
        };
        let result: RunResult = result
            .and_then(|r| serde_json::from_str(&r).ok())
            .unwrap_or_else(|| RunResult::lost("no result".into()));
        let root = PathBuf::from(folder);
        if let Err(e) = place(
            &root,
            &file,
            api::outcome(kind_of(&kind), &result, &self.dir(id)),
        ) {
            tracing::error!(job = id, file, error = %e, "could not deliver to the hot folder");
            let body = json!({"ok": false, "errorCode": "INTERNAL", "error": e.to_string()});
            if let Err(e) = place(
                &root,
                &file,
                Outcome::Json(StatusCode::INTERNAL_SERVER_ERROR, body),
            ) {
                tracing::error!(job = id, file, error = %e, "hot folder delivery failed");
                return;
            }
        }
        let _ = self
            .db()
            .execute("UPDATE jobs SET delivered=1 WHERE id=?1", [id]);
        tracing::info!(job = id, file, "delivered to hot folder");
    }

    fn in_flight(&self, folder: &str, file: &str) -> bool {
        found(self.db().query_row(
            "SELECT 1 FROM jobs WHERE folder=?1 AND file=?2 AND delivered=0",
            params![folder, file],
            |_| Ok(()),
        ))
        .is_some()
    }

    pub fn scan(&self, seen: &mut HashMap<PathBuf, (u64, SystemTime)>) {
        for f in &self.folders {
            let Ok(entries) = std::fs::read_dir(f.root.join("input")) else {
                continue;
            };
            for e in entries.flatten() {
                let Some(name) = e.file_name().to_str().map(str::to_string) else {
                    continue;
                };
                let Ok(meta) = e.path().symlink_metadata() else {
                    continue;
                };
                if name.starts_with('.') || !meta.is_file() {
                    continue;
                }
                let sig = (meta.len(), meta.modified().unwrap_or(UNIX_EPOCH));
                if seen.insert(e.path(), sig) != Some(sig) {
                    continue;
                }
                let folder = f.root.to_string_lossy().into_owned();
                if self.in_flight(&folder, &name) {
                    continue;
                }
                seen.remove(&e.path());
                if let Err(e) = self.enqueue_hot(f, &folder, &name) {
                    tracing::error!(file = name, error = %e, "could not queue hot folder file");
                }
            }
        }
        seen.retain(|p, _| p.exists());
    }

    fn enqueue_hot(&self, f: &HotFolder, folder: &str, name: &str) -> io::Result<()> {
        let tmp = tempfile::Builder::new()
            .prefix("hot-")
            .tempdir_in(&self.tmp)?;
        let upload = tmp.path().join("upload");
        std::fs::copy(f.root.join("input").join(name), &upload)?;
        let mut head = Vec::with_capacity(1024);
        io::Read::read_to_end(
            &mut io::Read::take(std::fs::File::open(&upload)?, 1024),
            &mut head,
        )?;
        let Some(ext) = api::sniff(&head) else {
            let body = json!({"ok": false, "errorCode": "NOT_A_PDF", "error": "the body is neither a PDF nor a JPEG"});
            return place(&f.root, name, Outcome::Json(StatusCode::BAD_REQUEST, body));
        };
        let fields: HashMap<String, Field> = HashMap::new();
        let mut args = match api::build_args(
            &Query::parse(&f.query),
            None,
            &fields,
            tmp.path(),
            f.kind,
            &self.set.profiles,
        ) {
            Ok(a) => a,
            Err(e) => return place(&f.root, name, Outcome::Json(e.0, e.body())),
        };
        let src = format!("in.{ext}");
        std::fs::rename(&upload, tmp.path().join(&src))?;
        args.push(Arg::File(src));
        if f.kind == Kind::Convert {
            args.push(Arg::Out("out.pdf".into()));
        }
        if let Err(e) = api::validate_job(f.kind, &args, Some(tmp.path()), &self.set.profiles) {
            let body = json!({"ok": false, "errorCode": "BAD_REQUEST", "error": e});
            return place(&f.root, name, Outcome::Json(StatusCode::BAD_REQUEST, body));
        }
        match self.submit(
            tmp.path(),
            f.kind,
            &args,
            name,
            Some((folder, name)),
            f.dedupe,
        ) {
            Ok(_) | Err(ExecError::Busy) => Ok(()),
            Err(ExecError::Failed(e)) => Err(io::Error::other(e)),
        }
    }

    pub fn status_json(&self, version: &str) -> String {
        let db = self.db();
        let now = now_ms();
        let mut m = serde_json::Map::new();
        m.insert("ok".into(), json!(true));
        m.insert("version".into(), json!(version));
        for s in ["queued", "running", "done", "failed"] {
            let n: i64 = db
                .query_row("SELECT COUNT(*) FROM jobs WHERE state=?1", [s], |r| {
                    r.get(0)
                })
                .unwrap_or(0);
            m.insert(s.into(), json!(n));
        }
        let alive_ms = self.set.lease_ms;
        let workers: Vec<Value> = db
            .prepare("SELECT id, name, slots, running, version, last_seen FROM workers ORDER BY id")
            .and_then(|mut st| {
                st.query_map([], |r| {
                    let last: i64 = r.get(5)?;
                    Ok(json!({
                        "id": r.get::<_, String>(0)?, "name": r.get::<_, String>(1)?, "slots": r.get::<_, i64>(2)?,
                        "running": r.get::<_, i64>(3)?, "version": r.get::<_, String>(4)?,
                        "lastSeenMs": now - last, "alive": now - last < alive_ms
                    }))
                })
                .map(|rows| rows.flatten().collect())
            })
            .unwrap_or_default();
        let jobs: Vec<Value> = db
            .prepare("SELECT id, name, kind, state, attempts, worker, folder IS NOT NULL, created, updated, result FROM jobs ORDER BY id DESC LIMIT 200")
            .and_then(|mut st| {
                st.query_map([], |r| {
                    let result: Option<String> = r.get(9)?;
                    let code = result.and_then(|s| serde_json::from_str::<RunResult>(&s).ok()).map(|r| json!(r.code)).unwrap_or(Value::Null);
                    Ok(json!({
                        "id": r.get::<_, i64>(0)?, "name": r.get::<_, String>(1)?, "kind": r.get::<_, String>(2)?,
                        "state": r.get::<_, String>(3)?, "attempts": r.get::<_, i64>(4)?, "worker": r.get::<_, Option<String>>(5)?,
                        "origin": if r.get::<_, bool>(6)? { "hot-folder" } else { "api" },
                        "created": r.get::<_, i64>(7)?, "updated": r.get::<_, i64>(8)?, "exitCode": code
                    }))
                })
                .map(|rows| rows.flatten().collect())
            })
            .unwrap_or_default();
        m.insert("workers".into(), Value::Array(workers));
        m.insert(
            "folders".into(),
            json!(self
                .folders
                .iter()
                .map(|f| f.root.to_string_lossy())
                .collect::<Vec<_>>()),
        );
        m.insert("jobs".into(), Value::Array(jobs));
        jsjson::to_string(&Value::Object(m))
    }
}

fn holds(db: &Connection, id: i64, attempt: i64, worker: &str) -> bool {
    found(db.query_row(
        "SELECT 1 FROM jobs WHERE id=?1 AND attempts=?2 AND worker=?3 AND state='running'",
        params![id, attempt, worker],
        |_| Ok(()),
    ))
    .is_some()
}

fn hash_job(kind: Kind, args: &[Arg], dir: &Path) -> io::Result<String> {
    let mut h = Sha256::new();
    h.update(kind_str(kind));
    h.update(serde_json::to_vec(args).map_err(io::Error::other)?);
    for n in exec::input_files(args) {
        h.update(n.as_bytes());
        io::copy(&mut std::fs::File::open(dir.join(&n))?, &mut h)?;
    }
    Ok(format!("{:x}", h.finalize()))
}

fn atomic_write(dest: &Path, write: impl FnOnce(&Path) -> io::Result<()>) -> io::Result<()> {
    let name = dest
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    let part = dest.with_file_name(format!(".{name}.part"));
    write(&part)?;
    std::fs::rename(&part, dest)
}

fn ignore_missing(r: io::Result<()>) -> io::Result<()> {
    match r {
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(()),
        other => other,
    }
}

fn place(root: &Path, file: &str, outcome: Outcome) -> io::Result<()> {
    let stem = Path::new(file)
        .file_stem()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_else(|| file.to_string());
    let input = root.join("input").join(file);
    match outcome {
        Outcome::Pdf(report, pdf) => {
            let done = root.join("done");
            atomic_write(&done.join(format!("{stem}.pdf")), |p| {
                std::fs::copy(&pdf, p).map(|_| ())
            })?;
            let text = jsjson::to_string(&Value::Object(report));
            atomic_write(&done.join(format!("{stem}.json")), |p| {
                std::fs::write(p, text)
            })?;
            ignore_missing(std::fs::remove_file(input))
        }
        Outcome::Json(status, v) => {
            let to = root.join(if status == StatusCode::OK {
                "done"
            } else {
                "failed"
            });
            let text = jsjson::to_string(&v);
            atomic_write(&to.join(format!("{stem}.json")), |p| {
                std::fs::write(p, text)
            })?;
            ignore_missing(std::fs::rename(input, to.join(file)))
        }
    }
}

pub fn spawn_background(c: Arc<Coord>) {
    let r = c.clone();
    tokio::spawn(async move {
        loop {
            tokio::time::sleep(Duration::from_millis(500)).await;
            let r = r.clone();
            let _ = tokio::task::spawn_blocking(move || r.reap()).await;
        }
    });
    let j = c.clone();
    tokio::spawn(async move {
        loop {
            tokio::time::sleep(Duration::from_secs(60)).await;
            let j = j.clone();
            let _ = tokio::task::spawn_blocking(move || j.janitor()).await;
        }
    });
    if !c.folders.is_empty() {
        std::thread::spawn(move || {
            let mut seen = HashMap::new();
            loop {
                c.scan(&mut seen);
                std::thread::sleep(Duration::from_millis(1000));
            }
        });
    }
}

fn plain(status: StatusCode, body: &'static str) -> Response {
    Response::builder()
        .status(status)
        .body(Body::from(body))
        .expect("valid response")
}

fn id_of(path: &str, suffix: &str) -> Option<i64> {
    path.strip_prefix("/internal/jobs/")?
        .strip_suffix(suffix)?
        .parse()
        .ok()
}

pub async fn internal(c: Arc<Coord>, app: &App, req: Request, path: &str) -> Response {
    if !api::bearer_ok(req.headers(), &app.cfg.cluster_token, false) {
        return plain(StatusCode::UNAUTHORIZED, "cluster token required");
    }
    let worker = req
        .headers()
        .get("x-kura-worker")
        .and_then(|v| v.to_str().ok())
        .unwrap_or("")
        .to_string();
    let method = req.method().clone();
    if method == Method::POST && path == "/internal/lease" {
        if worker.is_empty() {
            return plain(StatusCode::BAD_REQUEST, "x-kura-worker required");
        }
        let Some(meta) = c.lease_wait(&worker, Duration::from_secs(25)).await else {
            return plain(StatusCode::NO_CONTENT, "");
        };
        return match wire::encode(&meta, &c.dir(meta.id), &exec::input_files(&meta.args)).await {
            Ok(body) => Response::builder()
                .status(StatusCode::OK)
                .body(body)
                .expect("valid response"),
            Err(e) => {
                tracing::error!(job = meta.id, error = %e, "job files unreadable");
                c.finish(
                    meta.id,
                    &RunResult::lost(format!("job files unreadable: {e}")),
                    None,
                    None,
                );
                plain(StatusCode::NO_CONTENT, "")
            }
        };
    }
    if method == Method::POST && path == "/internal/heartbeat" {
        let Ok(bytes) = http_body_util::BodyExt::collect(req.into_body())
            .await
            .map(|b| b.to_bytes())
        else {
            return plain(StatusCode::BAD_REQUEST, "bad body");
        };
        let Ok(beat) = serde_json::from_slice::<Beat>(&bytes) else {
            return plain(StatusCode::BAD_REQUEST, "bad heartbeat");
        };
        let c2 = c.clone();
        let _ = tokio::task::spawn_blocking(move || c2.heartbeat(&beat)).await;
        return api::json_response(
            StatusCode::OK,
            json!({"heartbeatMs": (c.set.lease_ms / 3).max(200)}).to_string(),
        );
    }
    if method == Method::POST {
        if let Some(id) = id_of(path, "/complete") {
            let attempt: i64 = Query::parse(req.uri().query().unwrap_or(""))
                .get("attempt")
                .and_then(|a| a.parse().ok())
                .unwrap_or(-1);
            if !c.holds(id, attempt, &worker) {
                return plain(StatusCode::CONFLICT, "stale attempt");
            }
            let Ok(staging) = tempfile::Builder::new()
                .prefix(".in-")
                .tempdir_in(c.dir(id))
            else {
                return plain(StatusCode::CONFLICT, "job gone");
            };
            let (result, names) = match wire::decode::<RunResult, _, _>(
                req.into_body().into_data_stream(),
                staging.path(),
                u64::MAX,
            )
            .await
            {
                Ok(x) => x,
                Err(e) => {
                    tracing::warn!(job = id, error = %e, "result upload failed");
                    return plain(StatusCode::BAD_REQUEST, "bad result bundle");
                }
            };
            let staged = staging.path().to_path_buf();
            let c2 = c.clone();
            let ok = tokio::task::spawn_blocking(move || {
                c2.finish(
                    id,
                    &result,
                    Some((&staged, &names)),
                    Some((attempt, &worker)),
                )
            })
            .await
            .unwrap_or(false);
            return if ok {
                plain(StatusCode::OK, "")
            } else {
                plain(StatusCode::CONFLICT, "stale attempt")
            };
        }
        if let Some(id) = id_of(path, "/ack") {
            tokio::task::block_in_place(|| c.release(id));
            return plain(StatusCode::OK, "");
        }
        if path == "/internal/submit" {
            let Ok(tmp) = tempfile::Builder::new().prefix("sub-").tempdir_in(&c.tmp) else {
                return plain(StatusCode::INTERNAL_SERVER_ERROR, "no space");
            };
            let Ok((meta, _)) = wire::decode::<Submit, _, _>(
                req.into_body().into_data_stream(),
                tmp.path(),
                c.set.max_upload,
            )
            .await
            else {
                return plain(StatusCode::BAD_REQUEST, "bad job bundle");
            };
            if let Err(e) =
                api::validate_job(meta.kind, &meta.args, Some(tmp.path()), &c.set.profiles)
            {
                tracing::warn!(error = %e, "rejected submitted job");
                return api::fail(
                    StatusCode::BAD_REQUEST,
                    "BAD_REQUEST",
                    &format!("invalid job: {e}"),
                );
            }
            let submitted = tokio::task::block_in_place(|| {
                c.submit(
                    tmp.path(),
                    meta.kind,
                    &meta.args,
                    &meta.name,
                    None,
                    meta.dedupe,
                )
            });
            return match submitted {
                Ok(id) => api::json_response(StatusCode::OK, json!({"id": id}).to_string()),
                Err(ExecError::Busy) => plain(StatusCode::TOO_MANY_REQUESTS, "busy"),
                Err(ExecError::Failed(e)) => {
                    tracing::error!(error = e, "submit failed");
                    plain(StatusCode::INTERNAL_SERVER_ERROR, "submit failed")
                }
            };
        }
    }
    if method == Method::GET {
        if let Some(id) = id_of(path, "/result") {
            let Some(done) = c.wait_for(id, Duration::from_secs(25)).await else {
                return plain(StatusCode::NO_CONTENT, "");
            };
            let Some(result) = done else {
                return plain(StatusCode::NOT_FOUND, "no such job");
            };
            let dir = c.dir(id);
            let files: Vec<String> = ["out.pdf"]
                .iter()
                .filter(|n| dir.join(n).exists())
                .map(|n| n.to_string())
                .collect();
            return match wire::encode(&result, &dir, &files).await {
                Ok(body) if dir.exists() => Response::builder()
                    .status(StatusCode::OK)
                    .body(body)
                    .expect("valid response"),
                _ => plain(StatusCode::NOT_FOUND, "result expired"),
            };
        }
    }
    plain(StatusCode::NOT_FOUND, "no such route")
}

pub fn status_page() -> Response {
    Response::builder()
        .header(header::CONTENT_TYPE, "text/html; charset=utf-8")
        .header(header::CACHE_CONTROL, "no-cache")
        .header("x-content-type-options", "nosniff")
        .body(Body::from(include_str!("status.html")))
        .expect("valid response")
}
