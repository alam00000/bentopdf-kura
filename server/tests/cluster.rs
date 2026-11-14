use std::io::{Read, Write};
use std::net::TcpStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicU16, Ordering};
use std::time::{Duration, Instant};

const BIN: &str = env!("CARGO_BIN_EXE_kura-server");
const FAKE: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/compat/fake-kura.sh");
const TOKEN: &str = "cluster-secret";
static PORT: AtomicU16 = AtomicU16::new(0);

fn port() -> u16 {
    let _ = PORT.compare_exchange(
        0,
        21000 + (std::process::id() % 2000) as u16 * 4,
        Ordering::SeqCst,
        Ordering::SeqCst,
    );
    PORT.fetch_add(1, Ordering::SeqCst)
}

fn profiles() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../pdfa-engine/profiles")
}

struct Proc(Child);

impl Drop for Proc {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

fn spawn(args: &[&str], env: &[(&str, String)]) -> Proc {
    let mut cmd = Command::new(BIN);
    cmd.args(args)
        .env("KURA_BIN", FAKE)
        .env("KURA_PROFILES", profiles())
        .env("KURA_HOST", "127.0.0.1")
        .env("KURA_CLUSTER_TOKEN", TOKEN)
        .env("KURA_LOG_LEVEL", "warn")
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    for (k, v) in env {
        cmd.env(k, v);
    }
    Proc(cmd.spawn().expect("spawn kura-server"))
}

struct Reply {
    status: u16,
    headers: Vec<(String, String)>,
    body: Vec<u8>,
}

impl Reply {
    fn header(&self, k: &str) -> Option<&str> {
        self.headers
            .iter()
            .find(|(n, _)| n.eq_ignore_ascii_case(k))
            .map(|(_, v)| v.as_str())
    }

    fn json(&self) -> serde_json::Value {
        serde_json::from_slice(&self.body).unwrap_or(serde_json::Value::Null)
    }
}

fn http(
    port: u16,
    method: &str,
    path: &str,
    headers: &[(&str, &str)],
    body: &[u8],
) -> std::io::Result<Reply> {
    let mut s = TcpStream::connect(("127.0.0.1", port))?;
    s.set_read_timeout(Some(Duration::from_secs(60)))?;
    let mut req = format!(
        "{method} {path} HTTP/1.1\r\nhost: localhost\r\nconnection: close\r\ncontent-length: {}\r\n",
        body.len()
    );
    for (k, v) in headers {
        req.push_str(&format!("{k}: {v}\r\n"));
    }
    req.push_str("\r\n");
    s.write_all(req.as_bytes())?;
    s.write_all(body)?;
    let mut raw = Vec::new();
    s.read_to_end(&mut raw)?;
    let split = raw
        .windows(4)
        .position(|w| w == b"\r\n\r\n")
        .ok_or_else(|| std::io::Error::other("no header end"))?;
    let head = String::from_utf8_lossy(&raw[..split]).into_owned();
    let mut lines = head.split("\r\n");
    let status = lines
        .next()
        .and_then(|l| l.split(' ').nth(1))
        .and_then(|c| c.parse().ok())
        .unwrap_or(0);
    let headers: Vec<(String, String)> = lines
        .filter_map(|l| l.split_once(':'))
        .map(|(k, v)| (k.trim().to_lowercase(), v.trim().to_string()))
        .collect();
    let mut body = raw[split + 4..].to_vec();
    if headers
        .iter()
        .any(|(k, v)| k == "transfer-encoding" && v == "chunked")
    {
        let mut out = Vec::new();
        let mut rest = &body[..];
        while let Some(nl) = rest.windows(2).position(|w| w == b"\r\n") {
            let size =
                usize::from_str_radix(String::from_utf8_lossy(&rest[..nl]).trim(), 16).unwrap_or(0);
            if size == 0 {
                break;
            }
            out.extend_from_slice(&rest[nl + 2..nl + 2 + size]);
            rest = &rest[nl + 4 + size..];
        }
        body = out;
    }
    Ok(Reply {
        status,
        headers,
        body,
    })
}

fn wait_until(what: &str, max: Duration, mut f: impl FnMut() -> bool) {
    let start = Instant::now();
    while start.elapsed() < max {
        if f() {
            return;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    panic!("timed out waiting for {what}");
}

fn up(port: u16) {
    wait_until("server", Duration::from_secs(15), || {
        http(port, "GET", "/healthz", &[], b"")
            .map(|r| r.status == 200)
            .unwrap_or(false)
    });
}

fn status(port: u16) -> serde_json::Value {
    http(
        port,
        "GET",
        "/status.json",
        &[("authorization", "Bearer api")],
        b"",
    )
    .map(|r| r.json())
    .unwrap_or_default()
}

const PDF: &[u8] =
    b"%PDF-1.7\n1 0 obj\n<< /Type /Catalog >>\nendobj\ntrailer\n<< /Root 1 0 R >>\n%%EOF\n";

#[test]
fn serve_runs_jobs_locally() {
    let p = port();
    let _s = spawn(
        &["serve"],
        &[
            ("KURA_PORT", p.to_string()),
            ("KURA_SITE", "/nonexistent".into()),
        ],
    );
    up(p);
    let r = http(p, "POST", "/api/convert?level=2b", &[], PDF).unwrap();
    assert_eq!(r.status, 200);
    assert!(r.body.starts_with(b"%PDF-1.7 fake"));
    assert_eq!(r.header("x-kura-level"), Some(""));
    assert_eq!(r.header("x-kura-engine"), Some("Fake Engine (kura) 9.9.9"));
    let r = http(p, "POST", "/api/check?level=2b&lang=exit2", &[], PDF).unwrap();
    assert_eq!(
        (r.status, r.json()["errorCode"].as_str()),
        (422, Some("PASSWORD_REQUIRED"))
    );
    let r = http(p, "GET", "/", &[], b"").unwrap();
    assert_eq!(
        (r.status, r.json()["error"].as_str()),
        (404, Some("no such file"))
    );
    let r = http(p, "GET", "/status.json", &[], b"").unwrap();
    assert_eq!(r.status, 404);
    let r = http(p, "POST", "/internal/lease", &[], b"").unwrap();
    assert_eq!(r.status, 404);
}

#[test]
fn coordinator_workers_retry_restart_dedupe_and_hot_folders() {
    let root = tempfile::tempdir().unwrap();
    let data = root.path().join("data");
    let hot = root.path().join("hot");
    let cp = port();
    let coord_env = vec![
        ("KURA_PORT", cp.to_string()),
        ("KURA_DATA", data.to_string_lossy().into_owned()),
        ("KURA_LEASE_MS", "1500".into()),
        ("KURA_API_TOKEN", "api".into()),
        ("KURA_HOT_FOLDERS", format!("{}?level=2b", hot.display())),
    ];
    let mut coord = spawn(&["coordinator"], &coord_env);
    up(cp);
    let url = format!("http://127.0.0.1:{cp}");
    let worker = |name: &str| {
        spawn(
            &["worker", "--slots", "2", "--name", name],
            &[("KURA_COORDINATOR", url.clone())],
        )
    };

    let r = http(
        cp,
        "POST",
        "/internal/lease",
        &[("x-kura-worker", "x")],
        b"",
    )
    .unwrap();
    assert_eq!(r.status, 401);
    let r = http(cp, "GET", "/status.json", &[], b"").unwrap();
    assert_eq!(r.status, 401);
    let r = http(cp, "POST", "/api/convert?level=2b", &[], PDF).unwrap();
    assert_eq!(r.status, 401);

    let mut a = worker("a");
    let slow = std::thread::spawn(move || {
        http(
            cp,
            "POST",
            "/api/convert?level=2b&lang=slow",
            &[("authorization", "Bearer api")],
            PDF,
        )
        .unwrap()
    });
    wait_until(
        "worker a to run the slow job",
        Duration::from_secs(20),
        || {
            status(cp)["jobs"].as_array().is_some_and(|j| {
                j.iter().any(|j| {
                    j["state"] == "running"
                        && j["worker"].as_str().is_some_and(|w| w.starts_with("a-"))
                })
            })
        },
    );
    a.0.kill().unwrap();
    let _b = worker("b");
    let r = slow.join().unwrap();
    assert_eq!(r.status, 200, "{}", String::from_utf8_lossy(&r.body));
    assert!(r.body.starts_with(b"%PDF-1.7 fake"));
    let s = status(cp);
    let job = &s["jobs"][0];
    assert_eq!(
        (job["state"].as_str(), job["attempts"].as_i64()),
        (Some("done"), Some(2))
    );
    assert!(job["worker"].as_str().unwrap().starts_with("b-"));

    let first = http(
        cp,
        "POST",
        "/api/convert?level=2b&dedupe=true&report=json",
        &[("x-api-token", "api")],
        PDF,
    )
    .unwrap();
    let second = http(
        cp,
        "POST",
        "/api/convert?level=2b&dedupe=true&report=json",
        &[("x-api-token", "api")],
        PDF,
    )
    .unwrap();
    assert_eq!(first.status, 200);
    assert_eq!(first.body, second.body);
    assert_eq!(status(cp)["jobs"][0]["worker"], "dedupe");

    let r = http(
        cp,
        "POST",
        "/api/check?level=2b&lang=exit64",
        &[("x-api-token", "api")],
        PDF,
    )
    .unwrap();
    assert_eq!(
        (r.status, r.json()["error"].as_str()),
        (400, Some("unknown option --frob"))
    );

    let fp = port();
    let _front = spawn(
        &["serve", "--coordinator", &url],
        &[("KURA_PORT", fp.to_string())],
    );
    up(fp);
    let r = http(fp, "POST", "/api/convert?level=2b", &[], PDF).unwrap();
    assert_eq!(r.status, 200);
    assert!(r.body.starts_with(b"%PDF-1.7 fake"));
    let r = http(fp, "POST", "/api/convert?level=2b&lang=exit2", &[], PDF).unwrap();
    assert_eq!(r.status, 422);
    let health = http(fp, "GET", "/healthz", &[], b"").unwrap().json();
    assert_eq!(health["version"], "Fake Engine (kura) 9.9.9");

    drop(_b);
    for i in 0..3 {
        std::fs::write(hot.join("input").join(format!("doc{i}.pdf")), PDF).unwrap();
    }
    std::fs::write(hot.join("input").join("notes.txt"), b"hello").unwrap();
    wait_until("hot folder jobs queued", Duration::from_secs(20), || {
        status(cp)["queued"] == 3
    });
    coord.0.kill().unwrap();
    coord.0.wait().unwrap();
    let _coord2 = spawn(&["coordinator"], &coord_env);
    up(cp);
    assert_eq!(status(cp)["queued"], 3);
    let _c = worker("c");
    wait_until("hot folder output", Duration::from_secs(30), || {
        (0..3).all(|i| hot.join("done").join(format!("doc{i}.pdf")).exists())
    });
    for i in 0..3 {
        let report: serde_json::Value = serde_json::from_slice(
            &std::fs::read(hot.join("done").join(format!("doc{i}.json"))).unwrap(),
        )
        .unwrap();
        assert_eq!(report["ok"], true);
        assert!(!hot.join("input").join(format!("doc{i}.pdf")).exists());
    }
    assert!(hot.join("failed").join("notes.txt").exists());
    let failed: serde_json::Value =
        serde_json::from_slice(&std::fs::read(hot.join("failed").join("notes.json")).unwrap())
            .unwrap();
    assert_eq!(failed["errorCode"], "NOT_A_PDF");
    assert!(
        std::fs::read_dir(data.join("tmp"))
            .unwrap()
            .next()
            .is_none()
    );
}

#[test]
fn worker_kill_exhausts_attempts() {
    let root = tempfile::tempdir().unwrap();
    let cp = port();
    let _coord = spawn(
        &["coordinator", "--max-attempts", "1"],
        &[
            ("KURA_PORT", cp.to_string()),
            ("KURA_DATA", root.path().to_string_lossy().into_owned()),
            ("KURA_LEASE_MS", "1000".into()),
        ],
    );
    up(cp);
    let mut w = spawn(
        &["worker", "--name", "doomed"],
        &[("KURA_COORDINATOR", format!("http://127.0.0.1:{cp}"))],
    );
    let job = std::thread::spawn(move || {
        http(cp, "POST", "/api/convert?level=2b&lang=slow", &[], PDF).unwrap()
    });
    wait_until("job running", Duration::from_secs(20), || {
        status(cp)["running"] == 1
    });
    w.0.kill().unwrap();
    let r = job.join().unwrap();
    assert_eq!(
        (r.status, r.json()["errorCode"].as_str()),
        (500, Some("INTERNAL"))
    );
    assert_eq!(status(cp)["failed"], 1);
}

fn bundle(meta: serde_json::Value, files: &[(&str, &[u8])]) -> Vec<u8> {
    let entries: Vec<serde_json::Value> = files
        .iter()
        .map(|(n, b)| serde_json::json!({"name": n, "size": b.len()}))
        .collect();
    let head = serde_json::to_vec(&serde_json::json!({"meta": meta, "files": entries})).unwrap();
    let mut out = (head.len() as u32).to_be_bytes().to_vec();
    out.extend_from_slice(&head);
    for (_, b) in files {
        out.extend_from_slice(b);
    }
    out
}

#[test]
fn coordinator_rejects_jobs_outside_the_allow_list() {
    let root = tempfile::tempdir().unwrap();
    let cp = port();
    let _coord = spawn(
        &["coordinator"],
        &[
            ("KURA_PORT", cp.to_string()),
            ("KURA_DATA", root.path().to_string_lossy().into_owned()),
        ],
    );
    up(cp);
    let auth = format!("Bearer {TOKEN}");
    let lit = |s: &str| serde_json::json!({"t": "Lit", "v": s});
    let file = |s: &str| serde_json::json!({"t": "File", "v": s});
    let out = serde_json::json!({"t": "Out", "v": "out.pdf"});
    let attempts = [
        vec![
            lit("--level"),
            lit("2b"),
            lit("--font-folder"),
            lit("/etc"),
            file("in.pdf"),
            out.clone(),
        ],
        vec![
            lit("--level"),
            lit("2b"),
            lit("--profile"),
            serde_json::json!({"t": "Profile", "v": "../../../etc/passwd"}),
            file("in.pdf"),
            out.clone(),
        ],
        vec![
            lit("--level"),
            lit("2b"),
            lit("--dest-profile"),
            lit("/etc/passwd"),
            file("in.pdf"),
            out.clone(),
        ],
        vec![
            lit("--level"),
            lit("2b"),
            file("in.pdf"),
            serde_json::json!({"t": "Out", "v": "../../escape.pdf"}),
        ],
        vec![
            lit("--level"),
            lit("2b"),
            lit("--lang"),
            lit(&"x".repeat(600)),
            file("in.pdf"),
            out.clone(),
        ],
        vec![lit("--batch"), lit("/"), file("in.pdf"), out.clone()],
    ];
    for args in attempts {
        let meta =
            serde_json::json!({"kind": "convert", "args": args, "name": "evil", "dedupe": false});
        let r = http(
            cp,
            "POST",
            "/internal/submit",
            &[("authorization", &auth)],
            &bundle(meta, &[("in.pdf", PDF)]),
        )
        .unwrap();
        assert_eq!(r.status, 400, "{}", String::from_utf8_lossy(&r.body));
        assert_eq!(r.json()["errorCode"], "BAD_REQUEST");
    }
    let meta = serde_json::json!({"kind": "convert", "args": [lit("--level"), lit("2b"), file("in.pdf"), out], "name": "ok", "dedupe": false});
    let r = http(
        cp,
        "POST",
        "/internal/submit",
        &[("authorization", &auth)],
        &bundle(meta.clone(), &[("../in.pdf", PDF)]),
    )
    .unwrap();
    assert_eq!(r.status, 400);
    let r = http(
        cp,
        "POST",
        "/internal/submit",
        &[("authorization", &auth)],
        &bundle(meta, &[("in.pdf", PDF)]),
    )
    .unwrap();
    assert_eq!(r.status, 200);
    assert_eq!(status(cp)["queued"], 1);
}

#[test]
fn restarted_worker_sweeps_what_a_killed_one_left() {
    let root = tempfile::tempdir().unwrap();
    let work = root.path().join("work");
    let cp = port();
    let _coord = spawn(
        &["coordinator"],
        &[
            ("KURA_PORT", cp.to_string()),
            (
                "KURA_DATA",
                root.path().join("data").to_string_lossy().into_owned(),
            ),
            ("KURA_LEASE_MS", "1000".into()),
        ],
    );
    up(cp);
    let env = vec![
        ("KURA_COORDINATOR", format!("http://127.0.0.1:{cp}")),
        ("KURA_WORK_DIR", work.to_string_lossy().into_owned()),
        ("KURA_LEASE_MS", "1000".into()),
    ];
    let mut first = spawn(&["worker", "--name", "first"], &env);
    let job = std::thread::spawn(move || {
        http(cp, "POST", "/api/convert?level=2b&lang=slow", &[], PDF).unwrap()
    });
    wait_until("job running", Duration::from_secs(20), || {
        status(cp)["running"] == 1
    });
    first.0.kill().unwrap();
    first.0.wait().unwrap();
    let roots = |w: &Path| -> Vec<PathBuf> {
        std::fs::read_dir(w)
            .unwrap()
            .flatten()
            .map(|e| e.path())
            .collect()
    };
    let left = roots(&work);
    assert_eq!(left.len(), 1);
    assert!(
        std::fs::read_dir(&left[0])
            .unwrap()
            .flatten()
            .any(|e| e.file_name().to_string_lossy().starts_with("job-"))
    );
    std::thread::sleep(Duration::from_millis(1200));
    let mut second = spawn(&["worker", "--name", "second"], &env);
    wait_until("leftover removed", Duration::from_secs(10), || {
        !left[0].exists()
    });
    assert_eq!(job.join().unwrap().status, 200);
    Command::new("kill")
        .arg("-TERM")
        .arg(second.0.id().to_string())
        .status()
        .unwrap();
    second.0.wait().unwrap();
    assert!(
        roots(&work).is_empty(),
        "a cleanly stopped worker leaves nothing behind"
    );
}

#[test]
fn coordinator_needs_tls_off_loopback() {
    let root = tempfile::tempdir().unwrap();
    let out = Command::new(BIN)
        .args(["coordinator"])
        .env("KURA_BIN", FAKE)
        .env("KURA_CLUSTER_TOKEN", TOKEN)
        .env("KURA_HOST", "0.0.0.0")
        .env("KURA_PORT", port().to_string())
        .env("KURA_DATA", root.path())
        .env_remove("KURA_INSECURE_HTTP")
        .output()
        .unwrap();
    assert!(!out.status.success());
    assert!(String::from_utf8_lossy(&out.stderr).contains("without TLS"));
    let out = Command::new(BIN)
        .args(["worker", "--coordinator", "http://coord.example:8080"])
        .env("KURA_BIN", FAKE)
        .env("KURA_CLUSTER_TOKEN", TOKEN)
        .output()
        .unwrap();
    assert!(!out.status.success());
    assert!(String::from_utf8_lossy(&out.stderr).contains("refusing plain HTTP"));
}

fn openssl(dir: &Path, args: &[&str]) -> bool {
    Command::new("openssl")
        .current_dir(dir)
        .args(args)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .is_ok_and(|s| s.success())
}

pub fn make_certs(dir: &Path) -> bool {
    std::fs::write(
        dir.join("ext.cnf"),
        "subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=CA:FALSE\n",
    )
    .unwrap();
    openssl(
        dir,
        &[
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-keyout",
            "ca.key",
            "-out",
            "ca.pem",
            "-days",
            "2",
            "-subj",
            "/CN=kura test ca",
        ],
    ) && openssl(
        dir,
        &[
            "req",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-keyout",
            "key.pem",
            "-out",
            "srv.csr",
            "-subj",
            "/CN=localhost",
        ],
    ) && openssl(
        dir,
        &[
            "x509",
            "-req",
            "-in",
            "srv.csr",
            "-CA",
            "ca.pem",
            "-CAkey",
            "ca.key",
            "-CAcreateserial",
            "-out",
            "cert.pem",
            "-days",
            "2",
            "-extfile",
            "ext.cnf",
        ],
    )
}

#[test]
fn cluster_over_tls() {
    let root = tempfile::tempdir().unwrap();
    if !make_certs(root.path()) {
        eprintln!("openssl not available, skipping");
        return;
    }
    let cp = port();
    let pem = |n: &str| root.path().join(n).to_string_lossy().into_owned();
    let _coord = spawn(
        &["coordinator"],
        &[
            ("KURA_PORT", cp.to_string()),
            ("KURA_HOST", "0.0.0.0".into()),
            ("KURA_DATA", pem("data")),
            ("KURA_TLS_CERT", pem("cert.pem")),
            ("KURA_TLS_KEY", pem("key.pem")),
        ],
    );
    let url = format!("https://localhost:{cp}");
    let curl = |path: &str, extra: &[&str]| {
        Command::new("curl")
            .args([
                "-s",
                "-o",
                "/dev/null",
                "-w",
                "%{http_code}",
                "--cacert",
                &pem("ca.pem"),
            ])
            .args(extra)
            .arg(format!("{url}{path}"))
            .output()
            .unwrap()
    };
    wait_until("tls coordinator", Duration::from_secs(15), || {
        String::from_utf8_lossy(&curl("/healthz", &[]).stdout) == "200"
    });
    assert!(
        !http(cp, "GET", "/healthz", &[], b"")
            .map(|r| r.status == 200)
            .unwrap_or(false),
        "plain HTTP must not be answered"
    );
    let _w = spawn(
        &["worker", "--name", "tls"],
        &[
            ("KURA_COORDINATOR", url.clone()),
            ("KURA_CA_FILE", pem("ca.pem")),
        ],
    );
    let fp = port();
    let _front = spawn(
        &["serve", "--coordinator", &url],
        &[
            ("KURA_PORT", fp.to_string()),
            ("KURA_CA_FILE", pem("ca.pem")),
        ],
    );
    up(fp);
    let r = http(fp, "POST", "/api/convert?level=2b", &[], PDF).unwrap();
    assert_eq!(r.status, 200);
    assert!(r.body.starts_with(b"%PDF-1.7 fake"));
    let in_pdf = pem("in.pdf");
    std::fs::write(&in_pdf, PDF).unwrap();
    let out = curl(
        "/api/check?level=2b",
        &["--data-binary", &format!("@{in_pdf}")],
    );
    assert_eq!(String::from_utf8_lossy(&out.stdout), "200");
    let untrusting = spawn(
        &["worker", "--name", "untrusting"],
        &[("KURA_COORDINATOR", url.clone())],
    );
    std::thread::sleep(Duration::from_millis(1500));
    let names: Vec<String> = Command::new("curl")
        .args([
            "-s",
            "--cacert",
            &pem("ca.pem"),
            &format!("{url}/status.json"),
        ])
        .output()
        .map(|o| serde_json::from_slice::<serde_json::Value>(&o.stdout).unwrap_or_default())
        .unwrap()["workers"]
        .as_array()
        .cloned()
        .unwrap_or_default()
        .iter()
        .map(|w| w["name"].as_str().unwrap_or("").to_string())
        .collect();
    assert!(
        names.contains(&"tls".to_string()) && !names.contains(&"untrusting".to_string()),
        "{names:?}"
    );
    drop(untrusting);
}
