#pragma once

#include <qpdf/QPDFPageObjectHelper.hh>

#include <set>
#include <string>
#include <vector>

namespace pdfa {
struct TextRun {
  int mcid = -1;
  bool artifact = false;
  bool inForm = false;
  size_t op = 0;
  std::string text;
  std::vector<double> charX;
  std::vector<size_t> byteAt;
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  double size = 0;
  std::string font;
};

std::vector<TextRun> extractPageText(QPDFPageObjectHelper& ph);
std::string utf8Of(uint32_t cp);
}
