#include "tools.hh"

#include <qpdf/JSON.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <lcms2.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

#include "../core/assets/cmyk_icc.hh"
#include "../core/src/colorconv.hh"

#ifdef KURA_WITH_PDFIUM
#include "kura/raster.hh"
#endif

namespace kuratools {
namespace {
namespace fs = std::filesystem;
using OH = QPDFObjectHandle;

struct Fail : std::runtime_error {
  int code;
  Fail(int c, const std::string& m) : std::runtime_error(m), code(c) {}
};
[[noreturn]] void fail(const std::string& m, int code = 2) { throw Fail(code, m); }

std::string js(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += static_cast<char>(c);
    } else if (c == '\n') {
      o += "\\n";
    } else if (c == '\r') {
      o += "\\r";
    } else if (c == '\t') {
      o += "\\t";
    } else if (c < 0x20) {
      char b[8];
      std::snprintf(b, sizeof b, "\\u%04x", c);
      o += b;
    } else {
      o += static_cast<char>(c);
    }
  }
  return o + "\"";
}

std::string num(double v) {
  if (!std::isfinite(v)) return "0";
  char b[32];
  std::snprintf(b, sizeof b, "%.3f", v);
  std::string s = b;
  s.erase(s.find_last_not_of('0') + 1);
  if (s.back() == '.') s.pop_back();
  return s == "-0" ? "0" : s;
}

std::string nums(std::initializer_list<double> v) {
  std::string o = "[";
  for (double d : v) o += (o.size() > 1 ? "," : "") + num(d);
  return o + "]";
}

std::string joinJs(const std::vector<std::string>& items) {
  std::string o = "[";
  for (size_t i = 0; i < items.size(); ++i) o += (i ? "," : "") + items[i];
  return o + "]";
}

std::string readFile(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) fail("cannot read " + p);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

void writeFile(const std::string& p, const std::string& data) {
  std::ofstream f(p, std::ios::binary);
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (!f) fail("cannot write " + p);
}

void ensureDir(const std::string& d) {
  std::error_code ec;
  fs::create_directories(d, ec);
  if (!fs::is_directory(d)) fail("cannot create directory " + d);
}

struct Args {
  std::vector<std::string> pos;
  std::map<std::string, std::string> kv;
  std::set<std::string> flags;
  std::string get(const std::string& k, const std::string& d = "") const {
    auto it = kv.find(k);
    return it == kv.end() ? d : it->second;
  }
  bool has(const std::string& k) const { return kv.count(k) || flags.count(k); }
  double number(const std::string& k, double d) const {
    if (!kv.count(k)) return d;
    char* end = nullptr;
    double v = std::strtod(kv.at(k).c_str(), &end);
    if (!end || *end || !std::isfinite(v)) fail("bad number for --" + k, 64);
    return v;
  }
};

std::shared_ptr<QPDF> openPdf(const std::string& path, const std::string& pw) {
  auto q = QPDF::create();
  q->setSuppressWarnings(true);
  q->processFile(path.c_str(), pw.c_str());
  return q;
}

void savePdf(QPDF& q, const std::string& out, const std::function<void(QPDFWriter&)>& cfg = {}) {
  QPDFWriter w(q, out.c_str());
  if (cfg) cfg(w);
  w.write();
}

std::vector<QPDFPageObjectHelper> allPages(QPDF& q) { return QPDFPageDocumentHelper(q).getAllPages(); }

std::vector<int> pageSel(const Args& a, size_t n) {
  std::vector<int> out;
  std::string spec = a.get("pages");
  if (spec.empty()) {
    for (size_t i = 0; i < n; ++i) out.push_back(static_cast<int>(i));
    return out;
  }
  std::stringstream ss(spec);
  std::string part;
  while (std::getline(ss, part, ',')) {
    auto toInt = [&](const std::string& s, int d) {
      if (s.empty()) return d;
      if (s == "last") return static_cast<int>(n);
      if (s.find_first_not_of("0123456789") != std::string::npos) fail("bad page range " + spec, 64);
      return std::atoi(s.c_str());
    };
    size_t dash = part.find('-');
    int lo = toInt(part.substr(0, dash), 1);
    int hi = dash == std::string::npos ? lo : toInt(part.substr(dash + 1), static_cast<int>(n));
    for (int p = std::max(lo, 1); p <= std::min(hi, static_cast<int>(n)); ++p) out.push_back(p - 1);
  }
  if (out.empty()) fail("page range selects no pages", 64);
  return out;
}

std::map<QPDFObjGen, int> pageIndex(QPDF& q) {
  std::map<QPDFObjGen, int> m;
  auto pages = allPages(q);
  for (size_t i = 0; i < pages.size(); ++i) m[pages[i].getObjectHandle().getObjGen()] = static_cast<int>(i);
  return m;
}

std::string idOf(OH o) {
  return std::to_string(o.getObjectID()) + (o.getGeneration() ? "-" + std::to_string(o.getGeneration()) : "");
}

std::string streamBytes(OH s, qpdf_stream_decode_level_e lvl = qpdf_dl_all) {
  auto b = lvl == qpdf_dl_none ? s.getRawStreamData() : s.getStreamData(lvl);
  return std::string(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
}

void walkAll(QPDF& q, const std::function<void(OH, OH)>& fn) {
  std::function<void(OH, OH, int)> visit = [&](OH h, OH owner, int depth) {
    if (depth > 64) return;
    fn(h, owner);
    if (h.isStream()) {
      visit(h.getDict(), h, depth + 1);
    } else if (h.isDictionary()) {
      for (auto& k : h.getKeys()) {
        OH c = h.getKey(k);
        if (!c.isIndirect() && (c.isDictionary() || c.isArray())) visit(c, owner, depth + 1);
      }
    } else if (h.isArray()) {
      for (int i = 0; i < h.getArrayNItems(); ++i) {
        OH c = h.getArrayItem(i);
        if (!c.isIndirect() && (c.isDictionary() || c.isArray())) visit(c, owner, depth + 1);
      }
    }
  };
  for (auto& o : q.getAllObjects()) visit(o, o, 0);
  visit(q.getTrailer(), q.getTrailer(), 0);
}

bool nameIs(OH o, const char* n) { return o.isName() && o.getName() == n; }

struct M {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};
M mul(const M& x, const M& y) {
  return {x.a * y.a + x.b * y.c, x.a * y.b + x.b * y.d, x.c * y.a + x.d * y.c,
          x.c * y.b + x.d * y.d, x.e * y.a + x.f * y.c + y.e, x.e * y.b + x.f * y.d + y.f};
}
void apply(const M& m, double x, double y, double& ox, double& oy) {
  ox = m.a * x + m.c * y + m.e;
  oy = m.b * x + m.d * y + m.f;
}
struct Box {
  double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
  void add(double x, double y) {
    x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
  }
};
struct Seg {
  std::string op;
  std::vector<double> v;
};
struct Gs {
  M ctm;
  OH sCs = OH::newName("/DeviceGray"), fCs = OH::newName("/DeviceGray");
  std::string sCol, fCol;
  std::map<std::string, std::string> line;
  double lw = 1;
};
struct Sink {
  std::function<void(OH, const M&)> image;
  std::function<void(const std::vector<Seg>&, const Gs&, bool stroke, bool fill, const std::string& op)> path;
  std::function<void(OH)> space;
  std::function<void(OH, const std::string&)> font;
  int inlineImages = 0;
};

class Walk : public OH::ParserCallbacks {
 public:
  Walk(Sink& s, OH res, M base, int depth) : sink(s), res(res), depth(depth) { gs.ctm = base; }
  void handleEOF() override {}
  void handleObject(OH o) override {
    if (!o.isOperator()) {
      ops.push_back(o);
      return;
    }
    op(o.getOperatorValue());
    ops.clear();
  }

 private:
  double n(size_t i) const { return i < ops.size() && ops[i].isNumber() ? ops[i].getNumericValue() : 0; }
  std::string operandText() const {
    std::string s;
    for (auto& o : ops) s += o.unparse() + " ";
    return s;
  }
  OH resolveCs(OH name) {
    if (!name.isName()) return name;
    std::string nm = name.getName();
    if (nm == "/DeviceGray" || nm == "/DeviceRGB" || nm == "/DeviceCMYK" || nm == "/Pattern") return name;
    OH d = res.isDictionary() ? res.getKey("/ColorSpace") : OH::newNull();
    return d.isDictionary() && d.hasKey(nm) ? d.getKey(nm) : name;
  }
  void setCs(bool stroke, OH cs) {
    (stroke ? gs.sCs : gs.fCs) = cs;
    (stroke ? gs.sCol : gs.fCol).clear();
    if (sink.space) sink.space(cs);
  }
  void op(const std::string& o) {
    if (o == "q") {
      stack.push_back(gs);
    } else if (o == "Q") {
      if (!stack.empty()) gs = stack.back(), stack.pop_back();
    } else if (o == "cm" && ops.size() == 6) {
      gs.ctm = mul({n(0), n(1), n(2), n(3), n(4), n(5)}, gs.ctm);
    } else if ((o == "CS" || o == "cs") && !ops.empty()) {
      setCs(o == "CS", resolveCs(ops[0]));
    } else if (o == "G" || o == "g" || o == "RG" || o == "rg" || o == "K" || o == "k") {
      bool st = std::isupper(static_cast<unsigned char>(o[0]));
      const char* dev = (o == "K" || o == "k") ? "/DeviceCMYK" : (o == "RG" || o == "rg") ? "/DeviceRGB" : "/DeviceGray";
      setCs(st, OH::newName(dev));
      (st ? gs.sCol : gs.fCol) = operandText() + o;
    } else if (o == "SC" || o == "SCN" || o == "sc" || o == "scn") {
      (std::isupper(static_cast<unsigned char>(o[0])) ? gs.sCol : gs.fCol) = operandText() + o;
    } else if (o == "w" || o == "J" || o == "j" || o == "M" || o == "d") {
      gs.line[o] = operandText() + o;
      if (o == "w") gs.lw = n(0);
    } else if (o == "m" || o == "l" || o == "c" || o == "v" || o == "y" || o == "re" || o == "h") {
      Seg s{o, {}};
      for (size_t i = 0; i < ops.size(); ++i) s.v.push_back(n(i));
      path.push_back(std::move(s));
    } else if (o == "S" || o == "s" || o == "f" || o == "F" || o == "f*" || o == "B" || o == "B*" ||
               o == "b" || o == "b*" || o == "n") {
      bool stroke = o == "S" || o == "s" || o[0] == 'B' || o[0] == 'b';
      bool fill = o != "S" && o != "s" && o != "n";
      if (!path.empty() && sink.path && (stroke || fill)) sink.path(path, gs, stroke, fill, o);
      path.clear();
    } else if (o == "Tf" && !ops.empty() && ops[0].isName() && sink.font) {
      OH fd = res.isDictionary() ? res.getKey("/Font") : OH::newNull();
      if (fd.isDictionary() && fd.getKey(ops[0].getName()).isDictionary())
        sink.font(fd.getKey(ops[0].getName()), ops[0].getName());
    } else if (o == "EI") {
      ++sink.inlineImages;
    } else if (o == "Do" && !ops.empty() && ops[0].isName()) {
      OH xd = res.isDictionary() ? res.getKey("/XObject") : OH::newNull();
      OH x = xd.isDictionary() ? xd.getKey(ops[0].getName()) : OH::newNull();
      if (!x.isStream()) return;
      OH st = x.getDict().getKey("/Subtype");
      if (nameIs(st, "/Image")) {
        if (sink.image) sink.image(x, gs.ctm);
      } else if (nameIs(st, "/Form") && depth < 16 && !busy.count(x.getObjGen())) {
        M fm;
        OH mx = x.getDict().getKey("/Matrix");
        if (mx.isArray() && mx.getArrayNItems() == 6) {
          double v[6];
          for (int i = 0; i < 6; ++i) v[i] = mx.getArrayItem(i).isNumber() ? mx.getArrayItem(i).getNumericValue() : 0;
          fm = {v[0], v[1], v[2], v[3], v[4], v[5]};
        }
        OH fr = x.getDict().getKey("/Resources");
        Walk w(sink, fr.isDictionary() ? fr : res, mul(fm, gs.ctm), depth + 1);
        w.busy = busy;
        w.busy.insert(x.getObjGen());
        try {
          x.parseAsContents(&w);
        } catch (...) {
        }
      }
    }
  }

  Sink& sink;
  OH res;
  int depth;
  Gs gs;
  std::vector<Gs> stack;
  std::vector<OH> ops;
  std::vector<Seg> path;
  std::set<QPDFObjGen> busy;
};

void walkPage(QPDFPageObjectHelper& ph, Sink& sink) {
  Walk w(sink, ph.getAttribute("/Resources", false), M(), 0);
  try {
    ph.parseContents(&w);
  } catch (...) {
  }
}

Box pageBox(QPDFPageObjectHelper& ph) {
  auto r = ph.getCropBox(false, false).getArrayAsRectangle();
  Box b;
  b.add(r.llx, r.lly);
  b.add(r.urx, r.ury);
  return b;
}

#ifdef KURA_WITH_PDFIUM
std::string pngEncode(int w, int h, int comps, const std::string& px) {
  int ct = comps == 1 ? 0 : comps == 2 ? 4 : comps == 3 ? 2 : 6;
  std::string raw;
  size_t stride = static_cast<size_t>(w) * comps;
  raw.reserve((stride + 1) * h);
  for (int y = 0; y < h; ++y) {
    raw += '\0';
    raw.append(px, y * stride, stride);
  }
  uLongf zl = compressBound(static_cast<uLong>(raw.size()));
  std::string z(zl, '\0');
  if (compress2(reinterpret_cast<Bytef*>(&z[0]), &zl, reinterpret_cast<const Bytef*>(raw.data()),
                static_cast<uLong>(raw.size()), 6) != Z_OK)
    fail("png compression failed");
  z.resize(zl);
  std::string out = "\x89PNG\r\n\x1a\n";
  auto be32 = [](std::string& s, uint32_t v) {
    for (int i = 3; i >= 0; --i) s += static_cast<char>((v >> (8 * i)) & 0xff);
  };
  auto chunk = [&](const char* type, const std::string& data) {
    be32(out, static_cast<uint32_t>(data.size()));
    std::string td = std::string(type, 4) + data;
    out += td;
    be32(out, static_cast<uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(td.data()), static_cast<uInt>(td.size()))));
  };
  std::string ihdr;
  be32(ihdr, static_cast<uint32_t>(w));
  be32(ihdr, static_cast<uint32_t>(h));
  ihdr += std::string{8, static_cast<char>(ct), 0, 0, 0};
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", "");
  return out;
}
#endif

std::string boxJs(const Box& b) { return nums({b.x0, b.y0, b.x1, b.y1}); }

std::string pdfToJson(OH o, int depth = 0) {
  if (depth > 32) return "null";
  if (o.isString()) return js(o.getUTF8Value());
  if (o.isName()) return js(o.getName().substr(1));
  if (o.isBool()) return o.getBoolValue() ? "true" : "false";
  if (o.isInteger()) return std::to_string(o.getIntValue());
  if (o.isNumber()) return num(o.getNumericValue());
  if (o.isArray()) {
    std::vector<std::string> v;
    for (int i = 0; i < o.getArrayNItems(); ++i) v.push_back(pdfToJson(o.getArrayItem(i), depth + 1));
    return joinJs(v);
  }
  if (o.isDictionary()) {
    std::string s = "{";
    for (auto& k : o.getKeys()) s += (s.size() > 1 ? "," : "") + js(k.substr(1)) + ":" + pdfToJson(o.getKey(k), depth + 1);
    return s + "}";
  }
  return "null";
}

OH jsonToPdf(const JSON& j) {
  std::string s;
  bool b;
  if (j.getString(s)) return OH::newUnicodeString(s);
  if (j.getNumber(s)) {
    if (s.find_first_of(".eE") == std::string::npos) return OH::newInteger(std::stoll(s));
    return OH::newReal(std::strtod(s.c_str(), nullptr), 6);
  }
  if (j.getBool(b)) return OH::newBool(b);
  if (j.isArray()) {
    OH a = OH::newArray();
    j.forEachArrayItem([&](JSON v) { a.appendItem(jsonToPdf(v)); });
    return a;
  }
  if (j.isDictionary()) {
    OH d = OH::newDictionary();
    j.forEachDictItem([&](const std::string& k, JSON v) { d.replaceKey("/" + k, jsonToPdf(v)); });
    return d;
  }
  return OH::newNull();
}

JSON loadJson(const std::string& path) {
  try {
    return JSON::parse(readFile(path));
  } catch (Fail&) {
    throw;
  } catch (std::exception& e) {
    fail("invalid JSON in " + path + ": " + e.what(), 64);
  }
}

double jNum(const JSON& j, const std::string& k, double d) {
  std::string s;
  return j.getDictItem(k).getNumber(s) ? std::strtod(s.c_str(), nullptr) : d;
}

std::string safeName(std::string s) {
  size_t slash = s.find_last_of("/\\:");
  if (slash != std::string::npos) s = s.substr(slash + 1);
  for (char& c : s)
    if (static_cast<unsigned char>(c) < 0x20 || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
  if (s.empty() || s == "." || s == "..") s = "file";
  return s;
}

#ifdef KURA_WITH_PDFIUM
std::string pageImageName(int page, const std::string& suffix, const std::string& ext) {
  char b[32];
  std::snprintf(b, sizeof b, "page-%04d", page + 1);
  return b + suffix + "." + ext;
}
#endif

std::string toolExtractIcc(const Args& a) {
  auto q = openPdf(a.pos[0], a.get("password"));
  std::string dir = a.pos[1];
  ensureDir(dir);
  std::map<QPDFObjGen, std::pair<OH, std::string>> found;
  walkAll(*q, [&](OH h, OH) {
    if (h.isArray() && h.getArrayNItems() >= 2 && nameIs(h.getArrayItem(0), "/ICCBased") && h.getArrayItem(1).isStream())
      found.emplace(h.getArrayItem(1).getObjGen(), std::make_pair(h.getArrayItem(1), "ICCBased"));
    if (h.isDictionary() && h.getKey("/DestOutputProfile").isStream())
      found[h.getKey("/DestOutputProfile").getObjGen()] = {h.getKey("/DestOutputProfile"), "OutputIntent"};
  });
  std::vector<std::string> items;
  for (auto& [og, e] : found) {
    std::string data;
    try {
      data = streamBytes(e.first);
    } catch (...) {
      continue;
    }
    std::string name = "icc-" + idOf(e.first) + ".icc";
    writeFile((fs::path(dir) / name).string(), data);
    std::string cls = data.size() >= 20 ? data.substr(16, 4) : "";
    cls.erase(cls.find_last_not_of(' ') + 1);
    items.push_back("{\"id\":" + js(idOf(e.first)) + ",\"file\":" + js(name) + ",\"usage\":" + js(e.second) +
                    ",\"description\":" + js(pdfa::iccDescription(data)) + ",\"colorSpace\":" + js(cls) +
                    ",\"size\":" + std::to_string(data.size()) + "}");
  }
  return "\"outdir\":" + js(dir) + ",\"count\":" + std::to_string(items.size()) + ",\"profiles\":" + joinJs(items);
}

std::string toolExtractXmp(const Args& a) {
  auto q = openPdf(a.pos[0], a.get("password"));
  std::string dir = a.pos[1];
  ensureDir(dir);
  OH root = q->getRoot();
  std::set<QPDFObjGen> seen;
  std::vector<std::string> items;
  auto take = [&](OH holder, OH owner, bool doc) {
    OH m = holder.getKey("/Metadata");
    if (!m.isStream() || !seen.insert(m.getObjGen()).second) return;
    std::string type = holder.getKey("/Type").isName() ? holder.getKey("/Type").getName().substr(1)
                       : holder.getKey("/Subtype").isName() ? holder.getKey("/Subtype").getName().substr(1)
                                                            : "Object";
    std::string name = doc ? "document.xmp" : "obj-" + idOf(owner) + "-" + safeName(type) + ".xmp";
    writeFile((fs::path(dir) / name).string(), streamBytes(m));
    items.push_back("{\"file\":" + js(name) + ",\"owner\":" + js(doc ? "Catalog" : idOf(owner)) + ",\"type\":" +
                    js(doc ? "Catalog" : type) + "}");
  };
  take(root, root, true);
  if (!a.has("document-only"))
    walkAll(*q, [&](OH h, OH owner) {
      if (h.isDictionary()) take(h, owner, false);
    });
  return "\"outdir\":" + js(dir) + ",\"count\":" + std::to_string(items.size()) + ",\"xmp\":" + joinJs(items);
}

std::vector<std::string> colorantNames(OH cs) {
  std::vector<std::string> v;
  if (!cs.isArray() || cs.getArrayNItems() < 2) return v;
  OH n = cs.getArrayItem(1);
  if (nameIs(cs.getArrayItem(0), "/Separation") && n.isName()) v.push_back(n.getName().substr(1));
  if (nameIs(cs.getArrayItem(0), "/DeviceN") && n.isArray())
    for (int i = 0; i < n.getArrayNItems(); ++i)
      if (n.getArrayItem(i).isName()) v.push_back(n.getArrayItem(i).getName().substr(1));
  return v;
}

std::string segText(const std::vector<Seg>& path) {
  std::string s;
  for (auto& g : path) {
    for (double v : g.v) s += num(v) + " ";
    s += g.op + "\n";
  }
  return s;
}

std::string toolExtractDieline(const Args& a) {
  auto q = openPdf(a.pos[0], a.get("password"));
  auto pages = allPages(*q);
  std::string fmt = a.get("format", fs::path(a.pos[1]).extension() == ".json" ? "json" : "pdf");
  if (fmt != "json" && fmt != "pdf") fail("--format must be pdf or json", 64);
  std::regex names(a.get("names", "die|cut|stanz|crease"), std::regex::icase);
  QPDF dst;
  dst.emptyPDF();
  std::map<std::string, OH> spaces;
  std::vector<std::string> pj;
  size_t total = 0;
  std::set<std::string> sepNames;
  for (int p : pageSel(a, pages.size())) {
    std::vector<std::string> paths;
    std::string content;
    std::map<std::string, std::string> pageCs;
    Sink s;
    s.path = [&](const std::vector<Seg>& path, const Gs& gs, bool stroke, bool fill, const std::string&) {
      for (int which = 0; which < 2; ++which) {
        bool st = which == 0;
        if (st ? !stroke : !fill) continue;
        OH cs = st ? gs.sCs : gs.fCs;
        std::string hit;
        for (auto& n : colorantNames(cs))
          if (std::regex_search(n, names)) hit = n;
        if (hit.empty()) continue;
        sepNames.insert(hit);
        ++total;
        std::vector<std::string> sj;
        for (auto& g : path) {
          std::vector<double> pts = g.v;
          if (g.op == "re" && pts.size() == 4) {
            double x = pts[0], y = pts[1], w = pts[2], h = pts[3];
            pts = {x, y, x + w, y, x + w, y + h, x, y + h};
          }
          std::string pv = "[";
          for (size_t i = 0; i + 1 < pts.size(); i += 2) {
            double ox, oy;
            apply(gs.ctm, pts[i], pts[i + 1], ox, oy);
            pv += (i ? "," : "") + num(ox) + "," + num(oy);
          }
          sj.push_back("{\"op\":" + js(g.op) + ",\"points\":" + pv + "]}");
        }
        double scale = std::sqrt(std::fabs(gs.ctm.a * gs.ctm.d - gs.ctm.b * gs.ctm.c));
        paths.push_back("{\"colorant\":" + js(hit) + ",\"paint\":" + js(st ? "stroke" : "fill") +
                        ",\"lineWidth\":" + num(gs.lw * scale) + ",\"segments\":" + joinJs(sj) + "}");
        if (fmt != "pdf") continue;
        std::string key = cs.unparseResolved();
        if (!pageCs.count(key)) {
          if (!spaces.count(key)) {
            OH src = cs.isIndirect() ? cs : q->makeIndirectObject(cs);
            spaces[key] = dst.copyForeignObject(src);
          }
          pageCs[key] = "/CS" + std::to_string(pageCs.size());
        }
        std::string col = st ? gs.sCol : gs.fCol, tint;
        if (!col.empty()) tint = col.substr(0, col.find_last_of(' ') + 1);
        if (tint.find_first_not_of(' ') == std::string::npos)
          for (size_t i = 0; i < colorantNames(cs).size(); ++i) tint += "1 ";
        const M& m = gs.ctm;
        content += "q " + num(m.a) + " " + num(m.b) + " " + num(m.c) + " " + num(m.d) + " " + num(m.e) + " " + num(m.f) + " cm\n";
        for (auto& [k, v] : gs.line) content += v + "\n";
        content += pageCs[key] + (st ? " CS " : " cs ") + tint + (st ? "SCN\n" : "scn\n") + segText(path) + (st ? "S" : "f") + "\nQ\n";
      }
    };
    walkPage(pages[p], s);
    if (paths.empty()) continue;
    Box pb = pageBox(pages[p]);
    pj.push_back("{\"page\":" + std::to_string(p + 1) + ",\"box\":" + boxJs(pb) + ",\"paths\":" + joinJs(paths) + "}");
    if (fmt != "pdf") continue;
    OH res = OH::newDictionary();
    OH csd = OH::newDictionary();
    for (auto& [k, n] : pageCs) csd.replaceKey(n, spaces[k]);
    res.replaceKey("/ColorSpace", csd);
    OH page = dst.makeIndirectObject(OH::parse("<< /Type /Page >>"));
    page.replaceKey("/MediaBox", OH::newArray(QPDFObjectHandle::Rectangle(pb.x0, pb.y0, pb.x1, pb.y1)));
    page.replaceKey("/Resources", res);
    page.replaceKey("/Contents", OH::newStream(&dst, content));
    QPDFPageDocumentHelper(dst).addPage(QPDFPageObjectHelper(page), false);
  }
  if (pj.empty()) fail("no dieline found (colorant names matching " + a.get("names", "die|cut|stanz|crease") + ")", 1);
  if (fmt == "pdf") {
    savePdf(dst, a.pos[1]);
  } else {
    writeFile(a.pos[1], "{\"pages\":" + joinJs(pj) + "}\n");
  }
  std::vector<std::string> sn;
  for (auto& n : sepNames) sn.push_back(js(n));
  return "\"output\":" + js(a.pos[1]) + ",\"format\":" + js(fmt) + ",\"pages\":" + std::to_string(pj.size()) +
         ",\"paths\":" + std::to_string(total) + ",\"colorants\":" + joinJs(sn);
}

void dpartJson(OH node, const std::map<QPDFObjGen, int>& pidx, std::string& out, int depth) {
  out += "{";
  if (node.getKey("/DPM").isDictionary()) out += "\"metadata\":" + pdfToJson(node.getKey("/DPM")) + ",";
  OH parts = node.getKey("/DParts");
  if (parts.isArray() && depth < 64) {
    out += "\"parts\":[";
    bool first = true;
    for (int i = 0; i < parts.getArrayNItems(); ++i) {
      OH inner = parts.getArrayItem(i);
      for (int k = 0; inner.isArray() && k < inner.getArrayNItems(); ++k) {
        if (!first) out += ",";
        first = false;
        dpartJson(inner.getArrayItem(k), pidx, out, depth + 1);
      }
    }
    out += "]";
  } else {
    auto pg = [&](const char* k) {
      OH p = node.getKey(k);
      auto f = p.isDictionary() ? pidx.find(p.getObjGen()) : pidx.end();
      return f == pidx.end() ? std::string("null") : std::to_string(f->second + 1);
    };
    std::string s = pg("/Start"), e = node.hasKey("/End") ? pg("/End") : s;
    out += "\"start\":" + s + ",\"end\":" + e;
  }
  if (out.back() == ',') out.pop_back();
  out += "}";
}

std::string toolGetDparts(const Args& a) {
  auto q = openPdf(a.pos[0], a.get("password"));
  OH dr = q->getRoot().getKey("/DPartRoot");
  if (!dr.isDictionary()) fail("document has no DPart structure", 1);
  std::string out = "{\"recordLevel\":" + (dr.getKey("/RecordLevel").isInteger() ? std::to_string(dr.getKey("/RecordLevel").getIntValue()) : std::string("null"));
  out += ",\"nodeNames\":" + pdfToJson(dr.getKey("/NodeNameList").isArray() ? dr.getKey("/NodeNameList") : OH::newArray()) + ",\"root\":";
  dpartJson(dr.getKey("/DPartRootNode"), pageIndex(*q), out, 0);
  writeFile(a.pos[1], out + "}\n");
  return "\"output\":" + js(a.pos[1]);
}

int buildDpart(QPDF& q, const JSON& j, OH node, const std::vector<QPDFPageObjectHelper>& pages, int depth) {
  if (depth > 64) fail("DPart tree nested too deeply", 64);
  node.replaceKey("/Type", OH::newName("/DPart"));
  JSON md = j.getDictItem("metadata");
  if (md.isDictionary()) node.replaceKey("/DPM", jsonToPdf(md));
  JSON parts = j.getDictItem("parts");
  int n = 1;
  if (parts.isArray()) {
    OH kids = OH::newArray();
    parts.forEachArrayItem([&](JSON c) {
      OH kid = q.makeIndirectObject(OH::newDictionary());
      kid.replaceKey("/Parent", node);
      n += buildDpart(q, c, kid, pages, depth + 1);
      kids.appendItem(kid);
    });
    OH outer = OH::newArray();
    outer.appendItem(kids);
    node.replaceKey("/DParts", outer);
    return n;
  }
  int s = static_cast<int>(jNum(j, "start", 0)), e = static_cast<int>(jNum(j, "end", s));
  if (s < 1 || e < s || e > static_cast<int>(pages.size())) fail("DPart page range invalid: " + std::to_string(s) + "-" + std::to_string(e), 64);
  node.replaceKey("/Start", pages[s - 1].getObjectHandle());
  if (e != s) node.replaceKey("/End", pages[e - 1].getObjectHandle());
  for (int p = s; p <= e; ++p) OH(pages[p - 1].getObjectHandle()).replaceKey("/DPart", node);
  return n;
}

std::string toolSetDparts(const Args& a) {
  auto q = openPdf(a.pos[0], a.get("password"));
  JSON j = loadJson(a.pos[1]);
  auto pages = allPages(*q);
  for (auto& p : pages) p.getObjectHandle().removeKey("/DPart");
  JSON rootJ = j.getDictItem("root");
  if (!rootJ.isDictionary()) rootJ = j;
  OH dr = q->makeIndirectObject(OH::parse("<< /Type /DPartRoot >>"));
  OH rn = q->makeIndirectObject(OH::newDictionary());
  rn.replaceKey("/Parent", dr);
  int n = buildDpart(*q, rootJ, rn, pages, 0);
  dr.replaceKey("/DPartRootNode", rn);
  double rl = jNum(j, "recordLevel", NAN);
  if (!std::isnan(rl)) dr.replaceKey("/RecordLevel", OH::newInteger(static_cast<long long>(rl)));
  JSON nn = j.getDictItem("nodeNames");
  if (nn.isArray()) {
    OH arr = OH::newArray();
    nn.forEachArrayItem([&](JSON v) {
      std::string s;
      if (v.getString(s)) arr.appendItem(OH::newName("/" + s));
    });
    dr.replaceKey("/NodeNameList", arr);
  }
  q->getRoot().replaceKey("/DPartRoot", dr);
  savePdf(*q, a.pos[2]);
  return "\"output\":" + js(a.pos[2]) + ",\"nodes\":" + std::to_string(n);
}

OH metadataStream(QPDF& q, const std::string& xmp) {
  if (xmp.find("rdf:RDF") == std::string::npos && xmp.find("x:xmpmeta") == std::string::npos) fail("not an XMP packet", 64);
  OH s = q.newStream(xmp);
  s.getDict().replaceKey("/Type", OH::newName("/Metadata"));
  s.getDict().replaceKey("/Subtype", OH::newName("/XML"));
  return s;
}

std::string toolSetXmp(const Args& a) {
  auto q = openPdf(a.pos[0], a.get("password"));
  q->getRoot().replaceKey("/Metadata", metadataStream(*q, readFile(a.pos[1])));
  savePdf(*q, a.pos[2]);
  return "\"output\":" + js(a.pos[2]);
}

#ifdef KURA_WITH_PDFIUM
kura::Rasterizer rasterizerFor(const std::string& pdfBytes, const std::string& pw) {
  auto r = kura::makeRasterizer(pdfBytes.data(), pdfBytes.size(), pw);
  if (!r) fail("rasterizer could not open the document");
  return r;
}
#endif

std::string toolSeparations(const Args& a) {
#ifdef KURA_WITH_PDFIUM
  std::string bytes = readFile(a.pos[0]);
  auto q = openPdf(a.pos[0], a.get("password"));
  size_t n = allPages(*q).size();
  auto raster = rasterizerFor(bytes, a.get("password"));
  std::string dir = a.pos[1];
  ensureDir(dir);
  double dpi = a.number("dpi", 72);
  if (dpi < 1 || dpi > 2400) fail("--dpi must be 1..2400", 64);
  std::string iccBytes = a.has("profile") ? readFile(a.get("profile")) : std::string(reinterpret_cast<const char*>(pdfa::kCmykIcc), pdfa::kCmykIccLen);
  cmsHPROFILE src = cmsCreate_sRGBProfile();
  cmsHPROFILE dst = cmsOpenProfileFromMem(iccBytes.data(), static_cast<cmsUInt32Number>(iccBytes.size()));
  if (!dst || cmsGetColorSpace(dst) != cmsSigCmykData) {
    if (dst) cmsCloseProfile(dst);
    cmsCloseProfile(src);
    fail("CMYK output profile is not usable", 64);
  }
  cmsHTRANSFORM xf = cmsCreateTransform(src, TYPE_RGB_8, dst, TYPE_CMYK_8, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION);
  cmsCloseProfile(src);
  cmsCloseProfile(dst);
  if (!xf) fail("colour transform failed");
  std::unique_ptr<void, void (*)(void*)> guard(xf, [](void* t) { cmsDeleteTransform(t); });
  const char* plates[4] = {"Cyan", "Magenta", "Yellow", "Black"};
  std::vector<std::string> pages;
  for (int p : pageSel(a, n)) {
    int w = 0, h = 0;
    std::string rgb;
    if (!raster(p, dpi, w, h, rgb)) fail("rendering page " + std::to_string(p + 1) + " failed");
    size_t px = static_cast<size_t>(w) * h;
    std::string cmyk(px * 4, '\0');
    cmsDoTransform(xf, rgb.data(), &cmyk[0], static_cast<cmsUInt32Number>(px));
    std::vector<std::string> pl;
    for (int c = 0; c < 4; ++c) {
      std::string g(px, '\0');
      unsigned long long sum = 0;
      for (size_t i = 0; i < px; ++i) {
        unsigned char ink = static_cast<unsigned char>(cmyk[i * 4 + c]);
        sum += ink;
        g[i] = static_cast<char>(255 - ink);
      }
      std::string name = pageImageName(p, std::string("-") + plates[c], "png");
      writeFile((fs::path(dir) / name).string(), pngEncode(w, h, 1, g));
      pl.push_back("{\"plate\":" + js(plates[c]) + ",\"file\":" + js(name) + ",\"coverage\":" + num(px ? 100.0 * sum / (255.0 * px) : 0) + "}");
    }
    pages.push_back("{\"page\":" + std::to_string(p + 1) + ",\"width\":" + std::to_string(w) + ",\"height\":" + std::to_string(h) +
                    ",\"plates\":" + joinJs(pl) + "}");
  }
  return "\"outdir\":" + js(dir) + ",\"dpi\":" + num(dpi) + ",\"pages\":" + joinJs(pages);
#else
  (void)a;
  fail("this build has no page renderer");
#endif
}

std::string infoString(OH o) {
  if (o.isString()) return o.getUTF8Value();
  if (o.isName()) return o.getName().substr(1);
  return std::string();
}

std::string xmpValue(const std::string& xmp, const std::string& tag) {
  size_t p = xmp.find(tag);
  if (p == std::string::npos) return std::string();
  p += tag.size();
  if (p < xmp.size() && (xmp[p] == '=')) {
    char q = p + 1 < xmp.size() ? xmp[p + 1] : '"';
    size_t e = xmp.find(q, p + 2);
    return e == std::string::npos ? std::string() : xmp.substr(p + 2, e - p - 2);
  }
  size_t gt = xmp.find('>', p);
  size_t lt = gt == std::string::npos ? std::string::npos : xmp.find('<', gt);
  return lt == std::string::npos ? std::string() : xmp.substr(gt + 1, lt - gt - 1);
}

std::string boxString(OH b) {
  if (!b.isArray() || b.getArrayNItems() != 4) return std::string();
  std::string out;
  for (int i = 0; i < 4; ++i) out += (i ? " " : "") + num(b.getArrayItem(i).isNumber() ? b.getArrayItem(i).getNumericValue() : 0);
  return out;
}

std::string toolInfo(const Args& a) {
  std::string path = a.pos[0];
  auto q = openPdf(path, a.get("password"));
  OH info = q->getTrailer().getKey("/Info");
  OH root = q->getRoot();
  std::string xmp;
  if (root.getKey("/Metadata").isStream()) {
    try {
      xmp = streamBytes(root.getKey("/Metadata"));
    } catch (...) {
    }
  }
  auto field = [&](const char* key) { return info.isDictionary() ? infoString(info.getKey(key)) : std::string(); };
  auto pages = allPages(*q);
  std::string size, trim, bleed;
  if (!pages.empty()) {
    OH mb = pages[0].getAttribute("/MediaBox", false);
    if (mb.isArray() && mb.getArrayNItems() == 4)
      size = num(mb.getArrayItem(2).getNumericValue() - mb.getArrayItem(0).getNumericValue()) + " x " +
             num(mb.getArrayItem(3).getNumericValue() - mb.getArrayItem(1).getNumericValue()) + " pt";
    trim = boxString(pages[0].getObjectHandle().getKey("/TrimBox"));
    bleed = boxString(pages[0].getObjectHandle().getKey("/BleedBox"));
  }
  std::string pdfa;
  std::string part = xmpValue(xmp, "pdfaid:part"), conf = xmpValue(xmp, "pdfaid:conformance");
  if (!part.empty()) pdfa = "PDF/A-" + part + conf;
  std::string pdfx = field("/GTS_PDFXVersion");
  if (pdfx.empty()) pdfx = xmpValue(xmp, "pdfxid:GTS_PDFXVersion");
  std::string pdfe = xmpValue(xmp, "pdfe:ISO_PDFEVersion");
  std::string pdfvt = xmpValue(xmp, "pdfvtid:GTS_PDFVTVersion");
  std::string pdfua = xmpValue(xmp, "pdfuaid:part");
  if (!pdfua.empty()) pdfua = "PDF/UA-" + pdfua;
  std::error_code ec;
  auto bytes = fs::file_size(path, ec);
  std::vector<std::string> items = {
      "\"title\":" + js(field("/Title")), "\"author\":" + js(field("/Author")), "\"subject\":" + js(field("/Subject")),
      "\"keywords\":" + js(field("/Keywords")), "\"creator\":" + js(field("/Creator")), "\"producer\":" + js(field("/Producer")),
      "\"created\":" + js(field("/CreationDate")), "\"modified\":" + js(field("/ModDate")),
      "\"version\":" + js(q->getPDFVersion()), "\"fileSize\":" + std::to_string(ec ? 0 : bytes),
      "\"pages\":" + std::to_string(pages.size()), "\"pageSize\":" + js(size), "\"trimBox\":" + js(trim), "\"bleedBox\":" + js(bleed),
      "\"pageMode\":" + js(infoString(root.getKey("/PageMode"))), "\"pageLayout\":" + js(infoString(root.getKey("/PageLayout"))),
      "\"encrypted\":" + std::string(q->isEncrypted() ? "true" : "false"), "\"tagged\":" +
          std::string(root.getKey("/MarkInfo").isDictionary() && root.getKey("/MarkInfo").getKey("/Marked").isBool() &&
                              root.getKey("/MarkInfo").getKey("/Marked").getBoolValue() ? "true" : "false"),
      "\"pdfa\":" + js(pdfa), "\"pdfx\":" + js(pdfx), "\"pdfe\":" + js(pdfe), "\"pdfvt\":" + js(pdfvt), "\"pdfua\":" + js(pdfua)};
  std::string out;
  for (size_t i = 0; i < items.size(); ++i) out += (i ? "," : "") + items[i];
  return out;
}

std::string toolCxf(const Args& a) {
  auto q = openPdf(a.pos[0], a.get("password"));
  OH ois = q->getRoot().getKey("/OutputIntents");
  std::vector<OH> intents;
  for (int i = 0; ois.isArray() && i < ois.getArrayNItems(); ++i)
    if (ois.getArrayItem(i).isDictionary()) intents.push_back(ois.getArrayItem(i));
  std::set<std::string> spots;
  walkAll(*q, [&](OH h, OH) {
    if (h.isArray() && h.getArrayNItems() >= 2 && nameIs(h.getArrayItem(0), "/Separation") && h.getArrayItem(1).isName()) {
      std::string n = h.getArrayItem(1).getName().substr(1);
      if (n != "All" && n != "None" && n != "Cyan" && n != "Magenta" && n != "Yellow" && n != "Black") spots.insert(n);
    }
    if (h.isArray() && h.getArrayNItems() >= 2 && nameIs(h.getArrayItem(0), "/DeviceN") && h.getArrayItem(1).isArray()) {
      OH names = h.getArrayItem(1);
      for (int i = 0; i < names.getArrayNItems(); ++i) {
        if (!names.getArrayItem(i).isName()) continue;
        std::string n = names.getArrayItem(i).getName().substr(1);
        if (n != "Cyan" && n != "Magenta" && n != "Yellow" && n != "Black" && n != "None" && n != "All") spots.insert(n);
      }
    }
  });
  if (a.has("embed")) {
    if (intents.empty()) fail("the document has no output intent to attach colour data to", 2);
    if (a.pos.size() < 2) fail("missing output file", 64);
    std::string dir = a.get("embed");
    OH sd = OH::newDictionary();
    int added = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
      std::string ext = e.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (ext != ".cxf" && ext != ".xml") continue;
      std::string data = readFile(e.path().string());
      if (data.find("CxF") == std::string::npos) continue;
      OH st = q->makeIndirectObject(OH::newStream(q.get(), data));
      sd.replaceKey("/" + e.path().stem().string(), st);
      ++added;
    }
    if (!added) fail("no CxF files found in " + dir, 2);
    for (OH& oi : intents) oi.replaceKey("/SpectralData", sd);
    savePdf(*q, a.pos[1], [](QPDFWriter& w) { w.setMinimumPDFVersion("2.0"); });
    return "\"embedded\":" + std::to_string(added) + ",\"output\":" + js(a.pos[1]);
  }
  if (a.has("remove")) {
    if (a.pos.size() < 2) fail("missing output file", 64);
    int removed = 0;
    for (OH& oi : intents)
      if (oi.hasKey("/SpectralData")) oi.removeKey("/SpectralData"), ++removed;
    savePdf(*q, a.pos[1]);
    return "\"removed\":" + std::to_string(removed) + ",\"output\":" + js(a.pos[1]);
  }
  std::string dir = a.pos.size() > 1 ? a.pos[1] : std::string();
  if (!dir.empty()) ensureDir(dir);
  std::map<std::string, std::string> entries;
  for (OH& oi : intents) {
    OH sd = oi.getKey("/SpectralData");
    if (!sd.isDictionary()) continue;
    for (const std::string& k : sd.getKeys()) {
      if (!sd.getKey(k).isStream()) continue;
      try {
        entries[k.substr(1)] = streamBytes(sd.getKey(k));
      } catch (...) {
      }
    }
  }
  std::vector<std::string> items, findings;
  int full = 0, paperOnly = 0, solidOnly = 0;
  for (auto& [name, data] : entries) {
    bool wellFormed = data.find("<") != std::string::npos && data.find("CxF") != std::string::npos;
    bool onBlack = data.find("Black") != std::string::npos || data.find("black") != std::string::npos;
    int samples = 0;
    for (size_t p = data.find("<cc:Object"); p != std::string::npos; p = data.find("<cc:Object", p + 1))
      if (p + 10 < data.size() && (data[p + 10] == ' ' || data[p + 10] == '>' || data[p + 10] == '/')) ++samples;
    std::string level = samples <= 1 ? "X-4b" : onBlack ? "X-4" : "X-4a";
    (level == "X-4" ? full : level == "X-4a" ? paperOnly : solidOnly)++;
    std::string file;
    if (!dir.empty()) {
      for (char c : name) file += std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ' ' ? c : '_';
      file = (file.empty() || file.find_first_not_of('_') == std::string::npos ? "colorant" : file) + ".cxf";
      writeFile((fs::path(dir) / file).string(), data);
    }
    if (!wellFormed) findings.push_back("{\"severity\":\"error\",\"message\":" + js("colour data for " + name + " is not a valid CxF document") + "}");
    if (!spots.count(name)) findings.push_back("{\"severity\":\"info\",\"message\":" + js("colour data for " + name + " has no matching spot colour in the document") + "}");
    items.push_back("{\"colorant\":" + js(name) + ",\"level\":" + js(level) + ",\"samples\":" + std::to_string(samples) +
                    (file.empty() ? "" : ",\"file\":" + js(file)) + "}");
  }
  if (entries.empty()) findings.push_back("{\"severity\":\"error\",\"message\":\"the document carries no CxF colour data\"}");
  for (const std::string& sp : spots)
    if (!entries.count(sp)) findings.push_back("{\"severity\":\"warning\",\"message\":" + js("spot colour " + sp + " has no CxF colour data") + "}");
  return "\"entries\":" + joinJs(items) + ",\"spotColours\":" + std::to_string(spots.size()) + ",\"findings\":" + joinJs(findings);
}

struct PlateScan : public QPDFObjectHandle::ParserCallbacks {
  OH res;
  std::map<std::string, double>& maxTint;
  int depth;
  std::set<QPDFObjGen>& seen;
  std::vector<OH> ops;
  std::vector<std::string> fillNames, strokeNames;
  PlateScan(OH r, std::map<std::string, double>& m, int d, std::set<QPDFObjGen>& s) : res(r), maxTint(m), depth(d), seen(s) {}
  std::vector<std::string> colorantsOf(OH cs) {
    std::vector<std::string> out;
    if (cs.isName() && res.isDictionary() && res.getKey("/ColorSpace").isDictionary()) cs = res.getKey("/ColorSpace").getKey(cs.getName());
    if (!cs.isArray() || cs.getArrayNItems() < 2) return out;
    if (nameIs(cs.getArrayItem(0), "/Separation") && cs.getArrayItem(1).isName()) out.push_back(cs.getArrayItem(1).getName().substr(1));
    if (nameIs(cs.getArrayItem(0), "/DeviceN") && cs.getArrayItem(1).isArray())
      for (int i = 0; i < cs.getArrayItem(1).getArrayNItems(); ++i)
        if (cs.getArrayItem(1).getArrayItem(i).isName()) out.push_back(cs.getArrayItem(1).getArrayItem(i).getName().substr(1));
    return out;
  }
  void note(const std::vector<std::string>& names) {
    size_t n = std::min(names.size(), ops.size());
    for (size_t i = 0; i < n; ++i) {
      if (!ops[i].isNumber()) continue;
      const std::string& nm = names[i];
      if (nm == "Cyan" || nm == "Magenta" || nm == "Yellow" || nm == "Black" || nm == "None" || nm == "All") continue;
      double v = ops[i].getNumericValue() * 100;
      if (v > maxTint[nm]) maxTint[nm] = v;
    }
  }
  void handleObject(OH o, size_t, size_t) override {
    if (!o.isOperator()) {
      ops.push_back(o);
      return;
    }
    std::string op = o.getOperatorValue();
    if (op == "cs" && !ops.empty()) fillNames = colorantsOf(ops.back());
    else if (op == "CS" && !ops.empty()) strokeNames = colorantsOf(ops.back());
    else if (op == "scn" || op == "sc") note(fillNames);
    else if (op == "SCN" || op == "SC") note(strokeNames);
    else if (op == "Do" && !ops.empty() && ops.back().isName() && res.isDictionary() && res.getKey("/XObject").isDictionary()) {
      OH x = res.getKey("/XObject").getKey(ops.back().getName());
      if (x.isStream()) {
        OH d = x.getDict();
        if (nameIs(d.getKey("/Subtype"), "/Image")) {
          for (const std::string& nm : colorantsOf(d.getKey("/ColorSpace")))
            if (nm != "Cyan" && nm != "Magenta" && nm != "Yellow" && nm != "Black" && nm != "None" && nm != "All") maxTint[nm] = std::max(maxTint[nm], 100.0);
        } else if (nameIs(d.getKey("/Subtype"), "/Form") && depth < 10 && (!x.isIndirect() || seen.insert(x.getObjGen()).second)) {
          OH fr = d.getKey("/Resources");
          PlateScan sub(fr.isDictionary() ? fr : res, maxTint, depth + 1, seen);
          try {
            OH::parseContentStream(x, &sub);
          } catch (...) {
          }
        }
      }
    }
    ops.clear();
  }
  void handleEOF() override {}
};

std::string toolPlates(const Args& a) {
#ifdef KURA_WITH_PDFIUM
  std::string bytes = readFile(a.pos[0]);
  auto q = openPdf(a.pos[0], a.get("password"));
  auto pages = allPages(*q);
  auto raster = rasterizerFor(bytes, a.get("password"));
  double threshold = a.number("threshold", 1);
  double areaMm = a.number("area", 10);
  double dpi = a.number("dpi", 72);
  if (dpi < 18 || dpi > 600) fail("--dpi must be 18..600", 64);
  std::string iccBytes = a.has("profile") ? readFile(a.get("profile")) : std::string(reinterpret_cast<const char*>(pdfa::kCmykIcc), pdfa::kCmykIccLen);
  cmsHPROFILE src = cmsCreate_sRGBProfile();
  cmsHPROFILE dst = cmsOpenProfileFromMem(iccBytes.data(), static_cast<cmsUInt32Number>(iccBytes.size()));
  if (!dst || cmsGetColorSpace(dst) != cmsSigCmykData) {
    if (dst) cmsCloseProfile(dst);
    cmsCloseProfile(src);
    fail("CMYK output profile is not usable", 64);
  }
  cmsHTRANSFORM xf = cmsCreateTransform(src, TYPE_RGB_8, dst, TYPE_CMYK_8, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION);
  cmsCloseProfile(src);
  cmsCloseProfile(dst);
  if (!xf) fail("colour transform failed");
  std::unique_ptr<void, void (*)(void*)> guard(xf, [](void* t) { cmsDeleteTransform(t); });
  const char* process[4] = {"Cyan", "Magenta", "Yellow", "Black"};
  std::vector<std::string> out;
  int onlyBlack = 0, withSpot = 0, withCmy = 0;
  for (int p : pageSel(a, pages.size())) {
    int w = 0, h = 0;
    std::string rgb;
    if (!raster(p, dpi, w, h, rgb)) fail("rendering page " + std::to_string(p + 1) + " failed");
    size_t px = static_cast<size_t>(w) * h;
    std::string cmyk(px * 4, '\0');
    cmsDoTransform(xf, rgb.data(), &cmyk[0], static_cast<cmsUInt32Number>(px));
    for (size_t i = 0; i < px; ++i) {
      int r = static_cast<unsigned char>(rgb[i * 3]), g = static_cast<unsigned char>(rgb[i * 3 + 1]),
          b = static_cast<unsigned char>(rgb[i * 3 + 2]);
      if (std::max({r, g, b}) - std::min({r, g, b}) > 2) continue;
      cmyk[i * 4] = cmyk[i * 4 + 1] = cmyk[i * 4 + 2] = '\0';
      cmyk[i * 4 + 3] = static_cast<char>(255 - (r + g + b) / 3);
    }
    int cell = std::max(1, static_cast<int>(std::lround(areaMm / 25.4 * dpi)));
    double best[4] = {0, 0, 0, 0};
    for (int cy = 0; cy < h; cy += cell) {
      for (int cx = 0; cx < w; cx += cell) {
        double sum[4] = {0, 0, 0, 0};
        int count = 0;
        for (int y = cy; y < std::min(h, cy + cell); ++y)
          for (int x = cx; x < std::min(w, cx + cell); ++x, ++count)
            for (int c = 0; c < 4; ++c) sum[c] += static_cast<unsigned char>(cmyk[(static_cast<size_t>(y) * w + x) * 4 + c]);
        for (int c = 0; c < 4; ++c) best[c] = std::max(best[c], count ? 100.0 * sum[c] / (255.0 * count) : 0);
      }
    }
    std::map<std::string, double> spot;
    std::set<QPDFObjGen> seen;
    OH res = pages[p].getAttribute("/Resources", false);
    PlateScan scan(res, spot, 0, seen);
    try {
      pages[p].parseContents(&scan);
    } catch (...) {
    }
    std::vector<std::string> plates;
    bool cmy = false, black = best[3] >= threshold, anySpot = false;
    for (int c = 0; c < 4; ++c) {
      bool used = best[c] >= threshold;
      if (c < 3 && used) cmy = true;
      plates.push_back("{\"plate\":" + js(process[c]) + ",\"maxInk\":" + num(best[c]) + ",\"used\":" + (used ? "true" : "false") + "}");
    }
    for (auto& [name, tint] : spot) {
      bool used = tint >= threshold;
      anySpot = anySpot || used;
      plates.push_back("{\"plate\":" + js(name) + ",\"maxInk\":" + num(tint) + ",\"used\":" + (used ? "true" : "false") + ",\"spot\":true}");
    }
    int count = 0;
    for (int c = 0; c < 4; ++c) count += best[c] >= threshold;
    for (auto& [name, tint] : spot) count += tint >= threshold;
    if (black && !cmy && !anySpot) ++onlyBlack;
    if (anySpot) ++withSpot;
    if (cmy) ++withCmy;
    out.push_back("{\"page\":" + std::to_string(p + 1) + ",\"platesUsed\":" + std::to_string(count) + ",\"plates\":" + joinJs(plates) + "}");
  }
  return "\"threshold\":" + num(threshold) + ",\"areaMm\":" + num(areaMm) + ",\"pagesOnlyBlack\":" + std::to_string(onlyBlack) +
         ",\"pagesWithSpot\":" + std::to_string(withSpot) + ",\"pagesWithCmy\":" + std::to_string(withCmy) + ",\"pages\":" + joinJs(out);
#else
  (void)a;
  fail("this build has no page renderer");
#endif
}

struct Tool {
  const char* name;
  const char* usage;
  std::set<std::string> valued, bools;
  size_t minPos, maxPos;
  std::function<std::string(const Args&)> run;
};

const std::vector<Tool>& tools() {
  static const std::vector<Tool> t = {
      {"extract-icc", "extract-icc <in.pdf> <outdir>", {}, {}, 2, 2, toolExtractIcc},
      {"extract-xmp", "extract-xmp <in.pdf> <outdir> [--document-only]", {}, {"document-only"}, 2, 2, toolExtractXmp},
      {"extract-dieline", "extract-dieline <in.pdf> <out.pdf|out.json> [--format pdf|json] [--names REGEX] [--pages R]", {"format", "names"}, {}, 2, 2, toolExtractDieline},
      {"get-dparts", "get-dparts <in.pdf> <out.json>", {}, {}, 2, 2, toolGetDparts},
      {"set-dparts", "set-dparts <in.pdf> <dparts.json> <out.pdf>", {}, {}, 3, 3, toolSetDparts},
      {"set-xmp", "set-xmp <in.pdf> <metadata.xmp> <out.pdf>", {}, {}, 3, 3, toolSetXmp},
      {"separations", "separations <in.pdf> <outdir> [--dpi N] [--profile cmyk.icc] [--pages R]", {"dpi", "profile"}, {}, 2, 2, toolSeparations},
      {"info", "info <in.pdf>", {}, {}, 1, 1, toolInfo},
      {"cxf", "cxf <in.pdf> [<outdir>]  |  cxf <in.pdf> <out.pdf> --embed <dir>  |  cxf <in.pdf> <out.pdf> --remove", {"embed"}, {"remove"}, 1, 2, toolCxf},
      {"plates", "plates <in.pdf> [--threshold 1] [--area 10] [--dpi 72] [--profile cmyk.icc] [--pages R]", {"threshold", "area", "dpi", "profile"}, {}, 1, 1, toolPlates},
  };
  return t;
}
}

int toolOutput(const std::string& name, const std::vector<std::string>& args) {
  const auto& all = tools();
  auto it = std::find_if(all.begin(), all.end(), [&](const Tool& t) { return name == t.name; });
  if (it == all.end()) return kUnknownTool;
  const Tool& t = *it;
  std::string usage = std::string("usage: kura tool ") + t.usage + "  (all tools: [--password PW])";
  auto report = [&](int code, const std::string& msg) {
    std::cout << "{\"ok\":false,\"tool\":" << js(t.name) << ",\"error\":" << js(msg) << "}" << std::endl;
    if (code == 64) std::cerr << usage << std::endl;
    return code;
  };
  Args a;
  for (size_t i = 0; i < args.size(); ++i) {
    std::string s = args[i];
    if (s == "--help" || s == "-h") {
      std::cout << usage << std::endl;
      return 0;
    }
    if ((s.size() > 2 && s.compare(0, 2, "--") == 0) || s == "-p") {
      std::string k = s == "-p" ? "pages" : s.substr(2), v;
      size_t eq = k.find('=');
      bool hasEq = eq != std::string::npos;
      if (hasEq) v = k.substr(eq + 1), k = k.substr(0, eq);
      if (t.valued.count(k) || k == "password" || k == "pages") {
        if (!hasEq) {
          if (i + 1 >= args.size()) return report(64, "missing value for --" + k);
          v = args[++i];
        }
        a.kv[k] = v;
      } else if (t.bools.count(k) && !hasEq) {
        a.flags.insert(k);
      } else {
        return report(64, "unknown option --" + k);
      }
    } else {
      a.pos.push_back(s);
    }
  }
  if (a.pos.size() < t.minPos || a.pos.size() > t.maxPos) return report(64, "wrong number of arguments");
  try {
    std::string body = t.run(a);
    std::cout << "{\"ok\":true,\"tool\":" << js(t.name) << "," << body << "}" << std::endl;
    return 0;
  } catch (Fail& f) {
    return report(f.code, f.what());
  } catch (std::exception& e) {
    return report(2, e.what());
  }
}
}
