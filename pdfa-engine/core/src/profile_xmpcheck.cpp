#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "profile_ext.hh"

namespace pdfa {

namespace {
const std::string kRdf = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
const std::string kXmlNs = "http://www.w3.org/XML/1998/namespace";
const std::string kExtNs = "http://www.aiim.org/pdfa/ns/extension/";
const std::string kSchemaNs = "http://www.aiim.org/pdfa/ns/schema#";
const std::string kPropNs = "http://www.aiim.org/pdfa/ns/property#";
const std::string kTypeNs = "http://www.aiim.org/pdfa/ns/type#";
const std::string kFieldNs = "http://www.aiim.org/pdfa/ns/field#";
const std::string kDc = "http://purl.org/dc/elements/1.1/";
const std::string kXmp = "http://ns.adobe.com/xap/1.0/";
const std::string kRights = "http://ns.adobe.com/xap/1.0/rights/";
const std::string kMM = "http://ns.adobe.com/xap/1.0/mm/";
const std::string kBJ = "http://ns.adobe.com/xap/1.0/bj/";
const std::string kTPg = "http://ns.adobe.com/xap/1.0/t/pg/";
const std::string kDM = "http://ns.adobe.com/xmp/1.0/DynamicMedia/";
const std::string kPdf = "http://ns.adobe.com/pdf/1.3/";
const std::string kPs = "http://ns.adobe.com/photoshop/1.0/";
const std::string kTiff = "http://ns.adobe.com/tiff/1.0/";
const std::string kExif = "http://ns.adobe.com/exif/1.0/";
const std::string kAux = "http://ns.adobe.com/exif/1.0/aux/";
const std::string kCrs = "http://ns.adobe.com/camera-raw-settings/1.0/";
const std::string kIdq = "http://ns.adobe.com/xmp/Identifier/qual/1.0/";
const std::string kPdfaId = "http://www.aiim.org/pdfa/ns/id/";
const std::string kPdfeId = "http://www.aiim.org/pdfe/ns/id/";
const std::string kPdfxId = "http://www.npes.org/pdfx/ns/id/";
const std::string kPdfvtId = "http://www.npes.org/pdfvt/ns/id/";
const std::string kStRef = "http://ns.adobe.com/xap/1.0/sType/ResourceRef#";
const std::string kStEvt = "http://ns.adobe.com/xap/1.0/sType/ResourceEvent#";
const std::string kStVer = "http://ns.adobe.com/xap/1.0/sType/Version#";
const std::string kStDim = "http://ns.adobe.com/xap/1.0/sType/Dimensions#";
const std::string kStJob = "http://ns.adobe.com/xap/1.0/sType/Job#";
const std::string kStFnt = "http://ns.adobe.com/xap/1.0/sType/Font#";
const std::string kXapG = "http://ns.adobe.com/xap/1.0/g/";
const std::string kXapGImg = "http://ns.adobe.com/xap/1.0/g/img/";

struct XAttr {
  std::string ns, local, value, prefix;
};

struct XNode {
  std::string ns, local, prefix;
  std::vector<XAttr> attrs;
  std::vector<std::pair<std::string, std::string>> decls;
  std::vector<XNode> kids;
  std::string text;
};

class XmlParser {
 public:
  explicit XmlParser(const std::string& s) : s_(s) {}

  bool parse(XNode& root) {
    if (s_.compare(0, 3, "\xEF\xBB\xBF") == 0) p_ = 3;
    bool haveRoot = false;
    while (true) {
      skipWs();
      if (p_ >= s_.size()) return haveRoot;
      if (s_.compare(p_, 2, "<?") == 0) {
        if (!skipTo("?>")) return false;
      } else if (s_.compare(p_, 4, "<!--") == 0) {
        if (!skipTo("-->")) return false;
      } else if (s_.compare(p_, 9, "<!DOCTYPE") == 0) {
        size_t lb = s_.find('[', p_), gt = s_.find('>', p_);
        if (gt == std::string::npos) return false;
        if (lb != std::string::npos && lb < gt) {
          size_t e = s_.find("]>", lb);
          if (e == std::string::npos) return false;
          p_ = e + 2;
        } else {
          p_ = gt + 1;
        }
      } else if (s_[p_] == '<') {
        if (haveRoot) return false;
        if (!element(root, 0)) return false;
        haveRoot = true;
      } else if (s_[p_] == '\0') {
        ++p_;
      } else {
        return false;
      }
    }
  }

 private:
  const std::string& s_;
  size_t p_ = 0;
  std::vector<std::map<std::string, std::string>> scopes_;

  void skipWs() {
    while (p_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[p_]))) ++p_;
  }

  bool skipTo(const char* end) {
    size_t e = s_.find(end, p_);
    if (e == std::string::npos) return false;
    p_ = e + std::char_traits<char>::length(end);
    return true;
  }

  static bool nameStart(unsigned char c) { return std::isalpha(c) || c == '_' || c == ':' || c >= 0x80; }
  static bool nameChar(unsigned char c) { return nameStart(c) || std::isdigit(c) || c == '-' || c == '.'; }

  bool name(std::string& out) {
    if (p_ >= s_.size() || !nameStart(static_cast<unsigned char>(s_[p_]))) return false;
    size_t b = p_;
    while (p_ < s_.size() && nameChar(static_cast<unsigned char>(s_[p_]))) ++p_;
    out = s_.substr(b, p_ - b);
    return true;
  }

  static bool decode(const std::string& in, std::string& out) {
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) {
      if (in[i] == '<') return false;
      if (in[i] != '&') {
        out += in[i];
        continue;
      }
      size_t semi = in.find(';', i);
      if (semi == std::string::npos) return false;
      std::string ent = in.substr(i + 1, semi - i - 1);
      if (ent == "lt") out += '<';
      else if (ent == "gt") out += '>';
      else if (ent == "amp") out += '&';
      else if (ent == "quot") out += '"';
      else if (ent == "apos") out += '\'';
      else if (ent.size() > 1 && ent[0] == '#') {
        bool hex = ent[1] == 'x';
        std::string digits = ent.substr(hex ? 2 : 1);
        if (digits.empty()) return false;
        for (char c : digits) {
          if (hex ? !std::isxdigit(static_cast<unsigned char>(c)) : !std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        unsigned long cp = std::strtoul(digits.c_str(), nullptr, hex ? 16 : 10);
        if (cp < 0x80) {
          out += static_cast<char>(cp);
        } else if (cp < 0x800) {
          out += static_cast<char>(0xC0 | (cp >> 6));
          out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
          out += static_cast<char>(0xE0 | (cp >> 12));
          out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
          out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
          out += static_cast<char>(0xF0 | (cp >> 18));
          out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
          out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
          out += static_cast<char>(0x80 | (cp & 0x3F));
        }
      } else {
        return false;
      }
      i = semi;
    }
    return true;
  }

  bool resolve(const std::string& qname, bool isAttr, std::string& ns, std::string& local, std::string& prefix) {
    size_t c = qname.find(':');
    prefix = c == std::string::npos ? std::string() : qname.substr(0, c);
    local = c == std::string::npos ? qname : qname.substr(c + 1);
    if (prefix == "xml") {
      ns = kXmlNs;
      return true;
    }
    if (prefix.empty() && isAttr) {
      ns.clear();
      return true;
    }
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto f = it->find(prefix);
      if (f != it->end()) {
        ns = f->second;
        return true;
      }
    }
    ns.clear();
    return prefix.empty();
  }

  bool element(XNode& n, int depth) {
    if (depth > 200) return false;
    ++p_;
    std::string qn;
    if (!name(qn)) return false;
    std::vector<std::pair<std::string, std::string>> raw;
    std::map<std::string, std::string> scope;
    bool selfClose = false;
    while (true) {
      skipWs();
      if (p_ >= s_.size()) return false;
      if (s_.compare(p_, 2, "/>") == 0) {
        p_ += 2;
        selfClose = true;
        break;
      }
      if (s_[p_] == '>') {
        ++p_;
        break;
      }
      std::string an;
      if (!name(an)) return false;
      skipWs();
      if (p_ >= s_.size() || s_[p_] != '=') return false;
      ++p_;
      skipWs();
      if (p_ >= s_.size() || (s_[p_] != '"' && s_[p_] != '\'')) return false;
      char q = s_[p_++];
      size_t e = s_.find(q, p_);
      if (e == std::string::npos) return false;
      std::string val;
      if (!decode(s_.substr(p_, e - p_), val)) return false;
      p_ = e + 1;
      for (const auto& r : raw) {
        if (r.first == an) return false;
      }
      raw.push_back({an, val});
      if (an == "xmlns") scope[""] = val;
      else if (an.rfind("xmlns:", 0) == 0) scope[an.substr(6)] = val;
    }
    scopes_.push_back(scope);
    for (const auto& d : scope) n.decls.push_back(d);
    if (!resolve(qn, false, n.ns, n.local, n.prefix)) return false;
    for (const auto& r : raw) {
      if (r.first == "xmlns" || r.first.rfind("xmlns:", 0) == 0) continue;
      XAttr at;
      if (!resolve(r.first, true, at.ns, at.local, at.prefix)) return false;
      at.value = r.second;
      n.attrs.push_back(at);
    }
    if (!selfClose) {
      while (true) {
        if (p_ >= s_.size()) return false;
        if (s_.compare(p_, 2, "</") == 0) {
          p_ += 2;
          std::string en;
          if (!name(en) || en != qn) return false;
          skipWs();
          if (p_ >= s_.size() || s_[p_] != '>') return false;
          ++p_;
          break;
        }
        if (s_.compare(p_, 4, "<!--") == 0) {
          if (!skipTo("-->")) return false;
          continue;
        }
        if (s_.compare(p_, 9, "<![CDATA[") == 0) {
          size_t e = s_.find("]]>", p_);
          if (e == std::string::npos) return false;
          n.text += s_.substr(p_ + 9, e - p_ - 9);
          p_ = e + 3;
          continue;
        }
        if (s_.compare(p_, 2, "<?") == 0) {
          if (!skipTo("?>")) return false;
          continue;
        }
        if (s_[p_] == '<') {
          n.kids.emplace_back();
          if (!element(n.kids.back(), depth + 1)) return false;
          continue;
        }
        size_t e = s_.find('<', p_);
        if (e == std::string::npos) return false;
        std::string t;
        if (!decode(s_.substr(p_, e - p_), t)) return false;
        n.text += t;
        p_ = e;
      }
    }
    scopes_.pop_back();
    return true;
  }
};

struct XProp {
  std::string ns, prefix, name;
  int kind = 0;
  std::string arr;
  std::string value;
  bool hasLang = false;
  std::string lang;
  std::vector<XProp> kids;
};

struct XPacket {
  bool ok = false;
  std::vector<XProp> props;
  std::vector<std::pair<std::string, std::string>> decls;
};

bool isRdf(const XNode& n, const char* local) { return n.ns == kRdf && n.local == local; }

std::string trimWs(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return std::string();
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

const XAttr* attrOf(const XNode& n, const std::string& ns, const char* local) {
  for (const XAttr& a : n.attrs) {
    if (a.ns == ns && a.local == local) return &a;
  }
  return nullptr;
}

bool plainAttr(const XAttr& a) { return !a.ns.empty() && a.ns != kRdf && a.ns != kXmlNs; }

XProp fromAttr(const XAttr& a) {
  XProp p;
  p.ns = a.ns;
  p.prefix = a.prefix;
  p.name = a.local;
  p.value = a.value;
  return p;
}

XProp parseProp(const XNode& e);

void fillStruct(XProp& p, const XNode& holder) {
  p.kind = 1;
  for (const XAttr& a : holder.attrs) {
    if (plainAttr(a)) p.kids.push_back(fromAttr(a));
  }
  for (const XNode& k : holder.kids) p.kids.push_back(parseProp(k));
  for (const XProp& f : p.kids) {
    if (f.ns != kRdf || f.name != "value") continue;
    XProp v = f;
    v.ns = p.ns;
    v.prefix = p.prefix;
    v.name = p.name;
    v.hasLang = v.hasLang || p.hasLang;
    v.lang = v.hasLang && !f.hasLang ? p.lang : v.lang;
    p = v;
    return;
  }
}

XProp parseProp(const XNode& e) {
  XProp p;
  p.ns = e.ns;
  p.prefix = e.prefix;
  p.name = e.local;
  p.hasLang = attrOf(e, kXmlNs, "lang") != nullptr;
  if (p.hasLang) p.lang = attrOf(e, kXmlNs, "lang")->value;
  if (const XAttr* r = attrOf(e, kRdf, "resource")) {
    p.value = r->value;
    return p;
  }
  const XAttr* pt = attrOf(e, kRdf, "parseType");
  if (pt && pt->value == "Resource") {
    fillStruct(p, e);
    return p;
  }
  if (pt && pt->value == "Literal") {
    p.value = e.text;
    return p;
  }
  if (!e.kids.empty()) {
    const XNode& c = e.kids.front();
    if (e.kids.size() == 1 && (isRdf(c, "Bag") || isRdf(c, "Seq") || isRdf(c, "Alt"))) {
      p.kind = 2;
      p.arr = c.local;
      for (const XNode& li : c.kids) p.kids.push_back(parseProp(li));
      return p;
    }
    if (e.kids.size() == 1 && isRdf(c, "Description")) {
      fillStruct(p, c);
      return p;
    }
    fillStruct(p, e);
    return p;
  }
  bool attrFields = false;
  for (const XAttr& a : e.attrs) attrFields = attrFields || plainAttr(a);
  if (attrFields) {
    fillStruct(p, e);
    return p;
  }
  p.value = e.text;
  return p;
}

const XNode* findRdf(const XNode& n, int depth = 0) {
  if (isRdf(n, "RDF")) return &n;
  if (depth > 8) return nullptr;
  for (const XNode& k : n.kids) {
    if (const XNode* r = findRdf(k, depth + 1)) return r;
  }
  return nullptr;
}

bool rdfShapeOk(const XNode& n, int depth) {
  if (depth > 200) return false;
  const XAttr* pt = attrOf(n, kRdf, "parseType");
  if (pt && pt->value != "Resource") return false;
  if (isRdf(n, "Description")) {
    int ids = 0;
    for (const char* k : {"about", "ID", "nodeID"}) ids += attrOf(n, kRdf, k) ? 1 : 0;
    if (ids > 1) return false;
  }
  for (const XNode& k : n.kids) {
    if (!rdfShapeOk(k, depth + 1)) return false;
  }
  return true;
}

bool noDuplicateFields(const std::vector<XProp>& props, int depth) {
  if (depth > 64) return false;
  std::set<std::pair<std::string, std::string>> seen;
  for (const XProp& p : props) {
    if (!seen.insert({p.ns, p.name}).second) return false;
    if (p.kind == 1 && !noDuplicateFields(p.kids, depth + 1)) return false;
    for (const XProp& k : p.kind == 2 ? p.kids : std::vector<XProp>()) {
      if (k.kind == 1 && !noDuplicateFields(k.kids, depth + 1)) return false;
    }
  }
  return true;
}

void collectDecls(const XNode& n, std::vector<std::pair<std::string, std::string>>& out) {
  for (const auto& d : n.decls) out.push_back(d);
  for (const XNode& k : n.kids) collectDecls(k, out);
}

XPacket parsePacket(const std::string& s) {
  XPacket pk;
  XNode root;
  XmlParser xp(s);
  if (!xp.parse(root)) return pk;
  const XNode* rdf = findRdf(root);
  if (!rdf) return pk;
  collectDecls(root, pk.decls);
  if (!rdfShapeOk(*rdf, 0)) return pk;
  std::string about;
  for (const XNode& d : rdf->kids) {
    const XAttr* ab = attrOf(d, kRdf, "about");
    if (!ab || ab->value.empty()) continue;
    if (!about.empty() && ab->value != about) return pk;
    if (about.empty()) about = ab->value;
  }
  for (const XNode& d : rdf->kids) {
    for (const XAttr& a : d.attrs) {
      if (plainAttr(a)) pk.props.push_back(fromAttr(a));
    }
    for (const XNode& k : d.kids) pk.props.push_back(parseProp(k));
  }
  if (!noDuplicateFields(pk.props, 0)) {
    pk.props.clear();
    return pk;
  }
  pk.ok = true;
  return pk;
}

using FieldMap = std::map<std::string, std::string>;

struct StructDef {
  std::string ns;
  FieldMap fields;
  bool any = false;
};

const std::map<std::string, StructDef>& structTypes() {
  static const std::map<std::string, StructDef> m = {
      {"ResourceRef", {kStRef, {{"instanceID", "URI"}, {"documentID", "URI"}, {"versionID", "Text"}, {"renditionClass", "RenditionClass"}, {"renditionParams", "Text"}, {"manager", "AgentName"}, {"managerVariant", "Text"}, {"manageTo", "URI"}, {"manageUI", "URI"}}}},
      {"ResourceEvent", {kStEvt, {{"action", "Open Choice of Text"}, {"instanceID", "URI"}, {"parameters", "Text"}, {"softwareAgent", "AgentName"}, {"when", "Date"}}}},
      {"Version", {kStVer, {{"comments", "Text"}, {"event", "ResourceEvent"}, {"modifyDate", "Date"}, {"modifier", "ProperName"}, {"version", "Text"}}}},
      {"Thumbnail", {kXapGImg, {{"height", "Integer"}, {"width", "Integer"}, {"format", "Closed Choice of Text|JPEG"}, {"image", "Text"}}}},
      {"Dimensions", {kStDim, {{"w", "Real"}, {"h", "Real"}, {"unit", "Open Choice of Text"}}}},
      {"Job", {kStJob, {{"name", "Text"}, {"id", "Text"}, {"url", "URL"}}}},
      {"Font", {kStFnt, {{"fontName", "Text"}, {"fontFamily", "Text"}, {"fontFace", "Text"}, {"fontType", "Open Choice of Text"}, {"versionString", "Text"}, {"composite", "Boolean"}, {"fontFileName", "Text"}, {"childFontFiles", "Seq Text"}}}},
      {"Colorant", {kXapG, {{"swatchName", "Text"}, {"mode", "Closed Choice of Text|CMYK|RGB|LAB"}, {"type", "Closed Choice of Text|PROCESS|SPOT"}, {"cyan", "Real"}, {"magenta", "Real"}, {"yellow", "Real"}, {"black", "Real"}, {"red", "Integer"}, {"green", "Integer"}, {"blue", "Integer"}, {"L", "Real"}, {"A", "Integer"}, {"B", "Integer"}}}},
      {"Flash", {kExif, {{"Fired", "Boolean"}, {"Return", "Closed Choice of Integer|0|2|3"}, {"Mode", "Closed Choice of Integer|0|1|2|3"}, {"Function", "Boolean"}, {"RedEyeMode", "Boolean"}}}},
      {"OECF/SFR", {kExif, {{"Columns", "Integer"}, {"Rows", "Integer"}, {"Names", "Seq Text"}, {"Values", "Seq Rational"}}}},
      {"CFAPattern", {kExif, {{"Columns", "Integer"}, {"Rows", "Integer"}, {"Values", "Seq Integer"}}}},
      {"DeviceSettings", {kExif, {{"Columns", "Integer"}, {"Rows", "Integer"}, {"Settings", "Seq Text"}}}},
      {"Time", {kDM, {{"value", "Integer"}, {"scale", "Rational"}}}},
      {"Marker", {kDM, {{"startTime", "Time"}, {"duration", "Time"}, {"comment", "Text"}, {"name", "Text"}, {"location", "URI"}, {"target", "Text"}, {"type", "Closed Choice of Text|Chapter|Cue|Beat|Track|Index"}}}},
      {"Media", {kDM, {{"path", "URI"}, {"track", "Text"}, {"startTime", "Time"}, {"duration", "Time"}, {"managed", "Boolean"}, {"webStatement", "URI"}}}},
      {"ProjectLink", {kDM, {{"type", "Closed Choice of Text|movie|still|audio|custom"}, {"path", "URI"}}}},
      {"Timecode", {kDM, {{"timeValue", "Text"}, {"timeFormat", "Closed Choice of Text|24Timecode|25Timecode|2997DropTimecode (semicolon delimiter)|2997NonDropTimecode|30Timecode|50Timecode|5994DropTimecode|5994NonDropTimecode|60Timecode|23976Timecode"}}}},
      {"TimeScaleStretch", {kDM, {{"quality", "Closed Choice of Text|High|Medium|Low"}, {"frameSize", "Real"}, {"frameOverlappingPercentage", "Real"}}}},
      {"ResampleStretch", {kDM, {{"quality", "Closed Choice of Text|High|Medium|Low"}}}},
      {"BeatSpliceStretch", {kDM, {{"useFileBeatsMarker", "Boolean"}, {"riseInDecibel", "Real"}, {"riseInTimeDuration", "Time"}}}},
  };
  return m;
}

struct ExtDefs {
  std::map<std::string, FieldMap> props;
  std::map<std::string, StructDef> types;
};

struct XmpDate {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  long long ns = 0;
  bool tz = false;
  int off = 0;
};

bool parseXmpDate(const std::string& s, XmpDate& r) {
  size_t p = 0;
  auto ch = [&](size_t i) { return i < s.size() ? s[i] : '\0'; };
  auto more = [&] { return p < s.size(); };
  auto gather = [&](int max, int& out) {
    long long v = 0;
    bool ok = false;
    while (ch(p) >= '0' && ch(p) <= '9') {
      v = std::min(v * 10 + (ch(p) - '0'), 100000000000LL);
      ok = true;
      ++p;
    }
    out = static_cast<int>(std::min<long long>(v, max));
    return ok;
  };
  if (s.empty()) return true;
  bool neg = ch(0) == '-';
  if (neg) ++p;
  if (!gather(9999, r.y) || (more() && ch(p) != '-')) return false;
  if (neg) r.y = -r.y;
  if (!more()) return true;
  ++p;
  if (!gather(12, r.mo) || (more() && ch(p) != '-')) return false;
  if (!more()) return true;
  ++p;
  if (!gather(31, r.d) || (more() && ch(p) != 'T')) return false;
  if (!more()) return true;
  ++p;
  if (!gather(23, r.h)) return false;
  if (!more()) return true;
  auto zoneChar = [&](char c) { return c == 'Z' || c == '+' || c == '-'; };
  if (ch(p) == ':') {
    ++p;
    if (!gather(59, r.mi) || (more() && ch(p) != ':' && !zoneChar(ch(p)))) return false;
  }
  if (!more()) return true;
  if (ch(p) == ':') {
    ++p;
    if (!gather(59, r.s) || (more() && ch(p) != '.' && !zoneChar(ch(p)))) return false;
    if (ch(p) == '.') {
      ++p;
      size_t start = p;
      int frac = 0;
      if (!gather(999999999, frac) || (more() && !zoneChar(ch(p)))) return false;
      long long v = frac;
      for (size_t digits = p - start; digits != 9; digits > 9 ? --digits : ++digits) v = digits > 9 ? v / 10 : v * 10;
      r.ns = v;
    }
  } else if (!zoneChar(ch(p))) {
    return false;
  }
  if (!more()) return true;
  r.tz = true;
  if (ch(p) == 'Z') {
    ++p;
  } else {
    int sign = ch(p) == '+' ? 1 : -1;
    ++p;
    int hh = 0, mm = 0;
    if (!gather(23, hh)) return false;
    if (more()) {
      if (ch(p) != ':') return false;
      ++p;
      if (!gather(59, mm)) return false;
    }
    r.off = sign * (hh * 60 + mm);
  }
  return !more();
}

bool checkType(const XProp& p, std::string type, const ExtDefs* ext, int depth);

bool checkStruct(const XProp& p, const StructDef& sd, const ExtDefs* ext, int depth) {
  if (p.kind != 1) return false;
  if (sd.any) return true;
  for (const XProp& f : p.kids) {
    if (f.ns != sd.ns) return false;
    auto it = sd.fields.find(f.name);
    if (it == sd.fields.end()) return false;
    if (!checkType(f, it->second, ext, depth + 1)) return false;
  }
  return true;
}

thread_local bool tGpsPart1 = false;
thread_local bool tClosedChoices = false;

std::string lowerAscii(std::string v) {
  for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return v;
}

std::string simplifiedType(std::string type) {
  static const std::regex choice("(open |closed )?(choice |choice$)(of )?");
  type = trimWs(std::regex_replace(lowerAscii(type.substr(0, type.find('|'))), choice, ""));
  if (type.empty()) return "text";
  auto ends = [&](const std::string& t) { return type.size() >= t.size() && type.compare(type.size() - t.size(), t.size(), t) == 0; };
  if (ends("lang alt")) return type;
  for (const char* a : {"bag", "alt", "seq"}) {
    if (ends(a)) return type + " text";
  }
  return type;
}

const StructDef* findStruct(const std::string& low, const ExtDefs* ext, bool& textOnly) {
  textOnly = false;
  for (const auto& kv : structTypes()) {
    if (lowerAscii(kv.first) == low) return &kv.second;
  }
  for (const auto& kv : ext ? ext->types : std::map<std::string, StructDef>()) {
    if (lowerAscii(trimWs(kv.first)) != low) continue;
    textOnly = kv.second.fields.empty() && !kv.second.any;
    return &ext->types.at(kv.first);
  }
  return nullptr;
}

bool checkType(const XProp& p, std::string type, const ExtDefs* ext, int depth) {
  if (depth > 16) return false;
  std::string head = lowerAscii(trimWs(type));
  size_t bar = type.find('|');
  if (tClosedChoices && head.rfind("closed choice of ", 0) == 0 && bar != std::string::npos && p.kind == 0) {
    std::string v = trimWs(p.value);
    bool listed = false;
    for (size_t b = bar + 1; b <= type.size() && !listed;) {
      size_t e = type.find('|', b);
      if (e == std::string::npos) e = type.size();
      listed = type.compare(b, e - b, v) == 0;
      b = e + 1;
    }
    if (!listed) return false;
  }
  std::string t = simplifiedType(type);
  if (t == "any") return true;
  for (const char* arr : {"Bag", "Alt", "Seq"}) {
    std::string pre = lowerAscii(arr) + " ";
    if (t.rfind(pre, 0) != 0 || t == "lang alt") continue;
    if (p.kind != 2 || p.arr != arr) return false;
    for (const XProp& i : p.kids) {
      if (!checkType(i, t.substr(pre.size()), ext, depth + 1)) return false;
    }
    return true;
  }
  if (t == "lang alt") {
    if (p.kind != 2 || p.arr != "Alt") return false;
    bool lang = p.kids.empty();
    for (const XProp& i : p.kids) lang = lang || i.hasLang;
    return lang;
  }
  static const std::set<std::string> texty = {"text", "propername", "agentname", "rational", "renditionclass",
                                              "uri", "url", "locale", "xpath"};
  if (texty.count(t)) return p.kind == 0;
  static const std::regex kInt("[+-]?[0-9]+");
  static const std::regex kReal("[+-]?[0-9]+\\.?[0-9]*|[+-]?[0-9]*\\.?[0-9]+");
  static const std::regex kMime("[-A-Za-z0-9_+.]+/[-A-Za-z0-9_+.]+");
  static const std::regex kGps1("[0-9]{2},[0-9]{2}[,.][0-9]{2}[NSEW]");
  static const std::regex kGps2("[0-9]{1,3},[0-9]{1,2}(,[0-9]{1,2}|\\.[0-9]+)[NSEW]");
  const std::string& v = p.value;
  if (t == "integer") return p.kind == 0 && std::regex_match(v, kInt);
  if (t == "real") return p.kind == 0 && std::regex_match(v, kReal);
  if (t == "boolean") return p.kind == 0 && (v == "True" || v == "False");
  if (t == "mimetype") return p.kind == 0 && std::regex_match(v, kMime);
  if (t == "date") {
    XmpDate d;
    return p.kind == 0 && parseXmpDate(v, d);
  }
  if (t == "gpscoordinate") return p.kind == 0 && std::regex_match(v, tGpsPart1 ? kGps1 : kGps2);
  bool textOnly = false;
  const StructDef* sd = findStruct(t, ext, textOnly);
  if (!sd) return false;
  if (textOnly) return p.kind == 0;
  return checkStruct(p, *sd, ext, depth);
}

bool knownType(std::string type, const ExtDefs& ext) {
  std::string t = simplifiedType(type);
  for (bool again = true; again;) {
    again = false;
    for (const char* pre : {"bag ", "alt ", "seq "}) {
      if (t.rfind(pre, 0) == 0 && t != "lang alt") {
        t = t.substr(4);
        again = true;
      }
    }
  }
  static const std::set<std::string> basic = {"text", "propername", "agentname", "locale", "mimetype", "renditionclass", "xpath",
                                              "uri", "url", "integer", "real", "boolean", "rational", "date",
                                              "lang alt", "gpscoordinate"};
  bool textOnly = false;
  return basic.count(t) || findStruct(t, &ext, textOnly) != nullptr;
}

struct PropTable {
  std::map<std::string, FieldMap> ns;
  void add(const std::string& n, std::initializer_list<std::pair<const std::string, std::string>> l) {
    for (const auto& kv : l) ns[n][kv.first] = kv.second;
  }
  const std::string* find(const XProp& p) const {
    auto a = ns.find(p.ns);
    if (a == ns.end()) return nullptr;
    auto b = a->second.find(p.name);
    return b == a->second.end() ? nullptr : &b->second;
  }
};

void addXmpCommon(PropTable& t, bool y2005) {
  t.add(kDc, {{"contributor", "Bag ProperName"}, {"coverage", "Text"}, {"creator", "Seq ProperName"}, {"date", "Seq Date"},
              {"description", "Lang Alt"}, {"format", "MIMEType"}, {"identifier", "Text"}, {"language", "Bag Locale"},
              {"publisher", "Bag ProperName"}, {"relation", "Bag Text"}, {"rights", "Lang Alt"}, {"source", "Text"},
              {"subject", "Bag Text"}, {"title", "Lang Alt"}, {"type", "Bag Open Choice of Text"}});
  t.add(kXmp, {{"Advisory", "Bag Text"}, {"BaseURL", "URL"}, {"CreateDate", "Date"}, {"CreatorTool", "AgentName"},
               {"Identifier", "Bag Text"}, {"MetadataDate", "Date"}, {"ModifyDate", "Date"}, {"Nickname", "Text"},
               {"Thumbnails", "Alt Thumbnail"}});
  t.add(kIdq, {{"Scheme", "Text"}});
  t.add(kRights, {{"Certificate", "URL"}, {"Marked", "Boolean"}, {"Owner", "Bag ProperName"}, {"UsageTerms", "Lang Alt"},
                  {"WebStatement", "URL"}});
  t.add(kMM, {{"DerivedFrom", "ResourceRef"}, {"DocumentID", "URI"}, {"History", "Seq ResourceEvent"},
              {"ManagedFrom", "ResourceRef"}, {"Manager", "AgentName"}, {"ManageTo", "URI"}, {"ManageUI", "URI"},
              {"ManagerVariant", "Text"}, {"RenditionClass", "RenditionClass"}, {"RenditionParams", "Text"},
              {"VersionID", "Text"}, {"Versions", "Seq Version"}, {"LastURL", "URL"}, {"RenditionOf", "ResourceRef"},
              {"SaveID", "Integer"}});
  t.add(kBJ, {{"JobRef", "Bag Job"}});
  t.add(kTPg, {{"MaxPageSize", "Dimensions"}, {"NPages", "Integer"}});
  t.add(kPdf, {{"Keywords", "Text"}, {"PDFVersion", "Text"}, {"Producer", "AgentName"}});
  t.add(kPs, {{"AuthorsPosition", "Text"}, {"CaptionWriter", "ProperName"}, {"Category", "Text"}, {"City", "Text"},
              {"Country", "Text"}, {"Credit", "Text"}, {"DateCreated", "Date"}, {"Headline", "Text"}, {"Instructions", "Text"},
              {"Source", "Text"}, {"State", "Text"}, {"SupplementalCategories", y2005 ? "Bag Text" : "Text"},
              {"TransmissionReference", "Text"}, {"Urgency", "Integer"}});
  t.add(kTiff, {{"ImageWidth", "Integer"}, {"ImageLength", "Integer"}, {"BitsPerSample", "Seq Integer"},
                {"Compression", "Closed Choice of Integer|1|6"}, {"PhotometricInterpretation", "Closed Choice of Integer|2|6"},
                {"Orientation", "Closed Choice of Integer|1|2|3|4|5|6|7|8"}, {"SamplesPerPixel", "Integer"},
                {"PlanarConfiguration", "Closed Choice of Integer|1|2"}, {"YCbCrSubSampling", "Seq Integer"},
                {"YCbCrPositioning", "Closed Choice of Integer|1|2"}, {"XResolution", "Rational"}, {"YResolution", "Rational"},
                {"ResolutionUnit", "Closed Choice of Integer|2|3"}, {"TransferFunction", "Seq Integer"},
                {"WhitePoint", "Seq Rational"}, {"PrimaryChromaticities", "Seq Rational"},
                {"YCbCrCoefficients", "Seq Rational"}, {"ReferenceBlackWhite", "Seq Rational"}, {"DateTime", "Date"},
                {"ImageDescription", "Lang Alt"}, {"Make", "ProperName"}, {"Model", "ProperName"},
                {"Software", "AgentName"}, {"Artist", "ProperName"}, {"Copyright", "Lang Alt"}});
  t.add(kExif, {{"ExifVersion", "Closed Choice of Text|0210"}, {"FlashpixVersion", "Closed Choice of Text|0100"},
                {"ColorSpace", "Closed Choice of Integer|1|65535"}, {"ComponentsConfiguration", "Seq Integer"},
                {"CompressedBitsPerPixel", "Rational"}, {"PixelXDimension", "Integer"}, {"PixelYDimension", "Integer"},
                {"UserComment", "Lang Alt"}, {"RelatedSoundFile", "Text"}, {"DateTimeOriginal", "Date"},
                {"DateTimeDigitized", "Date"}, {"ExposureTime", "Rational"}, {"FNumber", "Rational"},
                {"ExposureProgram", "Closed Choice of Integer|0|1|2|3|4|5|6|7|8"}, {"SpectralSensitivity", "Text"},
                {"ISOSpeedRatings", "Seq Integer"}, {"OECF", "OECF/SFR"}, {"ShutterSpeedValue", "Rational"},
                {"ApertureValue", "Rational"}, {"BrightnessValue", "Rational"}, {"ExposureBiasValue", "Rational"},
                {"MaxApertureValue", "Rational"}, {"SubjectDistance", "Rational"},
                {"MeteringMode", "Closed Choice of Integer|0|1|2|3|4|5|6|255"},
                {"LightSource", "Closed Choice of Integer|0|1|2|3|4|9|10|11|12|13|14|15|17|18|19|20|21|22|23|24|255"},
                {"Flash", "Flash"}, {"FocalLength", "Rational"}, {"SubjectArea", "Seq Integer"}, {"FlashEnergy", "Rational"},
                {"SpatialFrequencyResponse", "OECF/SFR"}, {"FocalPlaneXResolution", "Rational"},
                {"FocalPlaneYResolution", "Rational"}, {"FocalPlaneResolutionUnit", "Closed Choice of Integer|2|3"},
                {"SubjectLocation", "Seq Integer"}, {"ExposureIndex", "Rational"},
                {"SensingMethod", "Closed Choice of Integer|1|2|3|4|5|7|8"}, {"FileSource", "Closed Choice of Integer|3"},
                {"SceneType", "Closed Choice of Integer|1"}, {"CFAPattern", "CFAPattern"},
                {"CustomRendered", "Closed Choice of Integer|0|1"}, {"ExposureMode", "Closed Choice of Integer|0|1|2"},
                {"WhiteBalance", "Closed Choice of Integer|0|1"}, {"DigitalZoomRatio", "Rational"},
                {"FocalLengthIn35mmFilm", "Integer"}, {"SceneCaptureType", "Closed Choice of Integer|0|1|2|3"},
                {"GainControl", "Closed Choice of Integer|0|1|2|3|4"}, {"Contrast", "Closed Choice of Integer|0|1|2"},
                {"Saturation", "Closed Choice of Integer|0|1|2"}, {"Sharpness", "Closed Choice of Integer|0|1|2"},
                {"DeviceSettingDescription", "DeviceSettings"}, {"SubjectDistanceRange", "Closed Choice of Integer|0|1|2|3"},
                {"ImageUniqueID", "Text"}, {"GPSVersionID", "Text"}, {"GPSLatitude", "GPSCoordinate"},
                {"GPSLongitude", "GPSCoordinate"}, {"GPSAltitudeRef", "Closed Choice of Integer|0|1"},
                {"GPSAltitude", "Rational"}, {"GPSTimeStamp", "Date"}, {"GPSSatellites", "Text"},
                {"GPSStatus", "Closed Choice of Text|A|V"}, {"GPSMeasureMode", y2005 ? "Closed Choice of Text|2|3" : "Closed Choice of Integer|2|3"}, {"GPSDOP", "Rational"},
                {"GPSSpeedRef", "Closed Choice of Text|K|M|N"}, {"GPSSpeed", "Rational"},
                {"GPSTrackRef", "Closed Choice of Text|T|M"}, {"GPSTrack", "Rational"},
                {"GPSImgDirectionRef", "Closed Choice of Text|T|M"}, {"GPSImgDirection", "Rational"},
                {"GPSMapDatum", "Text"}, {"GPSDestLatitude", "GPSCoordinate"}, {"GPSDestLongitude", "GPSCoordinate"},
                {"GPSDestBearingRef", "Closed Choice of Text|T|M"}, {"GPSDestBearing", "Rational"},
                {"GPSDestDistanceRef", "Closed Choice of Text|K|M|N"}, {"GPSDestDistance", "Rational"},
                {"GPSProcessingMethod", "Text"}, {"GPSAreaInformation", "Text"},
                {"GPSDifferential", "Closed Choice of Integer|0|1"}});
  if (!y2005) {
    t.add(kExif, {{"MakerNote", "Text"}});
    return;
  }
  t.add(kXmp, {{"Label", "Text"}, {"Rating", "Open Choice of Integer"}});
  t.add(kTPg, {{"Fonts", "Bag Font"}, {"Colorants", "Seq Colorant"}, {"PlateNames", "Seq Text"}});
  t.add(kAux, {{"Lens", "Text"}, {"SerialNumber", "Text"}});
  t.add(kCrs, {{"AutoBrightness", "Boolean"}, {"AutoContrast", "Boolean"}, {"AutoExposure", "Boolean"},
               {"AutoShadows", "Boolean"}, {"BlueHue", "Integer"}, {"BlueSaturation", "Integer"}, {"Brightness", "Integer"},
               {"CameraProfile", "Text"}, {"ChromaticAberrationB", "Integer"}, {"ChromaticAberrationR", "Integer"},
               {"ColorNoiseReduction", "Integer"}, {"Contrast", "Integer"}, {"CropTop", "Real"}, {"CropLeft", "Real"},
               {"CropBottom", "Real"}, {"CropRight", "Real"}, {"CropAngle", "Real"}, {"CropWidth", "Real"},
               {"CropHeight", "Real"}, {"CropUnits", "Integer"}, {"Exposure", "Real"}, {"GreenHue", "Integer"},
               {"GreenSaturation", "Integer"}, {"HasCrop", "Boolean"}, {"HasSettings", "Boolean"},
               {"LuminanceSmoothing", "Integer"}, {"RawFileName", "Text"}, {"RedHue", "Integer"},
               {"RedSaturation", "Integer"}, {"Saturation", "Integer"}, {"Shadows", "Integer"}, {"ShadowTint", "Integer"},
               {"Sharpness", "Integer"}, {"Temperature", "Integer"}, {"Tint", "Integer"}, {"ToneCurve", "Seq Text"},
               {"ToneCurveName", "Open Choice of Text"}, {"Version", "Text"}, {"VignetteAmount", "Integer"},
               {"VignetteMidpoint", "Integer"},
               {"WhiteBalance", "Closed Choice of Text|As Shot|Auto|Daylight|Cloudy|Shade|Tungsten|Fluorescent|Flash|Custom"}});
  t.add(kDM, {{"projectRef", "ProjectLink"}, {"videoFrameRate", "Open Choice of Text"}, {"videoFrameSize", "Dimensions"},
              {"videoPixelAspectRatio", "Rational"}, {"videoPixelDepth", "Closed Choice of Text|8Int|16Int|32Int|32Float"},
              {"videoColorSpace", "Closed Choice of Text|sRGB|CCIR-601|CCIR-709"},
              {"videoAlphaMode", "Closed Choice of Text|straight|pre-multiplied"}, {"videoAlphaPremultipleColor", "Colorant"},
              {"videoAlphaUnityIsTransparent", "Boolean"}, {"videoCompressor", "Text"},
              {"videoFieldOrder", "Closed Choice of Text|Upper|Lower|Progressive"},
              {"pullDown", "Closed Choice of Text|WSSWW|SSWWW|SWWWS|WWWSS|WWSSW|WSSWW_24p|SSWWW_24p|SWWWS_24p|WWWSS_24p|WWSSW_24p"},
              {"audioSampleRate", "Integer"}, {"audioSampleType", "Closed Choice of Text|8Int|16Int|32Int|32Float"},
              {"audioChannelType", "Closed Choice of Text|Mono|Stereo|5.1|7.1"}, {"audioCompressor", "Text"},
              {"speakerPlacement", "Text"}, {"fileDataRate", "Rational"}, {"tapeName", "Text"}, {"altTapeName", "Text"},
              {"startTimecode", "Timecode"}, {"altTimecode", "Timecode"}, {"duration", "Time"}, {"scene", "Text"},
              {"shotName", "Text"}, {"shotDate", "Date"}, {"shotLocation", "Text"}, {"logComment", "Text"},
              {"markers", "Seq Marker"}, {"contributedMedia", "Bag Media"}, {"absPeakAudioFilePath", "URI"},
              {"relativePeakAudioFilePath", "URI"}, {"videoModDate", "Date"}, {"audioModDate", "Date"},
              {"metadataModDate", "Date"}, {"artist", "Text"}, {"album", "Text"}, {"trackNumber", "Integer"},
              {"genre", "Text"}, {"copyright", "Text"}, {"releaseDate", "Date"}, {"composer", "Text"},
              {"engineer", "Text"}, {"tempo", "Real"}, {"instrument", "Text"}, {"introTime", "Time"}, {"outCue", "Time"},
              {"relativeTimestamp", "Time"}, {"loop", "Boolean"}, {"numberOfBeats", "Real"},
              {"key", "Closed Choice of Text|C|C#|D|D#|E|F|F#|G|G#|A|A#|B"},
              {"stretchMode", "Closed Choice of Text|Fixed length|Time-Scale|Resample|Beat Splice|Hybrid"},
              {"timeScaleParams", "TimeScaleStretch"}, {"resampleParams", "ResampleStretch"},
              {"beatSpliceParams", "BeatSpliceStretch"},
              {"timeSignature", "Closed Choice of Text|2/4|3/4|4/4|5/4|7/4|6/8|9/8|12/8|other"},
              {"scaleType", "Closed Choice of Text|Major|Minor|Both|Neither"}});
}

std::vector<PropTable> buildTables() {
  PropTable t2004, t2005, pdfa1, pdfa2, pdfe1, pdfx4;
  addXmpCommon(t2004, false);
  addXmpCommon(t2005, true);
  pdfa1 = t2004;
  pdfa1.add(kPdfaId, {{"part", "Open Choice of Integer"}, {"amd", "Open Choice of Text"}, {"conformance", "Closed Choice of Text|A|B"}});
  pdfa1.add(kMM, {{"InstanceID", "URI"}});
  pdfa2 = t2005;
  pdfa2.add(kPdfaId, {{"part", "Open Choice of Integer"}, {"amd", "Open Choice of Text"},
                      {"conformance", "Closed Choice of Text|A|B|U"}, {"corr", "Open Choice of Text"}});
  pdfa2.add(kXmp, {{"Rating", "Real"}});
  pdfa2.add(kMM, {{"InstanceID", "URI"}});
  pdfe1 = t2005;
  pdfe1.add(kPdfeId, {{"ISO_PDFEVersion", "Text"}});
  pdfx4 = t2005;
  pdfx4.add(kPdfxId, {{"GTS_PDFXVersion", "Text"}});
  pdfx4.add(kPdf, {{"Trapped", "Text"}});
  return {t2004, t2005, pdfa1, pdfa2, pdfe1, pdfx4};
}

const PropTable& table(int which) {
  static const std::vector<PropTable> tables = buildTables();
  return tables[which < 0 || which > 5 ? 5 : which];
}

const XProp* field(const XProp& s, const std::string& ns, const char* name) {
  for (const XProp& f : s.kids) {
    if (f.ns == ns && f.name == name) return &f;
  }
  return nullptr;
}

bool validateExt(const XPacket& pk, const ExtDefs* outer = nullptr) {
  for (const XProp& p : pk.props) {
    if (p.ns != kExtNs) {
      if (p.ns == kSchemaNs || p.ns == kPropNs || p.ns == kTypeNs || p.ns == kFieldNs) return false;
      continue;
    }
    if (p.name != "schemas" || p.kind != 2 || p.arr != "Bag") return false;
    ExtDefs local;
    for (const XProp& s : p.kids) {
      if (s.kind != 1) return false;
      for (const XProp& f : s.kids) {
        static const std::set<std::string> ok = {"schema", "namespaceURI", "prefix", "property", "valueType"};
        if (f.ns != kSchemaNs || !ok.count(f.name)) return false;
      }
      for (const char* req : {"schema", "namespaceURI", "prefix"}) {
        const XProp* f = field(s, kSchemaNs, req);
        if (!f || f->kind != 0 || trimWs(f->value).empty()) return false;
      }
      if (const XProp* vt = field(s, kSchemaNs, "valueType")) {
        if (vt->kind != 2 || vt->arr != "Seq") return false;
        for (const XProp& t : vt->kids) {
          if (t.kind != 1) return false;
          for (const XProp& f : t.kids) {
            static const std::set<std::string> ok = {"type", "namespaceURI", "prefix", "description", "field"};
            if (f.ns != kTypeNs || !ok.count(f.name)) return false;
          }
          for (const char* req : {"type", "namespaceURI", "prefix", "description"}) {
            const XProp* f = field(t, kTypeNs, req);
            if (!f || f->kind != 0) return false;
          }
          StructDef sd;
          sd.ns = field(t, kTypeNs, "namespaceURI")->value;
          if (const XProp* fl = field(t, kTypeNs, "field")) {
            if (fl->kind != 2 || fl->arr != "Seq") return false;
            for (const XProp& fd : fl->kids) {
              if (fd.kind != 1) return false;
              for (const XProp& f : fd.kids) {
                if (f.ns != kFieldNs || (f.name != "name" && f.name != "valueType" && f.name != "description")) return false;
              }
              for (const char* req : {"name", "valueType", "description"}) {
                const XProp* f = field(fd, kFieldNs, req);
                if (!f || f->kind != 0) return false;
              }
              sd.fields[field(fd, kFieldNs, "name")->value] = field(fd, kFieldNs, "valueType")->value;
            }
          }
          local.types[trimWs(field(t, kTypeNs, "type")->value)] = sd;
        }
      }
      if (const XProp* pr = field(s, kSchemaNs, "property")) {
        if (pr->kind != 2 || pr->arr != "Seq") return false;
        for (const XProp& d : pr->kids) {
          if (d.kind != 1) return false;
          for (const XProp& f : d.kids) {
            if (f.ns != kPropNs || (f.name != "name" && f.name != "valueType" && f.name != "category" && f.name != "description")) return false;
          }
          for (const char* req : {"name", "valueType", "category", "description"}) {
            const XProp* f = field(d, kPropNs, req);
            if (!f || f->kind != 0) return false;
          }
          std::string cat = trimWs(field(d, kPropNs, "category")->value);
          if (cat != "internal" && cat != "external") return false;
        }
      }
    }
    for (const XProp& s : p.kids) {
      const XProp* pr = field(s, kSchemaNs, "property");
      for (size_t i = 0; pr && i < pr->kids.size(); ++i) {
        const std::string& vt = field(pr->kids[i], kPropNs, "valueType")->value;
        if (!knownType(vt, local) && !(outer && knownType(vt, *outer))) return false;
      }
    }
  }
  return true;
}

void collectExt(const XPacket& pk, ExtDefs& out) {
  for (const XProp& p : pk.props) {
    if (p.ns != kExtNs || p.name != "schemas" || p.kind != 2) continue;
    for (const XProp& s : p.kids) {
      const XProp* ns = field(s, kSchemaNs, "namespaceURI");
      if (!ns) continue;
      std::string uri = trimWs(ns->value);
      FieldMap& fm = out.props[uri];
      if (const XProp* pr = field(s, kSchemaNs, "property")) {
        for (const XProp& d : pr->kids) {
          const XProp* n = field(d, kPropNs, "name");
          const XProp* vt = field(d, kPropNs, "valueType");
          if (n) fm[trimWs(n->value)] = vt ? vt->value : std::string();
        }
      }
      if (const XProp* vt = field(s, kSchemaNs, "valueType")) {
        for (const XProp& t : vt->kids) {
          const XProp* tn = field(t, kTypeNs, "type");
          const XProp* tns = field(t, kTypeNs, "namespaceURI");
          if (!tn) continue;
          StructDef sd;
          sd.ns = tns ? trimWs(tns->value) : std::string();
          if (const XProp* fl = field(t, kTypeNs, "field")) {
            for (const XProp& fd : fl->kids) {
              const XProp* n = field(fd, kFieldNs, "name");
              const XProp* v = field(fd, kFieldNs, "valueType");
              if (n) sd.fields[trimWs(n->value)] = v ? v->value : std::string();
            }
          }
          out.types[trimWs(tn->value)] = sd;
        }
      }
    }
  }
}

const StructDef* structOf(const std::string& type, const ExtDefs* ext) {
  auto st = structTypes().find(type);
  if (st != structTypes().end()) return &st->second;
  if (ext) {
    auto ct = ext->types.find(type);
    if (ct != ext->types.end()) return &ct->second;
  }
  return nullptr;
}

std::string baseType(std::string type) {
  type = trimWs(type);
  for (const char* pre : {"Closed Choice of ", "Open Choice of ", "closed Choice of ", "open Choice of "}) {
    if (type.rfind(pre, 0) == 0) type = type.substr(std::char_traits<char>::length(pre));
  }
  return type.substr(0, type.find('|'));
}

void walkDefined(const XProp& p, const std::string* type, const ExtDefs* ext, std::vector<char>& out) {
  out.push_back(type != nullptr);
  std::string ty = type ? baseType(*type) : std::string();
  if (p.kind == 2) {
    std::string item = ty == "Lang Alt" ? "Text" : ty;
    for (const char* c : {"Bag ", "Seq ", "Alt "}) {
      if (item.rfind(c, 0) == 0) item = item.substr(4);
    }
    for (const XProp& k : p.kids) walkDefined(k, type ? &item : nullptr, ext, out);
  } else if (p.kind == 1) {
    const StructDef* sd = type ? structOf(ty, ext) : nullptr;
    for (const XProp& k : p.kids) {
      const std::string* ft = nullptr;
      if (sd && sd->any) ft = &ty;
      if (sd && !sd->any && k.ns == sd->ns) {
        auto f = sd->fields.find(k.name);
        if (f != sd->fields.end()) ft = &f->second;
      }
      walkDefined(k, ft, ext, out);
    }
  }
}

}

namespace {
using NodeSet = std::set<std::tuple<size_t, size_t, size_t>>;
thread_local XmpNodeScope* gXmpScope = nullptr;
}

XmpNodeScope::XmpNodeScope() : prev(gXmpScope) { gXmpScope = this; }
XmpNodeScope::~XmpNodeScope() {
  delete static_cast<std::map<std::string, NodeSet>*>(sets);
  gXmpScope = static_cast<XmpNodeScope*>(prev);
}

bool xmpWellFormed(const std::string& packet) { return parsePacket(packet).ok; }

namespace {
std::string xmpUtf8(const std::string& raw) {
  std::string packet = raw;
  size_t lt = raw.find('<');
  if (lt != std::string::npos && lt < 8) {
    int width = 1;
    bool big = false;
    if (raw.compare(lt, 4, std::string("<\0\0\0", 4)) == 0) width = 4;
    else if (lt >= 3 && raw.compare(lt - 3, 4, std::string("\0\0\0<", 4)) == 0) width = 4, big = true;
    else if (raw.compare(lt, 2, std::string("<\0", 2)) == 0) width = 2;
    else if (lt >= 1 && raw[lt - 1] == '\0') width = 2, big = true;
    if (width > 1) {
      size_t start = big ? lt - (width - 1) : lt;
      packet.clear();
      for (size_t i = start; i + width <= raw.size(); i += width) {
        unsigned long cp = 0;
        for (int b = 0; b < width; ++b) {
          unsigned long byte = static_cast<unsigned char>(raw[i + (big ? b : width - 1 - b)]);
          cp = (cp << 8) | byte;
        }
        if (width == 2 && cp >= 0xD800 && cp < 0xDC00 && i + 4 <= raw.size()) {
          unsigned long lo = big ? (static_cast<unsigned char>(raw[i + 2]) << 8) | static_cast<unsigned char>(raw[i + 3])
                                 : (static_cast<unsigned char>(raw[i + 3]) << 8) | static_cast<unsigned char>(raw[i + 2]);
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          i += 2;
        }
        if (cp < 0x80) {
          packet += static_cast<char>(cp);
        } else if (cp < 0x800) {
          packet += static_cast<char>(0xC0 | (cp >> 6));
          packet += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
          packet += static_cast<char>(0xE0 | (cp >> 12));
          packet += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
          packet += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
          packet += static_cast<char>(0xF0 | (cp >> 18));
          packet += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
          packet += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
          packet += static_cast<char>(0x80 | (cp & 0x3F));
        }
      }
    }
  }
  return packet;
}
}

std::string xmpPropertiesProblem(const XPacket& pk, int tableId, const std::string* mainRaw);

bool prefixesCanonical(const std::vector<XProp>& props, const std::map<std::string, std::string>& canon, int depth) {
  if (depth > 64) return false;
  for (const XProp& p : props) {
    auto c = canon.find(p.ns);
    if (c != canon.end() && c->second != p.prefix) return false;
    if (!prefixesCanonical(p.kids, canon, depth + 1)) return false;
  }
  return true;
}

std::string pdfaXmpProblem(const std::string& raw, int part, char conf, const std::string* mainRaw) {
  std::string packet = xmpUtf8(raw);
  XPacket pk = parsePacket(packet);
  if (!pk.ok) return "XMP metadata is not well-formed RDF/XML";
  if (part >= 2 && packet != raw) return "XMP metadata is not encoded as UTF-8";
  std::map<std::string, std::string> id;
  for (const XProp& p : pk.props) {
    if (p.ns == kPdfaId && p.kind == 0) id[p.name] = trimWs(p.value);
  }
  size_t hdr = packet.find("<?xpacket");
  if (hdr != std::string::npos) {
    std::string h = packet.substr(hdr, packet.find("?>", hdr) - hdr);
    if (h.find("bytes=") != std::string::npos || h.find("encoding=") != std::string::npos)
      return "XMP packet header uses the bytes or encoding attribute";
  }
  static const std::map<std::string, std::string> canon = {{kPdfaId, "pdfaid"}, {kExtNs, "pdfaExtension"},
                                                           {kSchemaNs, "pdfaSchema"}, {kPropNs, "pdfaProperty"},
                                                           {kTypeNs, "pdfaType"}, {kFieldNs, "pdfaField"}};
  if (!prefixesCanonical(pk.props, canon, 0)) return "a PDF/A identification or extension schema property uses a non-standard prefix";
  if (mainRaw && part >= 4) return std::string();
  static const std::regex kPart("[+-]?[0-9]+");
  if (!mainRaw && (!std::regex_match(id["part"], kPart) || std::atoi(id["part"].c_str()) != part))
    return "pdfaid:part does not identify PDF/A-" + std::to_string(part);
  if (part >= 4) {
    static const std::regex kYear("[0-9]{4}");
    if (!std::regex_match(id["rev"], kYear)) return "pdfaid:rev is missing or not a four-digit year";
    auto c = id.find("conformance");
    bool flavoured = conf == 'E' || conf == 'F';
    if (flavoured ? (c == id.end() || c->second != std::string(1, conf)) : c != id.end())
      return "pdfaid:conformance does not match the PDF/A-4 level";
    return std::string();
  }
  std::string accepted = conf == 'A' ? "A" : conf == 'U' ? "UA" : part == 1 ? "BA" : "BUA";
  const std::string& declared = id["conformance"];
  if (!mainRaw && (declared.size() != 1 || accepted.find(declared[0]) == std::string::npos))
    return "pdfaid:conformance does not match the PDF/A conformance level";
  return xmpPropertiesProblem(pk, part == 1 ? 2 : 3, mainRaw);
}

std::string xmpPropertiesProblem(const XPacket& pk, int tableId, const std::string* mainRaw) {
  ExtDefs ext;
  if (mainRaw) {
    XPacket mainPk = parsePacket(xmpUtf8(*mainRaw));
    if (mainPk.ok) collectExt(mainPk, ext);
  }
  if (!validateExt(pk, mainRaw ? &ext : nullptr)) return "an XMP extension schema description is invalid";
  collectExt(pk, ext);
  tGpsPart1 = tableId == 2;
  const PropTable& t = table(tableId);
  for (const XProp& p : pk.props) {
    if (p.ns == kExtNs) continue;
    const std::string* def = t.find(p);
    if (!def) {
      auto n = ext.props.find(p.ns);
      if (n != ext.props.end()) {
        auto f = n->second.find(p.name);
        if (f != n->second.end()) def = &f->second;
      }
    }
    if (!def) return "XMP property " + p.name + " (" + p.ns + ") is neither predefined nor described by an extension schema";
    if (!checkType(p, *def, &ext, 0)) return "XMP property " + p.name + " does not have the value type " + trimWs(*def);
  }
  return std::string();
}

namespace {
bool dateParts(std::string s, bool pdf, long long& when) {
  if (pdf) {
    if (s.rfind("D:", 0) == 0) s = s.substr(2);
  } else {
    size_t tpos = s.find('T');
    size_t zone = tpos == std::string::npos ? std::string::npos : s.find_first_of("+-Z", tpos);
    std::string t;
    for (size_t k = 0; k < std::min(zone, s.size()); ++k) {
      if (s[k] != '-' && s[k] != ':' && s[k] != 'T') t += s[k];
    }
    if (zone != std::string::npos) t += s.substr(zone);
    s = t;
  }
  int f[6] = {0, 1, 1, 0, 0, 0};
  int lens[6] = {4, 2, 2, 2, 2, 2};
  size_t i = 0;
  for (int k = 0; k < 6; ++k) {
    if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) {
      if (k == 0) return false;
      break;
    }
    if (i + lens[k] > s.size()) return false;
    for (int j = 0; j < lens[k]; ++j) {
      if (!std::isdigit(static_cast<unsigned char>(s[i + j]))) return false;
    }
    f[k] = std::atoi(s.substr(i, lens[k]).c_str());
    i += lens[k];
  }
  if (i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
  }
  int off = 0;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    int sign = s[i] == '-' ? -1 : 1;
    std::string d;
    for (size_t j = i + 1; j < s.size(); ++j) {
      if (std::isdigit(static_cast<unsigned char>(s[j]))) d += s[j];
    }
    int hh = d.size() >= 2 ? std::atoi(d.substr(0, 2).c_str()) : 0;
    int mm = d.size() >= 4 ? std::atoi(d.substr(2, 2).c_str()) : 0;
    off = sign * (hh * 60 + mm);
  }
  int y = f[0], m = f[1], d = f[2];
  y -= m <= 2;
  long long era = (y >= 0 ? y : y - 399) / 400;
  long long yoe = y - era * 400;
  long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long long days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy;
  when = ((days * 24 + f[3]) * 60 + f[4] - off) * 60 + f[5];
  return true;
}
}

namespace {
bool pdfDateMs(std::string s, long long& ms) {
  if (s.rfind("D:", 0) != 0) return false;
  if (!s.empty() && s.back() == '\'') {
    if (s.size() >= 2 && s[s.size() - 2] == '\'') return false;
    s.pop_back();
  }
  auto digits = [&](size_t off, size_t len, int& out) {
    if (off + len > s.size()) return false;
    for (size_t i = off; i < off + len; ++i) {
      if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    out = std::atoi(s.substr(off, len).c_str());
    return true;
  };
  int year = 0, month = 1, day = 1, hour = 0, minute = 0, second = 0, tzh = 0, tzm = 0;
  if (!digits(2, 4, year)) return false;
  size_t n = s.size();
  if (n > 6 && !digits(6, 2, month)) return false;
  if (n > 8 && !digits(8, 2, day)) return false;
  if (n > 10 && !digits(10, 2, hour)) return false;
  if (n > 12 && !digits(12, 2, minute)) return false;
  if (n > 14 && !digits(14, 2, second)) return false;
  char sign = 'Z';
  if (n > 16) {
    sign = s[16];
    if (sign != 'Z' && sign != '+' && sign != '-') return false;
  }
  if (n > 17 && !digits(17, 2, tzh)) return false;
  if (n > 19 && s[19] != '\'') return false;
  if (n > 20 && !digits(20, 2, tzm)) return false;
  if (n > 22) return false;
  int off = 0;
  if (sign != 'Z') {
    if (tzh <= 23 && tzm <= 59) off = (sign == '-' ? -1 : 1) * (tzh * 60 + tzm);
  } else if (tzh != 0 || tzm != 0) {
    return false;
  }
  std::tm t{};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = second;
  ms = (static_cast<long long>(timegm(&t)) - off * 60LL) * 1000;
  return true;
}

bool xmpDateMs(const std::string& v, long long& ms) {
  XmpDate d;
  if (v.empty() || !parseXmpDate(v, d)) return false;
  std::tm t{};
  t.tm_year = d.y - 1900;
  t.tm_mon = d.mo - 1;
  t.tm_mday = d.d;
  t.tm_hour = d.h;
  t.tm_min = d.mi;
  t.tm_sec = d.s;
  long long sec;
  if (d.tz) {
    sec = static_cast<long long>(timegm(&t)) - d.off * 60LL;
  } else {
    t.tm_isdst = -1;
    sec = static_cast<long long>(std::mktime(&t));
  }
  ms = sec * 1000 + d.ns / 1000000;
  return true;
}
}

std::string xmpInfoMismatch(const std::string& raw, const std::map<std::string, std::string>& info) {
  XPacket pk = parsePacket(xmpUtf8(raw));
  if (!pk.ok) return std::string();
  auto prop = [&](const std::string& ns, const char* name) -> const XProp* {
    for (const XProp& p : pk.props) {
      if (p.ns == ns && p.name == name) return &p;
    }
    return nullptr;
  };
  auto alt = [&](const char* name, std::string& out) {
    const XProp* p = prop(kDc, name);
    if (!p || p->kind != 2 || p->kids.empty()) return false;
    for (const XProp& k : p->kids) {
      if (k.lang == "x-default") {
        out = k.value;
        return true;
      }
    }
    out = p->kids.front().value;
    return true;
  };
  for (const auto& kv : info) {
    const std::string& key = kv.first;
    std::string xv;
    bool have = false;
    if (key == "Title") have = alt("title", xv);
    else if (key == "Subject") have = alt("description", xv);
    else if (key == "Author") {
      const XProp* p = prop(kDc, "creator");
      have = p && p->kind == 2 && p->kids.size() == 1;
      if (have) xv = p->kids[0].value;
    } else {
      static const std::map<std::string, std::pair<std::string, const char*>> simple = {
          {"Keywords", {kPdf, "Keywords"}}, {"Producer", {kPdf, "Producer"}}, {"Creator", {kXmp, "CreatorTool"}},
          {"CreationDate", {kXmp, "CreateDate"}}, {"ModDate", {kXmp, "ModifyDate"}}};
      auto it = simple.find(key);
      if (it == simple.end()) continue;
      const XProp* p = prop(it->second.first, it->second.second);
      have = p && p->kind == 0;
      if (have) xv = p->value;
    }
    if (!have) return "document information entry " + key + " has no matching XMP property";
    if (key == "CreationDate" || key == "ModDate") {
      long long a = 0, b = 0;
      if (!pdfDateMs(kv.second, a) || !xmpDateMs(xv, b) || a != b)
        return "document information entry " + key + " does not match its XMP equivalent";
    } else if (xv != kv.second) {
      return "document information entry " + key + " does not match its XMP equivalent";
    }
  }
  return std::string();
}

std::string printXmpProblem(const std::string& raw, const std::string& xVersion,
                            const std::string& vtVersion, bool pdf20, bool objectLevel,
                            const std::string& eVersion) {
  std::string packet = xmpUtf8(raw);
  XPacket pk = parsePacket(packet);
  if (!pk.ok) return "XMP metadata is not well-formed RDF/XML";
  size_t hdr = packet.find("<?xpacket");
  if (hdr != std::string::npos) {
    std::string h = packet.substr(hdr, packet.find("?>", hdr) - hdr);
    if (h.find("bytes=") != std::string::npos || h.find("encoding=") != std::string::npos)
      return "XMP packet header uses the bytes or encoding attribute";
  }
  static const std::map<std::string, std::string> canon = {
      {kDc, "dc"}, {kXmp, "xmp"}, {kPdf, "pdf"}, {kMM, "xmpMM"}, {kPdfxId, "pdfxid"}, {kPdfvtId, "pdfvtid"},
      {kPdfeId, "pdfe"}};
  for (const auto& d : pk.decls) {
    auto c = canon.find(d.second);
    if (c != canon.end() && c->second != d.first)
      return "namespace " + d.second + " is bound to the prefix " + d.first + " instead of " + c->second;
  }
  if (!eVersion.empty()) {
    std::string why = xmpPropertiesProblem(pk, 4, nullptr);
    if (!why.empty() || objectLevel) return why;
    for (const XProp& p : pk.props) {
      if (p.ns == kPdfeId && p.kind == 0 && p.name == "ISO_PDFEVersion" && trimWs(p.value) == eVersion)
        return std::string();
    }
    return "pdfe:ISO_PDFEVersion is missing or does not identify " + eVersion;
  }
  if (objectLevel) return std::string();
  auto prop = [&](const std::string& ns, const char* name) -> const XProp* {
    for (const XProp& p : pk.props) {
      if (p.ns == ns && p.name == name) return &p;
    }
    return nullptr;
  };
  auto simple = [&](const std::string& ns, const char* name) {
    const XProp* p = prop(ns, name);
    return p && p->kind == 0 ? trimWs(p->value) : std::string();
  };
  for (const XProp& p : pk.props) {
    if (p.ns == kPdfxId && p.name != "GTS_PDFXVersion")
      return "the PDF/X identification namespace carries the undefined property " + p.name;
  }
  if (!xVersion.empty() && simple(kPdfxId, "GTS_PDFXVersion") != xVersion)
    return "pdfxid:GTS_PDFXVersion is missing or does not identify " + xVersion;
  std::string trapped = simple(kPdf, "Trapped");
  if (trapped != "True" && trapped != "False") return "pdf:Trapped is missing or holds a value other than True or False";
  long long when = 0;
  for (const char* n : {"CreateDate", "ModifyDate", "MetadataDate"}) {
    if (!dateParts(simple(kXmp, n), false, when)) return std::string("xmp:") + n + " is missing or not a valid date";
  }
  if (!pdf20) {
    for (const char* n : {"DocumentID", "VersionID", "RenditionClass"}) {
      if (simple(kMM, n).empty()) return std::string("xmpMM:") + n + " is missing or empty";
    }
    const XProp* title = prop(kDc, "title");
    bool titled = false;
    for (const XProp& k : title && title->kind == 2 ? title->kids : std::vector<XProp>()) {
      titled = titled || !trimWs(k.value).empty();
    }
    if (!titled) return "dc:title is missing or empty";
  }
  if (!vtVersion.empty()) {
    for (const XProp& p : pk.props) {
      if (p.ns == kPdfvtId && p.name != "GTS_PDFVTVersion" && p.name != "GTS_PDFVTModDate" &&
          !(pdf20 && (p.name == "rev" || p.name == "GTS_PDFVTRev")))
        return "the PDF/VT identification namespace carries the undefined property " + p.name;
    }
    if (simple(kPdfvtId, "GTS_PDFVTVersion") != vtVersion)
      return "pdfvtid:GTS_PDFVTVersion is missing or does not identify " + vtVersion;
    long long vtMod = 0, mod = 0;
    if (!dateParts(simple(kPdfvtId, "GTS_PDFVTModDate"), false, vtMod))
      return "pdfvtid:GTS_PDFVTModDate is missing or not a valid date";
    if (dateParts(simple(kXmp, "ModifyDate"), false, mod) &&
        simple(kXmp, "ModifyDate") != simple(kPdfvtId, "GTS_PDFVTModDate"))
      return "pdfvtid:GTS_PDFVTModDate differs from the document modification date";
    if (pdf20) {
      static const std::regex kYear("[0-9]{4}");
      if (!std::regex_match(simple(kPdfvtId, "rev"), kYear)) return "pdfvtid:rev is missing or not a four-digit year";
      if (!std::regex_match(simple(kPdfvtId, "GTS_PDFVTRev"), kYear))
        return "pdfvtid:GTS_PDFVTRev is missing or not a four-digit year";
    }
  }
  return std::string();
}

bool xmlRootOf(const std::string& xml, std::string& ns, std::string& local) {
  XNode root;
  XmlParser xp(xml);
  if (xml.empty() || !xp.parse(root)) return false;
  ns = root.ns;
  local = root.local;
  return true;
}


namespace {
struct ScopedPackets {
  std::vector<XPacket> pks;
  ExtDefs defs;
};

ScopedPackets packetsFor(const Events& ev, const std::string& scope, bool withDefs) {
  ScopedPackets sp;
  for (const auto& m : metadataPackets(ev, scope)) sp.pks.push_back(parsePacket(m.second));
  if (withDefs) {
    for (const auto& m : metadataPackets(ev, "document")) {
      XPacket doc = parsePacket(m.second);
      if (doc.ok) collectExt(doc, sp.defs);
    }
    for (const XPacket& pk : sp.pks) {
      if (pk.ok) collectExt(pk, sp.defs);
    }
  }
  return sp;
}

int narrowed(const std::string& scope, NodeSet& found) {
  if (gXmpScope) {
    auto* m = static_cast<std::map<std::string, NodeSet>*>(gXmpScope->sets);
    if (!m) gXmpScope->sets = m = new std::map<std::string, NodeSet>;
    std::string key = "xmp:" + scope;
    auto it = m->find(key);
    if (it != m->end()) {
      NodeSet both;
      std::set_intersection(found.begin(), found.end(), it->second.begin(), it->second.end(), std::inserter(both, both.end()));
      found.swap(both);
    }
    (*m)[key] = found;
  }
  return static_cast<int>(found.size());
}

int tableIndex(const std::string& by) {
  static const std::map<std::string, int> idx = {{"xmp-2004", 0}, {"xmp-2005", 1}, {"pdfa-1", 2},
                                                 {"pdfa-2", 3},   {"pdfe-1", 4},   {"pdfx-4", 5}};
  auto it = idx.find(by);
  return it == idx.end() ? -1 : it->second;
}

const std::string* extLookup(const ExtDefs& defs, const XProp& p) {
  auto n = defs.props.find(p.ns);
  if (n != defs.props.end()) {
    auto f = n->second.find(p.name);
    if (f != n->second.end()) return &f->second;
  }
  return table(1).find(p);
}

int evalPropertyKnown(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const std::string& scope = param(a, "scope");
  const std::string& by = param(a, "by");
  int ti = tableIndex(by);
  ScopedPackets sp = packetsFor(ev, scope, ti < 0);
  NodeSet found;
  for (size_t i = 0; i < sp.pks.size(); ++i) {
    if (!sp.pks[i].ok) continue;
    for (size_t j = 0; j < sp.pks[i].props.size(); ++j) {
      const XProp& p = sp.pks[i].props[j];
      if (p.ns == kExtNs) continue;
      std::vector<char> flags;
      const std::string* def = ti >= 0 ? table(ti).find(p) : extLookup(sp.defs, p);
      walkDefined(p, def, ti >= 0 ? nullptr : &sp.defs, flags);
      for (size_t k = 0; k < flags.size(); ++k) {
        if (compare(Value(flags[k] != 0), a)) found.insert({i, j, k});
      }
    }
  }
  return narrowed(scope, found);
}

int evalValueType(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const std::string& scope = param(a, "scope");
  const std::string& by = param(a, "by");
  int ti = by == "pdfa-1" ? 2 : by == "pdfa-2" ? 3 : -1;
  tGpsPart1 = ti == 2;
  tClosedChoices = true;
  struct Reset {
    ~Reset() { tClosedChoices = false; }
  } reset;
  ScopedPackets sp = packetsFor(ev, scope, ti < 0);
  NodeSet found;
  for (size_t i = 0; i < sp.pks.size(); ++i) {
    if (!sp.pks[i].ok) continue;
    for (size_t j = 0; j < sp.pks[i].props.size(); ++j) {
      const XProp& p = sp.pks[i].props[j];
      if (p.ns == kExtNs) continue;
      const std::string* def = ti >= 0 ? table(ti).find(p) : extLookup(sp.defs, p);
      if (!def) continue;
      bool ok = checkType(p, *def, ti >= 0 ? nullptr : &sp.defs, 0);
      if (compare(Value(ok), a)) found.insert({i, j, 0});
    }
  }
  return narrowed(scope, found);
}

int evalExtensionSchema(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const std::string& scope = param(a, "scope");
  std::vector<std::string> states;
  for (const auto& m : metadataPackets(ev, scope)) {
    XPacket pk = parsePacket(m.second);
    bool present = m.second.find("pdfaExtension:schemas") != std::string::npos;
    for (const XProp& p : pk.props) present = present || p.ns == kExtNs;
    states.push_back(!present ? "absent" : pk.ok && validateExt(pk) ? "valid" : "invalid");
  }
  if (states.empty() && scope == "document") states.push_back("absent");
  return compare(Value(states), a);
}

int evalHistoryIncomplete(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  const std::string& field = param(a, "field");
  static const char* all[] = {"action", "instanceID", "parameters", "softwareAgent", "when"};
  int hits = 0;
  for (const auto& m : metadataPackets(ev, param(a, "scope"))) {
    for (const XProp& p : parsePacket(m.second).props) {
      if (p.ns != kMM || p.name != "History" || p.kind != 2) continue;
      for (const XProp& e : p.kids) {
        bool missing = false;
        for (const char* f : all) {
          if (field != "any" && field != f) continue;
          bool has = false;
          for (const XProp& k : e.kids) has = has || (k.ns == kStEvt && k.name == f);
          missing = missing || !has;
        }
        hits += compare(Value(missing), a);
      }
    }
  }
  return hits;
}
}

void registerXmpChecks(CheckTable& t) {
  t.add("metadata.xmp_property_known", evalPropertyKnown);
  t.add("metadata.xmp_value_type", evalValueType);
  t.add("metadata.xmp_extension_schema", evalExtensionSchema);
  t.add("metadata.xmp_history_incomplete", evalHistoryIncomplete);
}
}
