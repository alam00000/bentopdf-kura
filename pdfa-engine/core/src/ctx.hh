#pragma once

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjGen.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <set>
#include <string>

#include "einvoice.hh"
#include "pdfa/pdfa.hh"

namespace pdfa {
struct Ctx {
  QPDF& pdf;
  const Options& opt;
  Result& res;
  int part;
  char conf;
  Family fam;
  std::set<QPDFObjGen> identityCmaps;
  InvoiceProfile inv;

  bool isA() const { return fam == Family::PDFA; }
  bool isX() const { return fam == Family::PDFX || fam == Family::PDFVT; }
  bool isE() const { return fam == Family::PDFE; }
  bool isVT() const { return fam == Family::PDFVT; }
  bool x1a() const { return opt.level == Level::X1A; }
  bool pdf20Print() const { return opt.level == Level::X6 || opt.level == Level::VT3; }
  bool ua2() const { return opt.ua && isA() && part == 4; }
  bool pdf14Target() const {
    return (isA() && part == 1) || opt.level == Level::X1A || opt.level == Level::X3;
  }
  bool transparencyBanned() const { return pdf14Target(); }
  bool needUnicode() const {
    return isA() && (conf == 'U' || conf == 'A' || part == 4 || opt.ua);
  }
  bool needTagging() const { return isA() && conf == 'A'; }
  std::string docLang() const { return opt.docLang.empty() ? "en" : opt.docLang; }
  bool allowEmbeddedFiles() const {
    return (isA() && (part == 3 || conf == 'F' || conf == 'E')) || isE();
  }
  bool allow3D() const { return conf == 'E' || isE(); }

  void issue(const std::string& code, const std::string& detail, bool fixed) {
    res.issues.push_back({code, detail, fixed});
  }

  void fatal(const std::string& code, const std::string& detail) {
    res.errorCode = code;
    res.error = detail;
  }

  bool failed() const { return !res.errorCode.empty(); }
};
}
