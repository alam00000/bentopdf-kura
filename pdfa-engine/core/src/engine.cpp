#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <cstdio>

#include <chrono>
#include <set>
#include <cstring>
#include <exception>

#include "ctx.hh"
#include "passes.hh"
#include "ocr.hh"
#include "sign.hh"
#include "pdfa/pdfa.hh"
#include "util.hh"

namespace pdfa {
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
  if (ctx.pdf14Target()) {
    w.setObjectStreamMode(qpdf_o_disable);
    w.forcePDFVersion("1.4");
  } else if (!ctx.isA()) {
    w.setObjectStreamMode(signing ? qpdf_o_disable : qpdf_o_generate);
    w.forcePDFVersion(ctx.pdf20Print() ? "2.0" : "1.6");
  } else {
    w.setObjectStreamMode(signing ? qpdf_o_disable : qpdf_o_generate);
    w.forcePDFVersion(ctx.part >= 4 ? "2.0" : "1.7");
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

bool issueIsNormalization(const std::string& code) {
  return code == "XMP_REBUILT" || code == "CONTENT_FILTERED" ||
         code == "OUTPUT_INTENT_PRESENT";
}

Result convert(const unsigned char* data, std::size_t size, const Options& optIn) {
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
    if (!opt.attachXml.empty()) {
      ctx.inv = detectInvoice(opt.attachXml, opt.facturxProfile, opt.attachXmlName);
    }
    if (opt.ua && !ctx.isA()) {
      res.errorCode = "UA_UNSUPPORTED_LEVEL";
      res.error = "PDF/UA layers only on PDF/A levels (UA-1 on parts 1-3, UA-2 on part 4)";
      res.suggestedLevel = "2u";
      return res;
    }
    if (levelVerifyOnly(opt.level) && !opt.verifyOnly) {
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
      ctx.issue("ENCRYPTION_REMOVED", "document was encrypted; encryption removed", true);
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
    passStructure(ctx);
    lap("structure");
    if (!ctx.failed()) passPages(ctx);
    if (!ctx.failed()) passImageResolution(ctx);
    lap("pages");
    if (!ctx.failed() && !opt.verifyOnly) passOcr(ctx);
    lap("ocr");
    if (!ctx.failed()) passCompleteResources(ctx);
    lap("resources");
    if (!ctx.failed()) passColor(ctx);
    lap("color");
    if (!ctx.failed()) passPrint(ctx);
    lap("print");
    if (!ctx.failed() && !opt.verifyOnly) passOutlineFonts(ctx);
    lap("outline");
    if (!ctx.failed()) passFonts(ctx);
    lap("fonts");
    if (!ctx.failed()) passGlyphClean(ctx);
    lap("glyphclean");
    if (!ctx.failed()) passTagging(ctx);
    lap("tagging");
    if (!ctx.failed() && !opt.verifyOnly && opt.signDocument) addSignaturePlaceholder(ctx);
    if (!ctx.failed()) passMetadata(ctx);
    lap("metadata");
    if (!ctx.failed()) passLimits(ctx);
    lap("limits");
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
        (res.errorCode == "TRANSPARENCY_P1" || res.errorCode == "JPX_IN_PDFA1" ||
         res.errorCode == "CMYK_MIXED_P1" || res.errorCode == "RGB_UNDER_CMYK_P1")) {
      res.suggestedLevel = ctx.conf == 'A' ? "2a" : "2b";
    }
    if (!res.ok && ctx.isX() &&
        (res.errorCode == "TRANSPARENCY_P1" || res.errorCode == "JPX_IN_PDFA1" ||
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
}
