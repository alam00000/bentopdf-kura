#pragma once

#include <qpdf/QPDFObjectHandle.hh>
#include <string>

#include "ctx.hh"

namespace pdfa {
struct RawImage {
  bool ok = false;
  int width = 0;
  int height = 0;
  int comps = 0;
  std::string samples;
  std::string error;
};

RawImage decodeImage(QPDFObjectHandle image);
std::string encodeCmykJpeg(const std::string& cmyk, int width, int height, int quality);
std::string encodeJpeg(const std::string& samples, int width, int height, int comps,
                       int quality);
std::string flateCompress(const std::string& data);
bool flattenImageSMask(Ctx& ctx, QPDFObjectHandle image);
RawImage decodeJpxData(const std::string& data, std::string& alphaOut);

bool jpxPdfaConformant(const std::string& data);
bool transcodeJpxImage(Ctx& ctx, QPDFObjectHandle image);
RawImage decodeDctData(const std::string& data, bool& cmykInverted);
bool inflateData(const std::string& in, std::string& out);
void passImageResolution(Ctx& ctx);
}
