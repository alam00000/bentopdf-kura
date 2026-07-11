#include <cstddef>
#include <cstdio>

#include <jpeglib.h>
#include <zlib.h>

#include <csetjmp>
#include <cstring>
#include <string>
#include <vector>

#include "images.hh"

namespace pdfa {
namespace {
struct JpegError {
  jpeg_error_mgr mgr;
  jmp_buf jump;
};

void jpegErrorExit(j_common_ptr cinfo) {
  JpegError* err = reinterpret_cast<JpegError*>(cinfo->err);
  longjmp(err->jump, 1);
}
}

bool inflateData(const std::string& in, std::string& out) {
  z_stream zs;
  std::memset(&zs, 0, sizeof(zs));
  if (inflateInit(&zs) != Z_OK) return false;
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  zs.avail_in = static_cast<uInt>(in.size());
  std::vector<char> buf(1 << 18);
  int ret = Z_OK;
  while (ret != Z_STREAM_END) {
    zs.next_out = reinterpret_cast<Bytef*>(buf.data());
    zs.avail_out = static_cast<uInt>(buf.size());
    ret = inflate(&zs, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
      inflateEnd(&zs);
      return false;
    }
    out.append(buf.data(), buf.size() - zs.avail_out);
    if (ret == Z_BUF_ERROR && zs.avail_in == 0) break;
  }
  inflateEnd(&zs);
  return !out.empty();
}

RawImage decodeDctData(const std::string& data, bool& cmykInverted) {
  RawImage out;
  jpeg_decompress_struct cinfo;
  JpegError jerr;
  cinfo.err = jpeg_std_error(&jerr.mgr);
  jerr.mgr.error_exit = jpegErrorExit;
  if (setjmp(jerr.jump)) {
    jpeg_destroy_decompress(&cinfo);
    out.error = "jpeg decode failed";
    out.ok = false;
    return out;
  }
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, reinterpret_cast<const unsigned char*>(data.data()),
               static_cast<unsigned long>(data.size()));
  jpeg_read_header(&cinfo, TRUE);
  if (cinfo.jpeg_color_space == JCS_YCCK || cinfo.jpeg_color_space == JCS_CMYK) {
    cinfo.out_color_space = JCS_CMYK;
  }
  jpeg_start_decompress(&cinfo);
  int width = static_cast<int>(cinfo.output_width);
  int height = static_cast<int>(cinfo.output_height);
  int comps = cinfo.output_components;
  if (width <= 0 || height <= 0 ||
      static_cast<long long>(width) * height > 100000000LL ||
      (comps != 1 && comps != 3 && comps != 4)) {
    jpeg_destroy_decompress(&cinfo);
    out.error = "jpeg unsupported geometry";
    return out;
  }
  cmykInverted = comps == 4 && cinfo.saw_Adobe_marker;
  out.samples.resize(static_cast<size_t>(width) * height * comps);
  std::vector<JSAMPLE*> rows(1);
  while (cinfo.output_scanline < cinfo.output_height) {
    rows[0] = reinterpret_cast<JSAMPLE*>(
        &out.samples[static_cast<size_t>(cinfo.output_scanline) * width * comps]);
    jpeg_read_scanlines(&cinfo, rows.data(), 1);
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  out.width = width;
  out.height = height;
  out.comps = comps;
  out.ok = true;
  return out;
}

std::string encodeCmykJpeg(const std::string& cmyk, int width, int height, int quality) {
  std::string inverted(cmyk.size(), '\0');
  for (size_t i = 0; i < cmyk.size(); ++i) {
    inverted[i] = static_cast<char>(0xFF - static_cast<unsigned char>(cmyk[i]));
  }
  jpeg_compress_struct cinfo;
  JpegError jerr;
  cinfo.err = jpeg_std_error(&jerr.mgr);
  jerr.mgr.error_exit = [](j_common_ptr ci) {
    JpegError* e = reinterpret_cast<JpegError*>(ci->err);
    longjmp(e->jump, 1);
  };
  unsigned char* buf = nullptr;
  unsigned long bufLen = 0;
  if (setjmp(jerr.jump)) {
    jpeg_destroy_compress(&cinfo);
    if (buf) free(buf);
    return std::string();
  }
  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &buf, &bufLen);
  cinfo.image_width = static_cast<JDIMENSION>(width);
  cinfo.image_height = static_cast<JDIMENSION>(height);
  cinfo.input_components = 4;
  cinfo.in_color_space = JCS_CMYK;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  jpeg_start_compress(&cinfo, TRUE);
  size_t stride = static_cast<size_t>(width) * 4;
  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row = reinterpret_cast<JSAMPROW>(
        const_cast<char*>(inverted.data()) + cinfo.next_scanline * stride);
    jpeg_write_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  std::string out(reinterpret_cast<char*>(buf), bufLen);
  free(buf);
  return out;
}

std::string flateCompress(const std::string& data) {
  uLongf cap = compressBound(static_cast<uLong>(data.size()));
  std::string out(cap, '\0');
  if (compress2(reinterpret_cast<Bytef*>(&out[0]), &cap,
                reinterpret_cast<const Bytef*>(data.data()),
                static_cast<uLong>(data.size()), 6) != Z_OK) {
    return std::string();
  }
  out.resize(cap);
  return out;
}
}
