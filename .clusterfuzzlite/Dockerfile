FROM gcr.io/oss-fuzz-base/base-builder

RUN apt-get update && apt-get install -y --no-install-recommends \
      make cmake pkg-config zip python3 autoconf automake libtool && \
    rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 -b v1.3.1 https://github.com/madler/zlib.git $SRC/zlib
RUN git clone --depth 1 -b 3.0.4 https://github.com/libjpeg-turbo/libjpeg-turbo.git $SRC/libjpeg-turbo
RUN git clone --depth 1 -b v1.6.43 https://github.com/pnggroup/libpng.git $SRC/libpng
RUN git clone --depth 1 -b VER-2-13-2 https://gitlab.freedesktop.org/freetype/freetype.git $SRC/freetype
RUN git clone --depth 1 -b lcms2.16 https://github.com/mm2/Little-CMS.git $SRC/lcms2
RUN git clone --depth 1 -b v2.5.2 https://github.com/uclouvain/openjpeg.git $SRC/openjpeg
RUN git clone --depth 1 -b v12.3.2 https://github.com/qpdf/qpdf.git $SRC/qpdf

COPY . $SRC/kura
COPY .clusterfuzzlite/build.sh $SRC/
WORKDIR $SRC/kura
