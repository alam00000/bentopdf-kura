#include "htmlshim.hh"

#include <qpdf/QPDFPageDocumentHelper.hh>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>

#include "profile_events.hh"
#include "profile_vars.hh"
#include "util.hh"

namespace pdfa {
namespace {
std::string js(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += static_cast<char>(c);
    } else if (c < 0x20 || c == '<' || c == '>' || c == '&') {
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
  char b[32];
  std::snprintf(b, sizeof b, "%.4f", v);
  std::string s = b;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s == "-0" ? "0" : s;
}

std::string box(QPDFObjectHandle b) {
  if (!b.isArray() || b.getArrayNItems() != 4) return "null";
  double v[4];
  for (int i = 0; i < 4; ++i) v[i] = numOf(b.getArrayItem(i), 0);
  return "{\"left\":" + num(std::min(v[0], v[2])) + ",\"bottom\":" + num(std::min(v[1], v[3])) + ",\"right\":" +
         num(std::max(v[0], v[2])) + ",\"top\":" + num(std::max(v[1], v[3])) + "}";
}

void inksOf(QPDFObjectHandle res, std::set<std::string>& out, int depth) {
  if (!res.isDictionary() || depth > 4) return;
  QPDFObjectHandle cs = res.getKey("/ColorSpace");
  if (cs.isDictionary()) {
    for (auto& k : cs.getKeys()) {
      ColorInfo ci = classifyColor(cs.getKey(k), res);
      if (ci.cls == "cmyk") out.insert({"Cyan", "Magenta", "Yellow", "Black"});
      if (!ci.spot.empty() && ci.spot != "None" && ci.spot != "All") out.insert(ci.spot);
      for (auto& c : ci.colorants) {
        if (c != "None" && c != "All") out.insert(c);
      }
    }
  }
  QPDFObjectHandle xo = res.getKey("/XObject");
  if (xo.isDictionary()) {
    for (auto& k : xo.getKeys()) {
      QPDFObjectHandle x = xo.getKey(k);
      if (x.isStream()) inksOf(x.getDict().getKey("/Resources"), out, depth + 1);
    }
  }
}

const char* kStamp = R"JS((function () {
  var data = __KURA_DATA__;
  var snaps = [], done = false, current = null;
  var units = { pt: 1, px: 0.75, mm: 72 / 25.4, cm: 72 / 2.54, in: 72, pc: 12 };
  function len(v, u) { return parseFloat(v) * (units[(u || 'pt').toLowerCase()] || 1); }
  function geometry() {
    var css = '', g = { w: 0, h: 0, crop: null };
    var st = document.querySelectorAll('style');
    for (var i = 0; i < st.length; i++) css += st[i].textContent + '\n';
    var n = '(-?[\\d.]+)\\s*(pt|px|mm|cm|in|pc)?';
    var sizeRx = new RegExp('(?:^|[;\\s{])size\\s*:\\s*' + n + '\\s+' + n);
    var cropRx = new RegExp('-[a-z]+-cropbox\\s*:\\s*' + n + '\\s+' + n + '\\s+' + n + '\\s+' + n);
    (css.match(/@page\s*\{[^}]*\}/g) || []).forEach(function (rule) {
      var s = rule.match(sizeRx), c = rule.match(cropRx);
      if (s) { g.w = len(s[1], s[2]); g.h = len(s[3], s[4]); g.crop = null; }
      if (c) g.crop = [len(c[1], c[2]), len(c[3], c[4]), len(c[5], c[6]), len(c[7], c[8])];
    });
    if (!g.w || !g.h) {
      var p = current || (data.pages && data.pages[0]);
      g.w = p ? p.mediaBox.right - p.mediaBox.left : 595.28;
      g.h = p ? p.mediaBox.top - p.mediaBox.bottom : 841.89;
    }
    return g;
  }
  var C128 = ['212222','222122','222221','121223','121322','131222','122213','122312','132212','221213','221312','231212','112232','122132','122231','113222','123122','123221','223211','221132','221231','213212','223112','312131','311222','321122','321221','312212','322112','322211','212123','212321','232121','111323','131123','131321','112313','132113','132311','211313','231113','231311','112133','112331','132131','113123','113321','133121','313121','211331','231131','213113','213311','213131','311123','311321','331121','312113','312311','332111','314111','221411','431111','111224','111422','121124','121421','141122','141221','112214','112412','122114','122411','142112','142211','241211','221114','413111','241112','134111','111242','121142','121241','114212','124112','124211','411212','421112','421211','212141','214121','412121','111143','111341','131141','114113','114311','411113','411311','113141','114131','311141','411131','211412','211214','211232','2331112'];
  function code128(text) {
    var v = [], i;
    if (/^(\d\d)+$/.test(text)) {
      v.push(105);
      for (i = 0; i < text.length; i += 2) v.push(parseInt(text.substr(i, 2), 10));
    } else {
      v.push(104);
      for (i = 0; i < text.length; i++) {
        var c = text.charCodeAt(i);
        if (c < 32 || c > 127) return null;
        v.push(c - 32);
      }
    }
    var sum = v[0];
    for (i = 1; i < v.length; i++) sum += v[i] * i;
    v.push(sum % 103, 106);
    var bars = [], x = 0, dark = true;
    v.forEach(function (k) {
      C128[k].split('').forEach(function (w) { if (dark) bars.push([x, +w]); x += +w; dark = !dark; });
    });
    return { bars: bars, width: x };
  }
  var NS = 'http://www.w3.org/2000/svg';
  function svgEl(tag, attrs) {
    var e = document.createElementNS(NS, tag);
    for (var k in attrs) e.setAttribute(k, attrs[k]);
    return e;
  }
  function barcodeSvg(type, text) {
    var enc = /128/.test(type || '128') ? code128(text || '') : null;
    var svg = svgEl('svg', { width: '100%', height: '100%', preserveAspectRatio: 'none' });
    if (!enc) {
      svg.setAttribute('viewBox', '0 0 100 20');
      var t0 = svgEl('text', { x: 50, y: 14, 'text-anchor': 'middle', 'font-size': 10, fill: 'currentColor' });
      t0.textContent = text || '';
      svg.appendChild(t0);
      return svg;
    }
    var q = 10, w = enc.width + 2 * q;
    svg.setAttribute('viewBox', '0 0 ' + w + ' 100');
    enc.bars.forEach(function (b) {
      svg.appendChild(svgEl('rect', { x: b[0] + q, y: 0, width: b[1], height: 78, fill: 'currentColor' }));
    });
    var t = svgEl('text', { x: w / 2, y: 97, 'text-anchor': 'middle', 'font-size': 18, fill: 'currentColor' });
    t.textContent = text;
    svg.appendChild(t);
    return svg;
  }
  function param(el, name) {
    var p = el.querySelector('param[name="' + name + '"]');
    return p ? p.getAttribute('value') : null;
  }
  function freeze(root) {
    var objs = root.querySelectorAll('object[type="application/barcode"]');
    for (var i = 0; i < objs.length; i++) {
      var o = objs[i], d = document.createElement('div');
      for (var a = 0; a < o.attributes.length; a++) {
        if (o.attributes[a].name !== 'type') d.setAttribute(o.attributes[a].name, o.attributes[a].value);
      }
      d.appendChild(barcodeSvg(param(o, 'type'), param(o, 'data')));
      o.parentNode.replaceChild(d, o);
    }
    var scripts = root.querySelectorAll('script');
    for (var j = 0; j < scripts.length; j++) scripts[j].parentNode.removeChild(scripts[j]);
    return root;
  }
  function printPages(n) {
    var g = geometry();
    for (var i = 0; i < (n || 1); i++) snaps.push({ g: g, body: freeze(document.body.cloneNode(true)) });
  }
  function finish() {
    if (done) return;
    done = true;
    if (!snaps.length) return;
    var css = '@page { margin: 0 } html, body { margin: 0 !important; padding: 0 !important; }\n';
    var body = document.body;
    while (body.firstChild) body.removeChild(body.firstChild);
    snaps.forEach(function (s, i) {
      var g = s.g, x = 0, y = 0, w = g.w, h = g.h;
      if (g.crop) { x = g.crop[0]; w = g.crop[2]; h = g.crop[3]; y = g.h - g.crop[1] - h; }
      css += '@page kpage' + i + ' { size: ' + w + 'pt ' + h + 'pt; margin: 0 }\n';
      var sheet = document.createElement('div');
      sheet.style.cssText = 'page: kpage' + i + '; position: relative; overflow: hidden; width: ' + w + 'pt; height: ' + h + 'pt;' +
                            (i ? ' break-before: page;' : '');
      var inner = document.createElement('div');
      inner.style.cssText = 'position: absolute; left: ' + (-x) + 'pt; top: ' + (-y) + 'pt; width: ' + g.w + 'pt; height: ' + g.h + 'pt;';
      while (s.body.firstChild) inner.appendChild(s.body.firstChild);
      sheet.appendChild(inner);
      body.appendChild(sheet);
    });
    var st = document.createElement('style');
    st.textContent = css;
    document.head.appendChild(st);
  }
  function ready() {
    return (document.fonts && document.fonts.ready ? document.fonts.ready : Promise.resolve()).then(function () {
      return new Promise(function (r) { setTimeout(r, 0); });
    });
  }
  var callback = null;
  window.kura = {
    document: data.document,
    pages: data.pages,
    onPages: function (fn) { callback = fn; },
    printPages: printPages
  };
  window.addEventListener('load', function () {
    ready().then(function () {
      if (typeof callback !== 'function') return;
      var chain = Promise.resolve();
      data.pages.forEach(function (page, i) {
        chain = chain.then(function () { current = page; return callback(page, i); });
      });
      return chain.then(finish);
    });
  });
})();
)JS";
}

std::string htmlStampScript(Ctx& ctx) {
  const std::string& path = ctx.opt.inputName.empty() ? ctx.opt.inputPath : ctx.opt.inputName;
  size_t slash = path.find_last_of("/\\");
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  std::string varsJson;
  for (const auto& kv : profileVariableDefaults(ctx.opt.preflightProfile, ctx.opt.profileVars)) {
    varsJson += (varsJson.empty() ? "" : ",") + js(kv.first) + ":" + js(kv.second);
  }
  QPDFPageDocumentHelper dh(ctx.pdf);
  auto pages = dh.getAllPages();
  std::set<std::string> inks;
  std::string pj;
  int number = 0;
  for (auto& ph : pages) {
    inksOf(ph.getAttribute("/Resources", false), inks, 0);
    QPDFObjectHandle rot = ph.getAttribute("/Rotate", false);
    QPDFObjectHandle media = ph.getAttribute("/MediaBox", true);
    QPDFObjectHandle crop = ph.getAttribute("/CropBox", false);
    if (!crop.isArray()) crop = media;
    auto boxOr = [&](const char* key) {
      QPDFObjectHandle b = ph.getAttribute(key, false);
      return box(b.isArray() ? b : crop);
    };
    pj += (pj.empty() ? "" : ",") + std::string("{\"number\":") + std::to_string(++number) + ",\"mediaBox\":" + box(media) +
          ",\"cropBox\":" + box(crop) + ",\"trimBox\":" + boxOr("/TrimBox") + ",\"bleedBox\":" + boxOr("/BleedBox") +
          ",\"artBox\":" + boxOr("/ArtBox") + ",\"rotation\":" + std::to_string(rot.isInteger() ? rot.getIntValue() : 0) + "}";
  }
  std::string ij;
  for (const std::string& k : inks) ij += (ij.empty() ? "" : ",") + js(k);
  std::string data = "{\"document\":{\"name\":" + js(name) + ",\"pageCount\":" + std::to_string(pages.size()) +
                     ",\"variables\":{" + varsJson + "},\"inks\":[" + ij + "]},\"pages\":[" + pj + "]}";
  std::string script = kStamp;
  script.replace(script.find("__KURA_DATA__"), 13, data);
  return script;
}
}
