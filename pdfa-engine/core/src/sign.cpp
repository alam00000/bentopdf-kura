#include "sign.hh"

#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <cstdio>
#include <cstring>

namespace pdfa {
namespace {
const char* kPlaceholderMark = "/Contents <";

std::string zeros(std::size_t n) { return std::string(n * 2, '0'); }
}

void addSignaturePlaceholder(Ctx& ctx) {
  QPDFObjectHandle root = ctx.pdf.getRoot();
  QPDFPageDocumentHelper dh(ctx.pdf);
  std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
  if (pages.empty()) {
    ctx.fatal("SIGN_NO_PAGES", "cannot place a signature field in a document with no pages");
    return;
  }

  QPDFObjectHandle sig = QPDFObjectHandle::newDictionary();
  sig.replaceKey("/Type", QPDFObjectHandle::newName("/Sig"));
  sig.replaceKey("/Filter", QPDFObjectHandle::newName("/Adobe.PPKLite"));
  sig.replaceKey("/SubFilter", QPDFObjectHandle::newName("/adbe.pkcs7.detached"));
  sig.replaceKey("/ByteRange",
                 QPDFObjectHandle::parse("[0 9999999999 9999999999 9999999999]"));
  sig.replaceKey("/Contents",
                 QPDFObjectHandle::newString(std::string(ctx.opt.signReserveBytes, '\0')));
  if (!ctx.opt.signName.empty()) {
    sig.replaceKey("/Name", QPDFObjectHandle::newUnicodeString(ctx.opt.signName));
  }
  if (!ctx.opt.signReason.empty()) {
    sig.replaceKey("/Reason", QPDFObjectHandle::newUnicodeString(ctx.opt.signReason));
  }
  if (!ctx.opt.signLocation.empty()) {
    sig.replaceKey("/Location", QPDFObjectHandle::newUnicodeString(ctx.opt.signLocation));
  }
  if (!ctx.opt.signContactInfo.empty()) {
    sig.replaceKey("/ContactInfo", QPDFObjectHandle::newUnicodeString(ctx.opt.signContactInfo));
  }
  if (!ctx.opt.nowOverride.empty()) {
    sig.replaceKey("/M", QPDFObjectHandle::newString(ctx.opt.nowOverride));
  }
  QPDFObjectHandle sigRef = ctx.pdf.makeIndirectObject(sig);

  QPDFObjectHandle field = QPDFObjectHandle::newDictionary();
  field.replaceKey("/Type", QPDFObjectHandle::newName("/Annot"));
  field.replaceKey("/Subtype", QPDFObjectHandle::newName("/Widget"));
  field.replaceKey("/FT", QPDFObjectHandle::newName("/Sig"));
  field.replaceKey("/T", QPDFObjectHandle::newUnicodeString("Signature1"));
  field.replaceKey("/V", sigRef);
  field.replaceKey("/Rect", QPDFObjectHandle::parse("[0 0 0 0]"));
  field.replaceKey("/F", QPDFObjectHandle::newInteger(132));
  QPDFObjectHandle page = pages[0].getObjectHandle();
  field.replaceKey("/P", page);
  QPDFObjectHandle blank = QPDFObjectHandle::newStream(&ctx.pdf, std::string());
  QPDFObjectHandle bd = blank.getDict();
  bd.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
  bd.replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
  bd.replaceKey("/BBox", QPDFObjectHandle::parse("[0 0 0 0]"));
  bd.replaceKey("/Resources", QPDFObjectHandle::newDictionary());
  QPDFObjectHandle ap = QPDFObjectHandle::newDictionary();
  ap.replaceKey("/N", ctx.pdf.makeIndirectObject(blank));
  field.replaceKey("/AP", ap);
  QPDFObjectHandle fieldRef = ctx.pdf.makeIndirectObject(field);

  QPDFObjectHandle annots = page.getKey("/Annots");
  if (!annots.isArray()) {
    annots = QPDFObjectHandle::newArray();
    page.replaceKey("/Annots", annots);
  }
  annots.appendItem(fieldRef);

  QPDFObjectHandle acro = root.getKey("/AcroForm");
  if (!acro.isDictionary()) {
    acro = QPDFObjectHandle::newDictionary();
    acro.replaceKey("/Fields", QPDFObjectHandle::newArray());
    root.replaceKey("/AcroForm", ctx.pdf.makeIndirectObject(acro));
    acro = root.getKey("/AcroForm");
  }
  QPDFObjectHandle fields = acro.getKey("/Fields");
  if (!fields.isArray()) {
    fields = QPDFObjectHandle::newArray();
    acro.replaceKey("/Fields", fields);
  }
  fields.appendItem(fieldRef);
  acro.replaceKey("/SigFlags", QPDFObjectHandle::newInteger(3));
}

bool applySignature(const Options& opt, std::vector<unsigned char>& pdf, std::string& err) {
  std::string body(reinterpret_cast<const char*>(pdf.data()), pdf.size());
  size_t c = body.find(kPlaceholderMark);
  if (c == std::string::npos) {
    err = "signature placeholder not found in the written file";
    return false;
  }
  size_t hexStart = c + std::strlen(kPlaceholderMark);
  size_t hexEnd = body.find('>', hexStart);
  if (hexEnd == std::string::npos) {
    err = "signature placeholder is malformed";
    return false;
  }

  size_t gapStart = hexStart - 1;
  size_t gapEnd = hexEnd + 1;
  long long r0 = 0;
  long long r1 = static_cast<long long>(gapStart);
  long long r2 = static_cast<long long>(gapEnd);
  long long r3 = static_cast<long long>(body.size()) - r2;

  size_t br = body.rfind("/ByteRange", c);
  if (br == std::string::npos) br = body.find("/ByteRange");
  if (br == std::string::npos) {
    err = "ByteRange placeholder not found";
    return false;
  }
  size_t brOpen = body.find('[', br);
  size_t brClose = body.find(']', brOpen);
  if (brOpen == std::string::npos || brClose == std::string::npos) {
    err = "ByteRange placeholder is malformed";
    return false;
  }
  char filled[128];
  std::snprintf(filled, sizeof(filled), "[%lld %lld %lld %lld]", r0, r1, r2, r3);
  std::string fill(filled);
  size_t span = brClose - brOpen + 1;
  if (fill.size() > span) {
    err = "ByteRange does not fit the reserved space";
    return false;
  }
  fill.append(span - fill.size(), ' ');
  body.replace(brOpen, span, fill);

  std::string signedData = body.substr(0, static_cast<size_t>(r1)) +
                           body.substr(static_cast<size_t>(r2), static_cast<size_t>(r3));
  std::string der;
  if (!opt.signDocument(signedData, der) || der.empty()) {
    err = "the signing callback produced no signature";
    return false;
  }
  size_t capacity = hexEnd - hexStart;
  if (der.size() * 2 > capacity) {
    err = "signature is larger than the reserved space; raise signReserveBytes";
    return false;
  }
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(capacity);
  for (unsigned char ch : der) {
    out += hex[ch >> 4];
    out += hex[ch & 0x0F];
  }
  out.append(capacity - out.size(), '0');
  body.replace(hexStart, capacity, out);

  pdf.assign(body.begin(), body.end());
  return true;
}
}
