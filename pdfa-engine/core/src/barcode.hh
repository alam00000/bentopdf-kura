#pragma once

#include <string>
#include <vector>

namespace pdfa {
struct BarcodeMatrix {
  int width = 0;
  int height = 0;
  bool linear = false;
  std::vector<unsigned char> dark;
  std::string error;
};

struct BarcodeHit {
  std::string format;
  std::string text;
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool valid = true;
  int rotation = 0;
  bool linear = false;
};

BarcodeMatrix makeBarcode(const std::string& type, const std::string& data);
std::vector<BarcodeHit> readBarcodes(const std::string& rgb, int width, int height);
bool barcodeReaderAvailable();
}
