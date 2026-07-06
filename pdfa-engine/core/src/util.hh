#pragma once

#include <qpdf/QPDFObjGen.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <set>
#include <string>

namespace pdfa {
inline bool nameIs(QPDFObjectHandle o, const std::string& n) {
  return o.isName() && o.getName() == n;
}

inline std::string nameOf(QPDFObjectHandle o) {
  return o.isName() ? o.getName() : std::string();
}

inline double numOf(QPDFObjectHandle o, double dflt) {
  return o.isNumber() ? o.getNumericValue() : dflt;
}

struct Visited {
  std::set<QPDFObjGen> seen;
  bool enter(QPDFObjectHandle o) {
    if (!o.isIndirect()) return true;
    return seen.insert(o.getObjGen()).second;
  }
};
}
