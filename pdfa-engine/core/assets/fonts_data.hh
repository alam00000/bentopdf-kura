#pragma once

#include <cstddef>

namespace pdfa {
struct FontAsset {
  const char* key;
  const char* psName;
  const unsigned char* data;
  unsigned int len;
};

extern const FontAsset kFontAssets[];
extern const unsigned int kFontAssetCount;
}
