#include <qpdf/JSON.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ctx.hh"
#include "passes.hh"
#include "profile_vars.hh"
#include "textextract.hh"

namespace pdfa {
namespace {
std::string trimmed(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return std::string();
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

bool isHit(const Issue& i) { return i.code == "CHECK_HIT"; }

long long hitCount(const Issue& i) {
  size_t h = i.detail.rfind(" hit");
  size_t d = h;
  while (d > 0 && h != std::string::npos && std::isdigit(static_cast<unsigned char>(i.detail[d - 1]))) --d;
  return h == std::string::npos || d == h ? 1 : std::atoll(i.detail.c_str() + d);
}

std::string levelName(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  if (s.rfind("pdf", 0) == 0) s.erase(0, 3);
  s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '-' || c == '/' || c == ' ' || c == '_'; }), s.end());
  if (s.size() > 1 && s[0] == 'a' && std::isdigit(static_cast<unsigned char>(s[1]))) s.erase(0, 1);
  return s;
}

double resolutionOf(const Json* j, double dflt) {
  if (!j) return dflt;
  double v = std::atof(j->text().c_str());
  return v > 0 ? std::clamp(v, 24.0, 1200.0) : dflt;
}

bool pageInRange(const std::string& range, int page) {
  size_t p = 0;
  while (p < range.size()) {
    size_t c = range.find(',', p);
    std::string part = trimmed(range.substr(p, c == std::string::npos ? std::string::npos : c - p));
    size_t dash = part.find('-');
    int lo = 0, hi = 0;
    if (part == "even" && page % 2 == 0) return true;
    if (part == "odd" && page % 2 == 1) return true;
    if (dash == std::string::npos) {
      lo = hi = std::atoi(part.c_str());
    } else {
      lo = dash == 0 ? 1 : std::atoi(part.c_str());
      hi = dash + 1 >= part.size() ? 1 << 30 : std::atoi(part.c_str() + dash + 1);
    }
    if (page >= lo && page <= hi) return true;
    if (c == std::string::npos) break;
    p = c + 1;
  }
  return false;
}

void filterRange(std::vector<Issue>& list, const std::string& range) {
  std::vector<Issue> kept;
  for (Issue i : list) {
    if (i.pages.empty()) {
      kept.push_back(i);
      continue;
    }
    std::vector<int> pages;
    for (int p : i.pages) {
      if (pageInRange(range, p)) pages.push_back(p);
    }
    if (pages.empty()) continue;
    i.pages = pages;
    kept.push_back(i);
  }
  list = kept;
}

void trace(const Options& opt, const std::string& msg) {
  if (opt.trace) opt.trace(msg);
}

std::string docJsonOf(const unsigned char* data, std::size_t size, const Options& opt) {
  try {
    QPDF pdf;
    pdf.setSuppressWarnings(true);
    pdf.setAttemptRecovery(true);
    pdf.processMemoryFile("vars", reinterpret_cast<const char*>(data), size, opt.password.c_str());
    const std::string& path = opt.inputName.empty() ? opt.inputPath : opt.inputName;
    size_t slash = path.find_last_of("/\\");
    return docFactsJson(pdf, slash == std::string::npos ? path : path.substr(slash + 1));
  } catch (...) {
    return "{}";
  }
}

Result verifyBytes(const std::vector<unsigned char>& pdf, const Options& opt) {
  Options v = opt;
  v.verifyOnly = true;
  v.preflightProfile.clear();
  v.profileFixOps.clear();
  v.colorJob = ColorJob();
  v.signDocument = nullptr;
  v.rasterizePage = nullptr;
  v.bookmarks.clear();
  return convertOnce(pdf.data(), pdf.size(), v);
}

void applyConvert(const Json& root, Options& opt, std::vector<Issue>& pre) {
  const Json* cv = root.get("convert");
  if (!cv || cv->type != Json::kObj) return;
  std::string what;
  const Json* lv = cv->get("level");
  if (lv && opt.levelFromProfile) {
    Level l;
    if (levelFromString(levelName(lv->text()), l)) {
      opt.level = l;
      opt.noStandard = false;
      what = "profile converts to " + levelToString(l);
    } else {
      pre.push_back({"PROFILE_INVALID", "convert.level: \"" + lv->text() + "\" is not a known standard", false, 3});
    }
  }
  if (const Json* cc = cv->get("colors"); cc && cc->type == Json::kObj) {
    const Json* to = cc->get("to");
    std::string target = to && to->type == Json::kStr ? to->text() : std::string();
    std::string lower = target;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    std::string icc;
    bool ok = !target.empty();
    std::string missing = target;
    if (const Json* dl = cc->get("device_link"); dl && dl->type == Json::kStr) {
      ok = opt.loadIcc && opt.loadIcc(dl->text(), opt.colorJob.deviceLink) && opt.colorJob.deviceLink.size() >= 132;
      missing = dl->text();
      if (target.empty()) target = "device-link";
    } else if (ok && lower != "cmyk" && lower != "gray" && lower != "grey" && lower != "rgb" && lower != "srgb") {
      ok = opt.loadIcc && opt.loadIcc(target, icc) && icc.size() >= 132;
    }
    int intent = -1;
    if (const Json* in = cc->get("intent"); in && in->type == Json::kStr) {
      std::string v = in->text();
      intent = v == "perceptual" ? 0 : v == "relative" ? 1 : v == "saturation" ? 2 : v == "absolute" ? 3 : -1;
      if (intent < 0)
        pre.push_back({"PROFILE_INVALID", "convert.colors: intent \"" + v +
                       "\" is not perceptual, relative, saturation or absolute", false, 3});
    }
    bool keepSpots = false;
    if (const Json* ks = cc->get("keep_spots"); ks && ks->type == Json::kBool) keepSpots = ks->b;
    if (const Json* gk = cc->get("gray_to_black"); gk && gk->type == Json::kBool) opt.colorJob.grayToBlackOnly = gk->b;
    if (const Json* il = cc->get("ink_limit"); il && il->type == Json::kNum) opt.colorJob.inkLimit = il->num;
    if (const Json* rg = cc->get("rgb_gray_to_black"); rg && rg->type == Json::kNum) {
      opt.colorJob.rgbGrayToBlackOnly = true;
      for (double& t : opt.colorJob.rgbGrayTolerance) t = rg->num / 100.0;
    }
    if (const Json* tc = cc->get("tone_curve"); tc && tc->type == Json::kArr && tc->arr.size() >= 4 && tc->arr.size() % 2 == 0) {
      std::vector<double> pts;
      for (const Json& v : tc->arr) pts.push_back(v.type == Json::kNum ? v.num : 0);
      opt.colorJob.toneCurves.push_back({15u, pts});
    }
    std::string from;
    if (const Json* fr = cc->get("from"); fr && fr->type == Json::kStr) {
      std::string probe;
      from = fr->text();
      if (ok && !(opt.loadIcc && opt.loadIcc(from, probe) && probe.size() >= 20 && probe.substr(16, 4) == "CMYK")) {
        ok = false;
        missing = from;
      }
    }
    if (ok && configureColorConversion(opt, target, icc, intent, keepSpots, ColorRule::kAllObjects)) {
      if (!from.empty()) {
        opt.colorJob.cmykName = from;
        for (ColorRule& r : opt.colorJob.rules)
          if (r.sources == ColorRule::kUncCmyk || r.sources == ColorRule::kCalCmyk) r.action = ColorRule::kConvert;
      }
      what += (what.empty() ? "" : ", ") + std::string("colours to \"") + target + "\"";
    } else {
      pre.push_back({"PROFILE_ICC_MISSING", "convert.colors: colour profile \"" + missing +
                     "\" was not found; install it or pass --icc-folder", false, 2});
    }
  }
  if (const Json* pv = cv->get("pdf_version")) {
    static const std::set<std::string> kVersions = {"1.3", "1.4", "1.5", "1.6", "1.7", "2.0"};
    if (pv->type == Json::kStr && kVersions.count(pv->text())) {
      opt.pdfVersion = pv->text();
      what += (what.empty() ? "" : ", ") + std::string("PDF ") + pv->text();
    } else {
      pre.push_back({"PROFILE_INVALID", "convert.pdf_version: expected 1.3, 1.4, 1.5, 1.6, 1.7 or 2.0", false, 3});
    }
  }
  if (const Json* pf = cv->get("profile")) {
    std::string bytes;
    if (pf->type == Json::kStr && opt.loadIcc && opt.loadIcc(pf->text(), bytes) && bytes.size() >= 132) {
      opt.destProfile = bytes;
      what += (what.empty() ? "" : ", ") + std::string("press profile \"") + pf->text() + "\"";
    } else {
      pre.push_back({"PROFILE_ICC_MISSING", "convert.profile: colour profile \"" + pf->text() +
                     "\" was not found; install it or pass --icc-folder", false, 2});
    }
  }
  const Json* also = cv->get("also");
  if (also && !also->text().empty()) {
    Level l;
    if (levelFromString(levelName(also->text()), l) && levelFamily(l) == Family::PDFX) {
      opt.dualPrint = true;
      what += std::string(what.empty() ? "profile adds" : ",") + " also " + levelToString(l);
    } else {
      pre.push_back({"PROFILE_INVALID", "convert.also: \"" + also->text() + "\" is not a PDF/X standard", false, 3});
    }
  }
  if (const Json* rc = cv->get("reconvert")) opt.reconvert = opt.reconvert || (rc->type == Json::kBool && rc->b);
  if (const Json* rf = cv->get("raster_fallback")) {
    if (rf->type == Json::kObj) {
      if (opt.fallbackDpi <= 0) opt.fallbackDpi = resolutionOf(rf->get("resolution"), 300);
      const Json* pg = rf->get("pages");
      if (pg && pg->text() == "problem_pages") opt.fallbackProblemPagesOnly = true;
      const Json* kt = rf->get("keep_text");
      if (kt && kt->type == Json::kBool && !kt->b) opt.rasterKeepText = false;
    }
  }
  if (!what.empty()) pre.push_back({"PROFILE_TARGET", what, false, 1});
}

Result runPrepared(const unsigned char* data, std::size_t size, const Options& optIn) {
  Options opt = optIn;
  std::vector<Issue> pre;
  if (!opt.preflightProfile.empty()) {
    Json root;
    std::string err;
    if (parseJson(opt.preflightProfile, root, err) && root.type == Json::kObj) {
      std::vector<ProfileVariable> defs = listProfileVariables(root);
      if (!defs.empty() || !opt.profileVars.empty()) {
        std::vector<std::string> notes;
        substituteVariables(root, resolveVariables(defs, docJsonOf(data, size, opt), opt.profileVars, notes));
        opt.preflightProfile = jsonText(root);
        for (const std::string& n : notes) pre.push_back({"VARIABLE_SET", n, false, n.rfind("variable ", 0) == 0 ? 2 : 1});
      }
      applyConvert(root, opt, pre);
    }
  }
  trace(opt, "convert level=" + levelToString(opt.level) + (opt.verifyOnly ? " check" : ""));
  Result r = convertOnce(data, size, opt);
  trace(opt, std::string("convert done ok=") + (r.ok ? "1" : "0"));
  if (!opt.verifyOnly && (opt.reconvert || opt.fallbackDpi > 0) && levelFamily(opt.level) == Family::PDFA) {
    Result check;
    bool good = r.ok && (check = verifyBytes(r.pdf, opt)).compliant;
    if (!good && opt.reconvert && r.ok) {
      trace(opt, "fallback reconvert");
      Options o = opt;
      o.preflightProfile.clear();
      o.profileFixOps.clear();
      o.colorJob = ColorJob();
      Result r2 = convertOnce(r.pdf.data(), r.pdf.size(), o);
      if (r2.ok && (check = verifyBytes(r2.pdf, opt)).compliant) {
        r2.analysis.insert(r2.analysis.begin(), r.analysis.begin(), r.analysis.end());
        r2.issues.push_back({"FALLBACK_RECONVERTED", "converted a second time from the first result", true, 1});
        r = r2;
        good = true;
      }
    }
    if (!good && opt.fallbackDpi > 0 && opt.rasterizePage) {
      Options o = opt;
      o.rasterDpi = opt.fallbackDpi;
      if (opt.fallbackProblemPagesOnly && r.ok) {
        for (const Issue& i : check.issues) {
          if (i.fixed && !issueIsNormalization(i.code)) o.rasterPages.insert(i.pages.begin(), i.pages.end());
        }
      }
      if (o.rasterPages.empty()) o.rasterizeAllPages = true;
      trace(opt, "fallback raster dpi=" + std::to_string(static_cast<int>(o.rasterDpi)));
      Result r3 = convertOnce(data, size, o);
      if (r3.ok) {
        r3.issues.push_back({"FALLBACK_RASTERIZED",
                             std::string("regular conversion did not reach compliance; ") +
                                 (o.rasterizeAllPages ? "all pages" : std::to_string(o.rasterPages.size()) + " page(s)") +
                                 " converted to images" + (o.rasterKeepText ? " with the original text kept invisible" : ""),
                             true, 2});
        r = r3;
      }
    }
  }
  if (opt.quickCheck && opt.verifyOnly) {
    auto firstFinding = std::find_if(r.issues.begin(), r.issues.end(), [](const Issue& i) {
      return i.fixed && !issueIsNormalization(i.code);
    });
    if (firstFinding != r.issues.end()) r.issues = {*firstFinding};
  }
  if (!opt.pageRange.empty()) {
    filterRange(r.issues, opt.pageRange);
    filterRange(r.analysis, opt.pageRange);
    if (opt.verifyOnly && r.ok) {
      r.compliant = std::none_of(r.issues.begin(), r.issues.end(), [](const Issue& i) {
        return i.fixed && !issueIsNormalization(i.code);
      });
    }
  }
  r.analysis.insert(r.analysis.begin(), pre.begin(), pre.end());
  return r;
}

struct StepOutcome {
  int sev = 0;
  long long hits = 0;
  std::set<int> pages;
};

void collectHits(const Result& r, StepOutcome& so) {
  for (const Issue& i : r.analysis) {
    if (!isHit(i)) continue;
    so.sev = std::max(so.sev, i.severity ? std::min(i.severity, 3) : 3);
    so.hits += hitCount(i);
    so.pages.insert(i.pages.begin(), i.pages.end());
  }
}

void setKey(Json& obj, const std::string& key, const Json& val) {
  for (auto& kv : obj.obj) {
    if (kv.first == key) {
      kv.second = val;
      return;
    }
  }
  obj.obj.push_back({key, val});
}

Json withoutKeys(const Json& root, const std::set<std::string>& drop) {
  Json out;
  out.type = Json::kObj;
  for (const auto& kv : root.obj) {
    if (!drop.count(kv.first)) out.obj.push_back(kv);
  }
  return out;
}

bool silenceChecks(Json& prof, const std::string& keep) {
  Json yes;
  yes.type = Json::kBool;
  yes.b = true;
  bool found = false;
  for (auto& kv : prof.obj) {
    if (kv.first != "checks") continue;
    for (Json& c : kv.second.arr) {
      const Json* n = c.get("name");
      if (n && !keep.empty() && n->text() == keep) found = true;
      else if (c.type == Json::kObj) setKey(c, "silent", yes);
    }
  }
  return found;
}

bool hasEntries(const Json& prof, const char* key) {
  const Json* j = prof.get(key);
  return j && ((j->type == Json::kArr && !j->arr.empty()) || j->type == Json::kObj);
}

bool readProfileFile(const std::string& path, const Options& opt, std::string& text) {
  std::string full = path;
  size_t slash = opt.profilePath.find_last_of("/\\");
  if (!path.empty() && path[0] != '/' && path.find(':') == std::string::npos && slash != std::string::npos) {
    full = opt.profilePath.substr(0, slash + 1) + path;
  }
  std::ifstream f(full, std::ios::binary);
  if (!f) return false;
  text.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return true;
}

bool truthy(const std::string& v) {
  return !(v.empty() || v == "0" || v == "false" || v == "undefined" || v == "null" || v == "NaN");
}

std::string resultsJson(const std::vector<std::string>& results) {
  std::string o = "[";
  for (size_t i = 0; i < results.size(); ++i) o += (i ? "," : "") + results[i];
  return o + "]";
}

Result runPlan(const unsigned char* data, std::size_t size, const Options& optIn, const Json& raw) {
  Options opt = optIn;
  Result out;
  out.ok = true;
  out.compliant = true;
  std::vector<ProfileVariable> defs = listProfileVariables(raw);
  const Json& steps = *raw.get("plan")->get("steps");
  std::set<std::string> stepVars;
  for (const Json& st : steps.arr) {
    const Json* d = st.get("do");
    const Json* v = st.get("variable");
    if (d && d->text() == "variable" && v) stepVars.insert(v->text());
  }
  std::vector<ProfileVariable> initial = defs;
  for (ProfileVariable& v : initial) {
    if (stepVars.count(v.name)) v.script.clear();
  }
  std::vector<std::string> notes;
  VarMap vars = resolveVariables(initial, docJsonOf(data, size, opt), opt.profileVars, notes);
  for (const std::string& n : notes) out.analysis.push_back({"VARIABLE_SET", n, false, n.rfind("variable ", 0) == 0 ? 2 : 1});
  {
    Json top = raw;
    substituteVariables(top, vars);
    applyConvert(top, opt, out.analysis);
  }
  out.level = levelToString(opt.level);
  std::vector<unsigned char> cur(data, data + size);
  std::vector<std::string> results;
  bool converted = false;
  int n = static_cast<int>(steps.arr.size());
  auto stepId = [&](int i) {
    const Json* id = steps.arr[i].get("id");
    return id ? substituteVariables(id->text(), vars) : "step" + std::to_string(i + 1);
  };
  int i = 0, guard = 0;
  while (i >= 0 && i < n && guard++ < 256) {
    const Json& st = steps.arr[i];
    auto field = [&](const char* k) {
      const Json* f = st.get(k);
      return f ? substituteVariables(f->text(), vars) : std::string();
    };
    const Json* en = st.get("enabled");
    if (en && ((en->type == Json::kBool && !en->b) || substituteVariables(en->text(), vars) == "false")) {
      ++i;
      continue;
    }
    std::string id = stepId(i), kind = field("do"), what, failure;
    StepOutcome so;
    if (kind == "revert") {
      cur.assign(data, data + size);
      what = "revert to the original file";
    } else if (kind == "variable") {
      std::string name = field("variable");
      auto def = std::find_if(defs.begin(), defs.end(), [&](const ProfileVariable& v) { return v.name == name; });
      std::string val = vars.count(name) ? vars[name] : std::string();
      if (def == defs.end()) {
        failure = "variable " + name + " is not defined";
      } else if (!def->script.empty()) {
        VarMap js = vars;
        js[name] = std::string();
        std::string res, err;
        if (evalScript(def->script, docJsonOf(cur.data(), cur.size(), opt), js, res, err, resultsJson(results))) {
          val = res.empty() ? js[name] : res;
          vars[name] = val;
        } else {
          failure = "variable " + name + ": " + err;
        }
      }
      if (failure.empty()) {
        what = "variable " + name + " = " + val;
        out.analysis.push_back({"VARIABLE_SET", "[" + id + "] " + name + " = " + val, false, 1});
        if (!truthy(val)) so.sev = 3;
      }
    } else if (kind == "check" || kind == "fix" || kind == "profile") {
      Json sub;
      bool converts = false;
      if (kind == "check") {
        std::string name = field("check");
        sub = withoutKeys(raw, {"plan", "fixes", "convert"});
        if (!silenceChecks(sub, name)) failure = "check " + name + " is not in the profile";
        what = "check " + name;
      } else if (kind == "fix") {
        const Json* f = st.get("fix");
        Json fx;
        if (f && f->type == Json::kObj) {
          fx = *f;
        } else if (f) {
          std::string name = substituteVariables(f->text(), vars);
          const Json* all = raw.get("fixes");
          for (const Json& c : all ? all->arr : std::vector<Json>{}) {
            const Json* cid = c.get("fix");
            if (cid && cid->text() == name && fx.type == Json::kNull) fx = c;
          }
        }
        const Json* fid = fx.get("fix");
        what = "fix " + (fid ? fid->text() : std::string("?"));
        if (fx.type != Json::kObj) failure = "fix " + (f ? f->text() : std::string()) + " is not in the profile";
        sub = withoutKeys(raw, {"plan", "fixes", "convert"});
        silenceChecks(sub, std::string());
        Json list;
        list.type = Json::kArr;
        list.arr.push_back(fx);
        setKey(sub, "fixes", list);
        converts = true;
      } else {
        const Json* p = st.get("profile");
        std::string text, err;
        if (p && p->type == Json::kObj) {
          sub = *p;
          what = "profile (inline)";
        } else {
          std::string path = field("profile");
          what = "profile " + path;
          if (!readProfileFile(path, opt, text)) failure = "cannot read profile " + path;
          else if (!parseJson(text, sub, err) || sub.type != Json::kObj) failure = "profile " + path + ": " + err;
        }
        converts = hasEntries(sub, "fixes") || hasEntries(sub, "convert");
      }
      if (failure.empty()) {
        Options o = opt;
        o.preflightProfile = jsonText(sub);
        o.profileVars = vars;
        o.verifyOnly = opt.verifyOnly || !converts;
        trace(opt, "plan step " + id + " " + what);
        Result r = runPrepared(cur.data(), cur.size(), o);
        for (Issue is : r.analysis) {
          if (is.code == "VARIABLE_SET") continue;
          is.detail = "[" + id + "] " + is.detail;
          out.analysis.push_back(is);
        }
        if (!r.ok) {
          so.sev = 3;
          out.issues.push_back({"PLAN_STEP_FAILED", "[" + id + "] " + what + ": " + r.error, true, 3});
        } else if (!o.verifyOnly) {
          out.issues.insert(out.issues.end(), r.issues.begin(), r.issues.end());
          if (kind == "profile" && hasEntries(sub, "checks")) {
            Options v = o;
            v.verifyOnly = true;
            v.levelFromProfile = false;
            Level lv;
            if (levelFromString(r.level, lv)) v.level = lv;
            Result after = runPrepared(r.pdf.data(), r.pdf.size(), v);
            if (after.ok) collectHits(after, so);
            else so.sev = 3;
          }
          cur = r.pdf;
          converted = true;
          out.level = r.level;
        } else {
          collectHits(r, so);
          if (!r.compliant) out.compliant = false;
        }
      }
    } else {
      failure = "unknown step kind \"" + kind + "\"";
    }
    if (!failure.empty()) {
      so.sev = 3;
      what = failure;
      out.issues.push_back({"PLAN_STEP_FAILED", "[" + id + "] " + failure, true, 3});
    }
    std::string pages;
    for (int p : so.pages) pages += (pages.empty() ? "" : ",") + std::to_string(p);
    results.push_back("{\"step\":\"" + id + "\",\"hits\":" + std::to_string(so.hits) + ",\"pages\":[" + pages + "]}");
    static const char* kResult[] = {"pass", "info", "warning", "error"};
    std::string key = kResult[std::clamp(so.sev, 0, 3)];
    const Json* nx = st.get("next");
    const Json* target = nx ? nx->get(key) : nullptr;
    if (!target && nx) target = nx->get("default");
    std::string next = target ? substituteVariables(target->text(), vars) : std::string();
    int ni = i + 1;
    if (next == "end") {
      ni = -1;
    } else if (!next.empty()) {
      ni = -2;
      for (int k = 0; k < n; ++k) {
        if (stepId(k) == next) ni = k;
      }
      if (ni == -2) {
        out.issues.push_back({"PLAN_STEP_FAILED", "[" + id + "] next step \"" + next + "\" does not exist", true, 3});
        ni = -1;
      }
    }
    std::string nextName = ni >= 0 && ni < n ? stepId(ni) : std::string("end");
    out.analysis.push_back({"PLAN_STEP", "step " + id + ": " + what + " -> " + key + ", next " + nextName, false, 1});
    i = ni;
  }
  if (!opt.verifyOnly) out.pdf = cur;
  if (!converted && !opt.verifyOnly) {
    out.issues.push_back({"PLAN_NO_CONVERSION", "the plan ran no converting step; output equals input", false, 1});
  }
  return out;
}

QPDFObjectHandle outlineItem(QPDF& pdf, const std::string& title, QPDFObjectHandle page) {
  QPDFObjectHandle d = QPDFObjectHandle::newDictionary();
  d.replaceKey("/Title", QPDFObjectHandle::newUnicodeString(title));
  QPDFObjectHandle dest = QPDFObjectHandle::newArray();
  dest.appendItem(page);
  dest.appendItem(QPDFObjectHandle::newName("/Fit"));
  d.replaceKey("/Dest", dest);
  return pdf.makeIndirectObject(d);
}

struct OutlineNode {
  std::string title;
  int page = 0;
  int level = 1;
  std::vector<OutlineNode> kids;
};

int linkOutline(QPDF& pdf, QPDFObjectHandle parent, std::vector<OutlineNode>& nodes,
                const std::vector<QPDFPageObjectHelper>& pages) {
  QPDFObjectHandle prev;
  int count = 0;
  for (size_t i = 0; i < nodes.size(); ++i) {
    int pi = std::clamp(nodes[i].page, 1, static_cast<int>(pages.size())) - 1;
    QPDFObjectHandle it = outlineItem(pdf, nodes[i].title, pages[pi].getObjectHandle());
    it.replaceKey("/Parent", parent);
    if (i == 0) parent.replaceKey("/First", it);
    if (prev.isInitialized()) {
      prev.replaceKey("/Next", it);
      it.replaceKey("/Prev", prev);
    }
    parent.replaceKey("/Last", it);
    int sub = linkOutline(pdf, it, nodes[i].kids, pages);
    if (sub) it.replaceKey("/Count", QPDFObjectHandle::newInteger(-sub));
    prev = it;
    count += 1;
  }
  return count;
}

void nest(std::vector<OutlineNode>& flat, std::vector<OutlineNode>& out) {
  std::vector<std::vector<OutlineNode>*> stack{&out};
  std::vector<int> levels{0};
  for (OutlineNode& n : flat) {
    while (levels.size() > 1 && levels.back() >= n.level) {
      levels.pop_back();
      stack.pop_back();
    }
    stack.back()->push_back(n);
    stack.push_back(&stack.back()->back().kids);
    levels.push_back(n.level);
  }
}

void jsonBookmarks(const JSON& arr, int level, std::vector<OutlineNode>& flat) {
  arr.forEachArrayItem([&](JSON item) {
    OutlineNode n;
    item.getDictItem("title").getString(n.title);
    std::string pg;
    if (item.getDictItem("page").getNumber(pg)) n.page = std::atoi(pg.c_str());
    n.level = level;
    flat.push_back(n);
    jsonBookmarks(item.getDictItem("children"), level + 1, flat);
  });
}

void collectHeadings(QPDFObjectHandle elem, QPDFObjectHandle page, std::map<QPDFObjGen, int>& pageIdx,
                     std::vector<QPDFPageObjectHelper>& pages, std::map<int, std::vector<TextRun>>& cache,
                     std::vector<OutlineNode>& flat, int depth);

void gatherMcids(QPDFObjectHandle k, QPDFObjectHandle page, std::vector<std::pair<QPDFObjGen, int>>& out, int depth) {
  if (depth > 64) return;
  if (k.isInteger()) {
    if (page.isDictionary()) out.push_back({page.getObjGen(), static_cast<int>(k.getIntValue())});
  } else if (k.isArray()) {
    for (auto& x : k.aitems()) gatherMcids(x, page, out, depth + 1);
  } else if (k.isDictionary()) {
    QPDFObjectHandle pg = k.getKey("/Pg").isDictionary() ? k.getKey("/Pg") : page;
    if (k.getKey("/MCID").isInteger()) {
      out.push_back({pg.getObjGen(), static_cast<int>(k.getKey("/MCID").getIntValue())});
    } else {
      gatherMcids(k.getKey("/K"), pg, out, depth + 1);
    }
  }
}

void collectHeadings(QPDFObjectHandle elem, QPDFObjectHandle page, std::map<QPDFObjGen, int>& pageIdx,
                     std::vector<QPDFPageObjectHelper>& pages, std::map<int, std::vector<TextRun>>& cache,
                     std::vector<OutlineNode>& flat, int depth) {
  if (depth > 64 || !elem.isDictionary()) return;
  if (elem.getKey("/Pg").isDictionary()) page = elem.getKey("/Pg");
  std::string s = elem.getKey("/S").isName() ? elem.getKey("/S").getName() : "";
  if (s.size() == 3 && s[1] == 'H' && s[2] >= '1' && s[2] <= '6') {
    std::vector<std::pair<QPDFObjGen, int>> ids;
    gatherMcids(elem.getKey("/K"), page, ids, 0);
    std::string text;
    int first = 0;
    for (auto& [og, mcid] : ids) {
      auto pi = pageIdx.find(og);
      if (pi == pageIdx.end()) continue;
      if (!first) first = pi->second + 1;
      if (!cache.count(pi->second)) cache[pi->second] = extractPageText(pages[pi->second]);
      for (const TextRun& r : cache[pi->second]) {
        if (r.mcid == mcid) text += r.text;
      }
    }
    if (text.empty() && elem.getKey("/T").isString()) text = elem.getKey("/T").getUTF8Value();
    if (text.empty() && elem.getKey("/Alt").isString()) text = elem.getKey("/Alt").getUTF8Value();
    text = trimmed(text);
    if (!text.empty() && first) flat.push_back({text, first, s[2] - '0', {}});
    return;
  }
  QPDFObjectHandle k = elem.getKey("/K");
  if (k.isArray()) {
    for (auto& x : k.aitems()) collectHeadings(x, page, pageIdx, pages, cache, flat, depth + 1);
  } else {
    collectHeadings(k, page, pageIdx, pages, cache, flat, depth + 1);
  }
}

thread_local int jobDepth = 0;
}

void passBookmarks(Ctx& ctx) {
  const std::string& spec = ctx.opt.bookmarks;
  if (spec.empty()) return;
  QPDFPageDocumentHelper dh(ctx.pdf);
  std::vector<QPDFPageObjectHelper> pages = dh.getAllPages();
  if (pages.empty()) return;
  std::vector<OutlineNode> flat;
  if (spec == "headings") {
    std::map<QPDFObjGen, int> pageIdx;
    for (size_t i = 0; i < pages.size(); ++i) pageIdx[pages[i].getObjectHandle().getObjGen()] = static_cast<int>(i);
    std::map<int, std::vector<TextRun>> cache;
    collectHeadings(ctx.pdf.getRoot().getKey("/StructTreeRoot"), QPDFObjectHandle(), pageIdx, pages, cache, flat, 0);
    if (flat.empty()) {
      ctx.issue("BOOKMARKS_NOT_CREATED", "no tagged headings found to build bookmarks from", false);
      return;
    }
  } else {
    try {
      JSON j = JSON::parse(spec);
      jsonBookmarks(j.isArray() ? j : j.getDictItem("bookmarks"), 1, flat);
    } catch (...) {
      ctx.issue("BOOKMARKS_NOT_CREATED", "bookmark JSON could not be read", false);
      return;
    }
  }
  std::vector<OutlineNode> tree;
  nest(flat, tree);
  QPDFObjectHandle outlines = ctx.pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
  outlines.replaceKey("/Type", QPDFObjectHandle::newName("/Outlines"));
  int n = linkOutline(ctx.pdf, outlines, tree, pages);
  outlines.replaceKey("/Count", QPDFObjectHandle::newInteger(n));
  ctx.pdf.getRoot().replaceKey("/Outlines", outlines);
  ctx.issue("BOOKMARKS_ADDED", "added " + std::to_string(flat.size()) + " bookmark(s)", true);
}

Result convert(const unsigned char* data, std::size_t size, const Options& opt) {
  if (jobDepth > 0) return convertOnce(data, size, opt);
  ++jobDepth;
  Result r;
  try {
    Json root;
    std::string err;
    const Json* plan = nullptr;
    if (opt.preflightProfile.find("\"plan\"") != std::string::npos && parseJson(opt.preflightProfile, root, err) &&
        root.type == Json::kObj && (plan = root.get("plan")) && plan->get("steps") && plan->get("steps")->type == Json::kArr) {
      r = runPlan(data, size, opt, root);
    } else {
      r = runPrepared(data, size, opt);
    }
  } catch (...) {
    --jobDepth;
    throw;
  }
  --jobDepth;
  return r;
}
}
