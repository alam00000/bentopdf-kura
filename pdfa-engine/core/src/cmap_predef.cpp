#include <zlib.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "assets/cjk_cmaps.hh"
#include "fonts_ft.hh"

namespace pdfa {
namespace {
struct Run {
  uint32_t lo;
  uint32_t hi;
  uint32_t cid;
};

struct PredefCmap {
  std::string name;
  int ordering = 0;
  int use = -1;
  int base = -1;
  std::vector<CmapSpace> spaces;
  std::vector<Run> notdefs;
  std::vector<Run> cids;
};

const char* const kOrderings[] = {"GB1", "CNS1", "Japan1", "Korea1"};

std::string unpackData() {
  std::string text;
  for (int i = 0; kCjkCmaps[i]; ++i) text += kCjkCmaps[i];
  std::string packed;
  packed.reserve(kCjkCmapsLen);
  uint32_t acc = 0;
  int bits = 0;
  for (char ch : text) {
    int v = ch >= 'A' && ch <= 'Z'   ? ch - 'A'
            : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26
            : ch >= '0' && ch <= '9' ? ch - '0' + 52
            : ch == '+'              ? 62
            : ch == '/'              ? 63
                                     : -1;
    if (v < 0) continue;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      packed += static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  std::string raw(kCjkCmapsRawLen, '\0');
  uLongf rawLen = kCjkCmapsRawLen;
  if (uncompress(reinterpret_cast<Bytef*>(&raw[0]), &rawLen, reinterpret_cast<const Bytef*>(packed.data()),
                 static_cast<uLong>(packed.size())) != Z_OK) {
    return std::string();
  }
  raw.resize(rawLen);
  return raw;
}

std::vector<PredefCmap> decodeAll() {
  std::vector<PredefCmap> out;
  std::string raw = unpackData();
  size_t p = 0;
  bool bad = false;
  auto byte = [&]() -> uint32_t {
    if (p >= raw.size()) {
      bad = true;
      return 0;
    }
    return static_cast<unsigned char>(raw[p++]);
  };
  auto varint = [&]() -> uint32_t {
    uint32_t v = 0;
    for (int shift = 0; shift < 35 && !bad; shift += 7) {
      uint32_t b = byte();
      v |= (b & 0x7F) << shift;
      if (!(b & 0x80)) break;
    }
    return v;
  };
  auto runs = [&](std::vector<Run>& dst) {
    uint32_t n = varint();
    long long prevHi = -1, prevCid = 0;
    for (uint32_t i = 0; i < n && !bad; ++i) {
      long long lo = prevHi + 1 + varint();
      long long hi = lo + varint();
      uint32_t z = varint();
      long long cid = prevCid + ((z & 1) ? -static_cast<long long>((z + 1) / 2) : static_cast<long long>(z / 2));
      dst.push_back({static_cast<uint32_t>(lo), static_cast<uint32_t>(hi), static_cast<uint32_t>(cid)});
      prevHi = hi;
      prevCid = cid + hi - lo + 1;
    }
  };
  uint32_t count = varint();
  for (uint32_t k = 0; k < count && !bad; ++k) {
    PredefCmap c;
    uint32_t len = byte();
    for (uint32_t i = 0; i < len; ++i) c.name += static_cast<char>(byte());
    c.ordering = static_cast<int>(byte());
    byte();
    byte();
    c.use = static_cast<int>(varint()) - 1;
    c.base = static_cast<int>(varint()) - 1;
    uint32_t ns = varint();
    for (uint32_t i = 0; i < ns && !bad; ++i) {
      int nb = static_cast<int>(byte());
      uint32_t lo = varint();
      uint32_t hi = varint();
      c.spaces.push_back({nb, lo, hi});
    }
    runs(c.notdefs);
    runs(c.cids);
    if (c.ordering > 3 || c.use >= static_cast<int>(count) || c.base >= static_cast<int>(count)) bad = true;
    out.push_back(std::move(c));
  }
  if (bad) out.clear();
  return out;
}

const std::vector<PredefCmap>& allCmaps() {
  static const std::vector<PredefCmap> all = decodeAll();
  return all;
}

int findCmap(const std::string& name) {
  std::string bare = !name.empty() && name[0] == '/' ? name.substr(1) : name;
  const auto& all = allCmaps();
  for (size_t i = 0; i < all.size(); ++i) {
    if (all[i].name == bare) return static_cast<int>(i);
  }
  return -1;
}

void applyRuns(const std::vector<Run>& runs, std::map<uint32_t, int>& code2cid) {
  for (const Run& r : runs) {
    for (uint32_t c = r.lo; c <= r.hi; ++c) code2cid[c] = static_cast<int>(r.cid + (c - r.lo));
  }
}

void applyCmap(int idx, bool notdef, int depth, std::vector<CmapSpace>* spaces, std::map<uint32_t, int>& code2cid) {
  const auto& all = allCmaps();
  if (idx < 0 || depth > 8) return;
  const PredefCmap& c = all[static_cast<size_t>(idx)];
  applyCmap(c.use, notdef, depth + 1, spaces, code2cid);
  if (c.base >= 0) {
    const PredefCmap& b = all[static_cast<size_t>(c.base)];
    applyRuns(notdef ? b.notdefs : b.cids, code2cid);
  }
  applyRuns(notdef ? c.notdefs : c.cids, code2cid);
  if (spaces) spaces->insert(spaces->end(), c.spaces.begin(), c.spaces.end());
}

std::vector<uint32_t> buildCidToUnicode(int ordering) {
  static const char* const kFamilies[] = {"UniGB", "UniCNS", "UniJIS", "UniKS"};
  static const char* const kSuffixes[] = {"-UCS2-H", "-UCS2-HW-H", "-UCS2-V", "-UCS2-HW-V", "-UTF16-H", "-UTF16-V"};
  std::vector<uint32_t> table;
  for (const char* suffix : kSuffixes) {
    int idx = findCmap(std::string(kFamilies[ordering]) + suffix);
    if (idx < 0) continue;
    std::map<uint32_t, int> code2cid;
    applyCmap(idx, false, 0, nullptr, code2cid);
    for (const auto& kv : code2cid) {
      uint32_t uni = kv.first;
      if (uni > 0xFFFF) {
        uint32_t hi = uni >> 16, lo = uni & 0xFFFF;
        if (hi < 0xD800 || hi > 0xDBFF || lo < 0xDC00 || lo > 0xDFFF) continue;
        uni = 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
      }
      uint32_t cid = static_cast<uint32_t>(kv.second);
      if (cid > 0xFFFF) continue;
      if (cid >= table.size()) table.resize(cid + 1, 0);
      if (!table[cid]) table[cid] = uni;
    }
  }
  return table;
}
}

bool predefinedCmap(const std::string& name, std::vector<CmapSpace>& spaces, std::map<uint32_t, int>& code2cid,
                    std::string* ordering) {
  int idx = findCmap(name);
  if (idx < 0) return false;
  applyCmap(idx, true, 0, nullptr, code2cid);
  applyCmap(idx, false, 0, &spaces, code2cid);
  if (ordering) *ordering = kOrderings[allCmaps()[static_cast<size_t>(idx)].ordering];
  return true;
}

uint32_t orderingCidCount(const std::string& ordering) {
  static const uint32_t kCounts[] = {30284, 19179, 23060, 18352};
  for (int i = 0; i < 4; ++i) {
    if (ordering == kOrderings[i]) return kCounts[i];
  }
  return 0;
}

uint32_t orderingCidToUnicode(const std::string& ordering, uint32_t cid) {
  static const std::vector<uint32_t> tables[] = {buildCidToUnicode(0), buildCidToUnicode(1), buildCidToUnicode(2),
                                                 buildCidToUnicode(3)};
  for (int i = 0; i < 4; ++i) {
    if (ordering == kOrderings[i]) return cid < tables[i].size() ? tables[i][cid] : 0;
  }
  return 0;
}
}
