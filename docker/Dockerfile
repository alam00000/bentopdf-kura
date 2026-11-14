FROM ubuntu:24.04 AS native
ENV DEBIAN_FRONTEND=noninteractive
WORKDIR /kura
COPY scripts/linux-static.sh scripts/linux-static.sh
RUN scripts/linux-static.sh packages && rm -rf /var/lib/apt/lists/*
RUN scripts/linux-static.sh qpdf
RUN scripts/linux-static.sh deps
COPY pdfa-engine/scripts/pdfium pdfa-engine/scripts/pdfium
RUN scripts/linux-static.sh pdfium
COPY pdfa-engine pdfa-engine
RUN scripts/linux-static.sh engine && scripts/linux-static.sh verify

FROM rust:1.100-bookworm AS server
WORKDIR /src
COPY server/Cargo.toml server/Cargo.lock ./
COPY server/src src
RUN cargo build --release --locked

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates && rm -rf /var/lib/apt/lists/*
RUN useradd --system --uid 10001 --no-create-home kura && mkdir -p /work /data && chown kura /work /data
WORKDIR /kura
COPY --from=native /kura/pdfa-engine/build/cli/kura /usr/local/bin/kura
COPY --from=server /src/target/release/kura-server /usr/local/bin/kura-server
COPY pdfa-engine/core/fonts-world /usr/local/share/kura/fonts-world
COPY pdfa-engine/core/fonts-cjk /usr/local/share/kura/fonts-cjk
COPY pdfa-engine/core/icc /usr/local/share/kura/icc
COPY pdfa-engine/third_party/icc /tmp/icc
RUN while read -r f; do [ -n "$f" ] && cp "/tmp/icc/$f" /usr/local/share/kura/icc/; done < /tmp/icc/SHIP && cp -R /tmp/icc/LICENSES /usr/local/share/kura/icc/LICENSES && rm -rf /tmp/icc
COPY pdfa-engine/third_party/cmap-resources/LICENSE.md /usr/local/share/kura/licenses/CMAP-RESOURCES-LICENSE.md
COPY site/index.html site/preflight.html site/app.js site/preflight.js site/ui.js site/pdf.js \
     site/profiles.js site/engine-http.js site/app.css site/logo.svg site/
COPY site/icons site/icons
COPY site/fonts site/fonts
COPY pdfa-engine/profiles pdfa-engine/profiles
COPY docker/entrypoint.sh /entrypoint.sh
ENV KURA_BIN=/usr/local/bin/kura KURA_PROFILES=/kura/pdfa-engine/profiles KURA_SITE=/kura/site KURA_DATA=/data
USER kura
EXPOSE 8080
WORKDIR /work
ENTRYPOINT ["/entrypoint.sh"]
CMD ["serve"]
