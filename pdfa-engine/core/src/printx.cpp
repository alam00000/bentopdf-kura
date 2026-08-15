#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <algorithm>
#include <string>
#include <vector>

#include "ctx.hh"
#include "passes.hh"
#include "util.hh"

namespace pdfa {
namespace {
struct Box {
  double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  bool ok = false;
};

Box readBox(QPDFObjectHandle arr) {
  Box b;
  if (!arr.isArray() || arr.getArrayNItems() != 4) return b;
  double v[4];
  for (int i = 0; i < 4; ++i) {
    if (!arr.getArrayItem(i).isNumber()) return b;
    v[i] = arr.getArrayItem(i).getNumericValue();
  }
  b.x1 = std::min(v[0], v[2]);
  b.y1 = std::min(v[1], v[3]);
  b.x2 = std::max(v[0], v[2]);
  b.y2 = std::max(v[1], v[3]);
  b.ok = b.x2 > b.x1 && b.y2 > b.y1;
  return b;
}

QPDFObjectHandle boxArray(const Box& b) {
  QPDFObjectHandle a = QPDFObjectHandle::newArray();
  a.appendItem(QPDFObjectHandle::newReal(b.x1, 4));
  a.appendItem(QPDFObjectHandle::newReal(b.y1, 4));
  a.appendItem(QPDFObjectHandle::newReal(b.x2, 4));
  a.appendItem(QPDFObjectHandle::newReal(b.y2, 4));
  return a;
}

Box clampTo(const Box& inner, const Box& outer) {
  Box r;
  r.x1 = std::max(inner.x1, outer.x1);
  r.y1 = std::max(inner.y1, outer.y1);
  r.x2 = std::min(inner.x2, outer.x2);
  r.y2 = std::min(inner.y2, outer.y2);
  r.ok = r.x2 > r.x1 && r.y2 > r.y1;
  return r;
}

void fixPageBoxes(Ctx& ctx, QPDFPageObjectHelper& ph, bool& boxAdded, bool& boxFixed) {
  QPDFObjectHandle page = ph.getObjectHandle();
  Box media = readBox(ph.getAttribute("/MediaBox", true));
  if (!media.ok) {
    media = {0, 0, 612, 792, true};
    page.replaceKey("/MediaBox", boxArray(media));
    boxFixed = true;
  }
  Box crop = readBox(ph.getAttribute("/CropBox", true));
  Box effCrop = crop.ok ? clampTo(crop, media) : media;
  if (crop.ok && effCrop.ok &&
      (effCrop.x1 != crop.x1 || effCrop.y1 != crop.y1 || effCrop.x2 != crop.x2 ||
       effCrop.y2 != crop.y2)) {
    page.replaceKey("/CropBox", boxArray(effCrop));
    boxFixed = true;
  }
  Box trim = readBox(page.getKey("/TrimBox"));
  Box art = readBox(page.getKey("/ArtBox"));
  if (trim.ok && art.ok) {
    page.removeKey("/ArtBox");
    art.ok = false;
    boxFixed = true;
  }
  if (!trim.ok && !art.ok) {
    page.replaceKey("/TrimBox", boxArray(effCrop));
    trim = effCrop;
    boxAdded = true;
  } else if (trim.ok) {
    Box clamped = clampTo(trim, effCrop);
    if (clamped.ok &&
        (clamped.x1 != trim.x1 || clamped.y1 != trim.y1 || clamped.x2 != trim.x2 ||
         clamped.y2 != trim.y2)) {
      page.replaceKey("/TrimBox", boxArray(clamped));
      trim = clamped;
      boxFixed = true;
    }
  } else if (art.ok) {
    Box clamped = clampTo(art, effCrop);
    if (clamped.ok && (clamped.x1 != art.x1 || clamped.y1 != art.y1 || clamped.x2 != art.x2 ||
                       clamped.y2 != art.y2)) {
      page.replaceKey("/ArtBox", boxArray(clamped));
      boxFixed = true;
    }
  }
  Box bleed = readBox(page.getKey("/BleedBox"));
  if (bleed.ok) {
    Box clamped = clampTo(bleed, effCrop);
    Box target = trim.ok ? trim : art;
    if (target.ok) {
      clamped.x1 = std::min(clamped.x1, target.x1);
      clamped.y1 = std::min(clamped.y1, target.y1);
      clamped.x2 = std::max(clamped.x2, target.x2);
      clamped.y2 = std::max(clamped.y2, target.y2);
    }
    if (clamped.ok && (clamped.x1 != bleed.x1 || clamped.y1 != bleed.y1 ||
                       clamped.x2 != bleed.x2 || clamped.y2 != bleed.y2)) {
      page.replaceKey("/BleedBox", boxArray(clamped));
      boxFixed = true;
    }
  }
}

std::vector<std::pair<int, int>> parseRecords(const std::string& spec, int pageCount) {
  std::vector<std::pair<int, int>> out;
  size_t i = 0;
  while (i < spec.size()) {
    size_t comma = spec.find(',', i);
    std::string part = spec.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
    i = comma == std::string::npos ? spec.size() : comma + 1;
    if (part.empty()) continue;
    size_t dash = part.find('-');
    int lo = 0, hi = 0;
    try {
      if (dash == std::string::npos) {
        lo = hi = std::stoi(part);
      } else {
        lo = std::stoi(part.substr(0, dash));
        std::string hiStr = part.substr(dash + 1);
        hi = hiStr.empty() ? pageCount : std::stoi(hiStr);
      }
    } catch (...) {
      return {};
    }
    if (lo < 1 || hi < lo || hi > pageCount) return {};
    out.push_back({lo, hi});
  }
  int expect = 1;
  for (auto& r : out) {
    if (r.first != expect) return {};
    expect = r.second + 1;
  }
  if (!out.empty() && expect != pageCount + 1) return {};
  return out;
}

void buildDPartTree(Ctx& ctx) {
  QPDFPageDocumentHelper dh(ctx.pdf);
  std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
  int n = static_cast<int>(pages.size());
  if (n == 0) return;
  std::vector<std::pair<int, int>> records;
  if (!ctx.opt.vtRecords.empty()) {
    records = parseRecords(ctx.opt.vtRecords, n);
    if (records.empty()) {
      ctx.issue("VT_RECORDS_INVALID",
                "vtRecords did not parse as contiguous 1-based page ranges covering the "
                "document; falling back to a single document part",
                false);
    }
  }
  if (records.empty()) records.push_back({1, n});

  QPDFObjectHandle rootDPart = QPDFObjectHandle::newDictionary();
  rootDPart.replaceKey("/Type", QPDFObjectHandle::newName("/DPart"));
  QPDFObjectHandle rootRef = ctx.pdf.makeIndirectObject(rootDPart);

  QPDFObjectHandle dpartRoot = QPDFObjectHandle::newDictionary();
  dpartRoot.replaceKey("/Type", QPDFObjectHandle::newName("/DPartRoot"));
  dpartRoot.replaceKey("/DPartRootNode", rootRef);
  QPDFObjectHandle nodeNames = QPDFObjectHandle::newArray();
  nodeNames.appendItem(QPDFObjectHandle::newName("/Root"));
  nodeNames.appendItem(QPDFObjectHandle::newName("/Record"));
  dpartRoot.replaceKey("/NodeNameList", nodeNames);
  QPDFObjectHandle dpartRootRef = ctx.pdf.makeIndirectObject(dpartRoot);
  rootDPart.replaceKey("/Parent", dpartRootRef);

  QPDFObjectHandle leaves = QPDFObjectHandle::newArray();
  for (auto& rec : records) {
    QPDFObjectHandle leaf = QPDFObjectHandle::newDictionary();
    leaf.replaceKey("/Type", QPDFObjectHandle::newName("/DPart"));
    leaf.replaceKey("/Parent", rootRef);
    leaf.replaceKey("/Start", pages[rec.first - 1].getObjectHandle());
    if (rec.second != rec.first) {
      leaf.replaceKey("/End", pages[rec.second - 1].getObjectHandle());
    }
    QPDFObjectHandle leafRef = ctx.pdf.makeIndirectObject(leaf);
    leaves.appendItem(leafRef);
    for (int p = rec.first; p <= rec.second; ++p) {
      pages[p - 1].getObjectHandle().replaceKey("/DPart", leafRef);
    }
  }
  QPDFObjectHandle dparts = QPDFObjectHandle::newArray();
  dparts.appendItem(leaves);
  rootDPart.replaceKey("/DParts", dparts);
  ctx.pdf.getRoot().replaceKey("/DPartRoot", dpartRootRef);
  ctx.issue(records.size() == 1 && ctx.opt.vtRecords.empty() ? "VT_SINGLE_PART"
                                                             : "VT_DPART_BUILT",
            "built document part hierarchy with " + std::to_string(records.size()) +
                " record part(s)" +
                (records.size() == 1 && ctx.opt.vtRecords.empty()
                     ? " (no record structure supplied; whole document is one part)"
                     : ""),
            true);
}
}

namespace {
void unifySeparations(Ctx& ctx) {
  std::map<std::string, QPDFObjectHandle> first;
  std::vector<std::pair<QPDFObjectHandle, std::string>> dups;
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    if (!obj.isArray() || obj.getArrayNItems() < 4 ||
        !nameIs(obj.getArrayItem(0), "/Separation")) {
      continue;
    }
    std::string name = nameOf(obj.getArrayItem(1));
    if (name.size() < 2 || name == "/None") continue;
    auto it = first.find(name);
    if (it == first.end()) {
      first[name] = obj;
    } else if (obj.getObjGen() != it->second.getObjGen()) {
      dups.push_back({obj, name});
    }
  }
  int unified = 0;
  for (auto& [obj, name] : dups) {
    QPDFObjectHandle ref = first[name];
    std::string a = obj.unparseResolved();
    std::string b = ref.unparseResolved();
    if (a == b) continue;
    while (obj.getArrayNItems() > 0) obj.eraseItem(obj.getArrayNItems() - 1);
    for (int i = 0; i < ref.getArrayNItems(); ++i) obj.appendItem(ref.getArrayItem(i));
    ++unified;
  }
  if (unified) {
    ctx.issue("SEPARATION_UNIFIED",
              "unified " + std::to_string(unified) +
                  " inconsistent representation(s) of same-name separation colorants",
              true);
  }
}
}

void passPrint(Ctx& ctx) {
  if (!ctx.isX()) return;
  unifySeparations(ctx);
  QPDFPageDocumentHelper dh(ctx.pdf);
  bool boxAdded = false, boxFixed = false;
  for (auto& ph : dh.getAllPages()) {
    fixPageBoxes(ctx, ph, boxAdded, boxFixed);
  }
  if (boxAdded) {
    ctx.issue("TRIMBOX_ADDED",
              "added /TrimBox (= effective CropBox) to pages lacking TrimBox/ArtBox", true);
  }
  if (boxFixed) {
    ctx.issue("PAGE_BOXES_NORMALIZED",
              "normalized page box nesting (Trim/Art/Bleed within MediaBox; single "
              "TrimBox|ArtBox per page)",
              true);
  }
  if (ctx.isVT()) buildDPartTree(ctx);
}
}
