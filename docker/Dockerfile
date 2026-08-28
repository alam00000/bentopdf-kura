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

FROM ubuntu:24.04
COPY --from=node:22-bookworm-slim /usr/local/bin/node /usr/local/bin/node
RUN useradd --system --uid 10001 --no-create-home kura && mkdir -p /work && chown kura /work
WORKDIR /kura
COPY --from=native /kura/pdfa-engine/build/cli/kura /usr/local/bin/kura
COPY server server
COPY site/index.html site/preflight.html site/app.js site/preflight.js site/ui.js site/pdf.js \
     site/profiles.js site/engine-http.js site/app.css site/logo.svg site/
COPY site/icons site/icons
COPY site/fonts site/fonts
COPY pdfa-engine/profiles pdfa-engine/profiles
COPY docker/entrypoint.sh /entrypoint.sh
ENV KURA_BIN=/usr/local/bin/kura KURA_PROFILES=/kura/pdfa-engine/profiles
USER kura
EXPOSE 8080
WORKDIR /work
ENTRYPOINT ["/entrypoint.sh"]
CMD ["serve"]
