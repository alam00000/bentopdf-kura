FROM debian:bookworm-slim AS build
ARG QPDF_VERSION=12.3.2
ARG QPDF_SHA256=6cba2f9f2cd887d905faeb99e0e51a307b217920d1bbf3e9cfbb2e8178a2deda
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++ make pkg-config ca-certificates curl zlib1g-dev libjpeg62-turbo-dev \
    libfreetype-dev libpng-dev libopenjp2-7-dev liblcms2-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
RUN curl -sSL -o /tmp/qpdf.tar.gz \
      "https://github.com/qpdf/qpdf/releases/download/v${QPDF_VERSION}/qpdf-${QPDF_VERSION}.tar.gz" \
    && echo "${QPDF_SHA256}  /tmp/qpdf.tar.gz" | sha256sum -c - \
    && tar -xzf /tmp/qpdf.tar.gz -C /tmp \
    && cmake -S "/tmp/qpdf-${QPDF_VERSION}" -B /tmp/qpdf-build \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
      -DREQUIRE_CRYPTO_OPENSSL=ON -DUSE_IMPLICIT_CRYPTO=OFF \
      -DBUILD_DOC=OFF -DBUILD_EXAMPLES=OFF -DQPDF_TEST_COMPARE_IMAGES=OFF \
    && cmake --build /tmp/qpdf-build -j"$(nproc)" --target libqpdf \
    && cmake --install /tmp/qpdf-build --component dev \
    && cmake --install /tmp/qpdf-build --component lib \
    && rm -rf /tmp/qpdf.tar.gz "/tmp/qpdf-${QPDF_VERSION}" /tmp/qpdf-build
COPY CMakeLists.txt CMakeLists.txt
COPY core core
COPY cli cli
COPY capi capi
COPY raster raster
COPY wasm wasm
COPY fuzz fuzz
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DPDFA_WITH_PDFIUM=OFF -DPDFA_BUILD_FUZZ=OFF \
    && cmake --build build -j"$(nproc)" --target kura

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    zlib1g libjpeg62-turbo libfreetype6 libpng16-16 libopenjp2-7 liblcms2-2 libssl3 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --no-create-home kura
COPY --from=build /src/build/cli/kura /usr/local/bin/kura
USER kura
WORKDIR /work
ENTRYPOINT ["/usr/local/bin/kura"]
