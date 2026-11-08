#include <cmath>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
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
  a.appendItem(QPDFObjectHandle::newReal(b.x1, 6));
  a.appendItem(QPDFObjectHandle::newReal(b.y1, 6));
  a.appendItem(QPDFObjectHandle::newReal(b.x2, 6));
  a.appendItem(QPDFObjectHandle::newReal(b.y2, 6));
  return a;
}

bool sameBox(const Box& a, const Box& b) {
  const double eps = 1e-9;
  return std::fabs(a.x1 - b.x1) < eps && std::fabs(a.y1 - b.y1) < eps && std::fabs(a.x2 - b.x2) < eps &&
         std::fabs(a.y2 - b.y2) < eps;
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
  if (crop.ok && !effCrop.ok) effCrop = media;
  if (crop.ok && !sameBox(effCrop, crop)) {
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
  if (!trim.ok && art.ok && ctx.pdf20Print()) {
    page.replaceKey("/TrimBox", page.getKey("/ArtBox"));
    page.removeKey("/ArtBox");
    trim = art;
    art.ok = false;
    boxFixed = true;
  }
  if (!trim.ok && !art.ok) {
    page.replaceKey("/TrimBox", boxArray(effCrop));
    trim = effCrop;
    boxAdded = true;
  } else if (trim.ok) {
    Box clamped = clampTo(trim, effCrop);
    if (!clamped.ok) clamped = effCrop;
    if (!sameBox(clamped, trim)) {
      page.replaceKey("/TrimBox", boxArray(clamped));
      trim = clamped;
      boxFixed = true;
    }
  } else if (art.ok) {
    Box clamped = clampTo(art, effCrop);
    if (!clamped.ok) clamped = effCrop;
    if (!sameBox(clamped, art)) {
      page.replaceKey("/ArtBox", boxArray(clamped));
      art = clamped;
      boxFixed = true;
    }
  }
  Box bleed = readBox(page.getKey("/BleedBox"));
  if (bleed.ok) {
    Box clamped = clampTo(bleed, effCrop);
    if (!clamped.ok) clamped = effCrop;
    Box target = trim.ok ? trim : art;
    if (target.ok) {
      clamped.x1 = std::min(clamped.x1, target.x1);
      clamped.y1 = std::min(clamped.y1, target.y1);
      clamped.x2 = std::max(clamped.x2, target.x2);
      clamped.y2 = std::max(clamped.y2, target.y2);
    }
    if (!sameBox(clamped, bleed)) {
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

bool nameToken(const std::string& name) {
  if (name.size() < 2) return false;
  for (size_t i = 1; i < name.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(name[i]);
    if (!std::isalnum(c) && c != '.' && c != '-' && c != '_' && c != ':') return false;
  }
  return true;
}

std::vector<QPDFObjectHandle> childParts(QPDFObjectHandle node, bool& wellFormed) {
  std::vector<QPDFObjectHandle> out;
  QPDFObjectHandle groups = node.getKey("/DParts");
  if (!groups.isArray() || groups.getArrayNItems() == 0) {
    wellFormed = false;
    return out;
  }
  for (int i = 0; i < groups.getArrayNItems(); ++i) {
    QPDFObjectHandle g = groups.getArrayItem(i);
    if (!g.isArray() || g.getArrayNItems() == 0) {
      wellFormed = false;
      return out;
    }
    for (int j = 0; j < g.getArrayNItems(); ++j) out.push_back(g.getArrayItem(j));
  }
  return out;
}

std::string dpartTreeProblem(QPDF& pdf, std::vector<QPDFPageObjectHelper>& pages) {
  QPDFObjectHandle root = pdf.getRoot().getKey("/DPartRoot");
  if (!root.isDictionary()) return "the document has no document part root";
  if (root.hasKey("/Type") && !nameIs(root.getKey("/Type"), "/DPartRoot"))
    return "the document part root has the wrong /Type";
  QPDFObjectHandle top = root.getKey("/DPartRootNode");
  if (!top.isDictionary() || !top.isIndirect()) return "the document part root has no root node";
  if (top.hasKey("/Type") && !nameIs(top.getKey("/Type"), "/DPart")) return "the root document part has the wrong /Type";
  QPDFObjectHandle names = root.getKey("/NodeNameList");
  if (!names.isArray() || names.getArrayNItems() == 0) return "the part level names are missing";
  for (int i = 0; i < names.getArrayNItems(); ++i) {
    if (!names.getArrayItem(i).isName()) return "a part level name is not a name";
  }
  std::map<QPDFObjGen, int> pageIndex;
  for (size_t i = 0; i < pages.size(); ++i) pageIndex[pages[i].getObjectHandle().getObjGen()] = static_cast<int>(i);
  std::set<QPDFObjGen> seen;
  std::vector<std::pair<QPDFObjectHandle, std::pair<int, int>>> leaves;
  std::string problem;
  std::function<void(QPDFObjectHandle, QPDFObjectHandle, int)> walk = [&](QPDFObjectHandle n, QPDFObjectHandle parent, int depth) {
    if (!problem.empty()) return;
    if (!n.isDictionary() || !n.isIndirect()) { problem = "a document part is not an indirect dictionary"; return; }
    if (!seen.insert(n.getObjGen()).second) { problem = "a document part is listed under more than one parent"; return; }
    if (n.hasKey("/Type") && !nameIs(n.getKey("/Type"), "/DPart")) { problem = "a document part has the wrong /Type"; return; }
    QPDFObjectHandle p = n.getKey("/Parent");
    if (!p.isIndirect() || p.getObjGen() != parent.getObjGen()) { problem = "a document part does not point back to its parent"; return; }
    bool kids = n.hasKey("/DParts"), start = n.hasKey("/Start");
    if (kids == start) { problem = "a document part must have either child parts or a start page"; return; }
    if (n.hasKey("/End") && !start) { problem = "a document part has an end page but no start page"; return; }
    if (start) {
      if (depth + 1 != names.getArrayNItems()) { problem = "the part level names do not match the depth of the hierarchy"; return; }
      QPDFObjectHandle sp = n.getKey("/Start"), ep = n.hasKey("/End") ? n.getKey("/End") : sp;
      auto si = sp.isIndirect() ? pageIndex.find(sp.getObjGen()) : pageIndex.end();
      auto ei = ep.isIndirect() ? pageIndex.find(ep.getObjGen()) : pageIndex.end();
      if (si == pageIndex.end() || ei == pageIndex.end() || ei->second < si->second) { problem = "a document part does not span a valid page range"; return; }
      leaves.push_back({n, {si->second, ei->second}});
      return;
    }
    bool ok = true;
    std::vector<QPDFObjectHandle> kidsList = childParts(n, ok);
    if (!ok) { problem = "a document part lists its child parts in the wrong form"; return; }
    for (QPDFObjectHandle k : kidsList) walk(k, n, depth + 1);
  };
  QPDFObjectHandle tp = top.getKey("/Parent");
  if (!tp.isIndirect() || !root.isIndirect() || tp.getObjGen() != root.getObjGen()) return "the root document part does not point back to the part root";
  seen.insert(top.getObjGen());
  if (!top.hasKey("/DParts") || top.hasKey("/Start")) return "the root document part has no child parts";
  bool ok = true;
  for (QPDFObjectHandle k : childParts(top, ok)) walk(k, top, 1);
  if (!ok) return "the root document part lists its child parts in the wrong form";
  if (!problem.empty()) return problem;
  int expect = 0;
  for (auto& [leaf, range] : leaves) {
    if (range.first != expect) return "the document parts do not cover the pages in order";
    for (int i = range.first; i <= range.second; ++i) {
      QPDFObjectHandle dp = pages[i].getObjectHandle().getKey("/DPart");
      if (!dp.isIndirect() || dp.getObjGen() != leaf.getObjGen()) return "a page does not point at the document part that contains it";
    }
    expect = range.second + 1;
  }
  if (expect != static_cast<int>(pages.size())) return "the document parts do not cover every page";
  QPDFObjectHandle rl = root.getKey("/RecordLevel");
  if (root.hasKey("/RecordLevel") && (!rl.isInteger() || rl.getIntValue() < 0 || rl.getIntValue() >= names.getArrayNItems()))
    return "the record level is outside the hierarchy";
  return "";
}

void fixPartMetadata(Ctx& ctx) {
  int fixed = 0;
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    if (!obj.isDictionary() || !obj.getKey("/DPM").isDictionary()) continue;
    if (!obj.hasKey("/Parent") || (!obj.hasKey("/DParts") && !obj.hasKey("/Start"))) continue;
    QPDFObjectHandle dpm = obj.getKey("/DPM");
    for (const std::string& k : dpm.getKeys()) {
      if (!nameToken(k)) { dpm.removeKey(k); ++fixed; }
    }
    QPDFObjectHandle managed = dpm.getKey("/GTS_Managed"), suspect = dpm.getKey("/GTS_Suspect");
    if (dpm.hasKey("/GTS_Managed") && !managed.isDictionary()) { dpm.removeKey("/GTS_Managed"); ++fixed; }
    if (dpm.hasKey("/GTS_Suspect") && !suspect.isDictionary()) { dpm.removeKey("/GTS_Suspect"); ++fixed; }
    managed = dpm.getKey("/GTS_Managed");
    suspect = dpm.getKey("/GTS_Suspect");
    if (managed.isDictionary() && suspect.isDictionary()) {
      for (const std::string& k : suspect.getKeys()) {
        if (managed.hasKey(k)) { suspect.removeKey(k); ++fixed; }
      }
    }
  }
  if (fixed) {
    ctx.issue("VT_PART_METADATA_FIXED",
              "removed " + std::to_string(fixed) + " invalid entr(ies) from document part metadata", true);
  }
}

void buildDPartTree(Ctx& ctx) {
  QPDFPageDocumentHelper dh(ctx.pdf);
  std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
  int n = static_cast<int>(pages.size());
  if (n == 0) return;
  if (ctx.pdf.getRoot().hasKey("/DPartRoot")) {
    std::string problem = dpartTreeProblem(ctx.pdf, pages);
    if (problem.empty()) {
      fixPartMetadata(ctx);
      ctx.issue("VT_DPART_PRESENT", "kept existing document part hierarchy", false);
      return;
    }
    ctx.issue("VT_DPART_REBUILT", "replaced the document part hierarchy: " + problem, true);
  }
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
  dpartRoot.replaceKey("/RecordLevel", QPDFObjectHandle::newInteger(1));
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
  QPDFObjectHandle groups = QPDFObjectHandle::newArray();
  groups.appendItem(leaves);
  rootDPart.replaceKey("/DParts", groups);
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

namespace {
class StateUse : public QPDFObjectHandle::ParserCallbacks {
 public:
  bool fill = false, stroke = false, width = false, state = false, unset = false, layers = false;
  bool needState = true;
  void handleObject(QPDFObjectHandle obj) override {
    if (!obj.isOperator()) {
      if (obj.isName() && obj.getName() == "/OC") layers = true;
      return;
    }
    const std::string op = obj.getOperatorValue();
    if (op == "g" || op == "rg" || op == "k" || op == "cs" || op == "sc" || op == "scn") fill = true;
    else if (op == "G" || op == "RG" || op == "K" || op == "CS" || op == "SC" || op == "SCN") stroke = true;
    else if (op == "w") width = true;
    else if (op == "gs") state = true;
    else if (op == "f" || op == "F" || op == "f*" || op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
      if (!fill || (needState && !state)) unset = true;
    } else if (op == "S" || op == "s") {
      if (!stroke || !width || (needState && !state)) unset = true;
    } else if (op == "B" || op == "B*" || op == "b" || op == "b*") {
      if (!fill || !stroke || !width || (needState && !state)) unset = true;
    } else if (op == "sh" || op == "Do") {
      if (needState && !state) unset = true;
    }
  }
  void handleEOF() override {}
};

bool formUsesTransparency(QPDFObjectHandle d) {
  QPDFObjectHandle g = d.getKey("/Group");
  if (g.isDictionary() && nameIs(g.getKey("/S"), "/Transparency")) return true;
  QPDFObjectHandle gs = d.getKey("/Resources").isDictionary() ? d.getKey("/Resources").getKey("/ExtGState") : QPDFObjectHandle::newNull();
  if (!gs.isDictionary()) return false;
  for (const std::string& k : gs.getKeys()) {
    QPDFObjectHandle e = gs.getKey(k);
    if (!e.isDictionary()) continue;
    if (e.getKey("/SMask").isDictionary()) return true;
    if (numOf(e.getKey("/ca"), 1) < 1 || numOf(e.getKey("/CA"), 1) < 1) return true;
    QPDFObjectHandle bm = e.getKey("/BM");
    if (bm.isName() && bm.getName() != "/Normal" && bm.getName() != "/Compatible") return true;
  }
  return false;
}

void collectXObjectUse(QPDFObjectHandle res, int page, std::set<QPDFObjGen>& visited,
                       std::map<QPDFObjGen, int>& refs, std::map<QPDFObjGen, std::set<int>>& pagesOf, int depth) {
  if (depth > 32 || !res.isDictionary()) return;
  QPDFObjectHandle xo = res.getKey("/XObject");
  if (!xo.isDictionary()) return;
  for (const std::string& k : xo.getKeys()) {
    QPDFObjectHandle x = xo.getKey(k);
    if (!x.isStream() || !x.isIndirect()) continue;
    ++refs[x.getObjGen()];
    pagesOf[x.getObjGen()].insert(page);
    if (nameIs(x.getDict().getKey("/Subtype"), "/Form") && visited.insert(x.getObjGen()).second) {
      collectXObjectUse(x.getDict().getKey("/Resources"), page, visited, refs, pagesOf, depth + 1);
    }
  }
}

QPDFObjectHandle recordOf(QPDFObjectHandle leaf, QPDFObjectHandle top, int level) {
  std::vector<QPDFObjectHandle> path;
  for (QPDFObjectHandle n = leaf; n.isDictionary() && path.size() < 64; n = n.getKey("/Parent")) {
    path.push_back(n);
    if (n.isIndirect() && top.isIndirect() && n.getObjGen() == top.getObjGen()) break;
  }
  std::reverse(path.begin(), path.end());
  if (level < 0 || level >= static_cast<int>(path.size())) return QPDFObjectHandle::newNull();
  return path[level];
}

void fixVtXObjects(Ctx& ctx) {
  QPDFPageDocumentHelper dh(ctx.pdf);
  std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
  std::map<QPDFObjGen, int> refs;
  std::map<QPDFObjGen, std::set<int>> pagesOf;
  for (size_t i = 0; i < pages.size(); ++i) {
    std::set<QPDFObjGen> visited;
    collectXObjectUse(pages[i].getAttribute("/Resources", false), static_cast<int>(i), visited, refs, pagesOf, 0);
  }
  bool vt3 = ctx.opt.level == Level::VT3;
  QPDFObjectHandle root = ctx.pdf.getRoot().getKey("/DPartRoot");
  QPDFObjectHandle rl = root.isDictionary() ? root.getKey("/RecordLevel") : QPDFObjectHandle::newNull();
  int recordLevel = rl.isInteger() ? static_cast<int>(rl.getIntValue()) : 0;
  QPDFObjectHandle top = root.isDictionary() ? root.getKey("/DPartRootNode") : QPDFObjectHandle::newNull();
  int scopes = 0, encaps = 0, intents = 0;
  for (QPDFObjectHandle obj : ctx.pdf.getAllObjects()) {
    if (!obj.isStream()) continue;
    QPDFObjectHandle d = obj.getDict();
    if (d.hasKey("/GTS_Env") && !d.getKey("/GTS_Env").isString()) {
      d.removeKey("/GTS_Env");
      ++scopes;
    }
    if (d.hasKey("/GTS_Scope")) {
      QPDFObjectHandle sc = d.getKey("/GTS_Scope");
      std::string v = sc.isName() ? sc.getName() : "";
      bool keep = v == "/SingleUse" || v == "/Record" || v == "/File" || (!vt3 && (v == "/Stream" || v == "/Global"));
      QPDFObjGen og = obj.getObjGen();
      if (keep && v == "/SingleUse" && refs[og] > 1) keep = false;
      if (keep && !vt3 && (v == "/Stream" || v == "/Global") && !d.getKey("/GTS_Env").isString()) keep = false;
      if (keep && v == "/Record" && (vt3 || !ctx.opt.verifyOnly)) {
        if (recordLevel <= 0) keep = false;
        std::set<QPDFObjGen> records;
        for (int pi : pagesOf[og]) {
          QPDFObjectHandle rec = recordOf(pages[pi].getObjectHandle().getKey("/DPart"), top, recordLevel);
          if (rec.isIndirect()) records.insert(rec.getObjGen());
        }
        if (records.size() > 1) keep = false;
      }
      if (!keep) {
        d.removeKey("/GTS_Scope");
        d.removeKey("/GTS_Env");
        ++scopes;
      }
    }
    QPDFObjectHandle enc = d.getKey("/GTS_Encapsulated");
    if (enc.isBool() && enc.getBoolValue() && nameIs(d.getKey("/Subtype"), "/Form")) {
      StateUse use;
      use.needState = !ctx.opt.verifyOnly;
      try {
        QPDFObjectHandle::parseContentStream(obj, &use);
      } catch (...) {
        use.unset = true;
      }
      if (use.unset || use.layers || d.hasKey("/OC") || (!ctx.opt.verifyOnly && formUsesTransparency(d))) {
        d.removeKey("/GTS_Encapsulated");
        ++encaps;
      } else {
        QPDFObjectHandle res = d.getKey("/Resources");
        QPDFObjectHandle xo = res.isDictionary() ? res.getKey("/XObject") : QPDFObjectHandle::newNull();
        if (xo.isDictionary()) {
          for (const std::string& k : xo.getKeys()) {
            QPDFObjectHandle im = xo.getKey(k);
            if (!im.isStream() || !nameIs(im.getDict().getKey("/Subtype"), "/Image")) continue;
            QPDFObjectHandle m = im.getDict().getKey("/ImageMask");
            if ((m.isBool() && m.getBoolValue()) || im.getDict().hasKey("/Intent")) continue;
            im.getDict().replaceKey("/Intent", QPDFObjectHandle::newName("/RelativeColorimetric"));
            ++intents;
          }
        }
      }
    }
  }
  if (scopes) {
    ctx.issue("VT_REUSE_HINT_FIXED",
              "removed " + std::to_string(scopes) + " reuse hint(s) on XObjects that did not match how they are used", true);
  }
  if (encaps) {
    ctx.issue("VT_ENCAPSULATION_DROPPED",
              "removed the self-contained flag from " + std::to_string(encaps) +
                  " form XObject(s) that depend on their surroundings",
              true);
  }
  int bareImages = 0;
  for (QPDFObjectHandle obj : ctx.opt.verifyOnly ? ctx.pdf.getAllObjects() : std::vector<QPDFObjectHandle>()) {
    if (!obj.isStream()) continue;
    QPDFObjectHandle d = obj.getDict();
    QPDFObjectHandle enc = d.getKey("/GTS_Encapsulated");
    if (enc.isBool() && enc.getBoolValue() && nameIs(d.getKey("/Subtype"), "/Image") && !d.hasKey("/Intent")) ++bareImages;
  }
  if (bareImages) {
    ctx.issue("VT_IMAGE_INTENT_ADDED",
              std::to_string(bareImages) + " image(s) marked self-contained carry no rendering intent", true);
  }
  if (intents) {
    ctx.issue("VT_IMAGE_INTENT_ADDED",
              "gave " + std::to_string(intents) + " image(s) in self-contained forms an explicit rendering intent", true);
  }
}
}

void passPrint(Ctx& ctx) {
  if (!ctx.isX()) return;
  unifySeparations(ctx);
  QPDFPageDocumentHelper dh(ctx.pdf);
  bool boxAdded = false, boxFixed = false;
  int kuraPage1 = 0;
  for (auto& ph : dh.getAllPages()) {
    PageScope kuraScope1(ctx, ++kuraPage1);
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
  if (ctx.isVT()) {
    buildDPartTree(ctx);
    fixVtXObjects(ctx);
  }
}
}
