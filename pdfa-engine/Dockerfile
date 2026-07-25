FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++ make pkg-config zlib1g-dev libjpeg62-turbo-dev \
    libfreetype-dev libopenjp2-7-dev liblcms2-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY third_party/qpdf-12.3.2 third_party/qpdf-12.3.2
RUN cmake -S third_party/qpdf-12.3.2 -B /tmp/qpdf-build \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
      -DREQUIRE_CRYPTO_NATIVE=ON -DUSE_IMPLICIT_CRYPTO=OFF \
      -DBUILD_DOC=OFF -DBUILD_EXAMPLES=OFF -DQPDF_TEST_COMPARE_IMAGES=OFF \
    && cmake --build /tmp/qpdf-build -j"$(nproc)" --target libqpdf \
    && cmake --install /tmp/qpdf-build --component dev \
    && cmake --install /tmp/qpdf-build --component lib
COPY core core
COPY cli cli
COPY wasm wasm
COPY CMakeLists.txt CMakeLists.txt
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)" --target kura

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    zlib1g libjpeg62-turbo libfreetype6 libopenjp2-7 liblcms2-2 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/cli/kura /usr/local/bin/kura
ENTRYPOINT ["/usr/local/bin/kura"]
