#include "profile_vars.hh"

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef KURA_WITH_QUICKJS
#include <quickjs/quickjs.h>
#endif

namespace pdfa {
namespace {
std::string jsonStr(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += static_cast<char>(c);
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

bool numeric(const std::string& v) {
  if (v.empty() || v.find_first_of("xXnNiI") != std::string::npos) return false;
  char* end = nullptr;
  std::strtod(v.c_str(), &end);
  return end && *end == 0;
}

std::string jsonValue(const std::string& v) {
  if (v == "true" || v == "false" || numeric(v)) return v;
  return jsonStr(v);
}

#ifdef KURA_WITH_QUICKJS
const char* kPrelude = R"js(
kura.variable = function (name, dflt) {
  name = String(name);
  var v = kura.variables[name];
  if (v === undefined || v === null || v === '') {
    if (dflt !== undefined) kura.variables[name] = dflt;
    return dflt;
  }
  return v;
};
if (typeof Intl === 'undefined') {
  var Intl = { DateTimeFormat: function (l, o) {
    o = o || {};
    return { format: function (d) {
      d = d || new Date();
      var p = function (n) { return (n < 10 ? '0' : '') + n; };
      var parts = [];
      if (o.year) parts.push(String(d.getFullYear()));
      if (o.month) parts.push(o.month === '2-digit' ? p(d.getMonth() + 1) : String(d.getMonth() + 1));
      if (o.day) parts.push(o.day === '2-digit' ? p(d.getDate()) : String(d.getDate()));
      return parts.length ? parts.join('-') : d.toISOString().substr(0, 10);
    } };
  } };
}
)js";

const char* kDump = R"js((function () {
  var o = [];
  for (var k in kura.variables) {
    var v = kura.variables[k];
    if (typeof v === 'object' && v !== null) v = JSON.stringify(v);
    o.push(k + '\u0001' + String(v));
  }
  return o.join('\u0002');
})())js";

int interruptAt(JSRuntime*, void* opaque) {
  auto deadline = static_cast<std::chrono::steady_clock::time_point*>(opaque);
  return std::chrono::steady_clock::now() > *deadline ? 1 : 0;
}

std::string toStr(JSContext* ctx, JSValueConst v) {
  const char* s = JS_ToCString(ctx, v);
  std::string out = s ? s : "";
  if (s) JS_FreeCString(ctx, s);
  return out;
}
#endif

bool parseProfile(const std::string& text, Json& root) {
  std::string err;
  return parseJson(text, root, err) && root.type == Json::kObj;
}

void substituteNode(Json& j, const VarMap& values) {
  if (j.type == Json::kStr) {
    const std::string& s = j.str;
    if (s.size() > 2 && s.front() == '{' && s.back() == '}') {
      auto it = values.find(s.substr(1, s.size() - 2));
      if (it != values.end()) {
        const std::string& v = it->second;
        if (v == "true" || v == "false") {
          j.type = Json::kBool;
          j.b = v == "true";
          return;
        }
        if (numeric(v)) {
          j.type = Json::kNum;
          j.num = std::strtod(v.c_str(), nullptr);
          return;
        }
      }
    }
    j.str = substituteVariables(j.str, values);
  } else if (j.type == Json::kArr) {
    for (Json& x : j.arr) substituteNode(x, values);
  } else if (j.type == Json::kObj) {
    for (auto& kv : j.obj) substituteNode(kv.second, values);
  }
}
}

std::vector<ProfileVariable> listProfileVariables(const Json& profile) {
  std::vector<ProfileVariable> out;
  const Json* vars = profile.get("variables");
  if (!vars || vars->type != Json::kArr) return out;
  for (const Json& v : vars->arr) {
    if (v.type != Json::kObj) continue;
    ProfileVariable pv;
    auto field = [&](const char* k) {
      const Json* f = v.get(k);
      return f ? f->text() : std::string();
    };
    pv.name = field("name");
    pv.label = field("label");
    pv.type = field("type");
    pv.defaultValue = field("default");
    pv.script = field("script");
    if (const Json* c = v.get("choices")) {
      for (const Json& x : c->arr) pv.choices.push_back(x.text());
    }
    if (!pv.name.empty()) out.push_back(pv);
  }
  return out;
}

std::vector<ProfileVariable> listProfileVariables(const std::string& profileJson) {
  Json root;
  if (!parseProfile(profileJson, root)) return {};
  return listProfileVariables(root);
}

VarMap profileVariableDefaults(const std::string& profileJson, const VarMap& overrides) {
  VarMap out;
  for (const ProfileVariable& v : listProfileVariables(profileJson)) out[v.name] = v.defaultValue;
  for (const auto& kv : overrides) out[kv.first] = kv.second;
  return out;
}

bool scriptEngineAvailable() {
#ifdef KURA_WITH_QUICKJS
  return true;
#else
  return false;
#endif
}

std::string docFactsJson(QPDF& pdf, const std::string& name) {
  QPDFPageDocumentHelper dh(pdf);
  auto pages = dh.getAllPages();
  std::string j = "{\"name\":" + jsonStr(name) + ",\"pageCount\":" + std::to_string(pages.size()) + ",\"pages\":[";
  auto boxJson = [](QPDFObjectHandle b) {
    double v[4] = {0, 0, 612, 792};
    if (b.isArray() && b.getArrayNItems() == 4) {
      for (int i = 0; i < 4; ++i) {
        QPDFObjectHandle n = b.getArrayItem(i);
        if (n.isNumber()) v[i] = n.getNumericValue();
      }
    }
    char buf[160];
    std::snprintf(buf, sizeof buf, "{\"left\":%.4f,\"bottom\":%.4f,\"right\":%.4f,\"top\":%.4f}",
                  std::min(v[0], v[2]), std::min(v[1], v[3]), std::max(v[0], v[2]), std::max(v[1], v[3]));
    return std::string(buf);
  };
  for (size_t i = 0; i < pages.size(); ++i) {
    auto& ph = pages[i];
    QPDFObjectHandle media = ph.getAttribute("/MediaBox", false);
    QPDFObjectHandle crop = ph.getAttribute("/CropBox", false);
    if (!crop.isArray()) crop = media;
    auto boxOr = [&](const char* k) {
      QPDFObjectHandle b = ph.getAttribute(k, false);
      return b.isArray() ? b : crop;
    };
    QPDFObjectHandle uu = ph.getObjectHandle().getKey("/UserUnit");
    j += (i ? "," : "") + std::string("{\"number\":") + std::to_string(i + 1) +
         ",\"mediaBox\":" + boxJson(media) + ",\"cropBox\":" + boxJson(crop) +
         ",\"bleedBox\":" + boxJson(boxOr("/BleedBox")) + ",\"trimBox\":" + boxJson(boxOr("/TrimBox")) +
         ",\"artBox\":" + boxJson(boxOr("/ArtBox")) +
         ",\"userUnit\":" + (uu.isNumber() ? std::to_string(uu.getNumericValue()) : "1") + "}";
  }
  return j + "]}";
}

bool evalScript(const std::string& script, const std::string& docJson, VarMap& vars, std::string& result,
                std::string& err, const std::string& resultsJson) {
#ifdef KURA_WITH_QUICKJS
  JSRuntime* rt = JS_NewRuntime();
  if (!rt) {
    err = "script engine unavailable";
    return false;
  }
  JS_SetMemoryLimit(rt, 64u << 20);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  JS_SetInterruptHandler(rt, interruptAt, &deadline);
  JSContext* ctx = JS_NewContext(rt);
  std::string varsJson = "{";
  for (const auto& kv : vars) varsJson += (varsJson.size() > 1 ? "," : "") + jsonStr(kv.first) + ":" + jsonValue(kv.second);
  varsJson += "}";
  std::string setup = "var kura = {variables:" + varsJson + ",document:" + (docJson.empty() ? "{}" : docJson) +
                      ",results:" + (resultsJson.empty() ? "[]" : resultsJson) + "};" + kPrelude;
  bool ok = true;
  JSValue r = JS_Eval(ctx, setup.c_str(), setup.size(), "<setup>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(r)) ok = false;
  JS_FreeValue(ctx, r);
  if (ok) {
    r = JS_Eval(ctx, script.c_str(), script.size(), "<variable>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
      ok = false;
    } else {
      result = JS_IsUndefined(r) ? std::string() : toStr(ctx, r);
    }
    JS_FreeValue(ctx, r);
  }
  if (!ok) {
    JSValue ex = JS_GetException(ctx);
    err = toStr(ctx, ex);
    JS_FreeValue(ctx, ex);
  } else {
    r = JS_Eval(ctx, kDump, std::strlen(kDump), "<dump>", JS_EVAL_TYPE_GLOBAL);
    std::string dump = JS_IsException(r) ? std::string() : toStr(ctx, r);
    JS_FreeValue(ctx, r);
    size_t p = 0;
    while (p < dump.size()) {
      size_t e = dump.find('\x02', p);
      std::string item = dump.substr(p, e == std::string::npos ? std::string::npos : e - p);
      size_t s = item.find('\x01');
      if (s != std::string::npos) vars[item.substr(0, s)] = item.substr(s + 1);
      if (e == std::string::npos) break;
      p = e + 1;
    }
  }
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return ok;
#else
  (void)script;
  (void)docJson;
  (void)vars;
  (void)result;
  (void)resultsJson;
  err = "this build has no script engine";
  return false;
#endif
}

VarMap resolveVariables(const std::vector<ProfileVariable>& defs, const std::string& docJson,
                        const VarMap& overrides, std::vector<std::string>& notes, const std::string& resultsJson) {
  VarMap values = overrides;
  for (const ProfileVariable& v : defs) {
    if (!values.count(v.name)) values[v.name] = v.defaultValue;
  }
  for (const ProfileVariable& v : defs) {
    auto ov = overrides.find(v.name);
    if (ov != overrides.end()) {
      notes.push_back(v.name + " = " + ov->second);
    } else if (!v.script.empty()) {
      VarMap js = values;
      js[v.name] = std::string();
      std::string res, err;
      if (evalScript(v.script, docJson, js, res, err, resultsJson)) {
        if (res.empty()) res = js[v.name];
        values[v.name] = res;
        notes.push_back(v.name + " = " + res);
      } else {
        notes.push_back("variable " + v.name + " not evaluated, default used: " + err);
      }
    }
    if (!v.choices.empty() &&
        std::find(v.choices.begin(), v.choices.end(), values[v.name]) == v.choices.end()) {
      notes.push_back("variable " + v.name + ": \"" + values[v.name] + "\" is not one of its choices");
    }
  }
  return values;
}

std::string substituteVariables(const std::string& text, const VarMap& values) {
  if (text.find('{') == std::string::npos) return text;
  std::string out;
  size_t p = 0;
  while (p < text.size()) {
    size_t o = text.find('{', p);
    size_t c = o == std::string::npos ? o : text.find('}', o + 1);
    if (c == std::string::npos) break;
    auto it = values.find(text.substr(o + 1, c - o - 1));
    if (it == values.end()) {
      out.append(text, p, o + 1 - p);
      p = o + 1;
      continue;
    }
    out.append(text, p, o - p);
    out += it->second;
    p = c + 1;
  }
  out.append(text, p, std::string::npos);
  return out;
}

void substituteVariables(Json& profile, const VarMap& values) {
  for (auto& kv : profile.obj) {
    if (kv.first != "variables") substituteNode(kv.second, values);
  }
}

std::string jsonText(const Json& j) {
  switch (j.type) {
    case Json::kNull: return "null";
    case Json::kBool:
    case Json::kNum: return j.text();
    case Json::kStr: return jsonStr(j.str);
    case Json::kArr: {
      std::string o = "[";
      for (size_t i = 0; i < j.arr.size(); ++i) o += (i ? "," : "") + jsonText(j.arr[i]);
      return o + "]";
    }
    case Json::kObj: {
      std::string o = "{";
      for (size_t i = 0; i < j.obj.size(); ++i) o += (i ? "," : "") + jsonStr(j.obj[i].first) + ":" + jsonText(j.obj[i].second);
      return o + "}";
    }
  }
  return "null";
}

}
