#include <openjpeg.h>

#include <cstring>
#include <string>
#include <vector>

#include "images.hh"
#include "util.hh"

namespace pdfa {
namespace {
struct MemStream {
  const unsigned char* data;
  size_t size;
  size_t pos;
};

OPJ_SIZE_T memRead(void* buffer, OPJ_SIZE_T bytes, void* user) {
  MemStream* ms = static_cast<MemStream*>(user);
  if (ms->pos >= ms->size) return static_cast<OPJ_SIZE_T>(-1);
  size_t n = std::min(static_cast<size_t>(bytes), ms->size - ms->pos);
  std::memcpy(buffer, ms->data + ms->pos, n);
  ms->pos += n;
  return n;
}

OPJ_OFF_T memSkip(OPJ_OFF_T bytes, void* user) {
  MemStream* ms = static_cast<MemStream*>(user);
  OPJ_OFF_T target = static_cast<OPJ_OFF_T>(ms->pos) + bytes;
  if (target < 0 || static_cast<size_t>(target) > ms->size) return -1;
  ms->pos = static_cast<size_t>(target);
  return bytes;
}

OPJ_BOOL memSeek(OPJ_OFF_T bytes, void* user) {
  MemStream* ms = static_cast<MemStream*>(user);
  if (bytes < 0 || static_cast<size_t>(bytes) > ms->size) return OPJ_FALSE;
  ms->pos = static_cast<size_t>(bytes);
  return OPJ_TRUE;
}
}

namespace {
RawImage decodeJpxDataAt(const std::string& data, std::string& alphaOut);
}

RawImage decodeJpxData(const std::string& rawData, std::string& alphaOut) {
  RawImage out = decodeJpxDataAt(rawData, alphaOut);
  if (out.ok || rawData.size() < 16) return out;
  static const std::string jp2Sig("\x00\x00\x00\x0c\x6a\x50\x20\x20", 8);
  static const std::string j2kSig("\xff\x4f\xff\x51", 4);
  size_t pos = rawData.find(jp2Sig);
  if (pos == std::string::npos) pos = rawData.find(j2kSig);
  if (pos != std::string::npos && pos > 0) {
    RawImage retry = decodeJpxDataAt(rawData.substr(pos), alphaOut);
    if (retry.ok) return retry;
  }
  return out;
}

namespace {
uint32_t be32(const unsigned char* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

bool scanJp2Boxes(const unsigned char* d, size_t n, int& colrCount, bool& colrOk,
                  int& nc, int& bpcVal, bool& bpcVaries, bool& bpccEqual, int depth) {
  if (depth > 4) return false;
  size_t i = 0;
  bool sawIhdr = false;
  while (i + 8 <= n) {
    uint64_t len = be32(d + i);
    std::string type(reinterpret_cast<const char*>(d + i + 4), 4);
    size_t hdr = 8;
    if (len == 1) {
      if (i + 16 > n) return false;
      len = (static_cast<uint64_t>(be32(d + i + 8)) << 32) | be32(d + i + 12);
      hdr = 16;
    } else if (len == 0) {
      len = n - i;
    }
    if (len < hdr || len > n - i) return false;
    const unsigned char* p = d + i + hdr;
    size_t plen = static_cast<size_t>(len - hdr);
    if (type == "jp2h") {
      if (!scanJp2Boxes(p, plen, colrCount, colrOk, nc, bpcVal, bpcVaries, bpccEqual,
                        depth + 1)) {
        return false;
      }
      sawIhdr = true;
    } else if (type == "ihdr" && plen >= 10) {
      nc = (p[8] << 8) | p[9];
      bpcVal = plen >= 11 ? p[10] : 0;
      bpcVaries = bpcVal == 0xFF;
      sawIhdr = true;
    } else if (type == "bpcc") {
      bpccEqual = true;
      for (size_t j = 1; j < plen; ++j) {
        if (p[j] != p[0]) bpccEqual = false;
      }
      if (plen > 0) bpcVal = p[0];
    } else if (type == "colr" && plen >= 3) {
      ++colrCount;
      int meth = p[0];
      int approx = p[2];
      bool ok = approx == 0;
      if (meth == 1) {
        if (plen < 7) {
          ok = false;
        } else {
          uint32_t enumCs = be32(p + 3);
          ok = ok && (enumCs == 16 || enumCs == 17 || enumCs == 18);
        }
      } else if (meth != 2) {
        ok = false;
      }
      colrOk = colrOk && ok;
    }
    i += static_cast<size_t>(len);
  }
  return depth > 0 ? true : sawIhdr;
}
}

bool jpxPdfaConformant(const std::string& data) {
  const unsigned char* d = reinterpret_cast<const unsigned char*>(data.data());
  size_t n = data.size();
  if (n < 12) return false;
  if (d[0] == 0xFF && d[1] == 0x4F) return false;
  int colrCount = 0, nc = 0, bpcVal = -1;
  bool colrOk = true, bpcVaries = false, bpccEqual = true;
  if (!scanJp2Boxes(d, n, colrCount, colrOk, nc, bpcVal, bpcVaries, bpccEqual, 0)) {
    return false;
  }
  if (nc != 1 && nc != 3 && nc != 4) return false;
  if (bpcVaries) {
    if (!bpccEqual) return false;
    if (bpcVal < 0) return false;
  }
  int depthBits = (bpcVal & 0x7F) + 1;
  if (depthBits < 1 || depthBits > 38) return false;
  if (colrCount != 1 || !colrOk) return false;
  return true;
}

namespace {
RawImage decodeJpxDataAt(const std::string& data, std::string& alphaOut) {
  RawImage out;
  alphaOut.clear();
  if (data.size() < 12) {
    out.error = "jpx data too short";
    return out;
  }
  const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data.data());
  OPJ_CODEC_FORMAT fmt = OPJ_CODEC_JP2;
  if (bytes[0] == 0xFF && bytes[1] == 0x4F) fmt = OPJ_CODEC_J2K;

  opj_codec_t* codec = opj_create_decompress(fmt);
  opj_dparameters_t params;
  opj_set_default_decoder_parameters(&params);
  opj_setup_decoder(codec, &params);

  opj_stream_t* stream = opj_stream_create(data.size(), OPJ_TRUE);
  MemStream ms{bytes, data.size(), 0};
  opj_stream_set_user_data(stream, &ms, nullptr);
  opj_stream_set_user_data_length(stream, data.size());
  opj_stream_set_read_function(stream, memRead);
  opj_stream_set_skip_function(stream, memSkip);
  opj_stream_set_seek_function(stream, memSeek);

  opj_image_t* image = nullptr;
  if (!opj_read_header(stream, codec, &image) || !image) {
    opj_stream_destroy(stream);
    opj_destroy_codec(codec);
    out.error = "jpx header unreadable";
    return out;
  }
  if (!opj_decode(codec, stream, image) || !opj_end_decompress(codec, stream)) {
    opj_image_destroy(image);
    opj_stream_destroy(stream);
    opj_destroy_codec(codec);
    out.error = "jpx decode failed";
    return out;
  }
  opj_stream_destroy(stream);
  opj_destroy_codec(codec);

  unsigned ncomps = image->numcomps;
  int width = static_cast<int>(image->comps[0].w);
  int height = static_cast<int>(image->comps[0].h);
  if (ncomps == 0 || width <= 0 || height <= 0 ||
      static_cast<long long>(width) * height > 100000000LL) {
    opj_image_destroy(image);
    out.error = "jpx unsupported geometry";
    return out;
  }

  int colorComps = static_cast<int>(ncomps);
  int alphaIdx = -1;
  for (unsigned c = 0; c < ncomps; ++c) {
    if (image->comps[c].alpha) alphaIdx = static_cast<int>(c);
  }
  if (alphaIdx < 0 && (ncomps == 2 || ncomps == 4)) {
    if (image->color_space == OPJ_CLRSPC_GRAY && ncomps == 2) alphaIdx = 1;
    if (image->color_space == OPJ_CLRSPC_SRGB && ncomps == 4) alphaIdx = 3;
  }
  if (alphaIdx >= 0) colorComps -= 1;
  if (colorComps != 1 && colorComps != 3 && colorComps != 4) {
    opj_image_destroy(image);
    out.error = "jpx unsupported component count";
    return out;
  }

  out.samples.resize(static_cast<size_t>(width) * height * colorComps);
  if (alphaIdx >= 0) alphaOut.resize(static_cast<size_t>(width) * height);

  int outC = 0;
  for (unsigned c = 0; c < ncomps; ++c) {
    opj_image_comp_t& comp = image->comps[c];
    int prec = comp.prec ? static_cast<int>(comp.prec) : 8;
    int shift = prec > 8 ? prec - 8 : 0;
    int scale = prec < 8 ? (255 / ((1 << prec) - 1)) : 1;
    int adjust = comp.sgnd ? 1 << (prec - 1) : 0;
    int dx = comp.dx ? static_cast<int>(comp.dx) : 1;
    int dy = comp.dy ? static_cast<int>(comp.dy) : 1;
    int cw = static_cast<int>(comp.w);
    size_t compSize = static_cast<size_t>(comp.w) * comp.h;
    if (!comp.data || compSize == 0) {
      opj_image_destroy(image);
      out.error = "jpx component has no data";
      return out;
    }
    bool isAlpha = static_cast<int>(c) == alphaIdx;
    for (int y = 0; y < height; ++y) {
      int sy = dy > 1 ? y / dy : y;
      for (int x = 0; x < width; ++x) {
        int sx = dx > 1 ? x / dx : x;
        size_t cidx = static_cast<size_t>(sy) * cw + sx;
        long v = (cidx < compSize ? comp.data[cidx] : 0) + adjust;
        v = (v >> shift) * scale;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        size_t p = static_cast<size_t>(y) * width + x;
        if (isAlpha) {
          alphaOut[p] = static_cast<char>(v);
        } else {
          out.samples[p * colorComps + outC] = static_cast<char>(v);
        }
      }
    }
    if (!isAlpha) ++outC;
  }
  opj_image_destroy(image);

  out.width = width;
  out.height = height;
  out.comps = colorComps;
  out.ok = true;
  return out;
}
}
}
