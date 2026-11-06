#pragma once

#include <qpdf/QPDFObjectHandle.hh>

#include <memory>
#include <string>
#include <vector>

namespace pdfa {
class PdfFunction {
 public:
  static std::unique_ptr<PdfFunction> load(QPDFObjectHandle fn, int depth = 0);
  static std::unique_ptr<PdfFunction> loadArray(QPDFObjectHandle fns);
  virtual ~PdfFunction() = default;
  virtual bool compute(const double* in, double* out) const = 0;
  int inputs() const { return static_cast<int>(domain.size() / 2); }
  int outputs() const { return nOut; }

 protected:
  std::vector<double> domain;
  std::vector<double> range;
  int nOut = 0;
  void clipIn(const double* in, double* out) const;
  void clipOut(double* out) const;
};
}
