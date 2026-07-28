#include "kura/raster.hh"

#include <memory>
#include <mutex>

#include "public/fpdfview.h"

namespace kura {
namespace {
void ensureInit() {
  static std::once_flag once;
  std::call_once(once, [] { FPDF_InitLibrary(); });
}

struct Doc {
  std::string bytes;
  FPDF_DOCUMENT doc = nullptr;
  ~Doc() {
    if (doc) FPDF_CloseDocument(doc);
  }
};

bool renderPage(Doc& d, int pageIndex, double dpi, int& w, int& h, std::string& rgb) {
  FPDF_PAGE page = FPDF_LoadPage(d.doc, pageIndex);
  if (!page) return false;
  double pw = FPDF_GetPageWidthF(page), ph = FPDF_GetPageHeightF(page);
  w = static_cast<int>(pw * dpi / 72.0 + 0.5);
  h = static_cast<int>(ph * dpi / 72.0 + 0.5);
  if (w <= 0 || h <= 0 || static_cast<long long>(w) * h > 50000000LL) {
    FPDF_ClosePage(page);
    return false;
  }
  FPDF_BITMAP bmp = FPDFBitmap_Create(w, h, 0);
  if (!bmp) {
    FPDF_ClosePage(page);
    return false;
  }
  FPDFBitmap_FillRect(bmp, 0, 0, w, h, 0xFFFFFFFFul);
  FPDF_RenderPageBitmap(bmp, page, 0, 0, w, h, 0, 0x800);
  const unsigned char* buf = static_cast<const unsigned char*>(FPDFBitmap_GetBuffer(bmp));
  int stride = FPDFBitmap_GetStride(bmp);
  if (!buf || stride <= 0) {
    FPDFBitmap_Destroy(bmp);
    FPDF_ClosePage(page);
    return false;
  }
  rgb.assign(static_cast<size_t>(w) * h * 3, '\0');
  for (int y = 0; y < h; ++y) {
    const unsigned char* row = buf + static_cast<size_t>(y) * stride;
    for (int x = 0; x < w; ++x) {
      size_t o = (static_cast<size_t>(y) * w + x) * 3;
      rgb[o] = static_cast<char>(row[x * 4 + 2]);
      rgb[o + 1] = static_cast<char>(row[x * 4 + 1]);
      rgb[o + 2] = static_cast<char>(row[x * 4]);
    }
  }
  FPDFBitmap_Destroy(bmp);
  FPDF_ClosePage(page);
  return true;
}
}

Rasterizer makeRasterizer(const void* data, std::size_t size, const std::string& password) {
  if (!data || !size) return Rasterizer();
  ensureInit();
  auto d = std::make_shared<Doc>();
  d->bytes.assign(static_cast<const char*>(data), size);
  d->doc = FPDF_LoadMemDocument(d->bytes.data(), static_cast<int>(d->bytes.size()),
                                password.empty() ? nullptr : password.c_str());
  if (!d->doc) return Rasterizer();
  return [d](int pageIndex, double dpi, int& w, int& h, std::string& rgb) {
    return renderPage(*d, pageIndex, dpi, w, h, rgb);
  };
}
}
