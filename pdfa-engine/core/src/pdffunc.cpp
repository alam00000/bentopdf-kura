#include "pdffunc.hh"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "util.hh"

namespace pdfa {
namespace {
constexpr double kPi = 3.14159265358979323846;
std::vector<double> numArray(QPDFObjectHandle a) {
  std::vector<double> v;
  if (!a.isArray()) return v;
  for (int i = 0; i < a.getArrayNItems(); ++i) v.push_back(numOf(a.getArrayItem(i), 0));
  return v;
}

double interp(double x, double xmin, double xmax, double ymin, double ymax) {
  if (xmax == xmin) return ymin;
  return ymin + (x - xmin) * (ymax - ymin) / (xmax - xmin);
}

class SampledFn : public PdfFunction {
 public:
  bool init(QPDFObjectHandle fn) {
    QPDFObjectHandle d = fn.getDict();
    domain = numArray(d.getKey("/Domain"));
    range = numArray(d.getKey("/Range"));
    std::vector<double> sz = numArray(d.getKey("/Size"));
    int m = inputs();
    nOut = static_cast<int>(range.size() / 2);
    if (m < 1 || m > 8 || nOut < 1 || nOut > 32 || static_cast<int>(sz.size()) != m) return false;
    bps = d.getKey("/BitsPerSample").isInteger()
              ? static_cast<int>(d.getKey("/BitsPerSample").getIntValue())
              : 0;
    if (bps != 1 && bps != 2 && bps != 4 && bps != 8 && bps != 12 && bps != 16 && bps != 24 &&
        bps != 32) {
      return false;
    }
    long long total = 1;
    for (double s : sz) {
      int si = static_cast<int>(s);
      if (si < 1 || si > 1 << 20) return false;
      size.push_back(si);
      total *= si;
      if (total > 16000000) return false;
    }
    encode = numArray(d.getKey("/Encode"));
    if (static_cast<int>(encode.size()) != 2 * m) {
      encode.clear();
      for (int i = 0; i < m; ++i) {
        encode.push_back(0);
        encode.push_back(size[i] - 1);
      }
    }
    decode = numArray(d.getKey("/Decode"));
    if (decode.size() != range.size()) decode = range;
    try {
      auto buf = fn.getStreamData(qpdf_dl_all);
      data.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
    } catch (...) {
      return false;
    }
    long long needBits = total * nOut * bps;
    if (static_cast<long long>(data.size()) * 8 < needBits) data.resize((needBits + 7) / 8, '\0');
    return true;
  }

  bool compute(const double* in, double* out) const override {
    int m = inputs();
    double x[8];
    clipIn(in, x);
    int lo[8];
    double frac[8];
    for (int i = 0; i < m; ++i) {
      double e = interp(x[i], domain[2 * i], domain[2 * i + 1], encode[2 * i], encode[2 * i + 1]);
      e = std::min(std::max(e, 0.0), static_cast<double>(size[i] - 1));
      lo[i] = static_cast<int>(std::floor(e));
      if (lo[i] >= size[i] - 1) lo[i] = std::max(0, size[i] - 2);
      frac[i] = size[i] == 1 ? 0 : e - lo[i];
    }
    double maxv = bps >= 32 ? 4294967295.0 : static_cast<double>((1ULL << bps) - 1);
    for (int o = 0; o < nOut; ++o) out[o] = 0;
    int corners = 1 << m;
    for (int c = 0; c < corners; ++c) {
      double w = 1;
      long long idx = 0;
      long long stride = 1;
      for (int i = 0; i < m; ++i) {
        int bit = (c >> i) & 1;
        int pos = lo[i] + (size[i] == 1 ? 0 : bit);
        w *= bit ? frac[i] : 1 - frac[i];
        idx += pos * stride;
        stride *= size[i];
      }
      if (w == 0) continue;
      for (int o = 0; o < nOut; ++o) {
        double s = sample(idx * nOut + o) / maxv;
        out[o] += w * interp(s, 0, 1, decode[2 * o], decode[2 * o + 1]);
      }
    }
    clipOut(out);
    return true;
  }

 private:
  double sample(long long k) const {
    long long bit = k * bps;
    unsigned long long v = 0;
    for (int b = 0; b < bps; ++b) {
      long long p = bit + b;
      unsigned char byte = static_cast<unsigned char>(data[static_cast<size_t>(p >> 3)]);
      v = (v << 1) | ((byte >> (7 - (p & 7))) & 1);
    }
    return static_cast<double>(v);
  }
  int bps = 8;
  std::vector<int> size;
  std::vector<double> encode, decode;
  std::string data;
};

class ExpFn : public PdfFunction {
 public:
  bool init(QPDFObjectHandle d) {
    domain = numArray(d.getKey("/Domain"));
    if (domain.size() < 2) domain = {0, 1};
    domain.resize(2);
    range = numArray(d.getKey("/Range"));
    c0 = numArray(d.getKey("/C0"));
    c1 = numArray(d.getKey("/C1"));
    if (c0.empty()) c0 = {0};
    if (c1.empty()) c1 = {1};
    if (c0.size() != c1.size()) return false;
    n = numOf(d.getKey("/N"), 1);
    nOut = static_cast<int>(c0.size());
    if (!range.empty() && static_cast<int>(range.size()) != 2 * nOut) range.clear();
    return nOut > 0 && nOut <= 32;
  }
  bool compute(const double* in, double* out) const override {
    double x;
    clipIn(in, &x);
    double p = (x < 0 && n != std::floor(n)) ? 0 : std::pow(x, n);
    if (!std::isfinite(p)) p = 0;
    for (int o = 0; o < nOut; ++o) out[o] = c0[o] + p * (c1[o] - c0[o]);
    clipOut(out);
    return true;
  }

 private:
  std::vector<double> c0, c1;
  double n = 1;
};

class StitchFn : public PdfFunction {
 public:
  bool init(QPDFObjectHandle d, int depth) {
    domain = numArray(d.getKey("/Domain"));
    if (domain.size() < 2) return false;
    domain.resize(2);
    range = numArray(d.getKey("/Range"));
    QPDFObjectHandle fa = d.getKey("/Functions");
    if (!fa.isArray() || fa.getArrayNItems() < 1) return false;
    for (int i = 0; i < fa.getArrayNItems(); ++i) {
      auto f = PdfFunction::load(fa.getArrayItem(i), depth + 1);
      if (!f || f->inputs() != 1) return false;
      if (i == 0) nOut = f->outputs();
      if (f->outputs() != nOut) return false;
      fns.push_back(std::move(f));
    }
    bounds = numArray(d.getKey("/Bounds"));
    encode = numArray(d.getKey("/Encode"));
    if (bounds.size() + 1 != fns.size()) return false;
    if (encode.size() != 2 * fns.size()) {
      encode.clear();
      for (size_t i = 0; i < fns.size(); ++i) {
        encode.push_back(0);
        encode.push_back(1);
      }
    }
    if (!range.empty() && static_cast<int>(range.size()) != 2 * nOut) range.clear();
    return true;
  }
  bool compute(const double* in, double* out) const override {
    double x;
    clipIn(in, &x);
    size_t k = 0;
    while (k < bounds.size() && x >= bounds[k]) ++k;
    double lo = k == 0 ? domain[0] : bounds[k - 1];
    double hi = k == bounds.size() ? domain[1] : bounds[k];
    double e = interp(x, lo, hi, encode[2 * k], encode[2 * k + 1]);
    if (!fns[k]->compute(&e, out)) return false;
    clipOut(out);
    return true;
  }

 private:
  std::vector<std::unique_ptr<PdfFunction>> fns;
  std::vector<double> bounds, encode;
};

enum PsOp {
  kAbs, kAdd, kAtan, kCeiling, kCos, kCvi, kCvr, kDiv, kExp, kFloor, kIdiv, kLn, kLog, kMod,
  kMul, kNeg, kRound, kSin, kSqrt, kSub, kTruncate, kAnd, kBitshift, kEq, kFalse, kGe, kGt,
  kLe, kLt, kNe, kNot, kOr, kTrue, kXor, kCopy, kDup, kExch, kIndex, kPop, kRoll, kIf,
  kIfelse, kPush, kJump, kJumpFalse
};

struct PsInstr {
  PsOp op;
  double val = 0;
  int target = 0;
};

class CalcFn : public PdfFunction {
 public:
  bool init(QPDFObjectHandle fn) {
    QPDFObjectHandle d = fn.getDict();
    domain = numArray(d.getKey("/Domain"));
    range = numArray(d.getKey("/Range"));
    nOut = static_cast<int>(range.size() / 2);
    if (inputs() < 1 || inputs() > 32 || nOut < 1 || nOut > 32) return false;
    std::string src;
    try {
      auto buf = fn.getStreamData(qpdf_dl_all);
      src.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
    } catch (...) {
      return false;
    }
    size_t pos = src.find('{');
    if (pos == std::string::npos) return false;
    ++pos;
    if (!parseBlock(src, pos, 0)) return false;
    return true;
  }

  bool compute(const double* in, double* out) const override {
    double st[128];
    int sp = 0;
    double x[32];
    clipIn(in, x);
    for (int i = 0; i < inputs(); ++i) st[sp++] = x[i];
    size_t pc = 0;
    long steps = 0;
    while (pc < code.size()) {
      if (++steps > 200000) return false;
      const PsInstr& ins = code[pc++];
      auto need = [&](int n) { return sp >= n; };
      switch (ins.op) {
        case kPush:
          if (sp >= 128) return false;
          st[sp++] = ins.val;
          break;
        case kJump:
          pc = static_cast<size_t>(ins.target);
          break;
        case kJumpFalse:
          if (!need(1)) return false;
          if (st[--sp] == 0) pc = static_cast<size_t>(ins.target);
          break;
        case kAbs: if (!need(1)) return false; st[sp - 1] = std::fabs(st[sp - 1]); break;
        case kNeg: if (!need(1)) return false; st[sp - 1] = -st[sp - 1]; break;
        case kCeiling: if (!need(1)) return false; st[sp - 1] = std::ceil(st[sp - 1]); break;
        case kFloor: if (!need(1)) return false; st[sp - 1] = std::floor(st[sp - 1]); break;
        case kRound: if (!need(1)) return false; st[sp - 1] = std::floor(st[sp - 1] + 0.5); break;
        case kTruncate:
        case kCvi: if (!need(1)) return false; st[sp - 1] = std::trunc(st[sp - 1]); break;
        case kCvr: break;
        case kSqrt: if (!need(1)) return false; st[sp - 1] = std::sqrt(std::max(0.0, st[sp - 1])); break;
        case kSin: if (!need(1)) return false; st[sp - 1] = std::sin(st[sp - 1] * kPi / 180); break;
        case kCos: if (!need(1)) return false; st[sp - 1] = std::cos(st[sp - 1] * kPi / 180); break;
        case kLn: if (!need(1)) return false; st[sp - 1] = st[sp - 1] > 0 ? std::log(st[sp - 1]) : 0; break;
        case kLog: if (!need(1)) return false; st[sp - 1] = st[sp - 1] > 0 ? std::log10(st[sp - 1]) : 0; break;
        case kNot:
          if (!need(1)) return false;
          st[sp - 1] = isBool(st[sp - 1]) ? (st[sp - 1] == 0 ? 1 : 0)
                                           : static_cast<double>(~static_cast<long long>(st[sp - 1]));
          break;
        case kTrue: if (sp >= 128) return false; st[sp++] = 1; break;
        case kFalse: if (sp >= 128) return false; st[sp++] = 0; break;
        case kDup: if (!need(1) || sp >= 128) return false; st[sp] = st[sp - 1]; ++sp; break;
        case kPop: if (!need(1)) return false; --sp; break;
        case kExch: if (!need(2)) return false; std::swap(st[sp - 1], st[sp - 2]); break;
        case kCopy: {
          if (!need(1)) return false;
          int n = static_cast<int>(st[--sp]);
          if (n < 0 || !need(n) || sp + n > 128) return false;
          for (int i = 0; i < n; ++i) st[sp + i] = st[sp - n + i];
          sp += n;
          break;
        }
        case kIndex: {
          if (!need(1)) return false;
          int n = static_cast<int>(st[--sp]);
          if (n < 0 || !need(n + 1) || sp >= 128) return false;
          st[sp] = st[sp - 1 - n];
          ++sp;
          break;
        }
        case kRoll: {
          if (!need(2)) return false;
          int j = static_cast<int>(st[--sp]);
          int n = static_cast<int>(st[--sp]);
          if (n < 0 || !need(n)) return false;
          if (n == 0) break;
          j = ((j % n) + n) % n;
          std::rotate(st + sp - n, st + sp - j, st + sp);
          break;
        }
        default: {
          if (!need(2)) return false;
          double b = st[--sp];
          double a = st[sp - 1];
          double r = 0;
          long long ia = static_cast<long long>(a), ib = static_cast<long long>(b);
          switch (ins.op) {
            case kAdd: r = a + b; break;
            case kSub: r = a - b; break;
            case kMul: r = a * b; break;
            case kDiv: r = b == 0 ? 0 : a / b; break;
            case kIdiv: r = ib == 0 ? 0 : static_cast<double>(ia / ib); break;
            case kMod: r = ib == 0 ? 0 : static_cast<double>(ia % ib); break;
            case kExp: r = std::pow(a, b); if (!std::isfinite(r)) r = 0; break;
            case kAtan: {
              r = std::atan2(a, b) * 180 / kPi;
              if (r < 0) r += 360;
              break;
            }
            case kAnd: r = static_cast<double>(ia & ib); break;
            case kOr: r = static_cast<double>(ia | ib); break;
            case kXor: r = static_cast<double>(ia ^ ib); break;
            case kBitshift:
              r = static_cast<double>(ib >= 0 ? (ia << std::min(ib, 62LL)) : (ia >> std::min(-ib, 62LL)));
              break;
            case kEq: r = a == b; break;
            case kNe: r = a != b; break;
            case kGt: r = a > b; break;
            case kGe: r = a >= b; break;
            case kLt: r = a < b; break;
            case kLe: r = a <= b; break;
            default: return false;
          }
          st[sp - 1] = r;
        }
      }
    }
    if (sp < nOut) return false;
    for (int o = 0; o < nOut; ++o) out[o] = st[sp - nOut + o];
    clipOut(out);
    return true;
  }

 private:
  static bool isBool(double v) { return v == 0 || v == 1; }

  bool parseBlock(const std::string& s, size_t& pos, int depth) {
    if (depth > 64) return false;
    while (pos < s.size()) {
      char c = s[pos];
      if (std::isspace(static_cast<unsigned char>(c))) {
        ++pos;
        continue;
      }
      if (c == '%') {
        while (pos < s.size() && s[pos] != '\n' && s[pos] != '\r') ++pos;
        continue;
      }
      if (c == '}') {
        ++pos;
        return true;
      }
      if (c == '{') {
        ++pos;
        size_t jf = code.size();
        code.push_back({kJumpFalse});
        if (!parseBlock(s, pos, depth + 1)) return false;
        size_t save = pos;
        std::string word = nextWord(s, pos);
        if (word == "if") {
          code[jf].target = static_cast<int>(code.size());
          continue;
        }
        pos = save;
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos >= s.size() || s[pos] != '{') return false;
        ++pos;
        size_t jmp = code.size();
        code.push_back({kJump});
        code[jf].target = static_cast<int>(code.size());
        if (!parseBlock(s, pos, depth + 1)) return false;
        if (nextWord(s, pos) != "ifelse") return false;
        code[jmp].target = static_cast<int>(code.size());
        continue;
      }
      std::string w = nextWord(s, pos);
      if (w.empty()) return false;
      char* end = nullptr;
      double v = std::strtod(w.c_str(), &end);
      if (end && *end == '\0') {
        code.push_back({kPush, v});
        continue;
      }
      static const std::pair<const char*, PsOp> ops[] = {
          {"abs", kAbs}, {"add", kAdd}, {"atan", kAtan}, {"ceiling", kCeiling}, {"cos", kCos},
          {"cvi", kCvi}, {"cvr", kCvr}, {"div", kDiv}, {"exp", kExp}, {"floor", kFloor},
          {"idiv", kIdiv}, {"ln", kLn}, {"log", kLog}, {"mod", kMod}, {"mul", kMul},
          {"neg", kNeg}, {"round", kRound}, {"sin", kSin}, {"sqrt", kSqrt}, {"sub", kSub},
          {"truncate", kTruncate}, {"and", kAnd}, {"bitshift", kBitshift}, {"eq", kEq},
          {"false", kFalse}, {"ge", kGe}, {"gt", kGt}, {"le", kLe}, {"lt", kLt}, {"ne", kNe},
          {"not", kNot}, {"or", kOr}, {"true", kTrue}, {"xor", kXor}, {"copy", kCopy},
          {"dup", kDup}, {"exch", kExch}, {"index", kIndex}, {"pop", kPop}, {"roll", kRoll}};
      bool found = false;
      for (const auto& o : ops) {
        if (w == o.first) {
          code.push_back({o.second});
          found = true;
          break;
        }
      }
      if (!found) return false;
    }
    return depth == 0;
  }

  static std::string nextWord(const std::string& s, size_t& pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    size_t start = pos;
    while (pos < s.size() && !std::isspace(static_cast<unsigned char>(s[pos])) && s[pos] != '{' &&
           s[pos] != '}' && s[pos] != '%') {
      ++pos;
    }
    return s.substr(start, pos - start);
  }

  std::vector<PsInstr> code;
};

class ArrayFn : public PdfFunction {
 public:
  bool init(QPDFObjectHandle fns) {
    for (int i = 0; i < fns.getArrayNItems(); ++i) {
      auto f = PdfFunction::load(fns.getArrayItem(i));
      if (!f || f->outputs() != 1) return false;
      if (i == 0) domain.assign(2 * f->inputs(), 0);
      else if (f->inputs() != inputs()) return false;
      parts.push_back(std::move(f));
    }
    for (size_t i = 0; i < domain.size(); i += 2) domain[i + 1] = 1;
    nOut = static_cast<int>(parts.size());

    return nOut > 0 && nOut <= 32;
  }
  bool compute(const double* in, double* out) const override {
    for (int o = 0; o < nOut; ++o) {
      if (!parts[o]->compute(in, out + o)) return false;
    }
    return true;
  }

 private:
  std::vector<std::unique_ptr<PdfFunction>> parts;

};
}

void PdfFunction::clipIn(const double* in, double* out) const {
  for (int i = 0; i < inputs(); ++i) {
    out[i] = std::min(std::max(in[i], domain[2 * i]), domain[2 * i + 1]);
  }
}

void PdfFunction::clipOut(double* out) const {
  if (static_cast<int>(range.size()) < 2 * nOut) return;
  for (int o = 0; o < nOut; ++o) {
    out[o] = std::min(std::max(out[o], range[2 * o]), range[2 * o + 1]);
  }
}

std::unique_ptr<PdfFunction> PdfFunction::load(QPDFObjectHandle fn, int depth) {
  if (depth > 16) return nullptr;
  QPDFObjectHandle d = fn.isStream() ? fn.getDict() : fn;
  if (!d.isDictionary()) return nullptr;
  long long type = d.getKey("/FunctionType").isInteger() ? d.getKey("/FunctionType").getIntValue()
                                                           : -1;
  if (type == 0 && fn.isStream()) {
    auto f = std::make_unique<SampledFn>();
    if (f->init(fn)) return f;
  } else if (type == 2) {
    auto f = std::make_unique<ExpFn>();
    if (f->init(d)) return f;
  } else if (type == 3) {
    auto f = std::make_unique<StitchFn>();
    if (f->init(d, depth)) return f;
  } else if (type == 4 && fn.isStream()) {
    auto f = std::make_unique<CalcFn>();
    if (f->init(fn)) return f;
  }
  return nullptr;
}

std::unique_ptr<PdfFunction> PdfFunction::loadArray(QPDFObjectHandle fns) {
  if (!fns.isArray()) return load(fns);
  if (fns.getArrayNItems() == 1) return load(fns.getArrayItem(0));
  auto f = std::make_unique<ArrayFn>();
  if (f->init(fns)) return f;
  return nullptr;
}
}
