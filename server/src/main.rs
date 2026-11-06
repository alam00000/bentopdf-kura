mod api;
mod coord;
mod exec;
mod jsjson;
mod wire;
mod worker;

use api::{App, Config, Exec};
use clap::{Parser, Subcommand};
use std::path::PathBuf;
use std::sync::{Arc, RwLock};
use std::time::Duration;
use tokio::sync::watch;

#[derive(Parser)]
#[command(
    name = "kura-server",
    version,
    about = "HTTP API, job coordinator and worker for the Kura PDF engine"
)]
struct Cli {
    #[command(subcommand)]
    role: Option<Role>,
    #[arg(long, env = "KURA_TLS_CERT", global = true)]
    tls_cert: Option<PathBuf>,
    #[arg(long, env = "KURA_TLS_KEY", global = true)]
    tls_key: Option<PathBuf>,
    #[arg(long, env = "KURA_CA_FILE", global = true)]
    ca_file: Option<PathBuf>,
    #[arg(long, env = "KURA_INSECURE_HTTP", global = true, value_parser = clap::builder::FalseyValueParser::new())]
    insecure_http: bool,
}

struct Tls {
    server: Option<Arc<rustls::ServerConfig>>,
    client: Arc<rustls::ClientConfig>,
    insecure_http: bool,
}

impl Tls {
    fn from_cli(cli: &Cli) -> Tls {
        let server = match (&cli.tls_cert, &cli.tls_key) {
            (Some(c), Some(k)) => Some(wire::server_tls(c, k).unwrap_or_else(|e| die(e))),
            (None, None) => None,
            _ => {
                die("set both KURA_TLS_CERT and KURA_TLS_KEY (or --tls-cert and --tls-key)".into())
            }
        };
        let client = wire::client_tls(cli.ca_file.as_deref()).unwrap_or_else(|e| die(e));
        Tls {
            server,
            client,
            insecure_http: cli.insecure_http,
        }
    }

    fn peer_url(&self, url: &str) {
        wire::check_url(url, self.insecure_http).unwrap_or_else(|e| die(e));
    }
}

#[derive(Subcommand)]
enum Role {
    Serve {
        #[arg(long, env = "KURA_COORDINATOR")]
        coordinator: Option<String>,
    },
    Coordinator {
        #[arg(long, env = "KURA_DATA", default_value = "kura-data")]
        data: PathBuf,
        #[arg(long = "hot-folder", env = "KURA_HOT_FOLDERS", value_delimiter = ';')]
        hot_folders: Vec<String>,
        #[arg(long, env = "KURA_MAX_ATTEMPTS", default_value_t = 3)]
        max_attempts: i64,
        #[arg(long, env = "KURA_LEASE_MS", default_value_t = 30000)]
        lease_ms: i64,
        #[arg(long, env = "KURA_RESULT_TTL_S", default_value_t = 3600)]
        result_ttl_s: i64,
    },
    Worker {
        #[arg(long, env = "KURA_COORDINATOR")]
        coordinator: String,
        #[arg(long, env = "KURA_SLOTS")]
        slots: Option<usize>,
        #[arg(long, env = "KURA_WORKER_NAME", default_value = "worker")]
        name: String,
    },
}

fn int_env(name: &str, dflt: i64, min: i64, max: i64) -> i64 {
    let Ok(raw) = std::env::var(name) else {
        return dflt;
    };
    let raw = raw.trim();
    if raw.is_empty() {
        return dflt;
    }
    match raw.parse::<f64>() {
        Ok(n) if n.is_finite() => (n.floor() as i64).clamp(min, max),
        _ => dflt,
    }
}

fn config(cluster: bool) -> Config {
    let env = |k: &str, d: &str| std::env::var(k).unwrap_or_else(|_| d.to_string());
    let abs = |p: String| std::path::absolute(&p).unwrap_or_else(|_| PathBuf::from(p));
    Config {
        host: env("KURA_HOST", "0.0.0.0"),
        port: int_env("KURA_PORT", 8080, 1, 65535) as u16,
        max_upload: int_env("KURA_MAX_UPLOAD_MB", 500, 1, 1024 * 1024) as u64 * 1024 * 1024,
        concurrency: int_env("KURA_CONCURRENCY", if cluster { 64 } else { 2 }, 1, 1024) as usize,
        queue_limit: int_env("KURA_QUEUE", if cluster { 1000 } else { 8 }, 0, 100000) as usize,
        timeout_ms: int_env("KURA_TIMEOUT_MS", 600000, 1000, 24 * 3600000) as u64,
        api_token: env("KURA_API_TOKEN", ""),
        cluster_token: env("KURA_CLUSTER_TOKEN", ""),
        kura_bin: abs(env("KURA_BIN", "/usr/local/bin/kura")),
        profiles: abs(env("KURA_PROFILES", "pdfa-engine/profiles")),
        site: abs(env("KURA_SITE", "site")),
    }
}

fn init_logs() {
    let filter = tracing_subscriber::EnvFilter::try_from_env("KURA_LOG_LEVEL")
        .unwrap_or_else(|_| "info".into());
    let builder = tracing_subscriber::fmt()
        .with_env_filter(filter)
        .with_writer(std::io::stderr)
        .with_target(false);
    if std::env::var("KURA_LOG_FORMAT").is_ok_and(|f| f == "json") {
        builder.json().init();
    } else {
        builder.init();
    }
}

fn die(msg: String) -> ! {
    eprintln!("{msg}");
    std::process::exit(1)
}

async fn signalled() {
    #[cfg(unix)]
    {
        let mut term = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())
            .expect("signal handler");
        tokio::select! {
            _ = term.recv() => {}
            _ = tokio::signal::ctrl_c() => {}
        }
    }
    #[cfg(not(unix))]
    let _ = tokio::signal::ctrl_c().await;
}

async fn serve_http(app: Arc<App>, extra: String, tls: Option<Arc<rustls::ServerConfig>>) {
    let addr = format!("{}:{}", app.cfg.host, app.cfg.port);
    let listener = tokio::net::TcpListener::bind(&addr)
        .await
        .unwrap_or_else(|e| die(format!("cannot listen on {addr}: {e}")));
    tracing::info!(
        "{} serving on {}://{addr} (concurrency {}, queue {}, max upload {} MB, {} bundled profiles{}{extra})",
        app.version(),
        if tls.is_some() { "https" } else { "http" },
        app.cfg.concurrency,
        app.cfg.queue_limit,
        app.cfg.max_upload / 1024 / 1024,
        app.profile_count(),
        if app.cfg.api_token.is_empty() { "" } else { ", token required" },
    );
    let router = axum::Router::new().fallback(api::handle).with_state(app);
    let acceptor = tls.map(tokio_rustls::TlsAcceptor::from);
    let graceful = hyper_util::server::graceful::GracefulShutdown::new();
    let stop = signalled();
    tokio::pin!(stop);
    loop {
        let (tcp, _) = tokio::select! {
            r = listener.accept() => match r {
                Ok(c) => c,
                Err(e) => {
                    tracing::warn!(error = %e, "accept failed");
                    tokio::time::sleep(Duration::from_millis(100)).await;
                    continue;
                }
            },
            _ = &mut stop => break,
        };
        let _ = tcp.set_nodelay(true);
        let svc = hyper_util::service::TowerToHyperService::new(router.clone());
        let acceptor = acceptor.clone();
        let watcher = graceful.watcher();
        tokio::spawn(async move {
            let http = hyper::server::conn::http1::Builder::new();
            match acceptor {
                Some(a) => {
                    if let Ok(Ok(stream)) =
                        tokio::time::timeout(Duration::from_secs(10), a.accept(tcp)).await
                    {
                        let conn = http.serve_connection(hyper_util::rt::TokioIo::new(stream), svc);
                        let _ = watcher.watch(conn).await;
                    }
                }
                None => {
                    let conn = http.serve_connection(hyper_util::rt::TokioIo::new(tcp), svc);
                    let _ = watcher.watch(conn).await;
                }
            }
        });
    }
    tracing::info!("shutting down");
    drop(listener);
    let _ = tokio::time::timeout(Duration::from_secs(10), graceful.shutdown()).await;
}

fn main() {
    let cli = Cli::parse();
    init_logs();
    let tls = Tls::from_cli(&cli);
    let rt = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .unwrap_or_else(|e| die(e.to_string()));
    rt.block_on(async move {
        match cli.role.unwrap_or(Role::Serve {
            coordinator: std::env::var("KURA_COORDINATOR").ok(),
        }) {
            Role::Serve { coordinator: None } => {
                let cfg = config(false);
                let version = exec::engine_version(&cfg.kura_bin).unwrap_or_else(|e| {
                    die(format!(
                        "cannot run the engine at {}: {e}",
                        cfg.kura_bin.display()
                    ))
                });
                let tmp = std::env::temp_dir();
                api::sweep_temp(&tmp, Duration::from_millis(cfg.timeout_ms) + Duration::from_secs(3600));
                serve_http(
                    Arc::new(App::new(
                        cfg,
                        Arc::new(RwLock::new(version)),
                        Exec::Local,
                        tmp,
                    )),
                    String::new(),
                    tls.server,
                )
                .await;
            }
            Role::Serve {
                coordinator: Some(url),
            } => {
                let cfg = config(true);
                if cfg.cluster_token.is_empty() {
                    die("KURA_CLUSTER_TOKEN must be set to talk to a coordinator".into());
                }
                let version = Arc::new(RwLock::new(
                    exec::engine_version(&cfg.kura_bin).unwrap_or_default(),
                ));
                tls.peer_url(&url);
                spawn_version_refresh(url.clone(), version.clone(), tls.client.clone());
                let peer = wire::Peer::new(&url, &cfg.cluster_token, tls.client.clone());
                let tmp = std::env::temp_dir();
                api::sweep_temp(&tmp, Duration::from_millis(cfg.timeout_ms) + Duration::from_secs(3600));
                let app = App::new(cfg, version, Exec::Remote(peer), tmp);
                serve_http(Arc::new(app), format!(", forwarding to {url}"), tls.server).await;
            }
            Role::Coordinator {
                data,
                hot_folders,
                max_attempts,
                lease_ms,
                result_ttl_s,
            } => {
                let cfg = config(true);
                if cfg.cluster_token.is_empty() {
                    die("KURA_CLUSTER_TOKEN must be set so workers can authenticate".into());
                }
                if tls.server.is_none() && !tls.insecure_http && !wire::is_loopback_host(&cfg.host) {
                    die(format!(
                        "refusing to run a coordinator on {} without TLS: set KURA_TLS_CERT and KURA_TLS_KEY, bind KURA_HOST=127.0.0.1, or set KURA_INSECURE_HTTP=1 on a trusted private network",
                        cfg.host
                    ));
                }
                let folders: Vec<coord::HotFolder> = hot_folders
                    .iter()
                    .filter(|s| !s.trim().is_empty())
                    .map(|s| {
                        coord::parse_hot_folder(s.trim(), &cfg.profiles).unwrap_or_else(|e| die(e))
                    })
                    .collect();
                let version = Arc::new(RwLock::new(
                    exec::engine_version(&cfg.kura_bin).unwrap_or_default(),
                ));
                let set = coord::Settings {
                    max_attempts: max_attempts.max(1),
                    lease_ms: lease_ms.max(1000),
                    ttl_ms: result_ttl_s.max(1) * 1000,
                    queue_limit: cfg.queue_limit,
                    timeout_ms: cfg.timeout_ms,
                    max_upload: cfg.max_upload,
                    profiles: cfg.profiles.clone(),
                };
                let data = std::path::absolute(&data).unwrap_or(data);
                let c = coord::Coord::open(&data, set, version.clone(), folders)
                    .unwrap_or_else(|e| die(e));
                let extra = format!(
                    ", coordinator data {}, {} hot folder(s)",
                    data.display(),
                    c.folders.len()
                );
                coord::spawn_background(c.clone());
                let tmp = c.tmp.clone();
                serve_http(Arc::new(App::new(cfg, version, Exec::Queue(c), tmp)), extra, tls.server).await;
            }
            Role::Worker {
                coordinator,
                slots,
                name,
            } => {
                let cfg = config(false);
                if cfg.cluster_token.is_empty() {
                    die("KURA_CLUSTER_TOKEN must be set to talk to a coordinator".into());
                }
                tls.peer_url(&coordinator);
                let slots = slots.unwrap_or(cfg.concurrency).max(1);
                let (tx, rx) = watch::channel(false);
                let peer = wire::Peer::new(&coordinator, &cfg.cluster_token, tls.client.clone());
                let work = tokio::spawn(worker::run(cfg, peer, coordinator, slots, name, rx));
                tokio::select! {
                    r = work => if let Ok(Err(e)) = r { die(e) },
                    _ = async {
                        signalled().await;
                        tracing::info!("finishing running jobs; signal again to stop now");
                        let _ = tx.send(true);
                        signalled().await;
                    } => tracing::warn!("stopping with jobs still running"),
                }
            }
        }
    });
    rt.shutdown_timeout(Duration::from_secs(2));
}

fn spawn_version_refresh(
    url: String,
    version: Arc<RwLock<String>>,
    tls: Arc<rustls::ClientConfig>,
) {
    tokio::spawn(async move {
        let peer = wire::Peer::new(&url, "", tls);
        loop {
            if version.read().map(|v| !v.is_empty()).unwrap_or(false) {
                return;
            }
            let res = peer
                .send(
                    hyper::Method::GET,
                    "/healthz",
                    &[],
                    axum::body::Body::empty(),
                    Duration::from_secs(10),
                )
                .await;
            if let Ok(res) = res {
                if let Ok(b) = http_body_util::BodyExt::collect(res.into_body()).await {
                    let v = serde_json::from_slice::<serde_json::Value>(&b.to_bytes()).ok();
                    if let Some(s) = v
                        .as_ref()
                        .and_then(|v| v["version"].as_str())
                        .filter(|s| !s.is_empty())
                    {
                        if let Ok(mut w) = version.write() {
                            *w = s.to_string();
                        }
                    }
                }
            }
            tokio::time::sleep(Duration::from_secs(5)).await;
        }
    });
}
