#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <cstdio>

#include <chrono>
#include <set>
#include <cctype>
#include <cstring>
#include <exception>

#include "colorconv.hh"
#include "images.hh"
#include "ctx.hh"
#include "passes.hh"
#include "ocr.hh"
#include "sign.hh"
#include "pdfa/pdfa.hh"
#include "util.hh"

namespace pdfa {
void fixIccChannelCounts(Ctx& ctx);

namespace {
struct LevelRow {
  Level level;
  const char* name;
  int part;
  char conformance;
  Family family;
  bool verifyOnly;
};

constexpr LevelRow kLevelTable[] = {
    {Level::A1B,  "1b",   1, 'B', Family::PDFA,  false},
    {Level::A1A,  "1a",   1, 'A', Family::PDFA,  false},
    {Level::A2B,  "2b",   2, 'B', Family::PDFA,  false},
    {Level::A2U,  "2u",   2, 'U', Family::PDFA,  false},
    {Level::A2A,  "2a",   2, 'A', Family::PDFA,  false},
    {Level::A3B,  "3b",   3, 'B', Family::PDFA,  false},
    {Level::A3U,  "3u",   3, 'U', Family::PDFA,  false},
    {Level::A3A,  "3a",   3, 'A', Family::PDFA,  false},
    {Level::A4,   "4",    4, 0,   Family::PDFA,  false},
    {Level::A4F,  "4f",   4, 'F', Family::PDFA,  false},
    {Level::A4E,  "4e",   4, 'E', Family::PDFA,  false},
    {Level::X1A,  "x1a",  0, 0,   Family::PDFX,  false},
    {Level::X3,   "x3",   0, 0,   Family::PDFX,  false},
    {Level::X4,   "x4",   0, 0,   Family::PDFX,  false},
    {Level::X6,   "x6",   0, 0,   Family::PDFX,  false},
    {Level::E1,   "e1",   0, 0,   Family::PDFE,  false},
    {Level::VT1,  "vt1",  0, 0,   Family::PDFVT, false},
    {Level::VT3,  "vt3",  0, 0,   Family::PDFVT, false},
    {Level::X4P,  "x4p",  0, 0,   Family::PDFX,  true},
    {Level::X5G,  "x5g",  0, 0,   Family::PDFX,  true},
    {Level::X5N,  "x5n",  0, 0,   Family::PDFX,  true},
    {Level::X5PG, "x5pg", 0, 0,   Family::PDFX,  true},
    {Level::X6N,  "x6n",  0, 0,   Family::PDFX,  true},
    {Level::X6P,  "x6p",  0, 0,   Family::PDFX,  true},
    {Level::VT2,  "vt2",  0, 0,   Family::PDFVT, true},
};

const LevelRow& levelRow(Level level) {
  for (const LevelRow& r : kLevelTable) {
    if (r.level == level) return r;
  }
  return kLevelTable[2];
}
}

bool levelFromString(const std::string& s, Level& out) {
  for (const LevelRow& r : kLevelTable) {
    if (s == r.name) {
      out = r.level;
      return true;
    }
  }
  return false;
}

std::string levelToString(Level level) { return levelRow(level).name; }

int levelPart(Level level) { return levelRow(level).part; }

char levelConformance(Level level) { return levelRow(level).conformance; }

Family levelFamily(Level level) { return levelRow(level).family; }

bool levelVerifyOnly(Level level) { return levelRow(level).verifyOnly; }

namespace {
std::string detectSecurityHandler(const unsigned char* data, std::size_t size) {
  try {
    QPDF probe;
    probe.setSuppressWarnings(true);
    probe.setAttemptRecovery(true);
    try {
      probe.processMemoryFile("probe", reinterpret_cast<const char*>(data), size, "");
    } catch (...) {
    }
    QPDFObjectHandle enc = probe.getTrailer().getKey("/Encrypt");
    if (enc.isDictionary() && enc.getKey("/Filter").isName()) {
      std::string f = enc.getKey("/Filter").getName();
      if (!f.empty() && f[0] == '/') f = f.substr(1);
      return f;
    }
  } catch (...) {
  }
  std::string hay(reinterpret_cast<const char*>(data),
                  size > 4096 ? 4096 : size);
  std::string tail(reinterpret_cast<const char*>(data) + (size > 4096 ? size - 4096 : 0),
                   size > 4096 ? 4096 : 0);
  for (const std::string& chunk : {hay, tail}) {
    if (chunk.find("EBX_HANDLER") != std::string::npos) return "EBX_HANDLER";
    if (chunk.find("ADEPT") != std::string::npos) return "ADEPT";
    if (chunk.find("FOPN") != std::string::npos) return "FOPN_foweb";
  }
  return std::string();
}

double versionNumber(const std::string& v) { return v.empty() ? 0 : std::atof(v.c_str()); }

void applyVersionCompat(Ctx& ctx, const std::string& version) {
  double v = versionNumber(version);
  if (v <= 0) return;
  std::vector<QPDFPageObjectHelper> pages = QPDFPageDocumentHelper(ctx.pdf).getAllPages();
  int images16 = 0, jpx = 0, flattened = 0;
  if (v < 1.5) {
    long long marks = 0, objects = 0;
    bool hadLayers = ctx.pdf.getRoot().getKey("/OCProperties").isDictionary();
    flattenOptionalContent(ctx, pages, false, marks, objects);
    if (hadLayers)
      ctx.issue("VERSION_LAYERS_FLATTENED", "merged layers into the page (layers need PDF 1.5); " +
                std::to_string(marks + objects) + " piece(s) of hidden content dropped", true);
    for (QPDFObjectHandle o : ctx.pdf.getAllObjects()) {
      if (!o.isStream() || !nameIs(o.getDict().getKey("/Subtype"), "/Image")) continue;
      QPDFObjectHandle d = o.getDict();
      QPDFObjectHandle f = d.getKey("/Filter");
      bool isJpx = nameIs(f, "/JPXDecode") || (f.isArray() && f.getArrayNItems() > 0 && nameIs(f.getArrayItem(f.getArrayNItems() - 1), "/JPXDecode"));
      if (isJpx && transcodeJpxImage(ctx, o, false)) ++jpx;
      else if (d.getKey("/BitsPerComponent").isInteger() && d.getKey("/BitsPerComponent").getIntValue() == 16 && reduceImageDepth(ctx, o)) ++images16;
    }
  }
  if (v < 1.4) {
    for (size_t i = 0; i < pages.size(); ++i) {
      if (!pageHasTransparency(ctx, pages[i])) continue;
      if (vectorFlattenPage(ctx, pages[i], static_cast<int>(i), nullptr, 300) ||
          (ctx.opt.rasterizePage && rasterFlattenPage(ctx, pages[i], static_cast<int>(i), true, 300)))
        ++flattened;
    }
    if (flattened)
      ctx.issue("VERSION_TRANSPARENCY_FLATTENED", "flattened transparency on " + std::to_string(flattened) +
                " page(s) (transparency needs PDF 1.4)", true);
  }
  if (jpx) ctx.issue("VERSION_JPX_RECODED", "re-encoded " + std::to_string(jpx) + " JPEG 2000 image(s) as JPEG (JPEG 2000 needs PDF 1.5)", true);
  if (images16) ctx.issue("VERSION_IMAGES_REDUCED", "reduced " + std::to_string(images16) + " 16-bit image(s) to 8 bits (16-bit images need PDF 1.5)", true);
}

void serialize(Ctx& ctx) {
  QPDFWriter w(ctx.pdf);
  const bool signing = static_cast<bool>(ctx.opt.signDocument);
  w.setOutputMemory();
  w.setPreserveEncryption(false);
  w.setLinearization(ctx.opt.linearize && !signing);
  w.setNewlineBeforeEndstream(true);
  w.setDeterministicID(true);
  w.setCompressStreams(true);
  w.setDecodeLevel(qpdf_dl_generalized);
  w.setRecompressFlate(true);
  w.setPreserveUnreferencedObjects(false);
  if (ctx.opt.noStandard) {
    double v = versionNumber(ctx.opt.pdfVersion);
    if (v > 0) {
      w.setObjectStreamMode(v < 1.5 || signing ? qpdf_o_disable : qpdf_o_preserve);
      w.forcePDFVersion(ctx.opt.pdfVersion);
    } else {
      w.setObjectStreamMode(signing ? qpdf_o_disable : qpdf_o_preserve);
    }
  } else if (ctx.pdf14Target()) {
    w.setObjectStreamMode(qpdf_o_disable);
    w.forcePDFVersion("1.4");
  } else if (!ctx.isA()) {
    w.setObjectStreamMode(signing ? qpdf_o_disable : qpdf_o_generate);
    w.forcePDFVersion(ctx.pdf20Print() ? "2.0" : "1.6");
  } else {
    w.setObjectStreamMode(signing ? qpdf_o_disable : qpdf_o_generate);
    w.forcePDFVersion(ctx.part >= 4 ? "2.0" : ctx.dualX() ? "1.6" : "1.7");
  }
  w.write();
  auto buf = w.getBufferSharedPointer();
  ctx.res.pdf.assign(buf->getBuffer(), buf->getBuffer() + buf->getSize());

  try {
    QPDF check;
    check.setSuppressWarnings(true);
    check.setAttemptRecovery(false);
    check.processMemoryFile("output", reinterpret_cast<const char*>(ctx.res.pdf.data()),
                            ctx.res.pdf.size(), "");
    check.getRoot().getKey("/Pages");
    if (check.anyWarnings()) {
      ctx.fatal("SERIALIZE_ERROR", "output failed self-verification after write");
    }
  } catch (const std::exception& e) {
    ctx.fatal("SERIALIZE_ERROR", std::string("output failed self-verification: ") + e.what());
  }
}
}

namespace {
bool jpegDims(const unsigned char* d, std::size_t n, int& w, int& h, int& comps) {
  if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
  std::size_t i = 2;
  while (i + 9 < n) {
    if (d[i] != 0xFF) {
      ++i;
      continue;
    }
    unsigned char m = d[i + 1];
    if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
      i += 2;
      continue;
    }
    std::size_t len = (static_cast<std::size_t>(d[i + 2]) << 8) | d[i + 3];
    if ((m >= 0xC0 && m <= 0xC3) || (m >= 0xC5 && m <= 0xC7) ||
        (m >= 0xC9 && m <= 0xCB) || (m >= 0xCD && m <= 0xCF)) {
      if (i + 9 >= n) return false;
      h = (d[i + 5] << 8) | d[i + 6];
      w = (d[i + 7] << 8) | d[i + 8];
      comps = d[i + 9];
      return w > 0 && h > 0 && (comps == 1 || comps == 3 || comps == 4);
    }
    i += 2 + len;
  }
  return false;
}

std::string wrapJpegAsPdf(const unsigned char* data, std::size_t size) {
  int w = 0, h = 0, comps = 0;
  if (!jpegDims(data, size, w, h, comps)) return std::string();
  double pw = w * 72.0 / 300.0, ph = h * 72.0 / 300.0;
  if (pw < 72) { ph *= 72.0 / pw; pw = 72; }
  if (ph < 72) { pw *= 72.0 / ph; ph = 72; }
  QPDF out;
  out.emptyPDF();
  QPDFObjectHandle img = QPDFObjectHandle::newStream(&out,
      std::string(reinterpret_cast<const char*>(data), size));
  QPDFObjectHandle id = img.getDict();
  id.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
  id.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
  id.replaceKey("/Width", QPDFObjectHandle::newInteger(w));
  id.replaceKey("/Height", QPDFObjectHandle::newInteger(h));
  id.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
  id.replaceKey("/ColorSpace", QPDFObjectHandle::newName(
      comps == 1 ? "/DeviceGray" : (comps == 4 ? "/DeviceCMYK" : "/DeviceRGB")));
  img.replaceStreamData(std::string(reinterpret_cast<const char*>(data), size),
                        QPDFObjectHandle::newName("/DCTDecode"), QPDFObjectHandle::newNull());
  QPDFObjectHandle imgRef = out.makeIndirectObject(img);
  std::string content =
      "q " + fmtFixed(pw, 2) + " 0 0 " + fmtFixed(ph, 2) + " 0 0 cm /Im0 Do Q";
  QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
  page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
  page.replaceKey("/MediaBox", QPDFObjectHandle::parse("[0 0 " + fmtFixed(pw, 2) + " " +
                                                      fmtFixed(ph, 2) + "]"));
  QPDFObjectHandle resd = QPDFObjectHandle::newDictionary();
  QPDFObjectHandle xod = QPDFObjectHandle::newDictionary();
  xod.replaceKey("/Im0", imgRef);
  resd.replaceKey("/XObject", xod);
  page.replaceKey("/Resources", resd);
  page.replaceKey("/Contents",
                  out.makeIndirectObject(QPDFObjectHandle::newStream(&out, content)));
  out.getRoot().getKey("/Pages");
  QPDFPageDocumentHelper(out).addPage(page, false);
  QPDFWriter w2(out);
  w2.setOutputMemory();
  w2.write();
  auto buf = w2.getBufferSharedPointer();
  return std::string(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
}
}

bool repairPageTree(QPDF& pdf) {
  try {
    QPDFPageDocumentHelper(pdf).getAllPages();
    return false;
  } catch (std::exception&) {
  }
  QPDFObjectHandle root = pdf.getRoot();
  std::vector<QPDFObjectHandle> pages;
  std::set<QPDFObjGen> seen;
  struct Item {
    QPDFObjectHandle node;
    std::map<std::string, QPDFObjectHandle> inherited;
  };
  std::vector<Item> stack{{root.getKey("/Pages"), {}}};
  while (!stack.empty() && pages.size() < 1000000) {
    Item it = stack.back();
    stack.pop_back();
    QPDFObjectHandle n = it.node;
    if (!n.isDictionary() || (n.isIndirect() && !seen.insert(n.getObjGen()).second)) continue;
    for (const char* k : {"/Resources", "/MediaBox", "/CropBox", "/Rotate"}) {
      if (n.hasKey(k)) it.inherited[k] = n.getKey(k);
    }
    QPDFObjectHandle kids = n.getKey("/Kids");
    if (kids.isArray() && !nameIs(n.getKey("/Type"), "/Page")) {
      for (int i = kids.getArrayNItems() - 1; i >= 0; --i) stack.push_back({kids.getArrayItem(i), it.inherited});
      continue;
    }
    if (!nameIs(n.getKey("/Type"), "/Page") && !n.hasKey("/Contents") && !n.hasKey("/MediaBox")) continue;
    for (const auto& [k, v] : it.inherited) {
      if (!n.hasKey(k)) n.replaceKey(k, v);
    }
    pages.push_back(n);
  }
  if (pages.empty()) {
    for (QPDFObjectHandle o : pdf.getAllObjects()) {
      if (o.isDictionary() && nameIs(o.getKey("/Type"), "/Page")) pages.push_back(o);
    }
  }
  if (pages.empty()) return false;
  QPDFObjectHandle tree = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
  QPDFObjectHandle kids = QPDFObjectHandle::newArray();
  for (QPDFObjectHandle pg : pages) {
    if (!pg.isIndirect()) pg = pdf.makeIndirectObject(pg);
    pg.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
    pg.replaceKey("/Parent", tree);
    if (!pg.getKey("/MediaBox").isArray()) pg.replaceKey("/MediaBox", QPDFObjectHandle::parse("[0 0 612 792]"));
    kids.appendItem(pg);
  }
  tree.replaceKey("/Type", QPDFObjectHandle::newName("/Pages"));
  tree.replaceKey("/Kids", kids);
  tree.replaceKey("/Count", QPDFObjectHandle::newInteger(static_cast<long long>(pages.size())));
  root.replaceKey("/Pages", tree);
  pdf.updateAllPagesCache();
  return true;
}

bool issueIsNormalization(const std::string& code) {
  return code == "XMP_REBUILT" || code == "CONTENT_FILTERED" ||
         code == "OUTPUT_INTENT_PRESENT";
}

int issueSeverity(const std::string& code) {
  if (issueIsNormalization(code) || code == "IMAGE_INPUT_WRAPPED" ||
      code.rfind("ANALYZE_", 0) == 0) {
    return 1;
  }
  if (code == "SCAN_INCOMPLETE" || code == "FONT_SUBSTITUTED" ||
      code == "STRUCT_TREE_SYNTHESIZED") {
    return 2;
  }
  return 3;
}

Result convertOnce(const unsigned char* data, std::size_t size, const Options& optIn) {
  static thread_local bool inDualPass = false;
  Level dualA = optIn.level;
  bool wantDual = optIn.dualPrint && levelFamily(optIn.level) == Family::PDFA;
  if (wantDual && !inDualPass && !optIn.verifyOnly && (levelPart(dualA) == 2 || levelPart(dualA) == 4)) {
    Options first = optIn;
    first.dualPrint = true;
    first.level = levelPart(dualA) == 4 ? Level::X6 : Level::X4;
    first.signDocument = nullptr;
    inDualPass = true;
    Result r1;
    try {
      r1 = convert(data, size, first);
    } catch (...) {
      inDualPass = false;
      throw;
    }
    inDualPass = false;
    if (!r1.ok) return r1;
    Options second = optIn;
    second.level = dualA;
    second.dualPrint = true;
    second.preflightProfile.clear();
    second.profileFixOps.clear();
    second.colorJob = ColorJob();
    second.rasterizePage = nullptr;
    second.ocrPage = nullptr;
    second.signDocument = optIn.signDocument;
    std::vector<unsigned char> mid = r1.pdf;
    inDualPass = true;
    Result r2;
    try {
      r2 = convert(mid.data(), mid.size(), second);
    } catch (...) {
      inDualPass = false;
      throw;
    }
    inDualPass = false;
    r2.analysis.insert(r2.analysis.begin(), r1.analysis.begin(), r1.analysis.end());
    r2.issues.insert(r2.issues.begin(), r1.issues.begin(), r1.issues.end());
    if (r2.ok) r2.level = levelToString(dualA) + "+" + levelToString(first.level);
    return r2;
  }
  Result res;
  Options opt = optIn;
  std::string wrapped;
  if (!data && size) {
    res.errorCode = "BAD_INPUT";
    res.error = "no input buffer supplied";
    return res;
  }
  try {
    if (!opt.preflightProfile.empty() && !opt.verifyOnly) applyProfileFixes(opt, res.analysis);
    if (opt.colorJob.enabled && !opt.verifyOnly) {
      const ColorJob& cj = opt.colorJob;
      std::string wanted = cj.destProfile.empty() && cj.deviceLink.empty() ? cj.destName : std::string();
      size_t before = res.analysis.size();
      prepareColorJob(opt, res.analysis);
      if (!wanted.empty() && opt.colorJob.destName.empty()) {
        res.errorCode = "ICC_PROFILE_MISSING";
        res.error = before < res.analysis.size() ? res.analysis[before].detail : "colour profile \"" + wanted + "\" was not found";
        return res;
      }
    }
    res.level = opt.noStandard ? std::string("none") : levelToString(opt.level);
    if (size > 4 && data[0] == 0xFF && data[1] == 0xD8) {
      wrapped = wrapJpegAsPdf(data, size);
      if (!wrapped.empty()) {
        data = reinterpret_cast<const unsigned char*>(wrapped.data());
        size = wrapped.size();
        res.issues.push_back({"IMAGE_INPUT_WRAPPED",
                              "JPEG input wrapped as a single-page PDF (300 dpi placement)",
                              true});
      }
    }
    QPDF pdf;
    pdf.setSuppressWarnings(true);
    pdf.setAttemptRecovery(true);
    pdf.processMemoryFile("input", reinterpret_cast<const char*>(data), size,
                          opt.password.c_str());
    QPDFObjectHandle trailer = pdf.getTrailer();
    if (!trailer.getKey("/Size").isInteger()) {
      trailer.replaceKey("/Size",
                         QPDFObjectHandle::newInteger(
                             static_cast<long long>(pdf.getObjectCount()) + 1));
    }
    std::shared_ptr<Buffer> normalized;
    QPDF decrypted;
    QPDF* active = &pdf;
    bool wasEncrypted = pdf.isEncrypted();
    bool encryptionAllowed = false;
    if (wasEncrypted && levelFamily(opt.level) == Family::PDFE) {
      QPDFObjectHandle enc = trailer.getKey("/Encrypt");
      QPDFObjectHandle r = enc.isDictionary() ? enc.getKey("/R") : QPDFObjectHandle::newNull();
      QPDFObjectHandle em = enc.isDictionary() ? enc.getKey("/EncryptMetadata") : QPDFObjectHandle::newNull();
      encryptionAllowed = nameIs(enc.getKey("/Filter"), "/Standard") && r.isInteger() && r.getIntValue() >= 2 &&
                          r.getIntValue() <= 4 && em.isBool() && !em.getBoolValue();
    }
    if (wasEncrypted) {
      QPDFWriter dw(pdf);
      dw.setOutputMemory();
      dw.setPreserveEncryption(false);
      dw.setDecodeLevel(qpdf_dl_none);
      dw.setCompressStreams(false);
      dw.setObjectStreamMode(qpdf_o_preserve);
      dw.write();
      normalized = dw.getBufferSharedPointer();
      decrypted.setSuppressWarnings(true);
      decrypted.setAttemptRecovery(true);
      decrypted.processMemoryFile("decrypted",
                                  reinterpret_cast<const char*>(normalized->getBuffer()),
                                  normalized->getSize(), "");
      QPDFObjectHandle dt = decrypted.getTrailer();
      if (!dt.getKey("/Size").isInteger()) {
        dt.replaceKey("/Size", QPDFObjectHandle::newInteger(
                                   static_cast<long long>(decrypted.getObjectCount()) + 1));
      }
      active = &decrypted;
    }
    Ctx ctx{*active, opt, res, levelPart(opt.level), levelConformance(opt.level),
            levelFamily(opt.level)};
    if (!wasEncrypted) {
      ctx.src = data;
      ctx.srcSize = size;
    }
    if (!opt.attachXml.empty()) {
      ctx.inv = detectInvoice(opt.attachXml, opt.facturxProfile, opt.attachXmlName);
    }
    if (!opt.verifyOnly && repairPageTree(*active)) {
      ctx.issue("PAGE_TREE_REBUILT", "rebuilt a damaged page tree as a single list of pages", true);
    }
    if (!opt.verifyOnly) {
      try {
        active->pushInheritedAttributesToPage();
      } catch (std::exception&) {
      }
    }
    if (opt.ua && !ctx.isA()) {
      res.errorCode = "UA_UNSUPPORTED_LEVEL";
      res.error = "PDF/UA layers only on PDF/A levels (UA-1 on parts 1-3, UA-2 on part 4)";
      res.suggestedLevel = "2u";
      return res;
    }
    bool externalReady = opt.level == Level::X5N && !opt.externalProfileUrl.empty() &&
                         opt.destProfile.size() >= 20 && opt.destProfile.substr(17, 3) == "CLR";
    if (levelVerifyOnly(opt.level) && !opt.verifyOnly && !externalReady) {
      res.errorCode = "LEVEL_VERIFY_ONLY";
      res.error = "this flavour relies on externally referenced press assets, so Kura checks "
                  "files against it but cannot produce one; convert to the self-contained "
                  "flavour instead";
      switch (opt.level) {
        case Level::X6N:
        case Level::X6P: res.suggestedLevel = "x6"; break;
        case Level::VT2: res.suggestedLevel = "vt1"; break;
        default: res.suggestedLevel = "x4"; break;
      }
      return res;
    }
    if (wasEncrypted) {
      ctx.issue("ENCRYPTION_REMOVED", "document was encrypted; encryption removed", !(opt.verifyOnly && encryptionAllowed));
    }
    {
      int maxDepth = 0;
      std::set<QPDFObjGen> path;
      std::vector<std::pair<QPDFObjectHandle, int>> stack;
      QPDFObjectHandle rootPages = active->getRoot().getKey("/Pages");
      if (rootPages.isDictionary()) stack.push_back({rootPages, 0});
      std::set<QPDFObjGen> seen;
      size_t visited = 0;
      while (!stack.empty()) {
        auto [node, d] = stack.back();
        stack.pop_back();
        if (d > maxDepth) maxDepth = d;
        if (maxDepth > 500 || ++visited > 5000000) {
          res.errorCode = "NESTING_TOO_DEEP";
          res.error = "document has pathologically nested resources or an object cycle "
                      "that cannot be safely processed; the file may be malformed or crafted";
          return res;
        }
        if (!node.isDictionary() && !node.isStream()) continue;
        QPDFObjectHandle dict = node.isStream() ? node.getDict() : node;
        if (node.isIndirect() && !seen.insert(node.getObjGen()).second) continue;
        QPDFObjectHandle kids = dict.getKey("/Kids");
        if (kids.isArray()) {
          for (int i = 0; i < kids.getArrayNItems(); ++i) {
            stack.push_back({kids.getArrayItem(i), d + 1});
          }
        }
        QPDFObjectHandle resrc = dict.getKey("/Resources");
        QPDFObjectHandle xo = resrc.isDictionary() ? resrc.getKey("/XObject")
                                                   : QPDFObjectHandle::newNull();
        if (xo.isDictionary()) {
          for (const std::string& k : xo.getKeys()) {
            QPDFObjectHandle x = xo.getKey(k);
            if (x.isStream() && x.getDict().getKey("/Subtype").isName() && x.getDict().getKey("/Subtype").getName() == "/Form") {
              stack.push_back({x, d + 1});
            }
          }
        }
      }
    }
    const bool timing = std::getenv("PDFA_TIMING") != nullptr;
    auto tmark = std::chrono::steady_clock::now();
    auto lap = [&](const char* name) {
      if (opt.trace) opt.trace(std::string("stage ") + name + " done");
      if (!timing) return;
      auto now = std::chrono::steady_clock::now();
      std::fprintf(stderr, "[timing] %-12s %.2fs\n", name,
                   std::chrono::duration<double>(now - tmark).count());
      tmark = now;
    };
    if (opt.analyze) passAnalyze(ctx);
    if (!opt.preflightProfile.empty()) passProfile(ctx, data, size);
    if (!opt.profileFixOps.empty() && !opt.verifyOnly) passProfileFixups(ctx);
    lap("analyze");
    const bool fullRun = !opt.noStandard;
    if (fullRun) passStructure(ctx);
    if (fullRun && opt.verifyOnly && (ctx.isA() || ctx.isX() || ctx.isE())) checkFileSyntax(ctx, data, size, wasEncrypted && !encryptionAllowed);
    lap("structure");
    if (fullRun && !ctx.failed()) fixIccChannelCounts(ctx);
    if (fullRun && !ctx.failed()) passPages(ctx);
    if (fullRun && !ctx.failed() && ctx.pdf14Target() && !opt.verifyOnly) {
      for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
        if (!obj.isStream() || !nameIs(obj.getDict().getKey("/Subtype"), "/Image")) continue;
        std::string f = obj.getDict().getKey("/Filter").unparse();
        if (f.find("/JPXDecode") != std::string::npos) transcodeJpxImage(ctx, obj, false);
      }
    }
    if (fullRun && !ctx.failed()) passImageResolution(ctx);
    lap("pages");
    if (fullRun && !ctx.failed() && !opt.verifyOnly) passOcr(ctx);
    lap("ocr");
    if (fullRun && !ctx.failed()) passCompleteResources(ctx);
    lap("resources");
    if (!ctx.failed()) passColorConvert(ctx);
    lap("colorconvert");
    if (fullRun && !ctx.failed()) passDowngradeIcc(ctx);
    if (fullRun && !ctx.failed()) passColor(ctx);
    lap("color");
    if (fullRun && !ctx.failed()) passPrint(ctx);
    lap("print");
    if (fullRun && !ctx.failed()) passDowngradeIcc(ctx);
    if (fullRun && !ctx.failed() && !opt.verifyOnly) passOutlineFonts(ctx);
    lap("outline");
    if (fullRun && !ctx.failed()) passFonts(ctx);
    lap("fonts");
    if (fullRun && !ctx.failed()) passGlyphClean(ctx);
    lap("glyphclean");
    if (fullRun && !ctx.failed()) passTagging(ctx);
    lap("tagging");
    if (!fullRun && !ctx.failed() && !opt.verifyOnly) applyVersionCompat(ctx, opt.pdfVersion);
    if (!ctx.failed() && !opt.verifyOnly && opt.signDocument) addSignaturePlaceholder(ctx);
    if (!ctx.failed() && !opt.verifyOnly) passBookmarks(ctx);
    if (fullRun && !ctx.failed()) passMetadata(ctx);
    lap("metadata");
    if (fullRun && !ctx.failed()) passLimits(ctx);
    lap("limits");
    if (!ctx.failed() && opt.verifyOnly && ctx.isX()) {
      bool damaged = false, noBox = false;
      for (const QPDFExc& w : active->getWarnings()) {
        std::string m = w.what();
        if (m.find("MediaBox is undefined") != std::string::npos) noBox = true;
        size_t at = m.find("expected ");
        if (at != std::string::npos && at + 9 < m.size() && std::isdigit(static_cast<unsigned char>(m[at + 9])) &&
            m.find(" obj", at) != std::string::npos)
          damaged = true;
      }
      if (noBox) ctx.issue("PAGE_BOXES_NORMALIZED", "a page has no usable MediaBox", true);
      if (damaged) {
        ctx.issue("FILE_STRUCTURE_NONCONFORMING",
                  "the cross-reference data is damaged and the file had to be repaired to be read", true);
      }
    }
    if (!ctx.failed() && !opt.verifyOnly) serialize(ctx);
    if (!ctx.failed() && ctx.inlineImagesFixed) {
      ctx.issue("INLINE_IMAGE_FILTER_FIXED",
                "re-encoded " + std::to_string(ctx.inlineImagesFixed) +
                    " inline image(s) with non-conforming filters",
                true);
    }
    if (!ctx.failed() && ctx.contentPuaFixed) {
      ctx.issue("ACTUALTEXT_PUA_REMOVED",
                "removed private-use-area characters from " +
                    std::to_string(ctx.contentPuaFixed) +
                    " ActualText value(s) in content streams",
                true);
    }
    if (!ctx.failed() && !opt.verifyOnly && opt.signDocument) {
      std::string signErr;
      if (!applySignature(opt, res.pdf, signErr)) {
        ctx.fatal("SIGN_FAILED", signErr);
      } else {
        ctx.issue("DOCUMENT_SIGNED",
                  "applied a PKCS#7 detached signature over the whole file", true);
      }
    }
    for (Issue& i : res.issues) {
      if (!i.severity) i.severity = issueSeverity(i.code);
    }
    for (Issue& i : res.analysis) {
      if (!i.severity) i.severity = issueSeverity(i.code);
    }
    res.ok = res.errorCode.empty();
    if (opt.verifyOnly && res.ok) {
      res.compliant = true;
      for (const Issue& i : res.issues) {
        if (i.fixed && !issueIsNormalization(i.code)) {
          res.compliant = false;
          break;
        }
      }
    }
    if (!res.ok && ctx.isA() && ctx.part == 1 &&
        (res.errorCode == "TRANSPARENCY_P1" || res.errorCode == "JPX_UNREADABLE" ||
         res.errorCode == "CMYK_MIXED_P1" || res.errorCode == "RGB_UNDER_CMYK_P1")) {
      res.suggestedLevel = ctx.conf == 'A' ? "2a" : "2b";
    }
    if (!res.ok && ctx.isX() &&
        (res.errorCode == "TRANSPARENCY_P1" || res.errorCode == "JPX_UNREADABLE" ||
         res.errorCode == "X1A_COLOR_UNCONVERTIBLE")) {
      res.suggestedLevel = "x4";
    }
  } catch (const std::exception& e) {
    res.ok = false;
    std::string msg = e.what();
    if (msg.find("invalid password") != std::string::npos ||
        msg.find("password") != std::string::npos) {
      res.errorCode = "PASSWORD_REQUIRED";
      res.error = opt.password.empty()
                      ? "document is password-protected; supply the open password to convert"
                      : "the supplied password did not open the document";
    } else if (msg.find("unsupported encryption filter") != std::string::npos) {
      std::string handler = detectSecurityHandler(data, size);
      if (handler == "EBX_HANDLER" || handler == "ADEPT") {
        res.errorCode = "ENCRYPTED_ADEPT";
        res.error =
            "document uses Adobe ADEPT (" + handler +
            ") encryption; the content key is bound to an Adobe account and is not present in "
            "the file, so it cannot be decrypted without the owner's ADEPT key material";
      } else {
        res.errorCode = "ENCRYPTED_UNSUPPORTED";
        res.error = "document uses a non-standard security handler" +
                    (handler.empty() ? std::string() : (" (" + handler + ")")) +
                    " that requires external key material not present in the file";
      }
    } else {
      res.errorCode = "PARSE_ERROR";
      res.error = msg;
    }
  } catch (...) {
    res.ok = false;
    res.errorCode = "INTERNAL_ERROR";
    res.error = "conversion aborted by an unrecognized error";
  }
  if (!res.ok) res.pdf.clear();
  return res;
}

bool verifyPassword(const unsigned char* data, std::size_t size, const std::string& password) {
  try {
    QPDF pdf;
    pdf.setSuppressWarnings(true);
    pdf.processMemoryFile("input", reinterpret_cast<const char*>(data), size, password.c_str());
    return true;
  } catch (const std::exception&) {
    return false;
  }
}
}
