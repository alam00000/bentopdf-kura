use crate::exec::safe_name;
use axum::body::Body;
use bytes::Bytes;
use futures_util::{Stream, StreamExt, TryStreamExt};
use http_body_util::BodyExt;
use hyper::body::Incoming;
use hyper::{Method, Request, Response, StatusCode};
use hyper_rustls::{HttpsConnector, HttpsConnectorBuilder};
use hyper_util::client::legacy::{Client, connect::HttpConnector};
use hyper_util::rt::TokioExecutor;
use rustls_pki_types::pem::PemObject;
use rustls_pki_types::{CertificateDer, PrivateKeyDer};
use serde::{Deserialize, Serialize, de::DeserializeOwned};
use std::io;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio_util::io::{ReaderStream, StreamReader};

#[derive(Serialize, Deserialize)]
struct Head<T> {
    meta: T,
    files: Vec<Entry>,
}

#[derive(Serialize, Deserialize)]
struct Entry {
    name: String,
    size: u64,
}

const MAX_HEAD: u32 = 64 * 1024 * 1024;

pub async fn encode<T: Serialize>(meta: &T, dir: &Path, names: &[String]) -> io::Result<Body> {
    let mut files = Vec::new();
    let mut streams = Vec::new();
    for name in names {
        let f = tokio::fs::File::open(dir.join(name)).await?;
        files.push(Entry {
            name: name.clone(),
            size: f.metadata().await?.len(),
        });
        streams.push(ReaderStream::new(f));
    }
    let head = serde_json::to_vec(&Head { meta, files }).map_err(io::Error::other)?;
    let mut first = (head.len() as u32).to_be_bytes().to_vec();
    first.extend_from_slice(&head);
    let first =
        futures_util::stream::once(async move { Ok::<Bytes, io::Error>(Bytes::from(first)) });
    Ok(Body::from_stream(
        first.chain(futures_util::stream::iter(streams).flatten()),
    ))
}

pub async fn decode<T, S, E>(stream: S, dir: &Path, max_total: u64) -> io::Result<(T, Vec<String>)>
where
    T: DeserializeOwned,
    S: Stream<Item = Result<Bytes, E>> + Unpin,
    E: Into<Box<dyn std::error::Error + Send + Sync>>,
{
    let mut reader = StreamReader::new(stream.map_err(io::Error::other));
    let len = reader.read_u32().await?;
    if len > MAX_HEAD {
        return Err(io::Error::other("bundle header too large"));
    }
    let mut head = vec![0; len as usize];
    reader.read_exact(&mut head).await?;
    let head: Head<T> = serde_json::from_slice(&head).map_err(io::Error::other)?;
    let mut total = 0u64;
    let mut names = Vec::new();
    for e in head.files {
        total = total.saturating_add(e.size);
        if !safe_name(&e.name) || total > max_total {
            return Err(io::Error::other(format!(
                "rejected bundle entry {}",
                e.name
            )));
        }
        let mut out = tokio::fs::File::create(dir.join(&e.name)).await?;
        let copied = tokio::io::copy(&mut (&mut reader).take(e.size), &mut out).await?;
        out.flush().await?;
        if copied != e.size {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "truncated bundle",
            ));
        }
        names.push(e.name);
    }
    Ok((head.meta, names))
}

fn provider() -> Arc<rustls::crypto::CryptoProvider> {
    Arc::new(rustls::crypto::ring::default_provider())
}

pub fn client_tls(ca_file: Option<&Path>) -> Result<Arc<rustls::ClientConfig>, String> {
    let mut roots = rustls::RootCertStore::empty();
    for cert in rustls_native_certs::load_native_certs().certs {
        let _ = roots.add(cert);
    }
    if let Some(ca) = ca_file {
        let certs = CertificateDer::pem_file_iter(ca)
            .and_then(|i| i.collect::<Result<Vec<_>, _>>())
            .map_err(|e| format!("cannot read the CA file {}: {e}", ca.display()))?;
        for cert in certs {
            roots
                .add(cert)
                .map_err(|e| format!("bad CA certificate in {}: {e}", ca.display()))?;
        }
    }
    let cfg = rustls::ClientConfig::builder_with_provider(provider())
        .with_safe_default_protocol_versions()
        .map_err(|e| e.to_string())?
        .with_root_certificates(roots)
        .with_no_client_auth();
    Ok(Arc::new(cfg))
}

pub fn server_tls(cert: &Path, key: &Path) -> Result<Arc<rustls::ServerConfig>, String> {
    let certs = CertificateDer::pem_file_iter(cert)
        .and_then(|i| i.collect::<Result<Vec<_>, _>>())
        .map_err(|e| format!("cannot read the certificate {}: {e}", cert.display()))?;
    let key = PrivateKeyDer::from_pem_file(key)
        .map_err(|e| format!("cannot read the key {}: {e}", key.display()))?;
    let mut cfg = rustls::ServerConfig::builder_with_provider(provider())
        .with_safe_default_protocol_versions()
        .map_err(|e| e.to_string())?
        .with_no_client_auth()
        .with_single_cert(certs, key)
        .map_err(|e| format!("certificate and key do not fit: {e}"))?;
    cfg.alpn_protocols = vec![b"http/1.1".to_vec()];
    Ok(Arc::new(cfg))
}

pub fn is_loopback_host(host: &str) -> bool {
    let host = host.trim_start_matches('[').trim_end_matches(']');
    host == "localhost"
        || host
            .parse::<std::net::IpAddr>()
            .is_ok_and(|ip| ip.is_loopback())
}

pub fn check_url(url: &str, insecure_http: bool) -> Result<(), String> {
    let uri: hyper::Uri = url
        .parse()
        .map_err(|_| format!("invalid coordinator address {url}"))?;
    match uri.scheme_str() {
        Some("https") => Ok(()),
        Some("http") if insecure_http || uri.host().is_some_and(is_loopback_host) => Ok(()),
        Some("http") => Err(format!(
            "refusing plain HTTP to {url}: use https://, or set KURA_INSECURE_HTTP=1 on a trusted private network"
        )),
        _ => Err(format!(
            "the coordinator address must start with https:// or http://, not {url}"
        )),
    }
}

#[derive(Clone)]
pub struct Peer {
    client: Client<HttpsConnector<HttpConnector>, Body>,
    base: String,
    token: String,
}

impl Peer {
    pub fn new(base: &str, token: &str, tls: Arc<rustls::ClientConfig>) -> Self {
        let mut conn = HttpConnector::new();
        conn.set_connect_timeout(Some(Duration::from_secs(10)));
        conn.set_nodelay(true);
        conn.enforce_http(false);
        let https = HttpsConnectorBuilder::new()
            .with_tls_config((*tls).clone())
            .https_or_http()
            .enable_http1()
            .wrap_connector(conn);
        Peer {
            client: Client::builder(TokioExecutor::new()).build(https),
            base: base.trim_end_matches('/').to_string(),
            token: token.to_string(),
        }
    }

    pub async fn send(
        &self,
        method: Method,
        path: &str,
        headers: &[(&str, String)],
        body: Body,
        timeout: Duration,
    ) -> io::Result<Response<Incoming>> {
        let mut req = Request::builder()
            .method(method)
            .uri(format!("{}{}", self.base, path))
            .header("authorization", format!("Bearer {}", self.token));
        for (k, v) in headers {
            req = req.header(*k, v);
        }
        let req = req.body(body).map_err(io::Error::other)?;
        match tokio::time::timeout(timeout, self.client.request(req)).await {
            Ok(r) => r.map_err(io::Error::other),
            Err(_) => Err(io::Error::new(io::ErrorKind::TimedOut, "request timed out")),
        }
    }

    pub async fn json<T: Serialize>(
        &self,
        path: &str,
        body: &T,
    ) -> io::Result<(StatusCode, Bytes)> {
        let body = Body::from(serde_json::to_vec(body).map_err(io::Error::other)?);
        let res = self
            .send(
                Method::POST,
                path,
                &[("content-type", "application/json".into())],
                body,
                Duration::from_secs(30),
            )
            .await?;
        let status = res.status();
        let bytes = res
            .into_body()
            .collect()
            .await
            .map_err(io::Error::other)?
            .to_bytes();
        Ok((status, bytes))
    }
}

pub fn body_stream(
    res: Response<Incoming>,
) -> impl Stream<Item = Result<Bytes, hyper::Error>> + Unpin {
    http_body_util::BodyDataStream::new(res.into_body())
}

pub fn job_dir(root: &Path, id: i64) -> PathBuf {
    root.join(id.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn round_trip_and_rejects_traversal() {
        let a = tempfile::tempdir().unwrap();
        let b = tempfile::tempdir().unwrap();
        std::fs::write(a.path().join("in.pdf"), b"%PDF-1.7 hello").unwrap();
        std::fs::write(a.path().join("x.icc"), vec![7u8; 100_000]).unwrap();
        let body = encode(
            &serde_json::json!({"k": 1}),
            a.path(),
            &["in.pdf".into(), "x.icc".into()],
        )
        .await
        .unwrap();
        let (meta, names): (serde_json::Value, _) =
            decode(body.into_data_stream(), b.path(), 1 << 20)
                .await
                .unwrap();
        assert_eq!(meta["k"], 1);
        assert_eq!(names, vec!["in.pdf", "x.icc"]);
        assert_eq!(
            std::fs::read(b.path().join("x.icc")).unwrap().len(),
            100_000
        );

        let body = encode(&1, a.path(), &["x.icc".into()]).await.unwrap();
        assert!(
            decode::<i32, _, _>(body.into_data_stream(), b.path(), 10)
                .await
                .is_err()
        );

        assert!(check_url("http://127.0.0.1:1", false).is_ok());
        assert!(check_url("http://[::1]:1", false).is_ok());
        assert!(check_url("http://coord:8080", false).is_err());
        assert!(check_url("http://coord:8080", true).is_ok());
        assert!(check_url("https://coord:8080", false).is_ok());
        assert!(check_url("ftp://coord", true).is_err());

        let head = br#"{"meta":0,"files":[{"name":"../evil","size":1}]}"#;
        let mut raw = (head.len() as u32).to_be_bytes().to_vec();
        raw.extend_from_slice(head);
        raw.push(b'x');
        let s = futures_util::stream::iter(vec![Ok::<Bytes, io::Error>(Bytes::from(raw))]);
        assert!(decode::<i32, _, _>(s, b.path(), 100).await.is_err());
        assert!(!b.path().parent().unwrap().join("evil").exists());
    }
}
