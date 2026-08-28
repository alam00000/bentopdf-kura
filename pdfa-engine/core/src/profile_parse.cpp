#include "profile_types.hh"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "limits.hh"
#include "util.hh"

namespace pdfa {
namespace {
int xmlSeverity(int raw) { return raw == 1 ? 2 : (raw == 2 ? 1 : 3); }

std::string tagText(const std::string& s, const std::string& tag, size_t from,
                    size_t limit) {
  size_t o = s.find("<" + tag + ">", from);
  if (o == std::string::npos || o > limit) return std::string();
  o += tag.size() + 2;
  size_t c = s.find("</" + tag + ">", o);
  if (c == std::string::npos || c > limit) return std::string();
  return s.substr(o, c - o);
}

std::string canonToken(std::string t) {
  size_t p = t.find("::");
  if (p != std::string::npos && p + 2 < t.size()) {
    t[p + 2] = static_cast<char>(std::toupper(static_cast<unsigned char>(t[p + 2])));
  }
  return t;
}

std::string unescape(std::string v) {
  struct {
    const char* from;
    const char* to;
  } reps[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"},
              {"&amp;", "&"}};
  for (auto& r : reps) {
    size_t p = 0;
    while ((p = v.find(r.from, p)) != std::string::npos) {
      v.replace(p, std::strlen(r.from), r.to);
      ++p;
    }
  }
  return v;
}

struct Json {
  enum Type { kNull, kBool, kNum, kStr, kArr, kObj } type = kNull;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;

  const Json* get(const std::string& key) const {
    for (const auto& kv : obj) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }
};

struct JsonParser {
  const std::string& s;
  size_t i = 0;
  bool ok = true;
  int depth = 0;

  struct DepthCounter {
    int& d;
    explicit DepthCounter(int& v) : d(v) { ++d; }
    ~DepthCounter() { --d; }
  };

  explicit JsonParser(const std::string& text) : s(text) {}

  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
      ++i;
    }
  }

  Json parse() {
    ws();
    if (i >= s.size()) { ok = false; return {}; }
    if (depth > kMaxObjectWalk) { ok = false; return {}; }
    DepthCounter dc(depth);
    char c = s[i];
    if (c == '{') return parseObj();
    if (c == '[') return parseArr();
    if (c == '"') return parseStr();
    if (c == 't' || c == 'f') return parseBool();
    if (c == 'n') { i += 4; return {}; }
    return parseNum();
  }

  Json parseObj() {
    Json v;
    v.type = Json::kObj;
    ++i;
    ws();
    if (i < s.size() && s[i] == '}') { ++i; return v; }
    while (i < s.size()) {
      ws();
      Json key = parseStr();
      ws();
      if (i >= s.size() || s[i] != ':') { ok = false; return v; }
      ++i;
      v.obj.push_back({key.str, parse()});
      ws();
      if (i < s.size() && s[i] == ',') { ++i; continue; }
      break;
    }
    if (i < s.size() && s[i] == '}') ++i;
    else ok = false;
    return v;
  }

  Json parseArr() {
    Json v;
    v.type = Json::kArr;
    ++i;
    ws();
    if (i < s.size() && s[i] == ']') { ++i; return v; }
    while (i < s.size()) {
      v.arr.push_back(parse());
      ws();
      if (i < s.size() && s[i] == ',') { ++i; continue; }
      break;
    }
    if (i < s.size() && s[i] == ']') ++i;
    else ok = false;
    return v;
  }

  Json parseStr() {
    Json v;
    v.type = Json::kStr;
    if (i >= s.size() || s[i] != '"') { ok = false; return v; }
    ++i;
    while (i < s.size() && s[i] != '"') {
      if (s[i] == '\\' && i + 1 < s.size()) {
        char e = s[i + 1];
        if (e == 'n') v.str += '\n';
        else if (e == 't') v.str += '\t';
        else if (e == 'u' && i + 5 < s.size()) { v.str += '?'; i += 4; }
        else v.str += e;
        i += 2;
      } else {
        v.str += s[i++];
      }
    }
    ++i;
    return v;
  }

  Json parseBool() {
    Json v;
    v.type = Json::kBool;
    if (s.compare(i, 4, "true") == 0) { v.b = true; i += 4; }
    else { i += 5; }
    return v;
  }

  Json parseNum() {
    Json v;
    v.type = Json::kNum;
    size_t start = i;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' ||
                            s[i] == '+' || s[i] == '.' || s[i] == 'e' || s[i] == 'E')) {
      ++i;
    }
    v.num = std::atof(s.substr(start, i - start).c_str());
    if (i == start) ok = false;
    return v;
  }
};

const std::map<std::string, std::string>& kuraPropMap() {
  static const std::map<std::string, std::string> m = {
      {"stroke.width", "content.stroke_width"},
      {"stroke.overprint", "color.overprint"},
      {"stroke.transparency", "transparency.used"},
      {"stroke.alpha", "transparency.alpha"},
      {"stroke.totalInk", "color.total_ink"},
      {"stroke.inkCount", "color.inks_used"},
      {"fill.overprint", "color.overprint"},
      {"fill.transparency", "transparency.used"},
      {"fill.alpha", "transparency.alpha"},
      {"fill.totalInk", "color.total_ink"},
      {"fill.processInk", "color.total_ink"},
      {"fill.inkCount", "color.inks_used"},
      {"gstate.overprint", "color.overprint"},
      {"gstate.overprintModeIllustrator", "color.overprint"},
      {"gstate.transparency", "transparency.used"},
      {"gstate.blendMode", "transparency.blend_mode"},
      {"gstate.blendColorspace", "transparency.group_colour_space"},
      {"gstate.inTransparencyGroup", "transparency.group"},
      {"gstate.hasSoftMask", "transparency.used"},
      {"gstate.flatness", "print.flatness"},
      {"gstate.hasBlackPointCompensation", "color.black_point_compensation"},
      {"paint.inkCount", "color.inks_used"},
      {"paint.maxInkPercent", "color.ink"},
      {"paint.isWhite", "color.ink_mix"},
      {"paint.isBlackOnly", "color.ink_mix"},
      {"paint.richBlackCmyPercent", "color.ink"},
      {"paint.isRgb", "color.space"},
      {"paint.isCmyk", "color.space"},
      {"paint.isGray", "color.space"},
      {"paint.isIccBased", "color.space"},
      {"paint.isLab", "color.space"},
      {"paint.isCalibrated", "color.space"},
      {"paint.isDeviceIndependent", "color.space"},
      {"paint.isSpot", "color.spot"},
      {"paint.isSeparation", "color.space"},
      {"paint.isPattern", "color.space"},
      {"paint.isRegistration", "color.spot"},
      {"paint.spotName", "color.spot"},
      {"paint.spotNameHasPantoneSuffix", "color.spot"},
      {"paint.colorspaceName", "color.space"},
      {"paint.altColorspaceName", "color.space"},
      {"paint.componentCount", "color.component_count"},
      {"paint.nonZeroCmykCount", "color.inks_used"},
      {"paint.is100Black", "color.ink_mix"},
      {"paint.blackPercent", "color.ink"},
      {"paint.cmykOnly", "color.ink_mix"},
      {"paint.spotOnly", "color.ink_mix"},
      {"paint.usesIccCmyk", "color.space"},
      {"paint.usesIccRgb", "color.space"},
      {"paint.processColourAsSpot", "color.spot"},
      {"paint.processColoursAsDeviceN", "color.devicen"},
      {"paint.deviceNColorants", "color.devicen_colorants"},
      {"text.size", "text.size"},
      {"text.isInvisible", "text.rendering"},
      {"text.renderMode", "text.state"},
      {"text.isStroked", "text.rendering"},
      {"text.isClippingPath", "text.rendering"},
      {"text.hasUnicode", "text.unicode"},
      {"text.glyphUndefined", "text.glyph"},
      {"text.glyphHasContour", "text.glyph"},
      {"text.glyphIsWhitespace", "text.glyph"},
      {"font.embedded", "font.embedding"},
      {"font.notEmbedded", "font.embedding"},
      {"font.name", "font.name"},
      {"font.isType3", "font.type"},
      {"font.isTrueType", "font.type"},
      {"font.isCid", "font.type"},
      {"font.subsetComplete", "font.problem"},
      {"font.unicodeComplete", "font.all_text_unicode"},
      {"font.invalid", "font.problem"},
      {"font.notdefUsed", "text.glyph"},
      {"font.bitmapOnly", "font.license"},
      {"font.restrictedLicense", "font.license"},
      {"font.canBeEmbedded", "font.license"},
      {"font.notSubset", "font.license"},
      {"font.widthsMatch", "font.problem"},
      {"font.nameUnique", "font.name_problem"},
      {"image.ppi", "image.resolution"},
      {"image.bitsPerComponent", "image.property"},
      {"image.bpc", "image.property"},
      {"image.filter", "image.property"},
      {"image.width", "image.property"},
      {"image.height", "image.property"},
      {"image.interpolate", "image.property"},
      {"image.hasInterpolateEntry", "image.property"},
      {"image.hasSoftMask", "image.property"},
      {"image.invalid", "image.property"},
      {"content.isImage", "content.kind"},
      {"content.isImageMask", "content.kind"},
      {"content.isBitmap", "content.kind"},
      {"content.isText", "content.kind"},
      {"content.isLine", "content.kind"},
      {"content.isStroked", "content.painting"},
      {"content.isFilled", "content.painting"},
      {"content.isFilledAndStroked", "content.painting"},
      {"content.isStrokedOnly", "content.painting"},
      {"content.isShading", "content.kind"},
      {"content.outsideMediaBox", "content.box_relation"},
      {"content.outsideBleedBox", "content.box_relation"},
      {"content.insideTrimAndArtBox", "content.box_relation"},
      {"content.distanceFromTrimBox", "content.distance_to_box"},
      {"content.distanceInsideTrimBox", "content.distance_to_box"},
      {"content.pathNodes", "content.path_measure"},
      {"content.unknownOperator", "syntax.unknown_operator"},
      {"content.emptyVector", "content.painting"},
      {"page.allHaveMediaBox", "page.box_present"},
      {"page.hasMediaBox", "page.box_present"},
      {"page.hasCropBox", "page.box_present"},
      {"page.cropEqualsMedia", "page.box_relation"},
      {"page.isRotated", "page.rotation"},
      {"page.isEmpty", "page.empty"},
      {"page.number", "page.number"},
      {"page.inkCoverage", "print.ink_coverage"},
      {"page.singleImage", "page.single_image"},
      {"page.contentCompressed", "page.content_stream"},
      {"page.hasOutputIntent", "page.output_intent"},
      {"page.usesPlates", "print.uses_plates"},
      {"page.transparencyGroupHasTransparency", "page.transparency_group"},
      {"doc.pages", "document.page_count"},
      {"doc.fileSizeBytes", "document.file_size"},
      {"doc.pdfVersion", "document.pdf_version"},
      {"doc.plates", "print.plates"},
      {"doc.spotPlates", "print.plates"},
      {"doc.pagesSameSize", "document.pages_uniform"},
      {"doc.dataAfterEof", "document.data_after_eof"},
      {"doc.spotNamesEquivalent", "print.spot_naming"},
      {"doc.spotNamesNotIdentical", "print.spot_naming"},
      {"doc.spotRepresentationsInconsistent", "print.spot_naming"},
      {"doc.xmpIsPlainText", "metadata.xmp_problem"},
      {"doc.requiresPdf20", "document.requirements"},
      {"doc.namesUtf8", "syntax.string_encoding"},
      {"doc.hexStringInvalid", "syntax.string_encoding"},
      {"docinfo.creator", "metadata.info"},
      {"docinfo.producer", "metadata.info"},
      {"docinfo.trapped", "print.trapped"},
      {"docinfo.hasPdfxFields", "metadata.info_keys"},
      {"outputIntent.count", "output_intent.count"},
      {"outputIntent.hasProfile", "output_intent.entry"},
      {"outputIntent.isPdfx", "output_intent.count"},
      {"outputIntent.isPdfa", "output_intent.count"},
      {"outputIntent.pdfxEntries", "output_intent.count"},
      {"outputIntent.icc.colorspace", "icc.header"},
      {"outputIntent.icc.version", "icc.header"},
      {"annot.type", "annotation.type"},
      {"annot.isType", "annotation.type"},
      {"annot.prints", "annotation.flags"},
      {"annot.hasOpacity", "annotation.entry"},
      {"annot.opacity", "annotation.entry"},
      {"annot.insideBleedOrTrim", "content.box_relation"},
      {"annot.unknownType", "annotation.type"},
      {"layers.onLayer", "layer.membership"},
      {"layers.visible", "layer.visible"},
      {"layers.hasConfigs", "layer.configuration"},
      {"layers.processingSteps", "layer.processing_step"},
      {"layers.hasProcessingSteps", "layer.processing_step"},
      {"halftone.hasOrigin", "print.halftone"},
      {"icc.version", "icc.header"},
      {"icc.colorspace", "icc.header"},
      {"signature.hasFields", "signature.present"},
      {"vt.hasDocumentParts", "variable_data.document_parts"},
  };
  return m;
}

const std::map<std::string, std::string>& kuraBuiltinMap() {
  static const std::map<std::string, std::string> m = {
      {"imageResolutionBelow", "image.resolution"},
      {"imageResolutionAbove", "image.resolution"},
      {"bitmapResolutionBelow", "image.resolution"},
      {"bitmapResolutionAbove", "image.resolution"},
      {"colourPlatesUsed", "print.plates"},
      {"deviceIndependentColour", "color.space"},
      {"rgbUsed", "color.space"},
      {"spotColoursMoreThan", "print.plates"},
      {"spotNamesInconsistent", "print.spot_naming"},
      {"fontsNotEmbedded", "font.embedding"},
      {"fontsEmbedded", "font.embedding"},
      {"type1CidFonts", "font.type"},
      {"trueTypeCidFonts", "font.type"},
      {"openTypeFonts", "font.type"},
      {"encrypted", "security.encryption"},
      {"damaged", "document.damaged"},
      {"syntaxProblems", "syntax.dictionary_error"},
      {"pdfVersionBelow", "document.pdf_version"},
      {"uncompressedImages", "image.property"},
      {"pageCount", "document.page_count"},
      {"pagesDifferInSize", "document.pages_uniform"},
      {"emptyPage", "page.empty"},
      {"transferCurves", "print.rendering_functions"},
      {"halftones", "print.halftone"},
      {"postscript", "syntax.postscript"},
      {"transparencyUsed", "transparency.used"},
      {"hairlinesBelow", "content.stroke_width"},
      {"conformsTo", "conformance.meets"},
      {"embeddedFilesConformTo", "conformance.meets"},
  };
  return m;
}

const std::map<std::string, std::string>& kuraBuiltinParamMap() {
  static const std::map<std::string, std::string> m = {
      {"ppi", "PixelsPerInch"},
      {"count", "SpotColorSepsOnPage"},
      {"version", "AcroVers"},
      {"points", "Points"},
  };
  return m;
}

const std::map<std::string, std::string>& kuraLevelFlagMap() {
  static const std::map<std::string, std::string> m = {
      {"1b", "PDFA1b2005"},
      {"1a", "PDFA1a2005"},
      {"2b", "PDFA2b"},
      {"2u", "PDFA2u"},
      {"2a", "PDFA2a"},
      {"3b", "PDFA3b"},
      {"3u", "PDFA3u"},
      {"3a", "PDFA3a"},
      {"4", "PDFA4"},
      {"4f", "PDFA4f"},
      {"4e", "PDFA4e"},
      {"x1a", "PDFX1A2003"},
      {"x3", "PDFX32003"},
      {"x4", "PDFX4"},
      {"x4p", "PDFX4p"},
      {"x5g", "PDFX5g"},
      {"x5n", "PDFX5n"},
      {"x5pg", "PDFX5pg"},
      {"x6", "PDFX6"},
      {"x6n", "PDFX6n"},
      {"x6p", "PDFX6p"},
      {"e1", "PDFE12008"},
      {"vt1", "PDFVT1"},
      {"vt2", "PDFVT2"},
      {"vt3", "PDFVT3"},
  };
  return m;
}

std::string kuraOpMap(const std::string& op) {
  if (op == "<") return "less";
  if (op == "<=") return "less_or_equal";
  if (op == ">") return "greater";
  if (op == ">=") return "greater_or_equal";
  if (op == "==" || op == "is") return "equal";
  if (op == "!=" || op == "isNot") return "unequal";
  return op;
}

std::string propToToken(const std::string& prop) {
  auto it = kuraPropMap().find(prop);
  if (it != kuraPropMap().end()) return it->second;
  static const std::map<std::string, std::string> head = {
      {"paint", "grp_37816"}, {"stroke", "grp_45919"}, {"fill", "grp_75736"},
      {"gstate", "grp_57935"}, {"text", "grp_37222"}, {"font", "CSFONT"},
      {"image", "grp_7226"}, {"page", "grp_46892"}, {"doc", "grp_86055"}, {"docinfo", "grp_7639"},
      {"annot", "grp_3254"}, {"content", "grp_78804"}, {"outputIntent", "grp_75300"},
      {"outputIntent.icc", "grp_39296"}, {"outputIntentA.icc", "grp_67275"},
      {"outputIntentA", "grp_77532"}, {"outputIntentE", "grp_16769"},
      {"docinfo", "grp_7639"}, {"signature", "grp_38257"},
      {"layers", "grp_34967"}, {"halftone", "grp_13332"}, {"icc", "grp_2412"},
      {"syntax", "grp_79118"}, {"contentSyntax", "grp_78948"}, {"structure", "grp_12910"},
      {"certificate", "grp_88767"}, {"vt", "grp_2184"}, {"compare", "grp_92045"},
      {"tagging", "grp_54741"}, {"form", "grp_28738"}, {"postscript", "grp_62682"},
  };
  size_t dot = prop.find('.');
  if (dot == std::string::npos) return "REMOVED" + prop;
  size_t dot2 = prop.find('.', dot + 1);
  auto hit = head.end();
  std::string tail;
  if (dot2 != std::string::npos) {
    hit = head.find(prop.substr(0, dot2));
    if (hit != head.end()) tail = prop.substr(dot2 + 1);
  }
  if (hit == head.end()) {
    hit = head.find(prop.substr(0, dot));
    if (hit != head.end()) tail = prop.substr(dot + 1);
  }
  if (hit == head.end()) return "REMOVED" + prop;
  if (!tail.empty()) tail[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(tail[0])));
  return hit->second + "::" + tail;
}

}

bool parseKuraJson(const std::string& text, PfProfile& out) {
  JsonParser jp(text);
  Json root = jp.parse();
  if (!jp.ok || root.type != Json::kObj || !root.get("kura-profile")) return false;
  const Json* name = root.get("name");
  if (name) out.name = name->str;
  const Json* bts = root.get("builtins");
  if (bts && bts->type == Json::kArr) {
    for (const Json& b : bts->arr) {
      if (b.type != Json::kObj) continue;
      const Json* bn = b.get("name");
      if (!bn || bn->type != Json::kStr) continue;
      PfBuiltin pb;
      auto alias = kuraBuiltinMap().find(bn->str);
      pb.name = alias == kuraBuiltinMap().end() ? bn->str : alias->second;
      const Json* sv = b.get("severity");
      if (sv && sv->type == Json::kNum) {
        pb.severity = xmlSeverity(static_cast<int>(sv->num));
      } else if (sv && sv->type == Json::kStr) {
        pb.severity = sv->str == "error" ? 3 : (sv->str == "warning" ? 2 : 1);
      }
      const Json* pr = b.get("params");
      if (pr && pr->type == Json::kObj) {
        for (const auto& kv : pr->obj) {
          if (kv.second.type != Json::kNum) continue;
          auto pk = kuraBuiltinParamMap().find(kv.first);
          pb.params[pk == kuraBuiltinParamMap().end() ? kv.first : pk->second] = kv.second.num;
        }
      }
      const Json* lv = b.get("level");
      if (lv && lv->type == Json::kStr) {
        auto fl = kuraLevelFlagMap().find(lv->str);
        if (fl != kuraLevelFlagMap().end()) pb.params[fl->second] = 1;
      }
      out.builtins.push_back(pb);
    }
  }
  const Json* checks = root.get("checks");
  if (!checks || checks->type != Json::kArr) return false;
  int serial = 0;
  for (const Json& c : checks->arr) {
    if (c.type != Json::kObj) continue;
    PfRule rule;
    rule.id = "K" + std::to_string(++serial);
    const Json* rn = c.get("name");
    rule.name = rn ? rn->str : rule.id;
    const Json* sev = c.get("severity");
    std::string sv = sev ? sev->str : "info";
    rule.severity = sv == "error" ? 3 : (sv == "warning" ? 2 : 1);
    const Json* sc = c.get("scope");
    if (sc && sc->type == Json::kStr) {
      rule.scope = sc->str == "trim" ? 2 : (sc->str == "bleed" ? 3 : 0);
    }
    const Json* any = c.get("any");
    std::vector<const Json*> groups;
    if (any && any->type == Json::kArr) {
      rule.logic = 1;
      for (const Json& g : any->arr) {
        const Json* ga = g.get("all");
        if (ga && ga->type == Json::kArr) groups.push_back(ga);
      }
    } else {
      const Json* all = c.get("all");
      if (all && all->type == Json::kArr) groups.push_back(all);
    }
    if (groups.empty()) continue;
    int gi = 0;
    for (const Json* grp : groups) {
      PfCondition cond;
    for (const Json& a : grp->arr) {
      if (a.type != Json::kObj) continue;
      PfAtom atom;
      const Json* prop = a.get("prop");
      if (!prop) continue;
      atom.token = propToToken(prop->str);
      const Json* op = a.get("op");
      atom.op = kuraOpMap(op ? op->str : "==");
      const Json* val = a.get("value");
      if (val) {
        if (val->type == Json::kNum) {
          atom.vals.push_back(fmtFixed(val->num, 6));
        } else if (val->type == Json::kStr) {
          atom.vals.push_back(val->str);
        } else if (val->type == Json::kBool) {
          atom.op = val->b ? (atom.op == "unequal" ? "is_not_true" : "is_true")
                          : (atom.op == "unequal" ? "is_true" : "is_not_true");
        }
      }
      cond.atoms.push_back(atom);
    }
      std::string cid = "C" + rule.id + "_" + std::to_string(gi++);
      out.conds[cid] = cond;
      rule.condIds.push_back(cid);
    }
    out.rules.push_back(rule);
  }
  return true;
}

bool parsePreflightXml(const std::string& text, PfProfile& out) {
  size_t pos = 0;
  while ((pos = text.find("<condition>", pos)) != std::string::npos) {
    size_t end = text.find("</condition>", pos);
    if (end == std::string::npos) break;
    std::string id = tagText(text, "id1", pos, end);
    if (id.empty() || id[0] != 'C') {
      pos = end + 1;
      continue;
    }
    PfCondition cond;
    size_t apos = pos;
    while (true) {
      apos = text.find("<atom>", apos);
      if (apos == std::string::npos || apos > end) break;
      size_t aend = text.find("</atom>", apos);
      if (aend == std::string::npos || aend > end) break;
      PfAtom atom;
      atom.token = canonToken(tagText(text, "token", apos, aend));
      std::string op = tagText(text, "operator", apos, aend);
      if (op.rfind("op.", 0) == 0) op = op.substr(4);
      atom.op = op;
      size_t vpos = apos;
      while (true) {
        vpos = text.find("<value", vpos);
        if (vpos == std::string::npos || vpos > aend) break;
        size_t vopen = text.find('>', vpos);
        size_t vclose = text.find("</value>", vopen);
        if (vopen == std::string::npos || vclose == std::string::npos || vclose > aend) {
          break;
        }
        atom.vals.push_back(unescape(text.substr(vopen + 1, vclose - vopen - 1)));
        vpos = vclose + 1;
      }
      size_t ipos = apos;
      while (true) {
        ipos = text.find("<item", ipos);
        if (ipos == std::string::npos || ipos > aend) break;
        size_t iopen = text.find('>', ipos);
        size_t iclose = text.find("</item>", iopen);
        if (iopen == std::string::npos || iclose == std::string::npos || iclose > aend) {
          break;
        }
        std::string iv = unescape(text.substr(iopen + 1, iclose - iopen - 1));
        size_t b = iv.find_first_not_of(" \t\r\n");
        if (b != std::string::npos) {
          atom.vals.push_back(iv.substr(b, iv.find_last_not_of(" \t\r\n") - b + 1));
        }
        ipos = iclose + 1;
      }
      cond.atoms.push_back(atom);
      apos = aend + 1;
    }
    out.conds[id] = cond;
    pos = end + 1;
  }
  pos = 0;
  while ((pos = text.find("<rule>", pos)) != std::string::npos) {
    size_t end = text.find("</rule>", pos);
    if (end == std::string::npos) break;
    PfRule rule;
    rule.id = tagText(text, "id1", pos, end);
    rule.name = unescape(tagText(text, "name", pos, end));
    size_t lp = text.find("condition_logic=\"", pos);
    if (lp != std::string::npos && lp < end) rule.logic = text[lp + 17] - '0';
    size_t ip = text.find("ignore_objects=\"", pos);
    if (ip != std::string::npos && ip < end) rule.scope = text[ip + 16] - '0';
    size_t cpos = text.find("<conditions>", pos);
    size_t cend = text.find("</conditions>", pos);
    if (cpos != std::string::npos && cend != std::string::npos && cend < end) {
      size_t p = cpos;
      while (true) {
        p = text.find("<condition>", p);
        if (p == std::string::npos || p > cend) break;
        size_t q = text.find("</condition>", p);
        if (q == std::string::npos || q > cend) break;
        rule.condIds.push_back(text.substr(p + 11, q - p - 11));
        p = q + 1;
      }
    }
    if (!rule.id.empty() && rule.id[0] == 'R') out.rules.push_back(rule);
    pos = end + 1;
  }
  std::vector<std::string> varVals;
  {
    size_t vp = text.find("<variables>");
    if (vp != std::string::npos) {
      size_t vend = text.find("</variables>", vp);
      size_t q = vp;
      while (true) {
        q = text.find("<varvalue>", q);
        if (q == std::string::npos || (vend != std::string::npos && q > vend)) break;
        size_t qc = text.find("</varvalue>", q);
        if (qc == std::string::npos || (vend != std::string::npos && qc > vend)) break;
        varVals.push_back(text.substr(q + 10, qc - q - 10));
        q = qc + 1;
      }
    }
  }
  std::map<std::string, std::vector<std::pair<int, std::string>>> rulesetDefs;
  pos = text.find("<rulesets>");
  if (pos != std::string::npos) {
    size_t end = text.find("</rulesets>", pos);
    size_t sp = pos;
    while (true) {
      sp = text.find("<ruleset>", sp);
      if (sp == std::string::npos || sp > end) break;
      size_t send = text.find("</ruleset>", sp);
      if (send == std::string::npos || send > end) break;
      std::string sid = tagText(text, "id1", sp, send);
      size_t p = sp;
      while (true) {
        p = text.find("<rule check_severity=\"", p);
        if (p == std::string::npos || p > send) break;
        int sev = text[p + 22] - '0';
        size_t open = text.find('>', p);
        if (open == std::string::npos || open > send) break;
        size_t close = text.find("</rule>", open);
        if (close == std::string::npos || close > send) break;
        std::string attrs = text.substr(p, open - p);
        bool varOff = false;
        if (attrs.find("var_usage=\"1\"") != std::string::npos) {
          size_t vi = attrs.find("var_idx_onoff=\"");
          if (vi != std::string::npos) {
            int idx = std::atoi(attrs.c_str() + vi + 15);
            if (idx >= 0 && idx < static_cast<int>(varVals.size()) && varVals[idx] == "0") {
              varOff = true;
            }
          }
        }
        if (!varOff) {
          rulesetDefs[sid].push_back(
              {xmlSeverity(sev), text.substr(open + 1, close - open - 1)});
        }
        p = close + 1;
      }
      sp = send + 1;
    }
  }
  size_t ppos = text.find("<profile>");
  if (ppos != std::string::npos) {
    size_t pend2 = text.find("</profile>", ppos);
    size_t cp = ppos;
    while (true) {
      cp = text.find("<check name=\"", cp);
      if (cp == std::string::npos || (pend2 != std::string::npos && cp > pend2)) break;
      size_t nend = text.find('"', cp + 13);
      PfBuiltin b;
      b.name = text.substr(cp + 13, nend - cp - 13);
      size_t sevp = text.find("check_severity=\"", cp);
      if (sevp != std::string::npos && sevp < cp + 120) {
        b.severity = xmlSeverity(text[sevp + 16] - '0');
      }
      size_t cend = text.find("</check>", cp);
      size_t selfclose = text.find("/>", cp);
      size_t bodyEnd = cend != std::string::npos ? cend : text.size();
      if (selfclose != std::string::npos && (cend == std::string::npos || selfclose < text.find('>', cp))) {
        bodyEnd = selfclose;
      }
      size_t dp = cp;
      while (true) {
        dp = text.find("<data name=\"", dp);
        if (dp == std::string::npos || dp > bodyEnd) break;
        size_t dn = text.find('"', dp + 12);
        std::string key = text.substr(dp + 12, dn - dp - 12);
        size_t vopen = text.find('>', dn);
        size_t vclose = text.find("</data>", vopen);
        if (vopen != std::string::npos && vclose != std::string::npos && vclose <= bodyEnd + 200) {
          b.params[key] = std::atof(text.substr(vopen + 1, vclose - vopen - 1).c_str());
        }
        dp = (vclose == std::string::npos ? dp + 12 : vclose + 1);
      }
      out.builtins.push_back(b);
      cp = cend == std::string::npos ? cp + 13 : cend + 1;
    }
  }
  std::set<std::string> activeRuleIds;
  std::map<std::string, int> activeSev;
  if (ppos != std::string::npos) {
    out.name = unescape(tagText(text, "name", ppos, text.size()));
    size_t pend = text.find("</profile>", ppos);
    size_t rp = ppos;
    while (true) {
      rp = text.find("<ruleset>", rp);
      if (rp == std::string::npos || (pend != std::string::npos && rp > pend)) break;
      size_t rclose = text.find("</ruleset>", rp);
      if (rclose == std::string::npos || (pend != std::string::npos && rclose > pend)) break;
      std::string sid = text.substr(rp + 9, rclose - rp - 9);
      size_t b = sid.find_first_not_of(" \t\r\n");
      if (b != std::string::npos) {
        sid = sid.substr(b, sid.find_last_not_of(" \t\r\n") - b + 1);
      }
      auto it = rulesetDefs.find(sid);
      if (it != rulesetDefs.end()) {
        for (const auto& [sev, rid] : it->second) {
          activeRuleIds.insert(rid);
          activeSev[rid] = sev;
        }
      }
      rp = rclose + 1;
    }
  }
  if (!activeRuleIds.empty()) {
    std::vector<PfRule> kept;
    for (auto& r : out.rules) {
      if (activeRuleIds.count(r.id)) {
        r.severity = activeSev[r.id];
        kept.push_back(r);
      }
    }
    out.rules = kept;
  } else {
    for (const auto& [sid, entries] : rulesetDefs) {
      for (const auto& [sev, rid] : entries) {
        for (auto& r : out.rules) {
          if (r.id == rid) r.severity = sev;
        }
      }
    }
  }
  return !out.rules.empty() || !out.builtins.empty();
}


std::vector<PfFix> collectFixes(const std::string& text) {
  std::vector<PfFix> fixes;
  size_t firstCh = text.find_first_not_of(" \t\r\n");
  bool isJson = firstCh != std::string::npos && text[firstCh] == '{';
  if (isJson) {
    JsonParser jp(text);
    Json root = jp.parse();
    if (!jp.ok || root.type != Json::kObj) return fixes;
    const Json* fx = root.get("fixes");
    if (!fx || fx->type != Json::kArr) return fixes;
    for (const Json& f : fx->arr) {
      if (f.type != Json::kObj) continue;
      const Json* op = f.get("op");
      if (!op || op->type != Json::kStr) continue;
      PfFix fix;
      fix.op = op->str;
      const Json* params = f.get("params");
      if (params && params->type == Json::kArr) {
        for (const Json& p : params->arr) {
          if (p.type == Json::kStr) fix.params.push_back(p.str);
          else if (p.type == Json::kNum) {
            fix.params.push_back(fmtFixed(p.num, 6));
          }
        }
      }
      fixes.push_back(fix);
    }
    return fixes;
  }
  size_t pos = 0;
  while ((pos = text.find("<ffeat>", pos)) != std::string::npos) {
    size_t end = text.find("</ffeat>", pos);
    if (end == std::string::npos) break;
    PfFix fix;
    fix.op = text.substr(pos + 7, end - pos - 7);
    size_t pstart = text.find("<fparams>", end);
    size_t nextf = text.find("<ffeat>", end);
    if (pstart != std::string::npos && (nextf == std::string::npos || pstart < nextf)) {
      size_t pend = text.find("</fparams>", pstart);
      size_t p = pstart;
      while (pend != std::string::npos) {
        p = text.find("<fparam", p);
        if (p == std::string::npos || p > pend) break;
        size_t gt = text.find('>', p);
        size_t close = text.find("</fparam>", gt);
        if (gt == std::string::npos || close == std::string::npos || close > pend) break;
        fix.params.push_back(text.substr(gt + 1, close - gt - 1));
        p = close + 1;
      }
    }
    fixes.push_back(fix);
    pos = end + 1;
  }
  pos = 0;
  while ((pos = text.find("<fixup>", pos)) != std::string::npos) {
    size_t end = text.find("</fixup>", pos);
    if (end == std::string::npos) break;
    if (text.find("<ffeat>", pos) < end) {
      pos = end + 1;
      continue;
    }
    size_t cf = text.find("<fcfg>", pos);
    if (cf == std::string::npos || cf > end) {
      pos = end + 1;
      continue;
    }
    size_t cfEnd = text.find("</fcfg>", cf);
    if (cfEnd == std::string::npos || cfEnd > end) {
      pos = end + 1;
      continue;
    }
    std::string raw = text.substr(cf + 6, cfEnd - cf - 6);
    std::string dec;
    dec.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
      if (raw[i] == '&') {
        if (raw.compare(i, 4, "&#9;") == 0) { dec += '\t'; i += 4; continue; }
        if (raw.compare(i, 5, "&#10;") == 0) { dec += '\n'; i += 5; continue; }
        if (raw.compare(i, 5, "&#13;") == 0) { i += 5; continue; }
        if (raw.compare(i, 5, "&amp;") == 0) { dec += '&'; i += 5; continue; }
        if (raw.compare(i, 4, "&lt;") == 0) { dec += '<'; i += 4; continue; }
        if (raw.compare(i, 4, "&gt;") == 0) { dec += '>'; i += 4; continue; }
        if (raw.compare(i, 6, "&apos;") == 0) { dec += '\''; i += 6; continue; }
        if (raw.compare(i, 6, "&quot;") == 0) { dec += '"'; i += 6; continue; }
      }
      dec += raw[i++];
    }
    size_t nl = dec.find('\n');
    std::string line = nl == std::string::npos ? dec : dec.substr(0, nl);
    PfFix fix;
    size_t f0 = 0;
    while (f0 <= line.size()) {
      size_t tab = line.find('\t', f0);
      std::string tok = line.substr(f0, tab == std::string::npos ? std::string::npos
                                                                 : tab - f0);
      if (fix.op.empty()) fix.op = tok;
      else fix.params.push_back(tok);
      if (tab == std::string::npos) break;
      f0 = tab + 1;
    }
    if (!fix.op.empty()) fixes.push_back(fix);
    pos = end + 1;
  }
  return fixes;
}

}
