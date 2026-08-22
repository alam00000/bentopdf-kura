#pragma once

#include <cstddef>
#include <cstdint>

namespace pdfa {

constexpr int kMaxJp2BoxNest = 4;
constexpr int kMaxAttrNest = 8;
constexpr int kMaxColorSpaceNest = 8;
constexpr int kMaxContentNest = 12;
constexpr int kMaxResourceNest = 24;
constexpr int kMaxObjectWalk = 64;
constexpr int kMaxPageTreeNest = 96;
constexpr int kMaxOutlineDepth = 128;
constexpr int kMaxStructDepth = 200;

constexpr long long kMaxImagePixels = 100000000LL;
constexpr long long kMaxImageSamples = 400000000LL;
constexpr size_t kMaxInflateBytes = size_t{1} << 29;
constexpr size_t kMaxXmpBytes = size_t{4} << 20;

}
