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
  std::set<QPDFObjGen> identityCmaps{};
  InvoiceProfile inv{};
  int inlineImagesFixed = 0;
  int contentPuaFixed = 0;
  std::set<std::string> incompleteScans{};
  int currentPage = 0;  // 1-based page a pass is working on; 0 = document level

  bool isA() const { return fam == Family::PDFA; }
  bool isX() const { return fam == Family::PDFX || fam == Family::PDFVT; }
  bool isE() const { return fam == Family::PDFE; }
  bool isVT() const { return fam == Family::PDFVT; }
  bool x1a() const { return opt.level == Level::X1A; }
  bool cmykIntentOnly() const {
    return opt.level == Level::X1A || opt.level == Level::VT1;
  }
  bool pdf20Print() const {
    return opt.level == Level::X6 || opt.level == Level::VT3 ||
           opt.level == Level::X6N || opt.level == Level::X6P;
  }
  bool externalIntent() const {
    return opt.level == Level::X4P || opt.level == Level::X5N ||
           opt.level == Level::X5PG || opt.level == Level::X6N ||
           opt.level == Level::X6P;
  }
  bool allowRefXObjects() const {
    return opt.level == Level::X5G || opt.level == Level::X5PG ||
           opt.level == Level::VT2;
  }
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
    return isA() && (part == 3 || conf == 'F' || conf == 'E');
  }
  bool allow3D() const { return conf == 'E' || isE(); }

  void issue(const std::string& code, const std::string& detail, bool fixed) {
    Issue i;
    i.code = code;
    i.detail = detail;
    i.fixed = fixed;
    i.severity = issueSeverity(code);
    if (currentPage > 0) i.pages.push_back(currentPage);
    res.issues.push_back(std::move(i));
  }

  void scanIncomplete(const std::string& what) {
    if (incompleteScans.insert(what).second) {
      issue("SCAN_INCOMPLETE",
            "could not read " + what + "; findings below may be incomplete for that content",
            false);
    }
  }

  void fatal(const std::string& code, const std::string& detail) {
    res.errorCode = code;
    res.error = detail;
  }

  bool failed() const { return !res.errorCode.empty(); }
};

// Marks the page a pass is working on so every issue raised inside carries it.
struct PageScope {
  Ctx& ctx;
  int previous;
  PageScope(Ctx& c, int page) : ctx(c), previous(c.currentPage) { ctx.currentPage = page; }
  ~PageScope() { ctx.currentPage = previous; }
  PageScope(const PageScope&) = delete;
  PageScope& operator=(const PageScope&) = delete;
};
}
