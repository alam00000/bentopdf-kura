#include "objlayer.hh"

#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "profile_eval.hh"
#include "profile_events.hh"
#include "profile_spatial.hh"
#include "profile_types.hh"
#include "util.hh"

namespace pdfa {
void gatherDocumentFacts(Ctx& ctx, Events& ev);
void gatherPageFacts(Ctx& ctx, Events& ev);
void gatherResourceFacts(Ctx& ctx, const PfProfile& prof, Events& ev);

namespace {
using Key = std::vector<size_t>;

struct Op {
  size_t start = 0, end = 0;
  std::string op;
  size_t nameOff = 0, nameLen = 0;
  std::string name;
  std::vector<std::string> oc;
};

struct Parsed {
  std::string data;
  std::vector<Op> ops;
  std::map<size_t, size_t> at;
  bool ok = false;
};

class Collect : public QPDFObjectHandle::ParserCallbacks {
 public:
  explicit Collect(Parsed& p) : p_(p) {}
  void handleObject(QPDFObjectHandle obj, size_t off, size_t len) override {
    if (obj.isInlineImage()) {
      inlineOff_ = off;
      return;
    }
    if (!obj.isOperator()) {
      if (argStart_ == SIZE_MAX) {
        argStart_ = off;
        first_ = obj.isName() ? obj.getName() : std::string();
      }
      if (obj.isName()) {
        o_.name = obj.getName();
        o_.nameOff = off;
        o_.nameLen = len;
      }
      return;
    }
    std::string op = obj.getOperatorValue();
    o_.op = op;
    o_.start = argStart_ == SIZE_MAX ? off : argStart_;
    o_.end = off + len;
    static const std::set<std::string> build = {"m", "l", "c", "v", "y", "h", "re"};
    static const std::set<std::string> paint = {"S", "s", "f", "F", "f*", "B", "B*", "b", "b*", "n"};
    if (op == "BI") biStart_ = o_.start;
    if (op == "EI") {
      o_.start = biStart_;
      p_.at[inlineOff_] = p_.ops.size();
    }
    if (build.count(op) && !inPath_) {
      inPath_ = true;
      pathStart_ = o_.start;
    }
    if (paint.count(op)) {
      if (inPath_) o_.start = pathStart_;
      inPath_ = false;
    }
    for (const std::string& m : marks_) {
      if (!m.empty()) o_.oc.push_back(m);
    }
    if (op == "BDC" || op == "BMC") {
      marks_.push_back(op == "BDC" && first_ == "/OC" ? (o_.name.empty() || o_.name == "/OC" ? "?" : o_.name) : "");
    } else if (op == "EMC" && !marks_.empty()) {
      marks_.pop_back();
    }
    p_.at[off] = p_.ops.size();
    p_.ops.push_back(o_);
    o_ = Op();
    argStart_ = SIZE_MAX;
    first_.clear();
  }
  void handleEOF() override {}

 private:
  Parsed& p_;
  Op o_;
  size_t argStart_ = SIZE_MAX, biStart_ = 0, inlineOff_ = SIZE_MAX, pathStart_ = 0;
  bool inPath_ = false;
  std::string first_;
  std::vector<std::string> marks_;
};

std::shared_ptr<Parsed> parseStreams(QPDFObjectHandle contents) {
  auto p = std::make_shared<Parsed>();
  try {
    Pl_Buffer buf("objlayer");
    std::string all;
    contents.pipeContentStreams(&buf, "objlayer", all);
    buf.finish();
    p->data = buf.getString();
    Collect col(*p);
    QPDFObjectHandle::parseContentStream(contents, &col);
    p->ok = true;
  } catch (...) {
    p->ok = false;
  }
  return p;
}

struct Node {
  std::shared_ptr<Parsed> ps;
  QPDFObjectHandle res;
  QPDFObjectHandle stream;
  QPDFObjGen og;
  std::vector<std::string> layers;
  bool ok = false;
};

void layerTitles(QPDFObjectHandle oc, std::vector<std::string>& out) {
  if (!oc.isDictionary()) return;
  QPDFObjectHandle ocgs = oc.getKey("/OCGs");
  if (ocgs.isDictionary()) layerTitles(ocgs, out);
  for (int i = 0; ocgs.isArray() && i < ocgs.getArrayNItems(); ++i) layerTitles(ocgs.getArrayItem(i), out);
  QPDFObjectHandle n = oc.getKey("/Name");
  if (!ocgs.isDictionary() && !ocgs.isArray()) out.push_back(n.isString() ? n.getUTF8Value() : std::string("?"));
}

void propertyTitles(QPDFObjectHandle res, const std::vector<std::string>& names, std::vector<std::string>& out) {
  QPDFObjectHandle props = res.isDictionary() ? res.getKey("/Properties") : QPDFObjectHandle();
  for (const std::string& nm : names) {
    size_t before = out.size();
    if (props.isDictionary() && props.hasKey(nm)) layerTitles(props.getKey(nm), out);
    if (out.size() == before) out.push_back(nm.size() > 1 && nm[0] == '/' ? nm.substr(1) : nm);
  }
}

struct Layering {
  Ctx& ctx;
  QPDFObjectHandle ocg;
  std::string tag;
  std::vector<QPDFPageObjectHelper> all;
  std::map<Key, Node> nodes;
  std::map<QPDFObjGen, std::shared_ptr<Parsed>> formCache;
  std::set<Key> matched, known;
  std::set<QPDFObjGen> foreign, edited;
  std::map<QPDFObjGen, std::set<Key>> usesOf;
  std::map<std::pair<QPDFObjGen, std::set<Key>>, QPDFObjectHandle> copies;

  Layering(Ctx& c, QPDFObjectHandle o) : ctx(c), ocg(o) {
    tag = "/KuraOL" + std::to_string(o.getObjectID());
    all = QPDFPageDocumentHelper(c.pdf).getAllPages();
  }

  const Node& node(const Key& k) {
    auto it = nodes.find(k);
    if (it != nodes.end()) return it->second;
    Node n;
    if (k.size() == 1) {
      if (k[0] >= 1 && k[0] <= all.size()) {
        QPDFPageObjectHelper& ph = all[k[0] - 1];
        n.stream = ph.getObjectHandle();
        n.res = ph.getAttribute("/Resources", true);
        n.ps = parseStreams(n.stream.getKey("/Contents"));
        n.ok = n.ps->ok && n.res.isDictionary();
      }
    } else if (k.size() > 1) {
      Key pk(k.begin(), k.end() - 1);
      const Node& par = node(pk);
      if (par.ok && par.ps->at.count(k.back())) {
        const Op& op = par.ps->ops[par.ps->at.at(k.back())];
        QPDFObjectHandle xd = par.res.getKey("/XObject");
        QPDFObjectHandle xo = op.op == "Do" && xd.isDictionary() ? xd.getKey(op.name) : QPDFObjectHandle();
        if (xo.isStream() && nameIs(xo.getDict().getKey("/Subtype"), "/Form")) {
          n.stream = xo;
          n.og = xo.getObjGen();
          QPDFObjectHandle fr = xo.getDict().getKey("/Resources");
          n.res = fr.isDictionary() ? fr : par.res;
          auto& cached = formCache[n.og];
          if (!cached) cached = parseStreams(xo);
          n.ps = cached;
          n.layers = par.layers;
          propertyTitles(par.res, op.oc, n.layers);
          layerTitles(xo.getDict().getKey("/OC"), n.layers);
          n.ok = n.ps->ok;
          usesOf[n.og].insert(k);
        }
      }
    }
    return nodes[k] = n;
  }

  std::vector<std::string> leafLayers(const Key& k, bool& ok) {
    Key pk(k.begin(), k.end() - 1);
    const Node& n = node(pk);
    ok = n.ok && n.ps->at.count(k.back());
    if (!ok) return {};
    std::vector<std::string> out = n.layers;
    propertyTitles(n.res, n.ps->ops[n.ps->at.at(k.back())].oc, out);
    return out;
  }

  std::set<Key> signature(const Key& prefix) {
    std::set<Key> s;
    for (auto it = matched.lower_bound(prefix); it != matched.end() && std::equal(prefix.begin(), prefix.end(), it->begin()); ++it) {
      if (it->size() > prefix.size()) s.insert(Key(it->begin() + static_cast<long>(prefix.size()), it->end()));
    }
    return s;
  }

  bool allMatched(const Key& prefix) {
    for (auto it = known.lower_bound(prefix); it != known.end() && std::equal(prefix.begin(), prefix.end(), it->begin()); ++it) {
      if (!matched.count(*it)) return false;
    }
    return true;
  }

  void ensureProps(QPDFObjectHandle res) {
    QPDFObjectHandle props = res.getKey("/Properties");
    if (!props.isDictionary()) {
      props = QPDFObjectHandle::newDictionary();
      res.replaceKey("/Properties", props);
    }
    props.replaceKey(tag, ocg);
  }

  std::string rewrite(const Key& prefix, const Node& n, bool& changed) {
    std::set<size_t> wraps;
    std::map<size_t, std::string> renames;
    std::set<size_t> seenNext;
    for (auto it = matched.lower_bound(prefix); it != matched.end() && std::equal(prefix.begin(), prefix.end(), it->begin()); ++it) {
      if (it->size() <= prefix.size()) continue;
      size_t o = (*it)[prefix.size()];
      auto at = n.ps->at.find(o);
      if (at == n.ps->at.end() || !seenNext.insert(o).second) continue;
      if (it->size() == prefix.size() + 1) {
        wraps.insert(at->second);
        continue;
      }
      Key child = prefix;
      child.push_back(o);
      if (allMatched(child)) {
        wraps.insert(at->second);
        continue;
      }
      const Node& cn = node(child);
      if (!cn.ok) continue;
      std::set<Key> sig = signature(child);
      bool inPlace = !foreign.count(cn.og);
      for (const Key& use : usesOf[cn.og]) {
        if (inPlace && signature(use) != sig) inPlace = false;
      }
      if (inPlace) {
        if (edited.insert(cn.og).second) {
          bool ch = false;
          std::string data = rewrite(child, cn, ch);
          if (ch) {
            QPDFObjectHandle st = cn.stream;
            st.replaceStreamData(data, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
          }
        }
        continue;
      }
      QPDFObjectHandle& copy = copies[{cn.og, sig}];
      if (!copy.isInitialized()) {
        bool ch = false;
        std::string data = rewrite(child, cn, ch);
        copy = ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf));
        copy.replaceDict(cn.stream.getDict().shallowCopy());
        copy.replaceStreamData(data, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
      }
      std::string nm = "/KuraFx" + std::to_string(copy.getObjectID());
      QPDFObjectHandle xd = n.res.getKey("/XObject");
      if (!xd.isDictionary()) continue;
      xd.replaceKey(nm, copy);
      renames[at->second] = nm;
    }
    changed = !wraps.empty() || !renames.empty();
    if (!changed) return n.ps->data;
    if (!wraps.empty()) ensureProps(n.res);
    struct Edit {
      size_t at, cut;
      std::string text;
    };
    std::vector<Edit> edits;
    for (size_t i : wraps) edits.push_back({n.ps->ops[i].end, 0, "\nEMC\n"});
    for (size_t i : wraps) edits.push_back({n.ps->ops[i].start, 0, "\n/OC " + tag + " BDC\n"});
    for (auto& [i, nm] : renames) edits.push_back({n.ps->ops[i].nameOff, n.ps->ops[i].nameLen, nm});
    std::stable_sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.at < b.at; });
    std::string out;
    size_t pos = 0;
    for (const Edit& e : edits) {
      if (e.at < pos) continue;
      out.append(n.ps->data, pos, e.at - pos);
      out += e.text;
      pos = e.at + e.cut;
    }
    out.append(n.ps->data, pos, std::string::npos);
    return out;
  }
};

bool ruleMatches(const PfRule& rule, const std::vector<const PfCondition*>& conds,
                 const std::function<bool(const PfAtom&)>& atom) {
  bool combined = rule.logic != 1;
  for (const PfCondition* c : conds) {
    bool cr = true;
    for (const PfAtom& a : c->atoms) {
      if (!atom(a)) {
        cr = false;
        break;
      }
    }
    combined = rule.logic == 1 ? (combined || cr) : (combined && cr);
    if (combined == (rule.logic == 1)) break;
  }
  return combined;
}
}

namespace {
struct RuleSet {
  PfProfile prof;
  std::vector<std::pair<const PfRule*, std::vector<const PfCondition*>>> rules;
};

bool loadRules(Ctx& ctx, const std::vector<std::string>& ruleIds, RuleSet& rs, Events& ev) {
  parseKuraJson(ctx.opt.preflightProfile, rs.prof);
  for (const PfRule& r : rs.prof.rules) {
    if (std::find(ruleIds.begin(), ruleIds.end(), r.id) == ruleIds.end()) continue;
    std::vector<const PfCondition*> cs;
    for (const std::string& cid : r.condIds) {
      auto it = rs.prof.conds.find(cid);
      if (it != rs.prof.conds.end()) cs.push_back(&it->second);
    }
    rs.rules.push_back({&r, cs});
  }
  if (rs.rules.empty()) return false;
  ev.trackSrc = true;
  ev.prof = &rs.prof;
  loadProfileShapes(rs.prof, ev);
  try {
    gatherDocumentFacts(ctx, ev);
    gatherPageFacts(ctx, ev);
    gatherResourceFacts(ctx, rs.prof, ev);
  } catch (...) {
    return false;
  }
  return true;
}

template <class E>
bool eventHits(const RuleSet& rs, const Events& ev, const E& e, Domain kind, const std::vector<std::string>* layers,
               bool& supported) {
  const PageFacts* pf = pageFor(ev, e.page);
  for (auto& [rule, conds] : rs.rules) {
    if (rule->scope >= 2 && e.bbox.valid && pf) {
      Box area = rule->scope == 2 ? (pf->trim.valid ? pf->trim : pf->media)
                                  : (pf->bleed.valid ? pf->bleed : pf->trim.valid ? pf->trim : pf->media);
      if (area.valid && !boxesIntersect(area, e.bbox)) continue;
    }
    bool hit = ruleMatches(*rule, conds, [&](const PfAtom& a) {
      if (layers && a.token == "layer.membership") return compare(Value(*layers), a) > 0;
      Domain d = domainOf(a);
      if ((d == Domain::kPaint || d == Domain::kText || d == Domain::kImage || d == Domain::kAnnot ||
           d == Domain::kFont) && d != kind && !(kind == Domain::kText && d == Domain::kFont)) {
        return false;
      }
      return evalCheck(a, subjectFor(kind, &e, e.page, ev), ev, supported) > 0;
    });
    if (hit) return true;
  }
  return false;
}
}

int layerObjectsByRules(Ctx& ctx, std::vector<QPDFPageObjectHelper>& pages, const std::vector<std::string>& ruleIds,
                        QPDFObjectHandle ocg, int& unmovable) {
  unmovable = 0;
  RuleSet rs;
  Events ev;
  if (!loadRules(ctx, ruleIds, rs, ev)) return -1;
  Layering L(ctx, ocg);
  L.foreign = ev.foreignForms;
  std::map<QPDFObjGen, size_t> pageNo;
  for (size_t i = 0; i < L.all.size(); ++i) pageNo[L.all[i].getObjectHandle().getObjGen()] = i + 1;
  std::set<size_t> targets;
  for (auto& ph : pages) {
    auto it = pageNo.find(ph.getObjectHandle().getObjGen());
    if (it != pageNo.end()) targets.insert(it->second);
  }
  bool supported = true;
  auto consider = [&](const auto& e, Domain kind) {
    if (e.src.empty() || e.page < 1) return;
    Key k{static_cast<size_t>(e.page)};
    k.insert(k.end(), e.src.begin(), e.src.end());
    bool valid = std::find(k.begin(), k.end(), SIZE_MAX) == k.end();
    bool layerOk = false;
    std::vector<std::string> layers = valid ? L.leafLayers(k, layerOk) : std::vector<std::string>();
    if (valid && layerOk) L.known.insert(k);
    if (!targets.count(static_cast<size_t>(e.page))) return;
    if (!eventHits(rs, ev, e, kind, &layers, supported)) return;
    if (valid && layerOk) {
      L.matched.insert(k);
    } else {
      ++unmovable;
    }
  };
  for (const PaintEvent& e : ev.paints) consider(e, Domain::kPaint);
  for (const TextEvent& e : ev.texts) consider(e, Domain::kText);
  for (const ImageEvent& e : ev.images) consider(e, Domain::kImage);
  if (!supported) return -1;
  for (size_t p : targets) {
    Key pk{p};
    const Node& n = L.node(pk);
    if (!n.ok) continue;
    bool changed = false;
    std::string data = L.rewrite(pk, n, changed);
    QPDFObjectHandle pg = n.stream;
    if (changed) pg.replaceKey("/Contents", ctx.pdf.makeIndirectObject(QPDFObjectHandle::newStream(&ctx.pdf, data)));
  }
  return static_cast<int>(L.matched.size());
}

bool objectBoxesByRules(Ctx& ctx, const std::vector<std::string>& ruleIds, std::map<int, std::vector<Box>>& out) {
  RuleSet rs;
  Events ev;
  if (!loadRules(ctx, ruleIds, rs, ev)) return false;
  bool supported = true;
  auto consider = [&](const auto& e, Domain kind) {
    if (e.page >= 1 && e.bbox.valid && eventHits(rs, ev, e, kind, nullptr, supported)) out[e.page].push_back(e.bbox);
  };
  for (const PaintEvent& e : ev.paints) consider(e, Domain::kPaint);
  for (const TextEvent& e : ev.texts) consider(e, Domain::kText);
  for (const ImageEvent& e : ev.images) consider(e, Domain::kImage);
  return supported;
}
}
