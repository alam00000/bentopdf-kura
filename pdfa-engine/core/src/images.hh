#pragma once

#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <set>
#include <string>
#include <vector>

#include "ctx.hh"

namespace pdfa {
struct RawImage {
  bool ok = false;
  int width = 0;
  int height = 0;
  int comps = 0;
  std::string samples;
  std::string error;
  bool damaged = false;
};

RawImage decodeImage(QPDFObjectHandle image);
std::string encodeCmykJpeg(const std::string& cmyk, int width, int height, int quality);
std::string encodeJpeg(const std::string& samples, int width, int height, int comps,
                       int quality);
std::string flateCompress(const std::string& data, int level = 6);
bool flattenImageSMask(Ctx& ctx, QPDFObjectHandle image);
RawImage decodeJpxData(const std::string& data, std::string& alphaOut);

bool jpxPdfaConformant(const std::string& data, bool hasColorSpace, bool archive);
bool transcodeJpxImage(Ctx& ctx, QPDFObjectHandle image, bool mustSucceed = true);
bool reduceImageDepth(Ctx& ctx, QPDFObjectHandle image);
bool repackImageBits(QPDFObjectHandle image, int target);
bool jpxPrintDepthOk(const std::string& data);
void retagDctColorSpace(Ctx& ctx, QPDFObjectHandle image);
bool reencodeJbig2Image(Ctx& ctx, QPDFObjectHandle image, bool mustSucceed = true);
RawImage decodeDctData(const std::string& data, bool& cmykInverted);
bool inflateData(const std::string& in, std::string& out);
void passImageResolution(Ctx& ctx);
void externalizeInlineImagesOnce(std::vector<QPDFPageObjectHelper>& pages, size_t minSize);

struct GraphicsUse {
  std::set<const void*> objs;
  std::vector<QPDFObjectHandle> inlineImages;
  std::vector<QPDFObjectHandle> maskImages;
  std::vector<QPDFObjectHandle> iccProfiles;
  std::vector<bool> pageTransparent;
  bool glyphUndefined = false;
  bool has(QPDFObjectHandle o) const { return o.isInitialized() && objs.count(o.getObj().get()) > 0; }
  void add(QPDFObjectHandle o) {
    if (o.isInitialized() && !o.isNull()) objs.insert(o.getObj().get());
  }
};

GraphicsUse collectGraphicsUse(QPDF& pdf);
}
