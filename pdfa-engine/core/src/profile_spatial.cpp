#include "profile_spatial.hh"

#include <qpdf/Pl_Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <lcms2.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "assets/cmyk_icc.hh"
#include "assets/srgb_icc.hh"
#include "check_registry.hh"
#include "images.hh"
#include "profile_eval.hh"
#include "profile_ext.hh"
#include "util.hh"

namespace pdfa {

std::vector<int> nearestSeed(const std::vector<unsigned char>& on, int w, int h) {
  std::vector<int> col(static_cast<size_t>(w) * h, -1), out(col.size(), -1);
  for (int x = 0; x < w; ++x) {
    int last = -1;
    for (int y = 0; y < h; ++y) {
      size_t i = static_cast<size_t>(y) * w + x;
      if (on[i]) last = y;
      col[i] = last;
    }
    last = -1;
    for (int y = h - 1; y >= 0; --y) {
      size_t i = static_cast<size_t>(y) * w + x;
      if (on[i]) last = y;
      if (last >= 0 && (col[i] < 0 || last - y < y - col[i])) col[i] = last;
    }
  }
  std::vector<int> v(static_cast<size_t>(w));
  std::vector<double> z(static_cast<size_t>(w) + 1), f(static_cast<size_t>(w));
  for (int y = 0; y < h; ++y) {
    size_t row = static_cast<size_t>(y) * w;
    int k = -1;
    for (int q = 0; q < w; ++q) {
      int cy = col[row + q];
      if (cy < 0) continue;
      f[q] = double(cy - y) * (cy - y);
      if (k < 0) {
        k = 0;
        v[0] = q;
        z[0] = -1e20;
        z[1] = 1e20;
        continue;
      }
      auto cut = [&](int a) { return ((f[q] + double(q) * q) - (f[a] + double(a) * a)) / (2.0 * q - 2.0 * a); };
      double sq = cut(v[k]);
      while (sq <= z[k]) sq = cut(v[--k]);
      v[++k] = q;
      z[k] = sq;
      z[k + 1] = 1e20;
    }
    if (k < 0) continue;
    for (int q = 0, j = 0; q < w; ++q) {
      while (z[j + 1] < q) ++j;
      out[row + q] = col[row + v[j]] * w + v[j];
    }
  }
  return out;
}

namespace {
Box inter(const Box& a, const Box& b) {
  if (!a.valid) return b;
  if (!b.valid) return a;
  Box r{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1), true};
  if (r.x1 < r.x0 || r.y1 < r.y0) r.valid = false;
  return r;
}

Box grow(const Box& b, double d) {
  Box r = b;
  r.x0 -= d;
  r.y0 -= d;
  r.x1 += d;
  r.y1 += d;
  if (r.x1 < r.x0 || r.y1 < r.y0) r.valid = false;
  return r;
}

bool touches(const Box& a, const Box& b) {
  return a.valid && b.valid && a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}

bool overlaps(const Box& a, const Box& b) {
  if (!a.valid || !b.valid) return false;
  double w = std::min(a.x1, b.x1) - std::max(a.x0, b.x0);
  double h = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
  if (w < 0 || h < 0) return false;
  bool aThin = a.x1 - a.x0 < 1e-6 || a.y1 - a.y0 < 1e-6;
  bool bThin = b.x1 - b.x0 < 1e-6 || b.y1 - b.y0 < 1e-6;
  return (w > 1e-6 && h > 1e-6) || aThin || bThin;
}

const Box& cropOf(const PageFacts& p) { return p.crop.valid ? p.crop : p.media; }

bool isBoxName(const std::string& n) { return n == "media" || n == "crop" || n == "bleed" || n == "trim" || n == "art"; }

const Box& boxOf(const PageFacts& p, const std::string& n) {
  if (n == "media") return p.media;
  if (n == "bleed") return p.bleed.valid ? p.bleed : cropOf(p);
  if (n == "art") return p.art.valid ? p.art : cropOf(p);
  if (n == "crop") return cropOf(p);
  return p.trim.valid ? p.trim : cropOf(p);
}

Box overlapOf(const Box& a, const Box& b) {
  if (!a.valid || !b.valid) return Box();
  Box r{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1), true};
  r.valid = r.x1 > r.x0 && r.y1 > r.y0;
  return r;
}

bool zLess(const std::vector<int>& a, const std::vector<int>& b) {
  return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
}

bool axisAligned(const Mat& m) {
  return (std::fabs(m.b) < 1e-6 && std::fabs(m.c) < 1e-6) || (std::fabs(m.a) < 1e-6 && std::fabs(m.d) < 1e-6);
}

bool whiteColor(const std::vector<double>& c, const ColorInfo& ci) {
  if (c.empty()) return false;
  double mx = 0, mn = 1;
  for (double v : c) {
    mx = std::max(mx, v);
    mn = std::min(mn, v);
  }
  if (ci.cls == "cmyk" || ci.cls == "separation" || ci.cls == "devicen") return mx < 0.001;
  if (ci.cls == "gray" || ci.cls == "rgb" || ci.cls == "cal") return mn > 0.999;
  if (ci.cls == "icc") return ci.declaredComps == 4 ? mx < 0.001 : mn > 0.999;
  return false;
}

bool normalBlend(const GsExtra& x, double alpha) {
  return alpha >= 0.999 && (x.blendMode == "Normal" || x.blendMode == "Compatible") && !x.hasSMask;
}

struct SObj {
  Box raw, ext;
  const std::vector<int>* zp = nullptr;
  bool vis = false, rect = false, stroke = false, paint = false, white = false, hidden = false, opaque = false;
  int kind = 0, page = 0;
  const void* p = nullptr;
};

struct Pt {
  double x = 0, y = 0;
};

using Poly = std::vector<Pt>;

struct Shape {
  std::vector<std::vector<Poly>> groups;
  Box clip, bb;
  std::vector<Pt> edge, fill;
  bool ready = false;
};

struct Geo {
  std::vector<double> angles, radii;
  std::vector<int> distinct;
  std::vector<std::vector<std::pair<double, double>>> polys;
};

Geo analyzeGeo(const std::vector<double>& g) {
  Geo out;
  double sx = 0, sy = 0, cx = 0, cy = 0, fx = 0, fy = 0, ix = 0, iy = 0;
  bool open = false, hasFirst = false, hasIn = false;
  int distinct = 0;
  std::vector<std::pair<double, double>> poly;
  auto corner = [&](double inx, double iny, double ox, double oy) {
    double l1 = std::hypot(inx, iny), l2 = std::hypot(ox, oy);
    if (l1 < 1e-9 || l2 < 1e-9) return;
    double c = (-inx * ox - iny * oy) / (l1 * l2);
    double ang = std::acos(std::max(-1.0, std::min(1.0, c))) * 180.0 / M_PI;
    if (ang < 179.0 && out.angles.size() < 20000) out.angles.push_back(ang);
  };
  auto finish = [&]() {
    if (!open) return;
    out.distinct.push_back(distinct);
    if (poly.size() >= 2) out.polys.push_back(poly);
    poly.clear();
    open = false;
  };
  auto start = [&](double x, double y) {
    finish();
    sx = cx = x;
    sy = cy = y;
    open = true;
    hasFirst = hasIn = false;
    distinct = 1;
    poly.assign(1, {x, y});
  };
  auto segment = [&](double ox, double oy, double inx, double iny, double ex, double ey) {
    if (hasIn) corner(ix, iy, ox, oy);
    else {
      fx = ox;
      fy = oy;
      hasFirst = true;
    }
    ix = inx;
    iy = iny;
    hasIn = true;
    ++distinct;
    cx = ex;
    cy = ey;
  };
  size_t i = 0;
  while (i < g.size()) {
    int code = static_cast<int>(g[i]);
    if (code == 0 && i + 2 < g.size()) {
      start(g[i + 1], g[i + 2]);
      i += 3;
    } else if (code == 1 && i + 2 < g.size()) {
      if (!open) start(sx, sy);
      double ex = g[i + 1], ey = g[i + 2];
      i += 3;
      if (std::hypot(ex - cx, ey - cy) < 1e-6) continue;
      segment(ex - cx, ey - cy, ex - cx, ey - cy, ex, ey);
      poly.push_back({ex, ey});
    } else if (code == 2 && i + 6 < g.size()) {
      if (!open) start(sx, sy);
      double x0 = cx, y0 = cy, x1 = g[i + 1], y1 = g[i + 2], x2 = g[i + 3], y2 = g[i + 4], x3 = g[i + 5], y3 = g[i + 6];
      i += 7;
      double ox = x1 - x0, oy = y1 - y0;
      if (std::hypot(ox, oy) < 1e-9) ox = x2 - x0, oy = y2 - y0;
      if (std::hypot(ox, oy) < 1e-9) ox = x3 - x0, oy = y3 - y0;
      double inx = x3 - x2, iny = y3 - y2;
      if (std::hypot(inx, iny) < 1e-9) inx = x3 - x1, iny = y3 - y1;
      if (std::hypot(inx, iny) < 1e-9) inx = x3 - x0, iny = y3 - y0;
      if (std::hypot(ox, oy) < 1e-9 && std::hypot(x3 - x0, y3 - y0) < 1e-6) continue;
      double rmin = 1e18;
      for (int k = 0; k <= 32; ++k) {
        double t = k / 32.0, u = 1 - t;
        double dx = 3 * (u * u * (x1 - x0) + 2 * u * t * (x2 - x1) + t * t * (x3 - x2));
        double dy = 3 * (u * u * (y1 - y0) + 2 * u * t * (y2 - y1) + t * t * (y3 - y2));
        double ddx = 6 * (u * (x2 - 2 * x1 + x0) + t * (x3 - 2 * x2 + x1));
        double ddy = 6 * (u * (y2 - 2 * y1 + y0) + t * (y3 - 2 * y2 + y1));
        double cr = std::fabs(dx * ddy - dy * ddx);
        double sp = std::hypot(dx, dy);
        if (cr > 1e-12 && sp > 1e-9) rmin = std::min(rmin, sp * sp * sp / cr);
      }
      if (rmin < 1e9 && out.radii.size() < 20000) out.radii.push_back(rmin);
      segment(ox, oy, inx, iny, x3, y3);
      for (int k = 1; k <= 16; ++k) {
        double t = k / 16.0, u = 1 - t;
        poly.push_back({u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
                        u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3});
      }
    } else if (code == 3) {
      ++i;
      if (!open) continue;
      if (std::hypot(sx - cx, sy - cy) > 1e-6) {
        segment(sx - cx, sy - cy, sx - cx, sy - cy, sx, sy);
        poly.push_back({sx, sy});
      }
      if (hasIn && hasFirst) corner(ix, iy, fx, fy);
      if (distinct > 1) --distinct;
      finish();
      cx = sx;
      cy = sy;
    } else {
      break;
    }
  }
  finish();
  return out;
}

struct Raster {
  int page = -1, w = 0, h = 0;
  double dpi = 0;
  std::vector<unsigned char> cmyk;
  std::vector<unsigned char> used;
  Box crop;
  int rot = 0;
  bool ok = false;
};

struct FormUse {
  int page = 0;
  Box bbox;
  bool nested = false;
};

struct PageWalk {
  std::set<std::string> problems;
  bool resMissing = false;
  std::vector<FormUse> forms;
};

struct InkObj {
  double ink = 0;
  Box bbox;
  bool cmyk = false, overprint = false, spot = false;
  int opm = 0;
  std::vector<double> comps;
  int gw = 0, gh = 0;
  std::vector<double> grid;
};

struct SpatialCache {
  bool indexed = false;
  std::map<int, std::vector<SObj>> objs;
  std::map<const void*, size_t> where;
  std::map<const void*, Geo> geo;
  std::map<std::pair<const void*, bool>, Shape> shapes;
  std::map<const void*, std::pair<int, int>> obl;
  std::map<std::pair<std::string, const void*>, int> member;
  std::map<std::pair<const PfAtom*, const void*>, std::vector<std::string>> relations;
  std::map<std::string, Shape> boxShapes;
  int depth = 0;
  std::map<int, PageWalk> walks;
  std::map<QPDFObjGen, PageWalk> formWalks;
  bool streamsChecked = false, resourcesChecked = false, inkReady = false;
  std::vector<std::string> streamProblems, resourceProblems;
  std::map<int, std::vector<InkObj>> ink;
  Raster raster;
};

SpatialCache& cacheOf(const Events& ev) {
  if (!ev.spatialCache) ev.spatialCache = std::make_shared<SpatialCache>();
  return *static_cast<SpatialCache*>(ev.spatialCache.get());
}

void buildIndex(const Events& ev, SpatialCache& c) {
  if (c.indexed) return;
  c.indexed = true;
  auto add = [&](int page, const Box& raw0, const Box& clip, const std::vector<int>& zp, bool vis, bool rect,
                 bool stroke, bool paint, bool white, const void* p, int kind, bool hidden, bool opaque) {
    const PageFacts* pf = pageFor(ev, page);
    Box ext = raw0.valid ? raw0 : clip;
    if (!ext.valid) return;
    if (raw0.valid && clip.valid) ext = inter(ext, clip);
    if (pf) ext = inter(ext, cropOf(*pf));
    SObj o;
    o.raw = raw0;
    o.ext = ext;
    o.zp = &zp;
    o.vis = vis && ext.valid;
    o.rect = rect;
    o.stroke = stroke;
    o.paint = paint;
    o.white = white;
    o.p = p;
    o.kind = kind;
    o.page = page;
    o.hidden = hidden;
    o.opaque = opaque;
    auto& v = c.objs[page];
    c.where[p] = v.size();
    v.push_back(o);
  };
  for (const PaintEvent& e : ev.paints) {
    if (e.noPaint) continue;
    Box raw = e.bbox;
    if (e.stroke && raw.valid) raw = grow(raw, e.width / 2);
    bool rect = e.fillOp && !e.shade && !e.overprint && e.rects > 0 && e.rects == e.subpaths &&
                axisAligned(e.x.ctm) && normalBlend(e.x, e.x.alphaFill);
    bool vis = !e.layerHidden && !e.clippedFull && (e.stroke ? e.x.alphaStroke : e.x.alphaFill) > 0.001;
    bool opq = !e.overprint && normalBlend(e.x, e.stroke ? e.x.alphaStroke : e.x.alphaFill);
    add(e.page, raw, e.clip, e.x.zp, vis, rect, e.stroke, true, whiteColor(e.comps, e.color), &e, 0, e.layerHidden, opq);
  }
  for (const TextEvent& e : ev.texts) {
    add(e.page, e.bbox, e.clip, e.x.zp, !e.layerHidden && !e.clippedFull && e.renderMode != 3 && e.renderMode != 7, false,
        false, false, whiteColor(e.comps, e.color), &e, 1, e.layerHidden, false);
  }
  for (const ImageEvent& e : ev.images) {
    bool rect = !e.mask && !e.hasSMask && !e.overprint && axisAligned(e.x.ctm) && normalBlend(e.x, e.x.alphaFill);
    bool opq = !e.mask && !e.hasSMask && !e.overprint && normalBlend(e.x, e.x.alphaFill);
    add(e.page, e.bbox, e.clip, e.x.zp, !e.layerHidden && !e.clippedFull, rect, false, false, false, &e, 2, e.layerHidden, opq);
  }
}

bool sameObj(const SObj& a, const SObj& b) {
  if (&a == &b) return true;
  if (!a.paint || !b.paint || a.stroke == b.stroke) return false;
  const auto& za = *a.zp;
  const auto& zb = *b.zp;
  if (za.size() != zb.size() || za.empty()) return false;
  if (!std::equal(za.begin(), za.end() - 1, zb.begin())) return false;
  if (std::abs(za.back() - zb.back()) != 1) return false;
  const PaintEvent* pa = static_cast<const PaintEvent*>(a.p);
  const PaintEvent* pb = static_cast<const PaintEvent*>(b.p);
  return pa->bbox.x0 == pb->bbox.x0 && pa->bbox.y0 == pb->bbox.y0 && pa->bbox.x1 == pb->bbox.x1 &&
         pa->bbox.y1 == pb->bbox.y1;
}

constexpr double kProbe = 2e-3;

Pt lerp(const Pt& a, const Pt& b, double t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

void flattenCubic(Poly& out, const Pt& p0, const Pt& p1, const Pt& p2, const Pt& p3, int depth) {
  double dd = std::fabs(p0.x - 2 * p1.x + p2.x) + std::fabs(p1.x - 2 * p2.x + p3.x) +
              std::fabs(p0.y - 2 * p1.y + p2.y) + std::fabs(p1.y - 2 * p2.y + p3.y);
  if (depth >= 10 || dd <= 0.05) {
    out.push_back(p3);
    return;
  }
  Pt a = lerp(p0, p1, 0.5), b = lerp(p1, p2, 0.5), cc = lerp(p2, p3, 0.5);
  Pt d = lerp(a, b, 0.5), e = lerp(b, cc, 0.5), m = lerp(d, e, 0.5);
  flattenCubic(out, p0, a, d, m, depth + 1);
  flattenCubic(out, m, e, cc, p3, depth + 1);
}

struct Sub {
  Poly pts;
  bool closed = false;
};

std::vector<Sub> subpaths(const std::vector<double>& g) {
  std::vector<Sub> out;
  Pt cur, start;
  bool open = false;
  auto begin = [&](const Pt& p) {
    out.push_back(Sub());
    out.back().pts.push_back(p);
    start = cur = p;
    open = true;
  };
  size_t i = 0;
  while (i < g.size()) {
    int code = static_cast<int>(g[i]);
    if (code == 0 && i + 2 < g.size()) {
      begin({g[i + 1], g[i + 2]});
      i += 3;
    } else if (code == 1 && i + 2 < g.size()) {
      if (!open) begin(start);
      cur = {g[i + 1], g[i + 2]};
      out.back().pts.push_back(cur);
      i += 3;
    } else if (code == 2 && i + 6 < g.size()) {
      if (!open) begin(start);
      Pt p3{g[i + 5], g[i + 6]};
      flattenCubic(out.back().pts, cur, {g[i + 1], g[i + 2]}, {g[i + 3], g[i + 4]}, p3, 0);
      cur = p3;
      i += 7;
    } else if (code == 3) {
      if (open) out.back().closed = true;
      open = false;
      cur = start;
      ++i;
    } else {
      break;
    }
  }
  for (Sub& s : out) {
    Poly clean;
    for (const Pt& p : s.pts) {
      if (clean.empty() || std::hypot(p.x - clean.back().x, p.y - clean.back().y) > 1e-9) clean.push_back(p);
    }
    if (s.closed && clean.size() > 1 && std::hypot(clean.front().x - clean.back().x, clean.front().y - clean.back().y) <= 1e-9) {
      clean.pop_back();
    }
    s.pts.swap(clean);
  }
  return out;
}

Poly rectPoly(const Box& b) { return {{b.x0, b.y0}, {b.x1, b.y0}, {b.x1, b.y1}, {b.x0, b.y1}}; }

Poly disc(const Pt& c, double r) {
  Poly p;
  for (int k = 0; k < 24; ++k) {
    double t = k * M_PI / 12;
    p.push_back({c.x + r * std::cos(t), c.y + r * std::sin(t)});
  }
  return p;
}

void strokeGroups(const PaintEvent& e, std::vector<std::vector<Poly>>& groups) {
  double hw = std::max(e.width / 2, 0.05);
  int cap = e.x.lineCap, join = e.x.lineJoin;
  for (const Sub& s : subpaths(e.geo)) {
    const Poly& p = s.pts;
    size_t n = p.size();
    if (n == 0) continue;
    if (n == 1) {
      if (cap == 1) groups.push_back({disc(p[0], hw)});
      if (cap == 2) groups.push_back({rectPoly({p[0].x - hw, p[0].y - hw, p[0].x + hw, p[0].y + hw, true})});
      continue;
    }
    bool closed = s.closed && n > 2;
    size_t segs = closed ? n : n - 1;
    auto dir = [&](size_t i) {
      Pt a = p[i % n], b = p[(i + 1) % n];
      double l = std::hypot(b.x - a.x, b.y - a.y);
      return Pt{(b.x - a.x) / l, (b.y - a.y) / l};
    };
    for (size_t i = 0; i < segs; ++i) {
      Pt a = p[i], b = p[(i + 1) % n], d = dir(i), nr{-d.y * hw, d.x * hw};
      if (!closed && cap == 2 && i == 0) a = {a.x - d.x * hw, a.y - d.y * hw};
      if (!closed && cap == 2 && i + 1 == segs) b = {b.x + d.x * hw, b.y + d.y * hw};
      groups.push_back({{{a.x + nr.x, a.y + nr.y}, {b.x + nr.x, b.y + nr.y}, {b.x - nr.x, b.y - nr.y}, {a.x - nr.x, a.y - nr.y}}});
    }
    size_t j0 = closed ? 0 : 1, j1 = closed ? n : n - 1;
    for (size_t j = j0; j < j1; ++j) {
      Pt v = p[j], d1 = dir((j + n - 1) % n), d2 = dir(j);
      if (join == 1) {
        groups.push_back({disc(v, hw)});
        continue;
      }
      double cr = d1.x * d2.y - d1.y * d2.x;
      if (std::fabs(cr) < 1e-12 && d1.x * d2.x + d1.y * d2.y > 0) continue;
      double sg = cr > 0 ? -1 : 1;
      Pt n1{-d1.y * hw * sg, d1.x * hw * sg}, n2{-d2.y * hw * sg, d2.x * hw * sg};
      Pt a{v.x + n1.x, v.y + n1.y}, b{v.x + n2.x, v.y + n2.y};
      groups.push_back({{v, a, b}});
      if (join == 0) {
        double cosT = std::max(-1.0, std::min(1.0, -(d1.x * d2.x + d1.y * d2.y)));
        double half = std::sin(std::acos(cosT) / 2);
        Pt mid{n1.x + n2.x, n1.y + n2.y};
        double ml = std::hypot(mid.x, mid.y);
        if (half > 1e-9 && 1 / half <= e.x.miterLimit && ml > 1e-12) {
          double len = hw / half;
          groups.push_back({{v, a, {v.x + mid.x / ml * len, v.y + mid.y / ml * len}, b}});
        }
      }
    }
    if (!closed && cap == 1) {
      groups.push_back({disc(p[0], hw)});
      groups.push_back({disc(p[n - 1], hw)});
    }
  }
}

int winding(const Poly& poly, const Pt& p) {
  int w = 0;
  size_t n = poly.size();
  for (size_t i = 0; i < n; ++i) {
    const Pt& a = poly[i];
    const Pt& b = poly[(i + 1) % n];
    double side = (b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y);
    if (a.y <= p.y) {
      if (b.y > p.y && side > 0) ++w;
    } else if (b.y <= p.y && side < 0) {
      --w;
    }
  }
  return w;
}

bool inBox(const Box& b, const Pt& p) { return p.x >= b.x0 && p.x <= b.x1 && p.y >= b.y0 && p.y <= b.y1; }

bool insideShape(const Shape& s, const Pt& p) {
  if (!s.bb.valid || !inBox(s.bb, p)) return false;
  if (s.clip.valid && !inBox(s.clip, p)) return false;
  for (const auto& g : s.groups) {
    int w = 0;
    for (const Poly& poly : g) w += winding(poly, p);
    if (w) return true;
  }
  return false;
}

void finishShape(Shape& s) {
  s.ready = true;
  Box bb;
  for (const auto& g : s.groups) {
    for (const Poly& poly : g) {
      for (const Pt& p : poly) {
        if (!bb.valid) bb = {p.x, p.y, p.x, p.y, true};
        bb.x0 = std::min(bb.x0, p.x);
        bb.y0 = std::min(bb.y0, p.y);
        bb.x1 = std::max(bb.x1, p.x);
        bb.y1 = std::max(bb.y1, p.y);
      }
    }
  }
  s.bb = s.clip.valid ? inter(bb, s.clip) : bb;
  if (!s.bb.valid) return;
  s.bb = grow(s.bb, kProbe);
  std::vector<std::pair<Pt, Pt>> edges;
  for (const auto& g : s.groups) {
    for (const Poly& poly : g) {
      for (size_t i = 0; i < poly.size() && poly.size() > 2; ++i) edges.push_back({poly[i], poly[(i + 1) % poly.size()]});
    }
  }
  if (s.clip.valid) {
    Poly r = rectPoly(s.clip);
    for (size_t i = 0; i < 4; ++i) edges.push_back({r[i], r[(i + 1) % 4]});
  }
  double per = 0;
  for (const auto& e : edges) per += std::hypot(e.second.x - e.first.x, e.second.y - e.first.y);
  double step = std::max(0.05, std::min(1.0, per / 1500));
  for (const auto& e : edges) {
    double len = std::hypot(e.second.x - e.first.x, e.second.y - e.first.y);
    if (len < 1e-9) continue;
    Pt nr{-(e.second.y - e.first.y) / len * kProbe, (e.second.x - e.first.x) / len * kProbe};
    int n = std::max(1, static_cast<int>(std::ceil(len / step)));
    for (int k = 0; k < n && s.edge.size() < 8000; ++k) {
      Pt p = lerp(e.first, e.second, static_cast<double>(k) / n);
      bool a = insideShape(s, {p.x + nr.x, p.y + nr.y}), b = insideShape(s, {p.x - nr.x, p.y - nr.y});
      if (a == b) continue;
      s.edge.push_back(p);
      s.fill.push_back(a ? Pt{p.x + 2 * nr.x, p.y + 2 * nr.y} : Pt{p.x - 2 * nr.x, p.y - 2 * nr.y});
    }
  }
  const int grid = 20;
  for (int i = 0; i < grid; ++i) {
    for (int j = 0; j < grid; ++j) {
      Pt p{s.bb.x0 + (i + 0.5) * (s.bb.x1 - s.bb.x0) / grid, s.bb.y0 + (j + 0.5) * (s.bb.y1 - s.bb.y0) / grid};
      if (insideShape(s, p)) s.fill.push_back(p);
    }
  }
}

const Shape& shapeOf(const SObj& o, const Events& ev, SpatialCache& c, bool noClip) {
  Shape& s = c.shapes[{o.p, noClip}];
  if (s.ready) return s;
  const PageFacts* pf = pageFor(ev, o.page);
  Box clip, raw;
  if (o.kind == 0) {
    const PaintEvent& e = *static_cast<const PaintEvent*>(o.p);
    clip = e.clip;
    raw = e.bbox;
    if (e.geo.empty()) {
      Box r = e.shade && e.clip.valid ? e.clip : e.bbox;
      if (r.valid) s.groups.push_back({rectPoly(r)});
    } else if (e.stroke) {
      strokeGroups(e, s.groups);
    } else {
      std::vector<Poly> g;
      for (Sub& sp : subpaths(e.geo)) {
        if (sp.pts.size() > 2) g.push_back(std::move(sp.pts));
      }
      if (!g.empty()) s.groups.push_back(std::move(g));
    }
  } else if (o.kind == 1) {
    const TextEvent& e = *static_cast<const TextEvent*>(o.p);
    clip = e.clip;
    if (e.bbox.valid) s.groups.push_back({rectPoly(e.bbox)});
  } else {
    const ImageEvent& e = *static_cast<const ImageEvent*>(o.p);
    clip = e.clip;
    const Mat& m = e.x.ctm;
    if (std::fabs(m.a * m.d - m.b * m.c) > 1e-12) {
      s.groups.push_back({{{m.e, m.f}, {m.a + m.e, m.b + m.f}, {m.a + m.c + m.e, m.b + m.d + m.f}, {m.c + m.e, m.d + m.f}}});
    } else if (e.bbox.valid) {
      s.groups.push_back({rectPoly(e.bbox)});
    }
  }
  if (!noClip) s.clip = pf ? inter(clip, cropOf(*pf)) : clip;
  if (!noClip && clip.valid && !s.clip.valid) s.clip = Box{0, 0, 0, 0, true};
  finishShape(s);
  return s;
}

using ShapeSet = std::vector<const Shape*>;

bool inSet(const ShapeSet& b, const Pt& p) {
  for (const Shape* s : b) {
    if (insideShape(*s, p)) return true;
  }
  return false;
}

int status(const ShapeSet& b, const Pt& p) {
  int n = inSet(b, {p.x + kProbe, p.y}) + inSet(b, {p.x - kProbe, p.y}) + inSet(b, {p.x, p.y + kProbe}) +
          inSet(b, {p.x, p.y - kProbe});
  return n == 4 ? 2 : n == 0 ? 0 : 1;
}

struct Tally {
  int in = 0, on = 0, out = 0;
};

template <class F>
void exposed(const ShapeSet& set, F f) {
  for (const Shape* s : set) {
    for (const Pt& p : s->edge) {
      bool hidden = false;
      for (const Shape* o : set) {
        if (o != s && status({o}, p) == 2) {
          hidden = true;
          break;
        }
      }
      if (!hidden) f(p);
    }
  }
}

Tally tally(const ShapeSet& from, const ShapeSet& vs) {
  Tally t;
  exposed(from, [&](const Pt& p) {
    int st = status(vs, p);
    if (st == 2) ++t.in;
    else if (st == 1) ++t.on;
    else ++t.out;
  });
  return t;
}

Box setBox(const ShapeSet& s) {
  Box b;
  for (const Shape* x : s) {
    if (!x->bb.valid) continue;
    if (!b.valid) b = x->bb;
    b = {std::min(b.x0, x->bb.x0), std::min(b.y0, x->bb.y0), std::max(b.x1, x->bb.x1), std::max(b.y1, x->bb.y1), true};
  }
  return b;
}

enum Rel { kApart, kAIn, kBIn, kCross };

Rel classify(const ShapeSet& a, const ShapeSet& b) {
  if (!touches(setBox(a), setBox(b))) return kApart;
  Tally ta = tally(a, b), tb = tally(b, a);
  if (!ta.in && !ta.on && !tb.in && !tb.on) return kApart;
  if (!ta.out && !ta.on && !tb.in && !tb.on) return kAIn;
  if (!tb.out && !tb.on && !ta.in && !ta.on) return kBIn;
  return kCross;
}

double edgeDistance(const ShapeSet& a, const ShapeSet& b, double limit) {
  Box bb = setBox(b);
  if (!bb.valid) return 1e18;
  double best = 1e18;
  std::vector<Pt> pb;
  exposed(b, [&](const Pt& p) { pb.push_back(p); });
  exposed(a, [&](const Pt& p) {
    double dx = std::max({0.0, bb.x0 - p.x, p.x - bb.x1}), dy = std::max({0.0, bb.y0 - p.y, p.y - bb.y1});
    if (std::hypot(dx, dy) > std::min(best, limit)) return;
    for (const Pt& q : pb) best = std::min(best, std::hypot(p.x - q.x, p.y - q.y));
  });
  return best;
}

std::vector<const Shape*> occluders(const SObj& o, const std::vector<SObj>& all, const Events& ev, SpatialCache& c,
                                    const std::vector<int>& above, const void* skip) {
  std::vector<const Shape*> out;
  for (const SObj& b : all) {
    if (b.p == o.p || b.p == skip || !b.opaque || b.hidden || sameObj(o, b) || !zLess(above, *b.zp)) continue;
    if (!touches(b.ext, o.ext)) continue;
    out.push_back(&shapeOf(b, ev, c, false));
  }
  return out;
}

std::pair<int, int> oblOf(const SObj& o, const std::vector<SObj>& all, const Events& ev, SpatialCache& c) {
  auto it = c.obl.find(o.p);
  if (it != c.obl.end()) return it->second;
  const Shape& s = shapeOf(o, ev, c, false);
  ShapeSet occ = occluders(o, all, ev, c, *o.zp, nullptr);
  int vis = 0, cov = 0;
  for (const Pt& p : s.fill) (inSet(occ, p) ? cov : vis)++;
  return c.obl[o.p] = {vis, cov};
}


const Geo& geoOf(const PaintEvent& e, SpatialCache& c) {
  auto it = c.geo.find(&e);
  if (it != c.geo.end()) return it->second;
  return c.geo.emplace(&e, analyzeGeo(e.geo)).first->second;
}

const SObj* objOf(const Subject& s, const Events& ev, SpatialCache& c, const std::vector<SObj>*& all) {
  const void* p = nullptr;
  int page = 0;
  if (s.paint) p = s.paint, page = s.paint->page;
  else if (s.text) p = s.text, page = s.text->page;
  else if (s.image) p = s.image, page = s.image->page;
  if (!p) return nullptr;
  buildIndex(ev, c);
  auto w = c.where.find(p);
  auto pit = c.objs.find(page);
  if (w == c.where.end() || pit == c.objs.end()) return nullptr;
  all = &pit->second;
  return &pit->second[w->second];
}

Domain kindOf(const SObj& o) { return o.kind == 0 ? Domain::kPaint : o.kind == 1 ? Domain::kText : Domain::kImage; }

bool atomOn(const PfAtom& at, const SObj& o, const Events& ev, bool& supported) {
  Domain d = domainOf(at);
  Domain k = kindOf(o);
  if (d == Domain::kAnnot) return false;
  if ((d == Domain::kPaint || d == Domain::kText || d == Domain::kImage || d == Domain::kFont) && d != k &&
      !(k == Domain::kText && d == Domain::kFont)) {
    return false;
  }
  return evalCheck(at, subjectFor(k, o.p, o.page, ev), ev, supported) > 0;
}

const PfRule* ruleOf(const std::string& name, const Events& ev) {
  if (!ev.prof) return nullptr;
  auto it = ev.prof->ruleIndex.find(name);
  return it == ev.prof->ruleIndex.end() ? nullptr : &it->second;
}

bool member(const std::string& name, const SObj& o, const Events& ev, SpatialCache& c, bool& supported) {
  auto key = std::make_pair(name, o.p);
  auto it = c.member.find(key);
  if (it != c.member.end()) return it->second == 1;
  const PfRule* r = ruleOf(name, ev);
  if (!r || c.depth > 16) {
    supported = false;
    return false;
  }
  c.member[key] = 0;
  ++c.depth;
  bool combined = r->logic != 1;
  for (const std::string& cid : r->condIds) {
    auto cit = ev.prof->conds.find(cid);
    if (cit == ev.prof->conds.end()) continue;
    bool ok = true;
    for (const PfAtom& at : cit->second.atoms) {
      if (!atomOn(at, o, ev, supported)) {
        ok = false;
        break;
      }
    }
    combined = r->logic == 1 ? (combined || ok) : (combined && ok);
    if (r->logic == 1 ? combined : !combined) break;
  }
  --c.depth;
  c.member[key] = combined ? 1 : 0;
  return combined;
}

const Shape* boxShape(const Box& b, SpatialCache& c) {
  if (!b.valid || b.x1 <= b.x0 || b.y1 <= b.y0) return nullptr;
  Shape& s = c.boxShapes[fmtFixed(b.x0, 4) + "|" + fmtFixed(b.y0, 4) + "|" + fmtFixed(b.x1, 4) + "|" + fmtFixed(b.y1, 4)];
  if (!s.ready) {
    s.groups.push_back({rectPoly(b)});
    finishShape(s);
  }
  return &s;
}

bool usable(const SObj& o, const Events& ev, SpatialCache& c) {
  return !o.hidden && o.vis && !shapeOf(o, ev, c, false).fill.empty();
}

bool seen(const SObj& o, const std::vector<SObj>& all, const Events& ev, SpatialCache& c) {
  return oblOf(o, all, ev, c).first > 0;
}

bool overlapVisible(const SObj& o, const SObj& b, const std::vector<SObj>& all, const Events& ev, SpatialCache& c) {
  const Shape& sa = shapeOf(o, ev, c, false);
  const Shape& sb = shapeOf(b, ev, c, false);
  std::vector<Pt> pts;
  for (const Pt& p : sa.fill) {
    if (insideShape(sb, p)) pts.push_back(p);
  }
  for (const Pt& p : sb.fill) {
    if (insideShape(sa, p)) pts.push_back(p);
  }
  if (pts.empty()) return true;
  const std::vector<int>& top = zLess(*o.zp, *b.zp) ? *b.zp : *o.zp;
  ShapeSet occ = occluders(o, all, ev, c, top, b.p);
  for (const Pt& p : pts) {
    if (!inSet(occ, p)) return true;
  }
  return false;
}

bool reaches(const ShapeSet& a, const ShapeSet& b, double in, double out) {
  std::vector<Pt> pb;
  exposed(b, [&](const Pt& p) { pb.push_back(p); });
  if (pb.empty()) return false;
  auto d = [&](const Pt& p) {
    double best = 1e18;
    for (const Pt& q : pb) best = std::min(best, std::hypot(p.x - q.x, p.y - q.y));
    return best;
  };
  bool within = true, intoBand = false;
  exposed(a, [&](const Pt& p) {
    if (!within) return;
    int st = status(b, p);
    double dp = st == 1 ? 0 : d(p);
    if (st == 0 && dp > out) within = false;
    if (st != 2 || dp < in) intoBand = true;
  });
  return within && intoBand;
}

const char* const kRelations[] = {"above", "below", "covers", "covered", "crosses", "inside", "outside", "near"};

std::vector<std::string> relationsOf(const PfAtom& a, const SObj& o, const std::vector<SObj>& all, const Events& ev,
                                     SpatialCache& c, bool& supported) {
  std::set<std::string> want;
  if (a.op == "count") want.insert(std::begin(kRelations), std::end(kRelations));
  else want.insert(a.vals.begin(), a.vals.end());
  std::vector<std::string> names = a.refs;
  if (!param(a, "other").empty()) names.push_back(param(a, "other"));
  long need = std::max(1L, std::lround(paramNum(a, "min_objects")));
  double dist = paramNum(a, "distance");
  bool nearOnly = want.size() == 1 && want.count("near");
  std::map<std::string, long> n;
  auto open = [&]() {
    for (const std::string& w : want) {
      if (n[w] < need) return true;
    }
    return false;
  };
  auto add = [&](const char* r) {
    if (want.count(r)) ++n[r];
  };
  ShapeSet sa{&shapeOf(o, ev, c, false)};
  for (const SObj& b : all) {
    if (!open()) break;
    if (&b == &o || sameObj(o, b) || !usable(b, ev, c)) continue;
    bool in = names.empty();
    for (const std::string& nm : names) in = in || member(nm, b, ev, c, supported);
    if (!in) continue;
    ShapeSet sb{&shapeOf(b, ev, c, false)};
    if (want.count("near") && edgeDistance(sa, sb, dist) < dist) ++n["near"];
    if (nearOnly) continue;
    Rel r = classify(sa, sb);
    if (r == kApart) {
      if (want.count("outside") && seen(o, all, ev, c) && seen(b, all, ev, c)) ++n["outside"];
      continue;
    }
    if (!overlapVisible(o, b, all, ev, c)) continue;
    bool top = zLess(*b.zp, *o.zp);
    add(top ? "above" : "below");
    if (r == kCross) add("crosses");
    if (r == kAIn) {
      add("inside");
      if (!top) add("covered");
    }
    if (r == kBIn && top) add("covers");
  }
  std::vector<std::string> out;
  for (const char* r : kRelations) {
    if (n[r] >= need) out.push_back(r);
  }
  return out;
}

const Shape* checkOutline(const std::string& name, int page, const std::vector<SObj>& all, const Events& ev, SpatialCache& c,
                          bool& supported) {
  Shape& own = c.boxShapes["outline|" + name + "|" + std::to_string(page)];
  if (!own.ready) {
    for (const SObj& o : all) {
      if (o.hidden || !member(name, o, ev, c, supported)) continue;
      if (o.kind == 0 && !static_cast<const PaintEvent*>(o.p)->geo.empty()) {
        std::vector<Poly> g;
        for (Sub& sp : subpaths(static_cast<const PaintEvent*>(o.p)->geo)) {
          if (sp.pts.size() > 2) g.push_back(std::move(sp.pts));
        }
        if (!g.empty()) own.groups.push_back(std::move(g));
      } else {
        const Shape& s = shapeOf(o, ev, c, true);
        own.groups.insert(own.groups.end(), s.groups.begin(), s.groups.end());
      }
    }
    finishShape(own);
  }
  return own.bb.valid ? &own : nullptr;
}

bool outline(std::string name, double off, int page, const std::vector<SObj>& all, const Events& ev, SpatialCache& c,
             ShapeSet& out, bool& supported) {
  auto sit = ev.shapes.find(name);
  if (sit != ev.shapes.end()) {
    off += sit->second.offset;
    name = sit->second.box.empty() ? sit->second.check : sit->second.box;
  }
  if (isBoxName(name)) {
    const PageFacts* pf = pageFor(ev, page);
    if (!pf) return false;
    if (const Shape* bs = boxShape(grow(boxOf(*pf, name), off), c)) out.push_back(bs);
    return true;
  }
  if (!ruleOf(name, ev) || std::fabs(off) > 1e-9) {
    supported = false;
    return false;
  }
  if (const Shape* s = checkOutline(name, page, all, ev, c, supported)) out.push_back(s);
  return true;
}

bool asks(const PfAtom& a, const char* v) { return std::find(a.vals.begin(), a.vals.end(), v) != a.vals.end(); }

std::string areaRelation(const PfAtom& a, const PageFacts& p, const Box& ext) {
  if (!ext.valid) return std::string();
  const std::string& origin = param(a, "origin");
  const Box& crop = cropOf(p);
  double ox = crop.x0, oy = crop.y0;
  if (origin == "center") {
    ox = (crop.x0 + crop.x1) / 2;
    oy = (crop.y0 + crop.y1) / 2;
  } else if (isBoxName(origin)) {
    ox = boxOf(p, origin).x0;
    oy = boxOf(p, origin).y0;
  }
  double x = ox + paramNum(a, "x"), y = oy + paramNum(a, "y");
  double w = paramNum(a, "width"), h = paramNum(a, "height"), tol = paramNum(a, "tolerance");
  if (std::fabs(w) < 1e-9 && std::fabs(h) < 1e-9) {
    double cx = std::min(std::max(x, ext.x0), ext.x1), cy = std::min(std::max(y, ext.y0), ext.y1);
    return std::hypot(x - cx, y - cy) <= tol + 1e-9 ? "at" : "outside";
  }
  Box ar{std::min(x, x + w), std::min(y, y + h), std::max(x, x + w), std::max(y, y + h), true};
  if (ext.x0 >= ar.x0 - tol && ext.y0 >= ar.y0 - tol && ext.x1 <= ar.x1 + tol && ext.y1 <= ar.y1 + tol) return "inside";
  bool apart = ext.x1 <= ar.x0 + tol || ext.x0 >= ar.x1 - tol || ext.y1 <= ar.y0 + tol || ext.y0 >= ar.y1 - tol;
  return apart ? "outside" : "crosses";
}

Box extentOf(const Subject& s) {
  if (s.paint) return s.paint->stroke && s.paint->bbox.valid ? grow(s.paint->bbox, s.paint->width / 2) : s.paint->bbox;
  if (s.text) return s.text->bbox;
  if (s.image) return s.image->bbox;
  return Box();
}

bool inkUsed(const SObj& o) { return o.vis && !o.white; }

QPDFObjectHandle inheritedKey(QPDFObjectHandle pg, const std::string& key) {
  for (int i = 0; i < 64 && pg.isDictionary(); ++i) {
    QPDFObjectHandle v = pg.getKey(key);
    if (!v.isNull()) return v;
    pg = pg.getKey("/Parent");
  }
  return QPDFObjectHandle::newNull();
}

Box xformBox(const Box& b, const Mat& m) {
  double xs[4] = {b.x0, b.x1, b.x0, b.x1}, ys[4] = {b.y0, b.y0, b.y1, b.y1};
  Box r;
  for (int k = 0; k < 4; ++k) {
    double x = m.a * xs[k] + m.c * ys[k] + m.e, y = m.b * xs[k] + m.d * ys[k] + m.f;
    if (!r.valid) r = {x, y, x, y, true};
    r.x0 = std::min(r.x0, x);
    r.y0 = std::min(r.y0, y);
    r.x1 = std::max(r.x1, x);
    r.y1 = std::max(r.y1, y);
  }
  return r;
}


const std::map<std::string, std::string>& operandTypes() {
  static const std::map<std::string, std::string> t = {
      {"w", "n"},      {"J", "n"},      {"j", "n"},      {"M", "n"},      {"d", "an"},     {"ri", "N"},   {"i", "n"},
      {"gs", "N"},     {"q", ""},       {"Q", ""},       {"cm", "nnnnnn"}, {"m", "nn"},    {"l", "nn"},   {"c", "nnnnnn"},
      {"v", "nnnn"},   {"y", "nnnn"},   {"h", ""},       {"re", "nnnn"},  {"S", ""},       {"s", ""},     {"f", ""},
      {"F", ""},       {"f*", ""},      {"B", ""},       {"B*", ""},      {"b", ""},       {"b*", ""},    {"n", ""},
      {"W", ""},       {"W*", ""},      {"BT", ""},      {"ET", ""},      {"Tc", "n"},     {"Tw", "n"},   {"Tz", "n"},
      {"TL", "n"},     {"Tf", "Nn"},    {"Tr", "n"},     {"Ts", "n"},     {"Td", "nn"},    {"TD", "nn"},  {"Tm", "nnnnnn"},
      {"T*", ""},      {"Tj", "s"},     {"TJ", "a"},     {"'", "s"},      {"\"", "nns"},   {"d0", "nn"},  {"d1", "nnnnnn"},
      {"CS", "N"},     {"cs", "N"},     {"G", "n"},      {"g", "n"},      {"RG", "nnn"},   {"rg", "nnn"}, {"K", "nnnn"},
      {"k", "nnnn"},   {"sh", "N"},     {"Do", "N"},     {"MP", "N"},     {"DP", "Nd"},    {"BMC", "N"},  {"BDC", "Nd"},
      {"EMC", ""},     {"BX", ""},      {"EX", ""},      {"SC", "*"},     {"sc", "*"},     {"SCN", "*"},  {"scn", "*"},
      {"BI", "*"},     {"ID", "*"},     {"EI", "*"}};
  return t;
}

bool operandIs(char t, QPDFObjectHandle o) {
  switch (t) {
    case 'n': return o.isNumber();
    case 'N': return o.isName();
    case 's': return o.isString();
    case 'a': return o.isArray();
    case 'd': return o.isDictionary() || o.isName();
    default: return true;
  }
}

struct Walker : QPDFObjectHandle::ParserCallbacks {
  PageWalk& w;
  QPDFObjectHandle res;
  int page;
  int depth;
  bool form;
  std::set<QPDFObjGen>& seen;
  std::map<QPDFObjGen, PageWalk>& fcache;
  std::vector<QPDFObjectHandle> ops;
  size_t count = 0;
  Mat ctm;
  Box clip;
  std::vector<std::pair<Mat, Box>> stack;
  std::vector<bool> marks;
  int bx = 0;
  bool inText = false, inPath = false, pathClip = false, named = false, done = false;
  Box pathBox;

  Walker(PageWalk& pw, QPDFObjectHandle r, int pg, int d, bool f, std::set<QPDFObjGen>& s,
         std::map<QPDFObjGen, PageWalk>& fc, const Mat& m, const Box& c)
      : w(pw), res(r), page(pg), depth(d), form(f), seen(s), fcache(fc), ctm(m), clip(c) {}

  void flag(const char* p) { w.problems.insert(p); }

  bool lookup(const char* cat, const std::string& name, QPDFObjectHandle* out = nullptr) {
    named = true;
    if (!res.isDictionary()) return false;
    QPDFObjectHandle d = res.getKey(cat);
    if (!d.isDictionary() || !d.hasKey(name)) return false;
    if (out) *out = d.getKey(name);
    return true;
  }

  void need(const char* cat, const std::string& name) {
    if (!name.empty() && !lookup(cat, name)) flag("missing_resource");
  }

  std::string name(size_t i) const { return i < ops.size() && ops[i].isName() ? ops[i].getName() : std::string(); }

  double num(size_t i) const { return i < ops.size() && ops[i].isNumber() ? ops[i].getNumericValue() : 0; }

  void addPt(double x, double y) {
    double tx = ctm.a * x + ctm.c * y + ctm.e, ty = ctm.b * x + ctm.d * y + ctm.f;
    if (!pathBox.valid) pathBox = {tx, ty, tx, ty, true};
    pathBox.x0 = std::min(pathBox.x0, tx);
    pathBox.y0 = std::min(pathBox.y0, ty);
    pathBox.x1 = std::max(pathBox.x1, tx);
    pathBox.y1 = std::max(pathBox.y1, ty);
  }

  void endPath() {
    if (pathClip && pathBox.valid) {
      clip = inter(clip, pathBox);
      if (!clip.valid) clip = Box{0, 0, 0, 0, true};
    }
    inPath = pathClip = false;
    pathBox = Box();
  }

  void colourSpace(const std::string& nm) {
    if (nm.empty() || nm == "/DeviceGray" || nm == "/DeviceRGB" || nm == "/DeviceCMYK" || nm == "/Pattern") return;
    QPDFObjectHandle cs;
    if (!lookup("/ColorSpace", nm, &cs)) {
      flag("missing_resource");
      return;
    }
    std::string fam = cs.isName() ? cs.getName()
                      : cs.isArray() && cs.getArrayNItems() > 0 && cs.getArrayItem(0).isName() ? cs.getArrayItem(0).getName()
                                                                                                 : std::string();
    static const std::set<std::string> known = {"/DeviceGray", "/DeviceRGB", "/DeviceCMYK", "/CalGray",
                                                "/CalRGB",     "/Lab",       "/ICCBased",   "/Indexed",
                                                "/Pattern",    "/Separation", "/DeviceN"};
    if (!known.count(fam)) flag("unknown_colour_space_family");
  }

  void doForm(QPDFObjectHandle xo) {
    QPDFObjectHandle dict = xo.getDict();
    Mat m = ctm;
    QPDFObjectHandle mtx = dict.getKey("/Matrix");
    if (mtx.isArray() && mtx.getArrayNItems() == 6) {
      Mat fm{numOf(mtx.getArrayItem(0), 1), numOf(mtx.getArrayItem(1), 0), numOf(mtx.getArrayItem(2), 0),
             numOf(mtx.getArrayItem(3), 1), numOf(mtx.getArrayItem(4), 0), numOf(mtx.getArrayItem(5), 0)};
      m = mul(fm, ctm);
    }
    Box bb;
    QPDFObjectHandle bba = dict.getKey("/BBox");
    if (bba.isArray() && bba.getArrayNItems() == 4) {
      double a0 = numOf(bba.getArrayItem(0), 0), a1 = numOf(bba.getArrayItem(1), 0);
      double a2 = numOf(bba.getArrayItem(2), 0), a3 = numOf(bba.getArrayItem(3), 0);
      bb = xformBox({std::min(a0, a2), std::min(a1, a3), std::max(a0, a2), std::max(a1, a3), true}, m);
    }
    if (w.forms.size() < 20000) w.forms.push_back({page, inter(bb, clip.valid ? clip : bb), form});
    QPDFObjGen og = xo.getObjGen();
    auto it = fcache.find(og);
    if (it == fcache.end()) {
      if (depth > 12 || !seen.insert(og).second) return;
      PageWalk sub;
      QPDFObjectHandle fres = dict.getKey("/Resources");
      Walker inner(sub, fres.isDictionary() ? fres : res, page, depth + 1, true, seen, fcache, Mat(), Box());
      try {
        QPDFObjectHandle::parseContentStream(xo, &inner);
      } catch (...) {
        sub.problems.insert("stream_state");
      }
      inner.finish();
      if (inner.named && !fres.isDictionary()) sub.resMissing = true;
      seen.erase(og);
      it = fcache.emplace(og, std::move(sub)).first;
    }
    const PageWalk& d = it->second;
    w.problems.insert(d.problems.begin(), d.problems.end());
    w.resMissing = w.resMissing || d.resMissing;
    for (const FormUse& u : d.forms) {
      if (w.forms.size() < 20000) w.forms.push_back({page, u.bbox.valid ? inter(xformBox(u.bbox, m), clip) : Box(), true});
    }
  }

  void inlineImage() {
    bool ok = ops.size() % 2 == 0;
    bool width = false, height = false;
    for (size_t i = 0; ok && i < ops.size(); i += 2) {
      ok = ops[i].isName();
      if (!ok) break;
      std::string k = ops[i].getName();
      width = width || k == "/W" || k == "/Width";
      height = height || k == "/H" || k == "/Height";
    }
    if (!ok || !width || !height) flag("corrupt_inline_dictionary");
  }

  bool operandsOk(const std::string& op) {
    auto it = operandTypes().find(op);
    if (it == operandTypes().end()) {
      if (bx == 0) flag("unknown_operator");
      return false;
    }
    const std::string& t = it->second;
    if (op == "SC" || op == "sc" || op == "SCN" || op == "scn") {
      if (count == 0 || count > ops.size()) {
        flag("wrong_operand_count");
        return false;
      }
      for (size_t i = 0; i < ops.size(); ++i) {
        bool pattern = i + 1 == ops.size() && (op == "SCN" || op == "scn") && ops[i].isName();
        if (!ops[i].isNumber() && !pattern) {
          flag("wrong_operand_type");
          return false;
        }
      }
      return true;
    }
    if (t == "*") return true;
    if (t.empty() && count > 0) {
      flag("operator_takes_no_operands");
      return false;
    }
    if (count != t.size()) {
      flag("wrong_operand_count");
      return false;
    }
    for (size_t i = 0; i < t.size(); ++i) {
      if (!operandIs(t[i], ops[i])) {
        flag("wrong_operand_type");
        return false;
      }
    }
    return true;
  }

  void placement(const std::string& op) {
    static const std::set<std::string> build = {"m", "l", "c", "v", "y", "h", "re"};
    static const std::set<std::string> paint = {"S", "s", "f", "F", "f*", "B", "B*", "b", "b*", "n"};
    static const std::set<std::string> textOnly = {"Td", "TD", "Tm", "T*", "Tj", "TJ", "'", "\""};
    static const std::set<std::string> notInText = {"q", "Q", "cm", "sh", "Do", "BI", "d0", "d1", "BT"};
    bool b = build.count(op) > 0, p = paint.count(op) > 0, cl = op == "W" || op == "W*";
    if (inText && (notInText.count(op) || b || p || cl)) flag("operator_out_of_place");
    else if (!inText && textOnly.count(op)) flag("operator_out_of_place");
    else if (inPath && !b && !p && !cl) flag("operator_out_of_place");
    else if (!inPath && op != "m" && op != "re" && (b || p || cl)) flag("operator_out_of_place");
  }

  void handleObject(QPDFObjectHandle obj) override {
    if (!obj.isOperator()) {
      if (obj.isInlineImage()) return;
      ++count;
      if (ops.size() < 64) ops.push_back(obj);
      return;
    }
    std::string op = obj.getOperatorValue();
    bool good = operandsOk(op);
    if (op != "BI" && op != "ID" && op != "EI") placement(op);
    if (inPath && op != "m" && op != "l" && op != "c" && op != "v" && op != "y" && op != "h" && op != "re" &&
        op != "W" && op != "W*") {
      endPath();
    }
    if (op == "ID") {
      inlineImage();
    } else if (op == "m" || op == "re") {
      inPath = true;
      if (good) {
        addPt(num(0), num(1));
        if (op == "re") addPt(num(0) + num(2), num(1) + num(3));
      }
    } else if (op == "l" || op == "c" || op == "v" || op == "y") {
      for (size_t k = 0; good && k + 1 < ops.size(); k += 2) addPt(num(k), num(k + 1));
    } else if (op == "W" || op == "W*") {
      pathClip = true;
    } else if (op == "q") {
      stack.push_back({ctm, clip});
    } else if (op == "Q") {
      if (stack.empty()) {
        flag("unmatched_operator");
      } else {
        ctm = stack.back().first;
        clip = stack.back().second;
        stack.pop_back();
      }
    } else if (op == "cm" && good) {
      ctm = mul(Mat{num(0), num(1), num(2), num(3), num(4), num(5)}, ctm);
    } else if (op == "BT") {
      inText = true;
    } else if (op == "ET") {
      if (!inText) flag("unmatched_operator");
      inText = false;
    } else if (op == "gs") {
      need("/ExtGState", name(0));
    } else if (op == "CS" || op == "cs") {
      colourSpace(name(0));
    } else if ((op == "SCN" || op == "scn") && good && !name(ops.size() - 1).empty()) {
      need("/Pattern", name(ops.size() - 1));
    } else if (op == "Tf") {
      need("/Font", name(0));
    } else if (op == "Tr" && good && (num(0) < 0 || num(0) > 7 || num(0) != std::floor(num(0)))) {
      flag("unknown_operand_value");
    } else if ((op == "J" || op == "j") && good && (num(0) < 0 || num(0) > 2 || num(0) != std::floor(num(0)))) {
      flag("unknown_operand_value");
    } else if (op == "ri" && good) {
      static const std::set<std::string> intents = {"/AbsoluteColorimetric", "/RelativeColorimetric", "/Saturation",
                                                    "/Perceptual"};
      if (!intents.count(name(0))) flag("unknown_operand_value");
    } else if (op == "sh") {
      need("/Shading", name(0));
    } else if (op == "Do" && !name(0).empty()) {
      QPDFObjectHandle xo;
      if (!lookup("/XObject", name(0), &xo) || !xo.isStream()) {
        flag("missing_resource");
      } else if (nameIs(xo.getDict().getKey("/Subtype"), "/Form")) {
        doForm(xo);
      }
    } else if (op == "BMC") {
      marks.push_back(false);
    } else if (op == "BDC") {
      QPDFObjectHandle props = ops.size() >= 2 ? ops[1] : QPDFObjectHandle::newNull();
      if (good && props.isName() && !lookup("/Properties", props.getName(), &props)) flag("missing_property_list");
      bool mcid = props.isDictionary() && props.hasKey("/MCID");
      if (mcid && std::find(marks.begin(), marks.end(), true) != marks.end()) flag("nested_marked_content");
      marks.push_back(mcid);
    } else if (op == "EMC") {
      if (marks.empty()) flag("unmatched_operator");
      else marks.pop_back();
    } else if (op == "BX") {
      ++bx;
    } else if (op == "EX") {
      if (bx == 0) flag("unmatched_operator");
      else --bx;
    }
    ops.clear();
    count = 0;
  }

  void handleEOF() override {}

  void finish() {
    if (done) return;
    done = true;
    if (count > 0) flag("unused_operands");
    if (!stack.empty() || inText || !marks.empty()) flag("unbalanced_operator");
    if (bx > 0) flag("unbalanced_compatibility");
  }
};

const PageWalk& walkPage(const PageFacts& p, SpatialCache& c) {
  auto it = c.walks.find(p.page);
  if (it != c.walks.end()) return it->second;
  PageWalk& w = c.walks[p.page];
  QPDFObjectHandle pg = p.obj;
  if (!pg.isDictionary()) return w;
  QPDFObjectHandle res = inheritedKey(pg, "/Resources");
  if (!res.isNull() && !res.isDictionary()) w.problems.insert("resources_unreadable");
  std::set<QPDFObjGen> seen;
  Walker walker(w, res, p.page, 0, false, seen, c.formWalks, Mat(), cropOf(p));
  try {
    QPDFPageObjectHelper(pg).parseContents(&walker);
  } catch (...) {
    w.problems.insert("stream_state");
  }
  walker.finish();
  if (walker.named && !res.isDictionary()) w.resMissing = true;
  return w;
}

int pageRotation(const PageFacts& p) {
  QPDFObjectHandle r = inheritedKey(p.obj, "/Rotate");
  int rot = r.isInteger() ? static_cast<int>(r.getIntValue()) : 0;
  return ((rot % 360) + 360) % 360;
}

const Raster* rasterOf(const Events& ev, const PageFacts& p, SpatialCache& c, double dpi) {
  Raster& r = c.raster;
  if (r.page == p.page && r.dpi == dpi) return r.ok ? &r : nullptr;
  r = Raster();
  r.page = p.page;
  r.dpi = dpi;
  if (!ev.raster) return nullptr;
  int w = 0, h = 0;
  std::string rgb;
  if (!ev.raster(p.page - 1, dpi, w, h, rgb) || w <= 0 || h <= 0 || rgb.size() < static_cast<size_t>(w) * h * 3) {
    return nullptr;
  }
  cmsHPROFILE rp = cmsOpenProfileFromMem(kSrgbIcc, kSrgbIccLen);
  cmsHPROFILE kp = cmsOpenProfileFromMem(kCmykIcc, kCmykIccLen);
  cmsHTRANSFORM t = rp && kp ? cmsCreateTransform(rp, TYPE_RGB_8, kp, TYPE_CMYK_8, INTENT_RELATIVE_COLORIMETRIC,
                                                   cmsFLAGS_BLACKPOINTCOMPENSATION)
                             : nullptr;
  r.w = w;
  r.h = h;
  r.cmyk.assign(static_cast<size_t>(w) * h * 4, 0);
  r.used.assign(static_cast<size_t>(w) * h, 0);
  const unsigned char* px = reinterpret_cast<const unsigned char*>(rgb.data());
  for (int y = 0; y < h; ++y) {
    const unsigned char* row = px + static_cast<size_t>(y) * w * 3;
    unsigned char* out = r.cmyk.data() + static_cast<size_t>(y) * w * 4;
    if (t) cmsDoTransform(t, row, out, static_cast<cmsUInt32Number>(w));
    for (int x = 0; x < w; ++x) {
      bool white = row[x * 3] >= 250 && row[x * 3 + 1] >= 250 && row[x * 3 + 2] >= 250;
      if (white) std::fill(out + x * 4, out + x * 4 + 4, 0);
      else if (!t) {
        int k = 255 - std::max({row[x * 3], row[x * 3 + 1], row[x * 3 + 2]});
        out[x * 4 + 3] = static_cast<unsigned char>(k);
      }
      r.used[static_cast<size_t>(y) * w + x] = white ? 0 : 1;
    }
  }
  if (t) cmsDeleteTransform(t);
  if (rp) cmsCloseProfile(rp);
  if (kp) cmsCloseProfile(kp);
  r.crop = cropOf(p);
  r.rot = pageRotation(p);
  r.ok = r.crop.valid;
  return r.ok ? &r : nullptr;
}

void pixelToUser(const Raster& r, double px, double py, double& x, double& y) {
  double s = 72.0 / r.dpi;
  double dx = px * s, dy = py * s;
  const Box& c = r.crop;
  switch (r.rot) {
    case 90: x = c.x0 + dy; y = c.y0 + dx; break;
    case 180: x = c.x1 - dx; y = c.y0 + dy; break;
    case 270: x = c.x1 - dy; y = c.y1 - dx; break;
    default: x = c.x0 + dx; y = c.y1 - dy; break;
  }
}

double rasterDpi(const PageFacts& p, double want) {
  const Box& b = cropOf(p);
  double area = (b.x1 - b.x0) * (b.y1 - b.y0) / (72.0 * 72.0);
  double dpi = std::max(1.0, want);
  if (area > 0 && area * dpi * dpi > 8e6) dpi = std::max(1.0, std::sqrt(8e6 / area));
  return dpi;
}

bool hasEmptyArea(const Raster& r, const Box& area, double minW, double minH) {
  double s = r.dpi / 72.0;
  int rw = std::max(1, static_cast<int>(std::ceil(minW * s))), rh = std::max(1, static_cast<int>(std::ceil(minH * s)));
  int W = r.w, H = r.h;
  std::vector<int> pre(static_cast<size_t>(W + 1) * (H + 1), 0);
  auto at = [&](int x, int y) -> int& { return pre[static_cast<size_t>(y) * (W + 1) + x]; };
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      double ux = 0, uy = 0;
      pixelToUser(r, x + 0.5, y + 0.5, ux, uy);
      bool inside = ux >= area.x0 && ux <= area.x1 && uy >= area.y0 && uy <= area.y1;
      int blocked = (!inside || r.used[static_cast<size_t>(y) * W + x]) ? 1 : 0;
      at(x + 1, y + 1) = blocked + at(x + 1, y) + at(x, y + 1) - at(x, y);
    }
  }
  for (int y = 0; y + rh <= H; ++y) {
    for (int x = 0; x + rw <= W; ++x) {
      if (at(x + rw, y + rh) - at(x + rw, y) - at(x, y + rh) + at(x, y) == 0) return true;
    }
  }
  return false;
}

double windowPeak(const std::vector<double>& v, const std::vector<double>& in, int W, int H, int kx, int ky) {
  if (kx > W || ky > H || W <= 0 || H <= 0) return 0;
  std::vector<double> ps(static_cast<size_t>(W + 1) * (H + 1), 0), pc(ps.size(), 0);
  auto idx = [&](int x, int y) { return static_cast<size_t>(y) * (W + 1) + x; };
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      size_t i = static_cast<size_t>(y) * W + x;
      ps[idx(x + 1, y + 1)] = v[i] + ps[idx(x + 1, y)] + ps[idx(x, y + 1)] - ps[idx(x, y)];
      pc[idx(x + 1, y + 1)] = in[i] + pc[idx(x + 1, y)] + pc[idx(x, y + 1)] - pc[idx(x, y)];
    }
  }
  double best = 0, full = static_cast<double>(kx) * ky;
  for (int y = 0; y + ky <= H; ++y) {
    for (int x = 0; x + kx <= W; ++x) {
      auto sum = [&](const std::vector<double>& p) {
        return p[idx(x + kx, y + ky)] - p[idx(x + kx, y)] - p[idx(x, y + ky)] + p[idx(x, y)];
      };
      if (sum(pc) < full - 0.5) continue;
      best = std::max(best, sum(ps) / full);
    }
  }
  return best;
}

void buildInk(const Events& ev, SpatialCache& c) {
  if (c.inkReady) return;
  c.inkReady = true;
  cmsHPROFILE rgbP = cmsOpenProfileFromMem(kSrgbIcc, kSrgbIccLen);
  cmsHPROFILE cmykP = cmsOpenProfileFromMem(kCmykIcc, kCmykIccLen);
  cmsHTRANSFORM t = rgbP && cmykP ? cmsCreateTransform(rgbP, TYPE_RGB_DBL, cmykP, TYPE_CMYK_DBL, INTENT_RELATIVE_COLORIMETRIC,
                                                       cmsFLAGS_BLACKPOINTCOMPENSATION)
                                  : nullptr;
  auto rgbInk = [&](double r, double g, double b) {
    if (r >= 0.999 && g >= 0.999 && b >= 0.999) return 0.0;
    double sum = (3.0 - r - g - b) * 100.0;
    if (t) {
      double in[3] = {r, g, b}, out[4] = {0, 0, 0, 0};
      cmsDoTransform(t, in, out, 1);
      sum = out[0] + out[1] + out[2] + out[3];
    }
    return std::min(sum, 300.0);
  };
  cmsHPROFILE labP = cmsCreateLab4Profile(nullptr);
  cmsHTRANSFORM lt = labP && cmykP ? cmsCreateTransform(labP, TYPE_Lab_DBL, cmykP, TYPE_CMYK_DBL, INTENT_RELATIVE_COLORIMETRIC,
                                                        cmsFLAGS_BLACKPOINTCOMPENSATION)
                                   : nullptr;
  auto labInk = [&](double l, double a, double b) {
    if (!lt) return std::max(0.0, 100.0 - l) * 3.0;
    cmsCIELab in{l, a, b};
    double out[4] = {0, 0, 0, 0};
    cmsDoTransform(lt, &in, out, 1);
    return std::min(out[0] + out[1] + out[2] + out[3], 300.0);
  };
  auto inkOf = [&](const std::vector<double>& v, const ColorInfo& ci) -> double {
    if (v.empty()) return 0;
    double sum = 0;
    for (double x : v) sum += x;
    if (ci.cls == "cmyk" || (ci.cls == "icc" && ci.declaredComps == 4 && v.size() == 4)) return sum * 100.0;
    if (ci.cls == "gray" || (ci.cls == "icc" && ci.declaredComps == 1) || (ci.cls == "cal" && v.size() == 1)) {
      return (1.0 - v[0]) * 100.0;
    }
    if (ci.cls == "separation" || ci.cls == "devicen") return sum * 100.0;
    if ((ci.cls == "rgb" || ci.cls == "cal" || ci.cls == "icc") && v.size() >= 3) return rgbInk(v[0], v[1], v[2]);
    if (ci.cls == "lab" && v.size() >= 3) return labInk(v[0], v[1], v[2]);
    return 0;
  };
  auto spot = [](const ColorInfo& ci) { return ci.cls == "separation" || ci.cls == "devicen"; };
  for (const PaintEvent& e : ev.paints) {
    if (e.noPaint || e.x.alphaFill <= 0.001) continue;
    InkObj o;
    o.ink = inkOf(e.comps, e.color);
    o.bbox = e.bbox;
    o.cmyk = e.color.cls == "cmyk";
    o.comps = e.comps;
    o.overprint = e.overprint;
    o.opm = e.opm;
    o.spot = spot(e.color);
    c.ink[e.page].push_back(std::move(o));
  }
  for (const TextEvent& e : ev.texts) {
    if (e.renderMode == 3) continue;
    InkObj o;
    o.ink = inkOf(e.comps, e.color);
    o.bbox = e.bbox;
    o.cmyk = e.color.cls == "cmyk";
    o.comps = e.comps;
    o.overprint = e.overprint;
    o.spot = spot(e.color);
    c.ink[e.page].push_back(std::move(o));
  }
  for (const ImageEvent& e : ev.images) {
    RawImage raw = e.obj.isStream() ? decodeImage(e.obj) : RawImage();
    if (!e.obj.isStream() && !e.inlineSamples.empty() && e.width > 0 && e.height > 0) {
      int n = e.color.cls == "cmyk" ? 4 : e.color.cls == "rgb" || e.color.cls == "lab" ? 3 : e.color.cls == "gray" ? 1 : e.color.declaredComps;
      size_t px = static_cast<size_t>(e.width) * e.height;
      QPDFObjectHandle cs = e.color.csObj;
      QPDFObjectHandle lut = e.color.indexed && cs.isArray() && cs.getArrayNItems() >= 4 ? cs.getArrayItem(3) : QPDFObjectHandle();
      std::string table = lut.isString() ? lut.getStringValue() : std::string();
      if (lut.isStream()) {
        auto b = lut.getStreamData(qpdf_dl_generalized);
        table.assign(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
      }
      if (n > 0 && !e.color.indexed && e.inlineSamples.size() >= px * n) {
        raw.samples = e.inlineSamples.substr(0, px * n);
      } else if (n > 0 && e.color.indexed && e.inlineSamples.size() >= px && !table.empty()) {
        for (size_t i = 0; i < px; ++i) {
          size_t at = static_cast<unsigned char>(e.inlineSamples[i]) * static_cast<size_t>(n);
          raw.samples += at + n <= table.size() ? table.substr(at, n) : std::string(n, '\0');
        }
      }
      if (!raw.samples.empty()) {
        raw.ok = true;
        raw.width = e.width;
        raw.height = e.height;
        raw.comps = n;
      }
    }
    if (!raw.ok || raw.width <= 0 || raw.height <= 0 || raw.comps <= 0) {
      if (e.color.cls == "cmyk" || (e.color.cls == "icc" && e.color.declaredComps == 4)) continue;
      InkObj o;
      o.bbox = e.bbox;
      o.overprint = e.overprint;
      c.ink[e.page].push_back(std::move(o));
      continue;
    }
    const unsigned char* d = reinterpret_cast<const unsigned char*>(raw.samples.data());
    size_t px = static_cast<size_t>(raw.width) * raw.height;
    size_t avail = std::min(px, raw.samples.size() / raw.comps);
    auto pixInk = [&](size_t i) {
      const unsigned char* p = d + i * raw.comps;
      if (raw.comps == 4) return (p[0] + p[1] + p[2] + p[3]) / 255.0 * 100.0;
      if (raw.comps == 3) return rgbInk(p[0] / 255.0, p[1] / 255.0, p[2] / 255.0);
      if (raw.comps == 1) return e.color.cls == "separation" ? p[0] / 255.0 * 100.0 : (255 - p[0]) / 255.0 * 100.0;
      return 0.0;
    };
    InkObj o;
    o.bbox = e.bbox;
    o.overprint = e.overprint;
    size_t stride = px > 20000 ? px / 20000 : 1;
    for (size_t i = 0; i < avail; i += stride) o.ink = std::max(o.ink, pixInk(i));
    o.gw = std::min(raw.width, 128);
    o.gh = std::min(raw.height, 128);
    o.grid.assign(static_cast<size_t>(o.gw) * o.gh, 0);
    for (int gy = 0; gy < o.gh; ++gy) {
      for (int gx = 0; gx < o.gw; ++gx) {
        int x0 = gx * raw.width / o.gw, x1 = std::max(x0 + 1, (gx + 1) * raw.width / o.gw);
        int y0 = gy * raw.height / o.gh, y1 = std::max(y0 + 1, (gy + 1) * raw.height / o.gh);
        double sum = 0;
        int n = 0;
        for (int sy = 0; sy < 4; ++sy) {
          for (int sx = 0; sx < 4; ++sx) {
            size_t i = static_cast<size_t>(y0 + (y1 - y0) * sy / 4) * raw.width + x0 + (x1 - x0) * sx / 4;
            if (i < avail) sum += pixInk(i), ++n;
          }
        }
        o.grid[static_cast<size_t>(gy) * o.gw + gx] = n ? sum / n : 0;
      }
    }
    c.ink[e.page].push_back(std::move(o));
  }
  if (lt) cmsDeleteTransform(lt);
  if (labP) cmsCloseProfile(labP);
  if (t) cmsDeleteTransform(t);
  if (rgbP) cmsCloseProfile(rgbP);
  if (cmykP) cmsCloseProfile(cmykP);
}

double inkPeak(const std::vector<InkObj>& objs, const Box& region, double m) {
  auto fits = [&](const Box& b) {
    if (!b.valid) return m <= 1e-9;
    if (m <= 1e-9) return touches(b, region);
    Box i = overlapOf(b, region);
    return i.valid && i.x1 - i.x0 >= m - 1e-9 && i.y1 - i.y0 >= m - 1e-9;
  };
  double best = 0;
  for (const InkObj& o : objs) {
    if (!fits(o.bbox)) continue;
    if (o.grid.empty() || m <= 1e-9) {
      best = std::max(best, o.ink);
      continue;
    }
    double cw = (o.bbox.x1 - o.bbox.x0) / o.gw, ch = (o.bbox.y1 - o.bbox.y0) / o.gh;
    if (cw <= 0 || ch <= 0) continue;
    int kx = std::max(1, static_cast<int>(std::ceil(m / cw - 1e-9))), ky = std::max(1, static_cast<int>(std::ceil(m / ch - 1e-9)));
    std::vector<double> all(o.grid.size(), 1.0);
    best = std::max(best, windowPeak(o.grid, all, o.gw, o.gh, kx, ky));
  }
  if (objs.size() > 2000) return best;
  for (const InkObj& o : objs) {
    if (!o.overprint) continue;
    for (const InkObj& u : objs) {
      Box both = overlapOf(o.bbox, u.bbox);
      if (&o == &u || !both.valid || !fits(both)) continue;
      double combined = 0;
      if (o.spot) {
        combined = o.ink + u.ink;
      } else if (o.cmyk && o.opm == 1 && u.cmyk && o.comps.size() == 4 && u.comps.size() == 4) {
        for (int ch = 0; ch < 4; ++ch) combined += (o.comps[ch] > 0.001 ? o.comps[ch] : u.comps[ch]) * 100.0;
      } else {
        continue;
      }
      best = std::max(best, std::min(combined, 400.0));
    }
  }
  return best;
}

double rasterPeak(const Raster& r, const Box& region, double m) {
  size_t n = static_cast<size_t>(r.w) * r.h;
  std::vector<double> ink(n, 0), in(n, 0);
  for (int y = 0; y < r.h; ++y) {
    for (int x = 0; x < r.w; ++x) {
      double ux = 0, uy = 0;
      pixelToUser(r, x + 0.5, y + 0.5, ux, uy);
      if (!inBox(region, {ux, uy})) continue;
      size_t i = static_cast<size_t>(y) * r.w + x;
      const unsigned char* p = r.cmyk.data() + i * 4;
      in[i] = 1;
      ink[i] = (p[0] + p[1] + p[2] + p[3]) / 255.0 * 100.0;
    }
  }
  int k = std::max(1, static_cast<int>(std::ceil(m * r.dpi / 72.0 - 1e-9)));
  return windowPeak(ink, in, r.w, r.h, k, k);
}

bool trimBleedMissing(const PageFacts& p, double need, const Events& ev, SpatialCache& c) {
  buildIndex(ev, c);
  auto it = c.objs.find(p.page);
  if (it == c.objs.end()) return false;
  Box trim = p.trim.valid ? p.trim : cropOf(p);
  Box limit = p.bleed.valid ? p.bleed : p.media;
  double minFound = 1e18;
  bool touched = false;
  for (int side = 0; side < 4; ++side) {
    double best = -1;
    for (const SObj& o : it->second) {
      if (!inkUsed(o)) continue;
      Box e = inter(o.ext, limit);
      if (!e.valid) continue;
      double reach = -1;
      bool alongV = e.y1 > trim.y0 && e.y0 < trim.y1;
      bool alongH = e.x1 > trim.x0 && e.x0 < trim.x1;
      if (side == 0 && alongV && e.x0 <= trim.x0 + 0.01 && e.x1 > trim.x0) reach = trim.x0 - e.x0;
      if (side == 1 && alongV && e.x1 >= trim.x1 - 0.01 && e.x0 < trim.x1) reach = e.x1 - trim.x1;
      if (side == 2 && alongH && e.y0 <= trim.y0 + 0.01 && e.y1 > trim.y0) reach = trim.y0 - e.y0;
      if (side == 3 && alongH && e.y1 >= trim.y1 - 0.01 && e.y0 < trim.y1) reach = e.y1 - trim.y1;
      best = std::max(best, reach);
    }
    if (best >= 0) {
      touched = true;
      minFound = std::min(minFound, best);
    }
  }
  return touched && minFound < need - 0.01;
}

int dieLineBleedMissing(const std::string& name, double width, const PageFacts& p, const Events& ev, SpatialCache& c) {
  if (!ev.raster || !ev.prof) return -1;
  buildIndex(ev, c);
  static const std::vector<SObj> none;
  auto it = c.objs.find(p.page);
  const std::vector<SObj>& all = it == c.objs.end() ? none : it->second;
  ShapeSet region;
  bool ok = true;
  if (!outline(name, 0, p.page, all, ev, c, region, ok) || !ok) return -1;
  if (region.empty()) return 0;
  Box crop = cropOf(p);
  if (!crop.valid) return -1;
  double dpi = rasterDpi(p, 144);
  Raster r;
  r.dpi = dpi;
  std::string rgb;
  if (!ev.raster(p.page - 1, dpi, r.w, r.h, rgb) || r.w <= 0 || r.h <= 0 ||
      rgb.size() < static_cast<size_t>(r.w) * r.h * 3) {
    return -1;
  }
  r.crop = crop;
  r.rot = pageRotation(p);
  Box bb = grow(setBox(region), width + 4 * 72.0 / dpi);
  size_t n = static_cast<size_t>(r.w) * r.h;
  std::vector<unsigned char> in(n, 0), ink(n, 0), near(n, 0);
  const unsigned char* px = reinterpret_cast<const unsigned char*>(rgb.data());
  for (int y = 0; y < r.h; ++y) {
    for (int x = 0; x < r.w; ++x) {
      size_t i = static_cast<size_t>(y) * r.w + x;
      const unsigned char* q = px + i * 3;
      ink[i] = q[0] < 250 || q[1] < 250 || q[2] < 250;
      double ux = 0, uy = 0;
      pixelToUser(r, x + 0.5, y + 0.5, ux, uy);
      if (!inBox(bb, {ux, uy})) continue;
      near[i] = 1;
      in[i] = inSet(region, {ux, uy});
    }
  }
  std::vector<int> src = nearestSeed(in, r.w, r.h);
  double wpx = width * dpi / 72.0;
  long need = 0, miss = 0;
  for (int y = 0; y < r.h; ++y) {
    for (int x = 0; x < r.w; ++x) {
      size_t i = static_cast<size_t>(y) * r.w + x;
      if (!near[i] || in[i] || src[i] < 0) continue;
      int sx = src[i] % r.w, sy = src[i] / r.w;
      double d = std::hypot(sx - x, sy - y);
      if (d < 1.5 || d > wpx - 0.5 || !ink[static_cast<size_t>(src[i])]) continue;
      ++need;
      if (!ink[i]) ++miss;
    }
  }
  return miss > std::max(2L, need / 500) ? 1 : 0;
}

bool decodes(QPDFObjectHandle s, qpdf_stream_decode_level_e level, std::string* out = nullptr) {
  try {
    Pl_Buffer buf("check");
    if (!s.pipeStreamData(&buf, nullptr, 0, level, true, false)) return false;
    buf.finish();
    if (out) *out = buf.getString();
    return true;
  } catch (...) {
    return false;
  }
}

std::vector<std::string> resourceProblems(const Events& ev) {
  std::set<std::string> out;
  FT_Library lib = nullptr;
  if (FT_Init_FreeType(&lib) == 0) {
    for (const FontFacts& f : ev.fonts) {
      if (!f.embedded || f.type3) continue;
      FT_Face face = nullptr;
      if (f.fontProgram.empty() ||
          FT_New_Memory_Face(lib, reinterpret_cast<const FT_Byte*>(f.fontProgram.data()),
                             static_cast<FT_Long>(f.fontProgram.size()), 0, &face) != 0) {
        out.insert("font");
        break;
      }
      FT_Done_Face(face);
    }
    FT_Done_FreeType(lib);
  }
  std::set<QPDFObjGen> done;
  for (const ImageEvent& e : ev.images) {
    if (!e.obj.isStream() || (e.obj.isIndirect() && !done.insert(e.obj.getObjGen()).second)) continue;
    if (!decodes(e.obj, qpdf_dl_all)) {
      out.insert("image");
      break;
    }
  }
  std::vector<QPDFObjectHandle> icc;
  auto addCs = [&](const ColorInfo& ci) {
    QPDFObjectHandle cs = ci.csObj;
    for (int k = 0; k < 4 && cs.isArray() && cs.getArrayNItems() >= 2; ++k) {
      QPDFObjectHandle head = cs.getArrayItem(0);
      if (nameIs(head, "/ICCBased")) {
        icc.push_back(cs.getArrayItem(1));
        return;
      }
      cs = nameIs(head, "/Indexed") ? cs.getArrayItem(1) : cs.getArrayNItems() >= 3 ? cs.getArrayItem(2) : QPDFObjectHandle();
    }
  };
  for (const PaintEvent& e : ev.paints) addCs(e.color);
  for (const TextEvent& e : ev.texts) addCs(e.color);
  for (const ImageEvent& e : ev.images) addCs(e.color);
  QPDFObjectHandle ois = ev.root.isDictionary() ? ev.root.getKey("/OutputIntents") : QPDFObjectHandle();
  for (int i = 0; ois.isArray() && i < ois.getArrayNItems(); ++i) {
    QPDFObjectHandle oi = ois.getArrayItem(i);
    if (oi.isDictionary() && oi.getKey("/DestOutputProfile").isStream()) icc.push_back(oi.getKey("/DestOutputProfile"));
  }
  done.clear();
  for (QPDFObjectHandle s : icc) {
    if (!s.isStream() || (s.isIndirect() && !done.insert(s.getObjGen()).second)) continue;
    std::string data;
    cmsHPROFILE h = decodes(s, qpdf_dl_generalized, &data)
                        ? cmsOpenProfileFromMem(data.data(), static_cast<cmsUInt32Number>(data.size()))
                        : nullptr;
    if (!h) {
      out.insert("icc_profile");
      break;
    }
    cmsCloseProfile(h);
  }
  std::vector<QPDFObjectHandle> xmp;
  if (ev.root.isDictionary()) xmp.push_back(ev.root.getKey("/Metadata"));
  for (const PageFacts& p : ev.pages) {
    if (p.obj.isDictionary()) xmp.push_back(p.obj.getKey("/Metadata"));
  }
  for (const ImageEvent& e : ev.images) {
    if (e.obj.isStream()) xmp.push_back(e.obj.getDict().getKey("/Metadata"));
  }
  for (QPDFObjectHandle s : xmp) {
    if (!s.isStream()) continue;
    std::string data;
    if (!decodes(s, qpdf_dl_generalized, &data) || !xmpWellFormed(data)) {
      out.insert("xmp");
      break;
    }
  }
  return {out.begin(), out.end()};
}

const PageFacts* pageOf(const Subject& s, const Events& ev) { return s.pageFacts ? s.pageFacts : pageFor(ev, s.page); }

int evalInArea(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  const PageFacts* p = pageOf(s, ev);
  std::string v = p ? areaRelation(a, *p, extentOf(s)) : std::string();
  return v.empty() ? 0 : compare(Value(v), a);
}

int evalRelation(const PfAtom& a, const Subject& s, const Events& ev, bool& supported) {
  SpatialCache& c = cacheOf(ev);
  const std::vector<SObj>* all = nullptr;
  const SObj* o = objOf(s, ev, c, all);
  if (!o || !usable(*o, ev, c)) return 0;
  auto key = std::make_pair(&a, o->p);
  auto it = c.relations.find(key);
  if (it == c.relations.end()) {
    bool ok = true;
    std::vector<std::string> v = relationsOf(a, *o, *all, ev, c, ok);
    if (!ok) {
      supported = false;
      return 0;
    }
    it = c.relations.emplace(key, std::move(v)).first;
  }
  return compare(Value(it->second), a);
}

int evalShapeRelation(const PfAtom& a, const Subject& s, const Events& ev, bool& supported) {
  SpatialCache& c = cacheOf(ev);
  const std::vector<SObj>* all = nullptr;
  const SObj* o = objOf(s, ev, c, all);
  if (!o || !usable(*o, ev, c) || !seen(*o, *all, ev, c)) return 0;
  ShapeSet master;
  bool ok = true;
  if (!outline(param(a, "shape"), paramNum(a, "offset"), o->page, *all, ev, c, master, ok)) {
    if (!ok) supported = false;
    return 0;
  }
  ShapeSet sa{&shapeOf(*o, ev, c, false)};
  Rel r = master.empty() ? kApart : classify(sa, master);
  std::string v = r == kAIn ? "inside" : r == kApart ? "outside" : "crosses";
  double band = paramNum(a, "edge_band");
  if (asks(a, "in_edge_band") && !master.empty() && reaches(sa, master, band, band)) v = "in_edge_band";
  return compare(Value(v), a);
}

int evalVisibility(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  SpatialCache& c = cacheOf(ev);
  const std::vector<SObj>* all = nullptr;
  const SObj* o = objOf(s, ev, c, all);
  if (!o) return 0;
  std::pair<int, int> ob = oblOf(*o, *all, ev, c);
  bool visible = o->vis && ob.first > 0;
  std::string v = visible ? "visible" : "hidden";
  if (visible && ob.second > 0 && asks(a, "partly_covered")) v = "partly_covered";
  return compare(Value(v), a);
}

int evalSharpCorners(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  SpatialCache& c = cacheOf(ev);
  double radius = paramNum(a, "min_radius"), angle = paramNum(a, "max_angle");
  auto sharp = [&](const PaintEvent& e) {
    const Geo& g = geoOf(e, c);
    long n = 0;
    for (double r : g.radii) n += r < radius ? 1 : 0;
    for (double x : g.angles) n += x < angle ? 1 : 0;
    return n;
  };
  auto stroked = [](const PaintEvent& e) { return e.stroke && !e.shade; };
  if (param(a, "scope") == "page") {
    int page = s.paint ? s.paint->page : s.text ? s.text->page : s.image ? s.image->page : s.page;
    const PaintEvent* first = nullptr;
    for (const PaintEvent& e : ev.paints) {
      if (e.page == page && stroked(e)) {
        first = &e;
        break;
      }
    }
    if (s.kind != Domain::kPage && (!first || s.paint != first)) return 0;
    long n = 0;
    for (const PaintEvent& e : ev.paints) n += e.page == page && stroked(e) ? sharp(e) : 0;
    return compare(Value(n), a);
  }
  if (!s.paint || !stroked(*s.paint)) return 0;
  return compare(Value(sharp(*s.paint)), a);
}

int evalStrokeGeometry(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  if (!s.paint || !s.paint->stroke || s.paint->shade) return 0;
  const std::string& m = param(a, "measure");
  if (m == "length") return compare(Value(s.paint->length), a);
  const Geo& g = geoOf(*s.paint, cacheOf(ev));
  const std::vector<double>& v = m == "angle" ? g.angles : g.radii;
  if (v.empty()) return 0;
  double f = m == "angle" ? lengthToPt("1", param(a, "unit")) : 1;
  if (a.op == "<" || a.op == "<=") return compare(Value(*std::min_element(v.begin(), v.end()) * f), a);
  if (a.op == ">" || a.op == ">=") return compare(Value(*std::max_element(v.begin(), v.end()) * f), a);
  for (double x : v) {
    if (compare(Value(x * f), a)) return 1;
  }
  return 0;
}

int evalPageEmpty(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  const PageFacts* p = pageOf(s, ev);
  if (!p) return 0;
  const std::string& within = param(a, "within");
  Box area = cropOf(*p);
  if (within == "trim") area = boxOf(*p, "trim");
  if (within == "bleed") area = p->bleed.valid ? p->bleed : boxOf(*p, "trim");
  SpatialCache& c = cacheOf(ev);
  buildIndex(ev, c);
  bool empty = true;
  auto it = c.objs.find(p->page);
  if (it != c.objs.end()) {
    for (const SObj& o : it->second) {
      if (inkUsed(o) && overlaps(o.ext, area)) {
        empty = false;
        break;
      }
    }
  }
  return compare(Value(empty), a);
}

int evalResourcesMissing(const PfAtom& a, const Subject& s, const Events& ev, bool&) {
  const PageFacts* p = pageOf(s, ev);
  return p ? compare(Value(walkPage(*p, cacheOf(ev)).resMissing), a) : 0;
}

int evalBleedMissing(const PfAtom& a, const Subject& s, const Events& ev, bool& supported) {
  const PageFacts* p = pageOf(s, ev);
  if (!p) return 0;
  SpatialCache& c = cacheOf(ev);
  if (param(a, "shape") == "die_line") {
    std::string name = param(a, "die_line_check");
    if (name.empty() && !a.refs.empty()) name = a.refs[0];
    int m = name.empty() ? -1 : dieLineBleedMissing(name, paramNum(a, "min"), *p, ev, c);
    if (m < 0) {
      supported = false;
      return 0;
    }
    return compare(Value(m == 1), a);
  }
  return compare(Value(trimBleedMissing(*p, paramNum(a, "min"), ev, c)), a);
}

const Raster* rasterFor(const Subject& s, const Events& ev, double dpi, bool& supported) {
  const PageFacts* p = pageOf(s, ev);
  const Raster* r = p ? rasterOf(ev, *p, cacheOf(ev), rasterDpi(*p, dpi)) : nullptr;
  if (p && !r) supported = false;
  return r;
}

int evalEmptyArea(const PfAtom& a, const Subject& s, const Events& ev, bool& supported) {
  const Raster* r = rasterFor(s, ev, 36, supported);
  if (!r) return 0;
  const PageFacts& p = *pageOf(s, ev);
  Box area = p.trim.valid ? inter(p.trim, cropOf(p)) : cropOf(p);
  double m = paramNum(a, "margin");
  bool found = area.valid && hasEmptyArea(*r, area, paramNum(a, "width") + 2 * m, paramNum(a, "height") + 2 * m);
  return compare(Value(found), a);
}

int evalInkArea(const PfAtom& a, const Subject& s, const Events& ev, bool& supported) {
  const std::string& inks = param(a, "inks");
  if (inks == "spot") {
    supported = false;
    return 0;
  }
  const Raster* r = rasterFor(s, ev, paramNum(a, "sample_resolution"), supported);
  if (!r) return 0;
  double thr = paramNum(a, "ink") * 255.0 / 100.0, px = std::pow(72.0 / r->dpi, 2), area = 0;
  int k0 = inks == "black" ? 3 : 0;
  for (size_t i = 0, n = static_cast<size_t>(r->w) * r->h; i < n; ++i) {
    int sum = 0;
    for (int k = k0; k < 4; ++k) sum += r->cmyk[i * 4 + k];
    if (sum > 0 && sum >= thr - 1e-9) area += px;
  }
  return compare(Value(area / lengthToPt("1", param(a, "unit"))), a);
}

int evalInkCoverage(const PfAtom& a, const Subject& s, const Events& ev, bool& supported) {
  const PageFacts* p = pageOf(s, ev);
  if (!p) return 0;
  SpatialCache& c = cacheOf(ev);
  Box region = boxOf(*p, param(a, "box"));
  const std::string& ar = param(a, "area");
  if (!ar.empty()) {
    std::vector<double> v;
    std::string cur;
    for (size_t i = 0; i <= ar.size(); ++i) {
      char ch = i < ar.size() ? ar[i] : ' ';
      if (ch != ' ' && ch != ',' && ch != '\n' && ch != '\t') {
        cur += ch;
        continue;
      }
      if (cur.empty()) continue;
      bool ok = false;
      v.push_back(lengthToPt(cur, "pt", &ok));
      if (!ok) v.clear(), v.resize(5);
      cur.clear();
    }
    if (v.size() != 4) {
      supported = false;
      return 0;
    }
    double x = region.x0 + v[0], y = region.y0 + v[1];
    region = overlapOf(region, {std::min(x, x + v[2]), std::min(y, y + v[3]), std::max(x, x + v[2]), std::max(y, y + v[3]), true});
    if (!region.valid) return compare(Value(0), a);
  }
  double m = paramNum(a, "min_area");
  buildInk(ev, c);
  double peak = inkPeak(c.ink[p->page], region, m);
  bool shaded = false;
  for (const PaintEvent& e : ev.paints) shaded = shaded || (e.page == p->page && (e.shade || e.color.cls == "pattern"));
  if (shaded && ev.raster) {
    if (const Raster* r = rasterOf(ev, *p, c, rasterDpi(*p, paramNum(a, "sample_resolution")))) {
      peak = std::max(peak, rasterPeak(*r, region, m));
    }
  }
  return compare(Value(peak), a);
}

int evalUsedArea(const PfAtom& a, const Subject& s, const Events& ev, bool& supported) {
  const Raster* r = rasterFor(s, ev, 72, supported);
  if (!r) return 0;
  Box used;
  for (int y = 0; y < r->h; ++y) {
    for (int x = 0; x < r->w; ++x) {
      if (!r->used[static_cast<size_t>(y) * r->w + x]) continue;
      for (int k = 0; k < 4; ++k) {
        double ux = 0, uy = 0;
        pixelToUser(*r, x + (k & 1), y + (k >> 1), ux, uy);
        if (!used.valid) used = {ux, uy, ux, uy, true};
        used.x0 = std::min(used.x0, ux);
        used.y0 = std::min(used.y0, uy);
        used.x1 = std::max(used.x1, ux);
        used.y1 = std::max(used.y1, uy);
      }
    }
  }
  return used.valid ? compare(Value::size(used.x1 - used.x0, used.y1 - used.y0), a) : 0;
}

int evalContentStreamError(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  SpatialCache& c = cacheOf(ev);
  if (!c.streamsChecked) {
    c.streamsChecked = true;
    std::set<std::string> all;
    for (const PageFacts& p : ev.pages) {
      const PageWalk& w = walkPage(p, c);
      all.insert(w.problems.begin(), w.problems.end());
    }
    c.streamProblems.assign(all.begin(), all.end());
  }
  return compare(Value(c.streamProblems), a);
}

int evalResourceError(const PfAtom& a, const Subject&, const Events& ev, bool&) {
  SpatialCache& c = cacheOf(ev);
  if (!c.resourcesChecked) {
    c.resourcesChecked = true;
    c.resourceProblems = resourceProblems(ev);
  }
  return compare(Value(c.resourceProblems), a);
}

int evalForm(const PfAtom& a, const FormUse& f, const Events& ev, bool& supported) {
  const std::string& id = a.token;
  if (id == "content.kind") return compare(Value("form_xobject"), a);
  if (id == "content.context") return compare(Value(std::vector<std::string>(f.nested ? 1 : 0, "form_xobject")), a);
  const PageFacts* p = pageFor(ev, f.page);
  if (id == "content.in_area") {
    std::string v = p ? areaRelation(a, *p, f.bbox) : std::string();
    return v.empty() ? 0 : compare(Value(v), a);
  }
  Domain d = domainOf(a);
  if (d == Domain::kDoc || d == Domain::kPage || d == Domain::kNone) {
    return evalCheck(a, subjectFor(Domain::kPage, p, f.page, ev), ev, supported);
  }
  if (a.check >= 0 && id != "content.box_relation" && id != "content.distance_to_box" && id != "content.outside_share" &&
      id != "content.size") {
    return 0;
  }
  PaintEvent pe;
  pe.page = f.page;
  pe.bbox = f.bbox;
  pe.noPaint = true;
  return evalCheck(a, subjectFor(Domain::kPaint, &pe, f.page, ev), ev, supported);
}
}

void loadProfileShapes(const PfProfile& prof, Events& ev) {
  for (const auto& [name, sh] : prof.shapes) {
    ShapeDef s;
    s.box = sh.box;
    s.check = sh.check;
    s.offset = sh.offset;
    ev.shapes[name] = s;
  }
}

long long sweepFormObjects(const std::vector<const PfCondition*>& conds, int logic, const Events& ev,
                           std::map<int, long long>& perPage, bool& supported) {
  SpatialCache& c = cacheOf(ev);
  long long hits = 0;
  for (const PageFacts& p : ev.pages) {
    const PageWalk& w = walkPage(p, c);
    for (const FormUse& f : w.forms) {
      bool combined = logic != 1;
      for (const PfCondition* cond : conds) {
        bool r = true;
        for (const PfAtom& a : cond->atoms) {
          if (evalForm(a, f, ev, supported) == 0) {
            r = false;
            break;
          }
        }
        if (!supported) return hits;
        combined = logic == 1 ? (combined || r) : (combined && r);
      }
      if (combined) {
        ++hits;
        ++perPage[f.page];
      }
    }
  }
  return hits;
}

void registerSpatialChecks(CheckTable& t) {
  t.add("content.in_area", evalInArea);
  t.add("content.relation", evalRelation);
  t.add("content.shape_relation", evalShapeRelation);
  t.add("content.sharp_corners", evalSharpCorners);
  t.add("content.stroke_geometry", evalStrokeGeometry);
  t.add("content.visibility", evalVisibility);
  t.add("page.empty", evalPageEmpty);
  t.add("page.resources_missing", evalResourcesMissing);
  t.add("print.bleed_missing", evalBleedMissing);
  t.add("print.empty_area", evalEmptyArea);
  t.add("print.ink_area", evalInkArea);
  t.add("print.ink_coverage", evalInkCoverage);
  t.add("print.used_area", evalUsedArea);
  t.add("syntax.content_stream_error", evalContentStreamError);
  t.add("syntax.resource_error", evalResourceError);
}
}
