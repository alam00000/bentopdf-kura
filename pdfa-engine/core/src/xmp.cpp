#include <qpdf/QPDF.hh>

#include <cctype>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "ctx.hh"
#include "passes.hh"
#include "util.hh"

namespace pdfa {
namespace {
std::string xmlEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (unsigned char c : in) {
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') continue;
    else out += static_cast<char>(c);
  }
  return out;
}

bool isXmpDate(const std::string& s) {
  if (s.size() < 4 || s.size() > 40) return false;
  for (size_t i = 0; i < 4; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  for (unsigned char c : s) {
    bool allowed = (c >= '0' && c <= '9') || c == '-' || c == ':' || c == 'T' || c == 'Z' ||
                   c == '+' || c == '.';
    if (!allowed) return false;
  }
  return true;
}

bool parseIntField(const std::string& s, size_t pos, size_t len, int lo, int hi, int& out) {
  if (pos + len > s.size()) return false;
  int v = 0;
  for (size_t i = pos; i < pos + len; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    v = v * 10 + (s[i] - '0');
  }
  if (v < lo || v > hi) return false;
  out = v;
  return true;
}

bool parseDateFlexible(const std::string& raw, std::string& iso, std::string& pdfDate) {
  std::string s = raw;
  if (s.rfind("D:", 0) == 0) s = s.substr(2);
  int year = 0, month = 1, day = 1, hour = 0, minute = 0, second = 0;
  bool neg = false;
  int tzh = 0, tzm = 0;
  bool haveTz = false;
  if (s.size() >= 10 && s[4] == '-' && s[7] == '-') {
    if (!parseIntField(s, 0, 4, 1, 9999, year) || !parseIntField(s, 5, 2, 1, 12, month) ||
        !parseIntField(s, 8, 2, 1, 31, day)) {
      return false;
    }
    if (s.size() >= 19 && (s[10] == 'T' || s[10] == ' ')) {
      parseIntField(s, 11, 2, 0, 23, hour);
      parseIntField(s, 14, 2, 0, 59, minute);
      parseIntField(s, 17, 2, 0, 59, second);
      size_t tzPos = 19;
      while (tzPos < s.size() && (std::isdigit(static_cast<unsigned char>(s[tzPos])) ||
                                  s[tzPos] == '.')) {
        ++tzPos;
      }
      if (tzPos < s.size() && (s[tzPos] == '+' || s[tzPos] == '-')) {
        neg = s[tzPos] == '-';
        haveTz = parseIntField(s, tzPos + 1, 2, 0, 23, tzh);
        if (tzPos + 4 < s.size() && s[tzPos + 3] == ':') {
          parseIntField(s, tzPos + 4, 2, 0, 59, tzm);
        }
      } else if (tzPos < s.size() && s[tzPos] == 'Z') {
        haveTz = true;
      }
    }
  } else {
    if (!parseIntField(s, 0, 4, 1, 9999, year)) return false;
    if (parseIntField(s, 4, 2, 1, 12, month)) {
      if (parseIntField(s, 6, 2, 1, 31, day)) {
        if (parseIntField(s, 8, 2, 0, 23, hour) && parseIntField(s, 10, 2, 0, 59, minute)) {
          parseIntField(s, 12, 2, 0, 59, second);
          size_t tzPos = 14;
          if (s.size() > tzPos && (s[tzPos] == '+' || s[tzPos] == '-')) {
            neg = s[tzPos] == '-';
            if (parseIntField(s, tzPos + 1, 2, 0, 23, tzh)) {
              haveTz = true;
              size_t mPos = tzPos + 3;
              if (s.size() > mPos && s[mPos] == '\'') ++mPos;
              parseIntField(s, mPos, 2, 0, 59, tzm);
            }
          } else if (s.size() > tzPos && s[tzPos] == 'Z') {
            haveTz = true;
          }
        }
      }
    }
  }
  char tzIso[16];
  char tzPdf[16];
  if (haveTz && (tzh || tzm)) {
    std::snprintf(tzIso, sizeof(tzIso), "%c%02d:%02d", neg ? '-' : '+', tzh, tzm);
    std::snprintf(tzPdf, sizeof(tzPdf), "%c%02d'%02d'", neg ? '-' : '+', tzh, tzm);
  } else {
    std::snprintf(tzIso, sizeof(tzIso), "Z");
    std::snprintf(tzPdf, sizeof(tzPdf), "Z");
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d%s", year, month, day, hour,
                minute, second, tzIso);
  iso = buf;
  std::snprintf(buf, sizeof(buf), "D:%04d%02d%02d%02d%02d%02d%s", year, month, day, hour,
                minute, second, tzPdf);
  pdfDate = buf;
  return true;
}

bool pdfDateToIso(const std::string& raw, std::string& iso) {
  std::string pdfDate;
  return parseDateFlexible(raw, iso, pdfDate);
}

struct InfoData {
  std::string title, author, subject, keywords, creator, producer;
  std::string createIso, modifyIso, trapped;
};

std::string pdfNow(Ctx& ctx) {
  if (!ctx.opt.nowOverride.empty()) return ctx.opt.nowOverride;
  std::time_t t = std::time(nullptr);
  std::tm g{};
  gmtime_r(&t, &g);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "D:%04d%02d%02d%02d%02d%02dZ", g.tm_year + 1900,
                g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
  return buf;
}

std::string xVersionString(Ctx& ctx) {
  switch (ctx.opt.level) {
    case Level::X1A: return "PDF/X-1a:2003";
    case Level::X3: return "PDF/X-3:2003";
    case Level::X4P: return "PDF/X-4p";
    case Level::X5G: return "PDF/X-5g";
    case Level::X5N: return "PDF/X-5n";
    case Level::X5PG: return "PDF/X-5pg";
    case Level::X6N: return "PDF/X-6n";
    case Level::X6P: return "PDF/X-6p";
    default: break;
  }
  if (ctx.pdf20Print()) return "PDF/X-6";
  return "PDF/X-4";
}

std::string vtVersionString(Ctx& ctx) {
  if (ctx.opt.level == Level::VT2) return "PDF/VT-2";
  return ctx.pdf20Print() ? "PDF/VT-3" : "PDF/VT-1";
}

std::string oldXmpText(Ctx& ctx) {
  QPDFObjectHandle meta = ctx.pdf.getRoot().getKey("/Metadata");
  if (!meta.isStream()) return std::string();
  try {
    auto buf = meta.getStreamData(qpdf_dl_all);
    return std::string(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
  } catch (...) {
    return std::string();
  }
}

std::string xmpElementText(const std::string& xmp, const std::string& localName) {
  size_t p = xmp.find(localName + ">");
  if (p != std::string::npos) {
    size_t start = p + localName.size() + 1;
    size_t end = xmp.find('<', start);
    if (end != std::string::npos) return xmp.substr(start, end - start);
  }
  p = xmp.find(localName + "=\"");
  if (p != std::string::npos) {
    size_t start = p + localName.size() + 2;
    size_t end = xmp.find('"', start);
    if (end != std::string::npos) return xmp.substr(start, end - start);
  }
  return std::string();
}

bool xVersionAcceptable(Ctx& ctx, const std::string& v) {
  switch (ctx.opt.level) {
    case Level::X1A: return v == "PDF/X-1a:2001" || v == "PDF/X-1a:2003";
    case Level::X3: return v == "PDF/X-3:2002" || v == "PDF/X-3:2003";
    case Level::X4: return v == "PDF/X-4" || v == "PDF/X-4:2010";
    case Level::X4P: return v == "PDF/X-4p" || v == "PDF/X-4p:2010";
    case Level::X5G: return v == "PDF/X-5g" || v == "PDF/X-5g:2010";
    case Level::X5N: return v == "PDF/X-5n" || v == "PDF/X-5n:2010";
    case Level::X5PG: return v == "PDF/X-5pg" || v == "PDF/X-5pg:2010";
    case Level::X6: return v == "PDF/X-6";
    case Level::X6N: return v == "PDF/X-6n";
    case Level::X6P: return v == "PDF/X-6p";
    case Level::VT1: return v == "PDF/X-4" || v == "PDF/X-4:2010";
    case Level::VT2:
      return v == "PDF/X-4p" || v == "PDF/X-4p:2010" || v == "PDF/X-5g" ||
             v == "PDF/X-5g:2010" || v == "PDF/X-5pg" || v == "PDF/X-5pg:2010";
    case Level::VT3: return v == "PDF/X-6";
    default: return true;
  }
}

std::string cleanText(QPDFObjectHandle v) {
  if (!v.isString()) return std::string();
  std::string s = v.getUTF8Value();
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') continue;
    out += static_cast<char>(c);
  }
  return out;
}

InfoData collectInfo(Ctx& ctx) {
  InfoData d;
  d.producer = std::string(kEngineName) + " " + kEngineVersion;
  QPDFObjectHandle trailer = ctx.pdf.getTrailer();
  QPDFObjectHandle info = trailer.getKey("/Info");
  if (!info.isDictionary()) {
    if (!info.isNull()) trailer.removeKey("/Info");
    if (!ctx.isX()) return d;
    info = QPDFObjectHandle::newDictionary();
  }
  QPDFObjectHandle clean = QPDFObjectHandle::newDictionary();
  d.title = cleanText(info.getKey("/Title"));
  d.author = cleanText(info.getKey("/Author"));
  d.subject = cleanText(info.getKey("/Subject"));
  d.keywords = cleanText(info.getKey("/Keywords"));
  d.creator = cleanText(info.getKey("/Creator"));
  std::string rawCreate = cleanText(info.getKey("/CreationDate"));
  std::string rawModify = cleanText(info.getKey("/ModDate"));
  std::string canonCreate, canonModify;
  bool dropped = false;
  if (!rawCreate.empty() && !parseDateFlexible(rawCreate, d.createIso, canonCreate)) {
    dropped = true;
  }
  if (!rawModify.empty() && !parseDateFlexible(rawModify, d.modifyIso, canonModify)) {
    dropped = true;
  }
  if (!d.title.empty()) clean.replaceKey("/Title", QPDFObjectHandle::newUnicodeString(d.title));
  if (!d.author.empty()) clean.replaceKey("/Author", QPDFObjectHandle::newUnicodeString(d.author));
  if (!d.subject.empty()) clean.replaceKey("/Subject", QPDFObjectHandle::newUnicodeString(d.subject));
  if (!d.keywords.empty()) clean.replaceKey("/Keywords", QPDFObjectHandle::newUnicodeString(d.keywords));
  if (!d.creator.empty()) clean.replaceKey("/Creator", QPDFObjectHandle::newUnicodeString(d.creator));
  if (!d.producer.empty()) clean.replaceKey("/Producer", QPDFObjectHandle::newUnicodeString(d.producer));
  if (!d.createIso.empty() && !canonCreate.empty()) {
    clean.replaceKey("/CreationDate", QPDFObjectHandle::newString(canonCreate));
  }
  if (!d.modifyIso.empty() && !canonModify.empty()) {
    clean.replaceKey("/ModDate", QPDFObjectHandle::newString(canonModify));
  }
  bool hadCustom = false;
  for (const std::string& k : info.getKeys()) {
    if (k != "/Title" && k != "/Author" && k != "/Subject" && k != "/Keywords" &&
        k != "/Creator" && k != "/Producer" && k != "/CreationDate" && k != "/ModDate") {
      hadCustom = true;
    }
  }
  if (dropped) {
    ctx.issue("DOCINFO_DATE_DROPPED", "dropped unparseable date from document info", true);
  }
  if (ctx.isX()) {
    std::string declared = cleanText(info.getKey("/GTS_PDFXVersion"));
    std::string declaredVt = cleanText(info.getKey("/GTS_PDFVTVersion"));
    if (ctx.pdf20Print()) {
      std::string xmpOld = oldXmpText(ctx);
      declared = xmpElementText(xmpOld, "GTS_PDFXVersion");
      declaredVt = xmpElementText(xmpOld, "GTS_PDFVTVersion");
      if (d.createIso.empty()) {
        std::string c = xmpElementText(xmpOld, "xmp:CreateDate");
        if (isXmpDate(c)) d.createIso = c;
      }
      if (d.modifyIso.empty()) {
        std::string m = xmpElementText(xmpOld, "xmp:ModifyDate");
        if (isXmpDate(m)) d.modifyIso = m;
      }
      if (d.title.empty()) {
        size_t tp = xmpOld.find("<dc:title");
        size_t tend = tp == std::string::npos ? std::string::npos
                                             : xmpOld.find("</dc:title", tp);
        if (tend != std::string::npos) {
          size_t li = xmpOld.find("<rdf:li", tp);
          size_t open = (li == std::string::npos || li > tend) ? std::string::npos
                                                               : xmpOld.find('>', li);
          size_t close = (open == std::string::npos || open > tend)
                             ? std::string::npos
                             : xmpOld.find('<', open + 1);
          if (close != std::string::npos && close <= tend && close > open + 1) {
            d.title = xmpOld.substr(open + 1, close - open - 1);
          }
        }
      }
    }
    if (d.title.empty()) {
      d.title = "Untitled document";
      clean.replaceKey("/Title", QPDFObjectHandle::newUnicodeString(d.title));
      ctx.issue("TITLE_SYNTHESIZED",
                "PDF/X requires a document title; set placeholder \"Untitled document\"", true);
    }
    std::string now = pdfNow(ctx);
    if (d.createIso.empty()) {
      clean.replaceKey("/CreationDate", QPDFObjectHandle::newString(now));
      pdfDateToIso(now, d.createIso);
      ctx.issue("DOCINFO_DATE_SYNTHESIZED", "added required /CreationDate", true);
    }
    if (d.modifyIso.empty()) {
      clean.replaceKey("/ModDate", QPDFObjectHandle::newString(now));
      pdfDateToIso(now, d.modifyIso);
      ctx.issue("DOCINFO_DATE_SYNTHESIZED", "added required /ModDate", true);
    }
    if (!xVersionAcceptable(ctx, declared)) {
      ctx.issue("X_VERSION_SET",
                declared.empty()
                    ? "recorded the " + xVersionString(ctx) + " version marker"
                    : "replaced the declared version marker \"" + declared + "\" with " +
                          xVersionString(ctx),
                true);
    }
    if (ctx.isVT() && declaredVt != vtVersionString(ctx)) {
      ctx.issue("VT_VERSION_SET",
                declaredVt.empty()
                    ? "recorded the " + vtVersionString(ctx) + " version marker"
                    : "replaced the declared version marker \"" + declaredVt + "\" with " +
                          vtVersionString(ctx),
                true);
    }
    if (ctx.pdf20Print()) {
      QPDFObjectHandle pinfo = trailer.getKey("/Info");
      bool hadInfo = pinfo.isDictionary() && !pinfo.getKeys().empty();
      if (ctx.pdf.getRoot().getKey("/PieceInfo").isDictionary() && !d.modifyIso.empty() &&
          !canonModify.empty()) {
        QPDFObjectHandle keep = QPDFObjectHandle::newDictionary();
        keep.replaceKey("/ModDate", QPDFObjectHandle::newString(canonModify));
        trailer.replaceKey("/Info", ctx.pdf.makeIndirectObject(keep));
        if (hadInfo && pinfo.getKeys().size() > 1) {
          ctx.issue("DOCINFO_REDUCED",
                    "kept only /ModDate in document info (required alongside /PieceInfo)",
                    true);
        }
      } else if (hadInfo) {
        trailer.removeKey("/Info");
        ctx.issue("DOCINFO_REMOVED",
                  "removed the document information dictionary (PDF 2.0 print standards "
                  "keep document metadata in XMP only)",
                  true);
      }
      return d;
    }
    clean.replaceKey("/GTS_PDFXVersion", QPDFObjectHandle::newString(xVersionString(ctx)));
    if (ctx.isVT()) {
      clean.replaceKey("/GTS_PDFVTVersion",
                       QPDFObjectHandle::newString(vtVersionString(ctx)));
    }
    std::string trapped = nameOf(info.getKey("/Trapped"));
    if (trapped != "/True" && trapped != "/False") {
      std::string fromXmp = xmpElementText(oldXmpText(ctx), "pdf:Trapped");
      if (fromXmp == "True" || fromXmp == "False") trapped = "/" + fromXmp;
    }
    if (trapped != "/True" && trapped != "/False") {
      trapped = "/False";
      ctx.issue("TRAPPED_SET",
                "PDF/X requires /Trapped to state True or False; set False", true);
    }
    clean.replaceKey("/Trapped", QPDFObjectHandle::newName(trapped));
    d.trapped = trapped == "/True" ? "True" : "False";
    trailer.replaceKey("/Info", ctx.pdf.makeIndirectObject(clean));
    return d;
  }
  if (ctx.isA() && ctx.part >= 4) {
    QPDFObjectHandle root = ctx.pdf.getRoot();
    if (root.getKey("/PieceInfo").isDictionary() && !d.modifyIso.empty() &&
        !canonModify.empty()) {
      QPDFObjectHandle keep = QPDFObjectHandle::newDictionary();
      keep.replaceKey("/ModDate", QPDFObjectHandle::newString(canonModify));
      trailer.replaceKey("/Info", ctx.pdf.makeIndirectObject(keep));
      ctx.issue("DOCINFO_REDUCED",
                "kept only /ModDate in document info (required alongside /PieceInfo)", true);
    } else {
      if (root.getKey("/PieceInfo").isDictionary()) {
        root.removeKey("/PieceInfo");
        ctx.issue("PIECEINFO_REMOVED",
                  "removed catalog /PieceInfo (no valid /ModDate to pair with it)", true);
      }
      trailer.removeKey("/Info");
      if (!clean.getKeys().empty()) {
        ctx.issue("DOCINFO_MIGRATED",
                  "migrated document info dictionary into XMP (deprecated in PDF 2.0)", true);
      }
    }
    return d;
  }
  if (hadCustom) {
    ctx.issue("DOCINFO_CUSTOM_REMOVED", "removed nonstandard document info entries", true);
  }
  if (clean.getKeys().empty()) {
    trailer.removeKey("/Info");
  } else {
    trailer.replaceKey("/Info", ctx.pdf.makeIndirectObject(clean));
  }
  return d;
}

void appendSimple(std::string& xmp, const std::string& tag, const std::string& value) {
  if (value.empty()) return;
  xmp += "      <" + tag + ">" + xmlEscape(value) + "</" + tag + ">\n";
}

void appendAlt(std::string& xmp, const std::string& tag, const std::string& value,
               const std::string& lang = "x-default") {
  if (value.empty()) return;
  xmp += "      <" + tag + "><rdf:Alt><rdf:li xml:lang=\"" + xmlEscape(lang) + "\">" +
         xmlEscape(value) + "</rdf:li></rdf:Alt></" + tag + ">\n";
}

void appendSeq(std::string& xmp, const std::string& tag, const std::string& value) {
  if (value.empty()) return;
  xmp += "      <" + tag + "><rdf:Seq><rdf:li>" + xmlEscape(value) + "</rdf:li></rdf:Seq></" +
         tag + ">\n";
}
}

void passMetadata(Ctx& ctx) {
  QPDFObjGen rootGen = ctx.pdf.getRoot().getObjGen();
  int stripped = 0;
  {
    Visited visited;
    std::vector<QPDFObjectHandle> stack;
    for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) stack.push_back(obj);
    while (!stack.empty()) {
      QPDFObjectHandle o = stack.back();
      stack.pop_back();
      if (o.isIndirect() && !visited.enter(o)) continue;
      QPDFObjectHandle d;
      if (o.isStream()) d = o.getDict();
      else if (o.isDictionary()) d = o;
      if (d.isInitialized() && d.isDictionary()) {
        bool refForm = ctx.allowRefXObjects() && d.getKey("/Ref").isDictionary() &&
                       nameIs(d.getKey("/Subtype"), "/Form");
        if (o.getObjGen() != rootGen && d.getKey("/Metadata").isStream() && !refForm) {
          d.removeKey("/Metadata");
          ++stripped;
        }
        for (const std::string& k : d.getKeys()) {
          QPDFObjectHandle v = d.getKey(k);
          if (v.isDictionary() || v.isArray() || v.isStream()) stack.push_back(v);
        }
        continue;
      }
      if (o.isArray()) {
        for (int i = 0; i < o.getArrayNItems(); ++i) {
          QPDFObjectHandle v = o.getArrayItem(i);
          if (v.isDictionary() || v.isArray() || v.isStream()) stack.push_back(v);
        }
      }
    }
  }
  if (stripped) {
    ctx.issue("OBJECT_METADATA_REMOVED",
              "removed " + std::to_string(stripped) +
                  " object-level XMP metadata stream reference(s) with nonconforming schemas",
              true);
  }

  InfoData info = collectInfo(ctx);

  std::string xmp;
  xmp += "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n";
  xmp += "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n";
  xmp += "  <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n";
  xmp += "    <rdf:Description rdf:about=\"\"\n";
  if (ctx.isA()) {
    xmp += "        xmlns:pdfaid=\"http://www.aiim.org/pdfa/ns/id/\"\n";
  }
  if (ctx.opt.ua) {
    xmp += "        xmlns:pdfuaid=\"http://www.aiim.org/pdfua/ns/id/\"\n";
  }
  if (ctx.isX()) {
    xmp += "        xmlns:pdfxid=\"http://www.npes.org/pdfx/ns/id/\"\n";
  }
  if (ctx.isVT()) {
    xmp += "        xmlns:pdfvtid=\"http://www.npes.org/pdfvt/ns/id/\"\n";
  }
  if (ctx.isE()) {
    xmp += "        xmlns:pdfe=\"http://www.aiim.org/pdfe/ns/id/\"\n";
  }
  if (!ctx.opt.attachXml.empty()) {
    xmp += "        xmlns:" + ctx.inv.prefix + "=\"" + ctx.inv.nsUri + "\"\n";
  }
  xmp += "        xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n";
  xmp += "        xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n";
  if (ctx.isX()) {
    xmp += "        xmlns:xmpMM=\"http://ns.adobe.com/xap/1.0/mm/\"\n";
  }
  xmp += "        xmlns:pdf=\"http://ns.adobe.com/pdf/1.3/\">\n";
  if (ctx.isA()) {
    xmp += "      <pdfaid:part>" + std::to_string(ctx.part) + "</pdfaid:part>\n";
    if (ctx.part >= 4) {
      xmp += "      <pdfaid:rev>2020</pdfaid:rev>\n";
      if (ctx.conf == 'F' || ctx.conf == 'E') {
        xmp += std::string("      <pdfaid:conformance>") + ctx.conf + "</pdfaid:conformance>\n";
      }
    } else {
      xmp += std::string("      <pdfaid:conformance>") + ctx.conf + "</pdfaid:conformance>\n";
    }
  }
  if (ctx.opt.ua) {
    if (ctx.ua2()) {
      xmp += "      <pdfuaid:part>2</pdfuaid:part>\n";
      xmp += "      <pdfuaid:rev>2024</pdfuaid:rev>\n";
    } else {
      xmp += "      <pdfuaid:part>1</pdfuaid:part>\n";
    }
  }
  if (ctx.isX()) {
    xmp += "      <pdfxid:GTS_PDFXVersion>" + xVersionString(ctx) +
           "</pdfxid:GTS_PDFXVersion>\n";
    xmp += "      <pdf:Trapped>" + (info.trapped.empty() ? std::string("False") : info.trapped) +
           "</pdf:Trapped>\n";
  }
  if (ctx.isVT()) {
    xmp += "      <pdfvtid:GTS_PDFVTVersion>" + vtVersionString(ctx) +
           "</pdfvtid:GTS_PDFVTVersion>\n";
    xmp += "      <pdfvtid:GTS_PDFVTModDate>" + xmlEscape(info.modifyIso) +
           "</pdfvtid:GTS_PDFVTModDate>\n";
    if (ctx.pdf20Print()) {
      xmp += "      <pdfvtid:GTS_PDFVTRev>2020</pdfvtid:GTS_PDFVTRev>\n";
      xmp += "      <pdfvtid:rev>2020</pdfvtid:rev>\n";
    }
  }
  if (ctx.isE()) {
    xmp += "      <pdfe:ISO_PDFEVersion>PDF/E-1</pdfe:ISO_PDFEVersion>\n";
  }
  if (!ctx.opt.attachXml.empty()) {
    const std::string& px = ctx.inv.prefix;
    xmp += "      <" + px + ":DocumentType>" + xmlEscape(ctx.inv.documentType) + "</" + px +
           ":DocumentType>\n";
    xmp += "      <" + px + ":DocumentFileName>" + xmlEscape(ctx.inv.filename) + "</" + px +
           ":DocumentFileName>\n";
    xmp += "      <" + px + ":Version>" + xmlEscape(ctx.inv.version) + "</" + px + ":Version>\n";
    xmp += "      <" + px + ":ConformanceLevel>" + xmlEscape(ctx.inv.profile) + "</" + px +
           ":ConformanceLevel>\n";
  }
  xmp += "      <dc:format>application/pdf</dc:format>\n";
  if (ctx.opt.ua) {
    std::string title = info.title;
    if (title.empty()) {
      title = "Untitled document";
      ctx.issue("TITLE_SYNTHESIZED",
                "document has no title; set placeholder dc:title \"Untitled document\" "
                "(PDF/UA requires a title — supply a real one for meaningful accessibility)",
                true);
    }
    appendAlt(xmp, "dc:title", title, ctx.docLang());
  } else {
    appendAlt(xmp, "dc:title", info.title);
  }
  appendSeq(xmp, "dc:creator", info.author);
  appendAlt(xmp, "dc:description", info.subject);
  appendSimple(xmp, "pdf:Keywords", info.keywords);
  appendSimple(xmp, "xmp:CreatorTool", info.creator);
  appendSimple(xmp, "pdf:Producer", info.producer);
  appendSimple(xmp, "xmp:CreateDate", info.createIso);
  appendSimple(xmp, "xmp:ModifyDate", info.modifyIso);
  if (ctx.isX()) {
    appendSimple(xmp, "xmp:MetadataDate", info.modifyIso);
    uint32_t hash = fnv1a32(info.createIso + info.modifyIso + levelToString(ctx.opt.level));
    char idbuf[64];
    std::snprintf(idbuf, sizeof(idbuf), "uuid:%08x-0000-4000-8000-%08x0000", hash,
                  hash ^ 0x9e3779b9u);
    appendSimple(xmp, "xmpMM:DocumentID", idbuf);
    appendSimple(xmp, "xmpMM:InstanceID", idbuf);
    appendSimple(xmp, "xmpMM:VersionID", "1");
    appendSimple(xmp, "xmpMM:RenditionClass", "default");
  }
  xmp += "    </rdf:Description>\n";
  bool wantUaSchema = ctx.opt.ua && ctx.part <= 3;
  bool wantFxSchema = !ctx.opt.attachXml.empty() && ctx.isA() && ctx.part <= 3;
  if (wantUaSchema || wantFxSchema) {
    xmp += "    <rdf:Description rdf:about=\"\"\n";
    xmp += "        xmlns:pdfaExtension=\"http://www.aiim.org/pdfa/ns/extension/\"\n";
    xmp += "        xmlns:pdfaSchema=\"http://www.aiim.org/pdfa/ns/schema#\"\n";
    xmp += "        xmlns:pdfaProperty=\"http://www.aiim.org/pdfa/ns/property#\">\n";
    xmp += "      <pdfaExtension:schemas>\n";
    xmp += "        <rdf:Bag>\n";
    if (wantUaSchema) {
      xmp += "          <rdf:li rdf:parseType=\"Resource\">\n";
      xmp += "            <pdfaSchema:schema>PDF/UA identification schema</pdfaSchema:schema>\n";
      xmp += "            <pdfaSchema:namespaceURI>http://www.aiim.org/pdfua/ns/id/</pdfaSchema:namespaceURI>\n";
      xmp += "            <pdfaSchema:prefix>pdfuaid</pdfaSchema:prefix>\n";
      xmp += "            <pdfaSchema:property>\n";
      xmp += "              <rdf:Seq>\n";
      xmp += "                <rdf:li rdf:parseType=\"Resource\">\n";
      xmp += "                  <pdfaProperty:name>part</pdfaProperty:name>\n";
      xmp += "                  <pdfaProperty:valueType>Integer</pdfaProperty:valueType>\n";
      xmp += "                  <pdfaProperty:category>internal</pdfaProperty:category>\n";
      xmp += "                  <pdfaProperty:description>Indicates, which part of ISO 14289 standard is followed</pdfaProperty:description>\n";
      xmp += "                </rdf:li>\n";
      xmp += "              </rdf:Seq>\n";
      xmp += "            </pdfaSchema:property>\n";
      xmp += "          </rdf:li>\n";
    }
    if (wantFxSchema) {
      xmp += "          <rdf:li rdf:parseType=\"Resource\">\n";
      xmp += "            <pdfaSchema:schema>" + ctx.inv.schemaName +
             "</pdfaSchema:schema>\n";
      xmp += "            <pdfaSchema:namespaceURI>" + ctx.inv.nsUri +
             "</pdfaSchema:namespaceURI>\n";
      xmp += "            <pdfaSchema:prefix>" + ctx.inv.prefix + "</pdfaSchema:prefix>\n";
      xmp += "            <pdfaSchema:property>\n";
      xmp += "              <rdf:Seq>\n";
      const std::string& std_ = ctx.inv.standard;
      const std::string props[4][2] = {
          {"DocumentFileName", "name of the embedded XML document file"},
          {"DocumentType", ctx.inv.documentType},
          {"Version", "The actual version of the " + std_ + " data"},
          {"ConformanceLevel", "The conformance level of the embedded " + std_ + " data"}};
      for (auto& pr : props) {
        xmp += "                <rdf:li rdf:parseType=\"Resource\">\n";
        xmp += std::string("                  <pdfaProperty:name>") + pr[0] +
               "</pdfaProperty:name>\n";
        xmp += "                  <pdfaProperty:valueType>Text</pdfaProperty:valueType>\n";
        xmp += "                  <pdfaProperty:category>external</pdfaProperty:category>\n";
        xmp += std::string("                  <pdfaProperty:description>") + pr[1] +
               "</pdfaProperty:description>\n";
        xmp += "                </rdf:li>\n";
      }
      xmp += "              </rdf:Seq>\n";
      xmp += "            </pdfaSchema:property>\n";
      xmp += "          </rdf:li>\n";
    }
    xmp += "        </rdf:Bag>\n";
    xmp += "      </pdfaExtension:schemas>\n";
    xmp += "    </rdf:Description>\n";
  }
  xmp += "  </rdf:RDF>\n";
  xmp += "</x:xmpmeta>\n";
  for (int i = 0; i < 20; ++i) {
    xmp += "                                                                                                   \n";
  }
  xmp += "<?xpacket end=\"w\"?>";

  QPDFObjectHandle stream = QPDFObjectHandle::newStream(&ctx.pdf, xmp);
  QPDFObjectHandle d = stream.getDict();
  d.replaceKey("/Type", QPDFObjectHandle::newName("/Metadata"));
  d.replaceKey("/Subtype", QPDFObjectHandle::newName("/XML"));
  ctx.pdf.getRoot().replaceKey("/Metadata", ctx.pdf.makeIndirectObject(stream));
  ctx.issue("XMP_REBUILT", "regenerated XMP metadata with PDF/A identification", true);
}
}
